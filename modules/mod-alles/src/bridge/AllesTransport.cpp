/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "AllesTransport.h"
#include "Wire.h"
#include <boost/asio.hpp>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace Alles::Bridge
{
namespace
{
void WriteAll(int fd, std::string const& text)
{
    std::size_t done = 0;
    while (done < text.size())
    {
        auto count = write(fd, text.data() + done, text.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("ledger write failed");
        done += count;
    }
}

} // namespace

struct Transport::Impl
{
    using Tcp = boost::asio::ip::tcp;
    struct Client : std::enable_shared_from_this<Client>
    {
        Client(Impl& parent, uint64_t number) : host(parent), id(number), socket(host.io) {}
        void Read()
        {
            boost::asio::async_read_until(socket, boost::asio::dynamic_buffer(input, 65537), '\n',
                                          [self = shared_from_this()](auto error, std::size_t count)
                                          {
                                              if (error || count > 65536 ||
                                                  !self->host.Push({self->id, self->input.substr(0, count)}))
                                              {
                                                  self->Close();
                                                  return;
                                              }
                                              self->input.erase(0, count);
                                              self->Read();
                                          });
        }
        void Send(std::string text)
        {
            if (output.size() >= 16 || text.size() > 65536)
            {
                Close();
                return;
            }
            output.push_back(std::move(text) + '\n');
            if (output.size() == 1)
                Write();
        }
        void Write()
        {
            boost::asio::async_write(socket, boost::asio::buffer(output.front()),
                                     [self = shared_from_this()](auto error, std::size_t)
                                     {
                                         if (error)
                                         {
                                             self->Close();
                                             return;
                                         }
                                         self->output.pop_front();
                                         if (!self->output.empty())
                                             self->Write();
                                     });
        }
        void Close()
        {
            boost::system::error_code ignored;
            socket.close(ignored);
            host.clients.erase(id);
            host.Push({0, boost::json::serialize(boost::json::object{{"disconnected", id}})});
        }
        Impl& host;
        uint64_t id;
        Tcp::socket socket;
        std::string input;
        std::deque<std::string> output;
    };

    Impl(uint16_t port, std::string const& path, std::string const& profile, uint32_t maximum)
        : acceptor(io, Tcp::endpoint(boost::asio::ip::address_v4::loopback(), port)), ledgerPath(path),
          ledgerProfile(profile)
    {
        // Reopening a trial never resets its allowance; malformed or incompatible ledgers fail closed.
        lockFile = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lockFile < 0)
            throw std::runtime_error("cannot open provider ledger lock");
        ledger = open(path.c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (ledger < 0)
        {
            close(lockFile);
            throw std::runtime_error("cannot open provider ledger");
        }
        try
        {
            if (flock(lockFile, LOCK_EX | LOCK_NB) != 0 || flock(ledger, LOCK_EX | LOCK_NB) != 0)
                throw std::runtime_error("provider ledger already owned");
            if (std::filesystem::file_size(path) > 1024 * 1024)
                throw std::runtime_error("provider ledger too large");
            std::ifstream previous(path);
            std::string line;
            while (std::getline(previous, line))
            {
                auto entry = Parse(line).as_object();
                if (String(entry, "profile") != profile)
                    throw std::runtime_error("provider ledger profile mismatch");
                if (entry.contains("charged"))
                {
                    if (charged)
                        throw std::runtime_error("invalid ledger checkpoint");
                    charged = Number(entry, "charged");
                    if (entry.contains("recentBuckets"))
                        for (auto const& value : entry.at("recentBuckets").as_array())
                        {
                            auto const& bucket = value.as_array();
                            if (bucket.size() != 2)
                                throw std::runtime_error("invalid reservation bucket");
                            AddReservation(recent, bucket[0].to_number<uint64_t>(), bucket[1].to_number<uint64_t>());
                        }
                    else
                        for (auto const& value : entry.at("recent").as_array())
                            AddReservation(recent, value.to_number<uint64_t>());
                }
                else
                {
                    if (charged == UINT64_MAX)
                        throw std::runtime_error("reservation count exhausted");
                    ++charged;
                    if (entry.contains("reservedUnixMs"))
                        AddReservation(recent, Number(entry, "reservedUnixMs"));
                }
            }
            if (maximum && charged > maximum)
                throw std::runtime_error("provider ledger exceeds configured budget");
            initialRecent = recent;
            initialCharged = charged;
            Accept();
            thread = std::thread([this] { io.run(); });
        }
        catch (...)
        {
            close(ledger);
            close(lockFile);
            throw;
        }
    }
    ~Impl()
    {
        io.stop();
        if (thread.joinable())
            thread.join();
        close(ledger);
        close(lockFile);
    }
    void Compact()
    {
        if (std::filesystem::file_size(ledgerPath) < 512 * 1024)
            return;
        boost::json::array times;
        for (auto const& [time, count] : recent)
            times.emplace_back(boost::json::array{time, count});
        auto text = boost::json::serialize(
                        boost::json::object{{"profile", ledgerProfile}, {"charged", charged},
                            {"recentBuckets", times}}) +
                    '\n';
        auto temp = ledgerPath + ".checkpoint";
        int replacement = open(temp.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (replacement < 0)
            throw std::runtime_error("cannot create ledger checkpoint");
        try
        {
            WriteAll(replacement, text);
            if (fdatasync(replacement) != 0 || flock(replacement, LOCK_EX | LOCK_NB) != 0)
                throw std::runtime_error("cannot sync ledger checkpoint");
            std::filesystem::rename(temp, ledgerPath);
            int directory = open(std::filesystem::path(ledgerPath).parent_path().c_str(), O_RDONLY | O_DIRECTORY);
            bool const synced = directory >= 0 && fsync(directory) == 0;
            if (directory >= 0)
                close(directory);
            if (!synced)
                throw std::runtime_error("cannot sync checkpoint directory");
            close(ledger);
            ledger = replacement;
        }
        catch (...)
        {
            close(replacement);
            throw;
        }
    }
    void Accept()
    {
        auto client = std::make_shared<Client>(*this, ++nextConnection);
        acceptor.async_accept(client->socket,
                              [this, client](auto error)
                              {
                                  if (!error)
                                  {
                                      if (clients.size() < 16)
                                      {
                                          clients.emplace(client->id, client);
                                          client->Read();
                                      }
                                      else
                                          client->Close();
                                  }
                                  if (acceptor.is_open())
                                      Accept();
                              });
    }
    bool Push(Frame frame)
    {
        std::lock_guard lock(mutex);
        if (incoming.size() >= 64)
            return false;
        incoming.push_back(std::move(frame));
        return true;
    }
    boost::asio::io_context io;
    Tcp::acceptor acceptor;
    std::thread thread;
    std::map<uint64_t, std::shared_ptr<Client>> clients;
    uint64_t nextConnection = 0;
    std::mutex mutex;
    std::vector<Frame> incoming;
    std::optional<Frame> policyCompletion;
    std::atomic<unsigned> posts{0};
    int ledger = -1;
    int lockFile = -1;
    std::string ledgerPath;
    std::string ledgerProfile;
    uint64_t charged = 0;
    uint64_t initialCharged = 0;
    ReservationHistory recent;
    ReservationHistory initialRecent;
    bool ledgerFailed = false;
};

Transport::Transport(uint16_t port, std::string ledger, std::string profile, uint32_t maxRequests)
    : _impl(std::make_unique<Impl>(port, ledger, profile, maxRequests))
{
}
Transport::~Transport() = default;
uint16_t Transport::Port() const
{
    return _impl->acceptor.local_endpoint().port();
}

uint64_t Transport::Charged() const
{
    return _impl->initialCharged;
}

ReservationHistory Transport::RecentReservations() const
{
    return _impl->initialRecent;
}

std::vector<Frame> Transport::Poll()
{
    std::vector<Frame> frames;
    std::lock_guard lock(_impl->mutex);
    frames.swap(_impl->incoming);
    if (_impl->policyCompletion)
    {
        frames.push_back(std::move(*_impl->policyCompletion));
        _impl->policyCompletion.reset();
    }
    return frames;
}

void Transport::Reply(uint64_t connection, std::string text)
{
    if (_impl->posts.fetch_add(1) >= 64)
    {
        --_impl->posts;
        return;
    }
    boost::asio::post(_impl->io,
                      [p = _impl.get(), connection, text = std::move(text)]() mutable
                      {
                          --p->posts;
                          auto found = p->clients.find(connection);
                          if (found != p->clients.end())
                              found->second->Send(std::move(text));
                      });
}

bool Transport::PersistPolicy(std::string path, std::string command, std::string record)
{
    if (_impl->posts.fetch_add(1) >= 64)
    {
        --_impl->posts;
        return false;
    }
    boost::asio::post(_impl->io, [p = _impl.get(), path = std::move(path), command = std::move(command),
        record = std::move(record)]
    {
        --p->posts;
        bool ok = false;
        auto const temporary = path + ".pending";
        int fd = open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        try
        {
            if (fd < 0)
                throw std::runtime_error("cannot open policy override");
            WriteAll(fd, record + '\n');
            if (fsync(fd) != 0)
                throw std::runtime_error("cannot sync policy override");
            std::filesystem::rename(temporary, path);
            auto parent = std::filesystem::path(path).parent_path();
            int directory = open((parent.empty() ? "." : parent.c_str()), O_RDONLY | O_DIRECTORY);
            ok = directory >= 0 && fsync(directory) == 0;
            if (directory >= 0)
                close(directory);
        }
        catch (...)
        {
            ok = false;
        }
        if (fd >= 0)
            close(fd);
        // A full completion mailbox must never silently lose a durable control acknowledgement.
        std::lock_guard lock(p->mutex);
        if (p->incoming.size() < 64)
            p->incoming.push_back({0, boost::json::serialize(boost::json::object{
                {"command", command}, {"ok", ok}})});
        else
            p->policyCompletion = Frame{0, boost::json::serialize(boost::json::object{
                {"command", command}, {"ok", ok}})};
    });
    return true;
}

void Transport::Reserve(std::string permit, std::string record)
{
    if (_impl->posts.fetch_add(1) >= 64)
    {
        --_impl->posts;
        _impl->Push({0, boost::json::serialize(boost::json::object{{"permit", permit}, {"ok", false}})});
        return;
    }
    boost::asio::post(
        _impl->io,
        [p = _impl.get(), permit = std::move(permit), record = std::move(record)]
        {
            --p->posts;
            try
            {
                if (p->ledgerFailed || p->charged == UINT64_MAX)
                    throw std::runtime_error("ledger fault");
                WriteAll(p->ledger, record + '\n');
                if (fdatasync(p->ledger) != 0)
                    throw std::runtime_error("ledger sync failed");
                auto entry = Parse(record).as_object();
                ++p->charged;
                if (entry.contains("reservedUnixMs"))
                {
                    auto const now = Number(entry, "reservedUnixMs");
                    AddReservation(p->recent, now);
                }
                p->Compact();
            }
            catch (...)
            {
                p->ledgerFailed = true;
            }
            p->Push({0, boost::json::serialize(boost::json::object{{"permit", permit}, {"ok", !p->ledgerFailed}})});
        });
}

} // namespace Alles::Bridge

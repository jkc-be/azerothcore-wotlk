#include "Transport.h"
#include "Log.h"
#include <utility>
#include <boost/asio.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <atomic>
#include <charconv>
#include <cmath>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace PythonAPI
{
namespace
{
constexpr std::size_t MaxRequestBytes = 4096;
constexpr std::size_t MaxQueuedCommands = 16;
constexpr auto RequestTimeout = std::chrono::seconds(30);
using TCP = boost::asio::ip::tcp;

template<class T>
T Number(std::string const& value)
{
    T result{};
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("invalid number");
    return result;
}

Command Parse(std::string const& line)
{
    boost::property_tree::ptree fields;
    std::istringstream input(line);
    boost::property_tree::read_json(input, fields);
    std::set<std::string> names;
    for (auto const& field : fields)
        if (!field.second.empty() || !names.insert(field.first).second)
            throw std::invalid_argument("flat unique fields required");

    Command command;
    if (fields.get<std::string>("version") != "1")
        throw std::invalid_argument("unsupported version");
    command.id = Number<uint64_t>(fields.get<std::string>("id"));
    command.operation = fields.get<std::string>("op");
    std::set<std::string> allowed = {"version", "id", "op"};
    if (command.operation != "list")
    {
        allowed.insert("bot");
        command.bot = Number<uint64_t>(fields.get<std::string>("bot"));
        if (!command.bot)
            throw std::invalid_argument("empty bot");
    }
    if (command.operation == "observe" || command.operation == "stop" || command.operation == "move_to"
        || command.operation == "cast")
    {
        allowed.insert("wait_ticks");
        command.waitTicks = Number<uint32_t>(fields.get<std::string>("wait_ticks", "1"));
        if (!command.waitTicks || command.waitTicks > 100)
            throw std::invalid_argument("wait_ticks must be 1..100");
    }
    if (command.operation == "move_to")
    {
        allowed.insert({"x", "y", "z"});
        command.x = Number<float>(fields.get<std::string>("x"));
        command.y = Number<float>(fields.get<std::string>("y"));
        command.z = Number<float>(fields.get<std::string>("z"));
        if (!std::isfinite(command.x) || !std::isfinite(command.y) || !std::isfinite(command.z))
            throw std::invalid_argument("finite coordinates required");
    }
    else if (command.operation == "cast")
    {
        allowed.insert({"spell", "target"});
        command.spell = Number<uint32_t>(fields.get<std::string>("spell"));
        command.target = Number<uint64_t>(fields.get<std::string>("target"));
    }
    else if (command.operation != "list" && command.operation != "claim" && command.operation != "release"
        && command.operation != "observe" && command.operation != "stop" && command.operation != "reset")
        throw std::invalid_argument("unknown operation");

    for (auto const& name : names)
        if (!allowed.count(name))
            throw std::invalid_argument("unknown field");
    return command;
}
}

struct Transport::Impl
{
    struct Session : std::enable_shared_from_this<Session>
    {
        Impl& owner;
        TCP::socket socket;
        boost::asio::steady_timer deadline;
        boost::asio::streambuf input{MaxRequestBytes};
        uint64_t id;
        uint64_t lastRequest = 0;

        Session(Impl& server, TCP::socket connection, uint64_t identity)
            : owner(server), socket(std::move(connection)), deadline(owner.io), id(identity) { }

        void Close()
        {
            boost::system::error_code ignored;
            socket.close(ignored);
            deadline.cancel(ignored);
            if (owner.active.load() == id)
                owner.active.store(0);
        }

        void ArmDeadline()
        {
            deadline.expires_after(RequestTimeout);
            deadline.async_wait([self = shared_from_this()](boost::system::error_code error)
            {
                if (!error)
                    self->Close();
            });
        }

        void Read()
        {
            ArmDeadline();
            boost::asio::async_read_until(socket, input, '\n',
                [self = shared_from_this()](boost::system::error_code error, std::size_t)
            {
                if (error)
                {
                    self->Close();
                    return;
                }
                try
                {
                    std::string line;
                    std::istream stream(&self->input);
                    std::getline(stream, line);
                    Command command = Parse(line);
                    if (!command.id || command.id <= self->lastRequest)
                        throw std::invalid_argument("request IDs must increase");
                    self->lastRequest = command.id;
                    command.client = self->id;
                    std::lock_guard lock(self->owner.mutex);
                    if (self->owner.commands.size() >= MaxQueuedCommands)
                        throw std::invalid_argument("command queue full");
                    self->owner.commands.push_back(std::move(command));
                    // Read the next request only after this request's response has been written.
                }
                catch (std::exception const&)
                {
                    self->Close();
                }
            });
        }

        void Write(std::string message)
        {
            auto output = std::make_shared<std::string>(std::move(message) + "\n");
            boost::asio::async_write(socket, boost::asio::buffer(*output),
                [self = shared_from_this(), output](boost::system::error_code error, std::size_t)
            {
                if (error)
                    self->Close();
                else
                    self->Read();
            });
        }
    };

    boost::asio::io_context io;
    TCP::acceptor acceptor{io};
    std::shared_ptr<Session> session;
    std::atomic<uint64_t> active{0};
    uint64_t nextClient = 0;
    std::mutex mutex;
    std::vector<Command> commands;
    std::thread worker;

    void Accept()
    {
        acceptor.async_accept([this](boost::system::error_code error, TCP::socket socket)
        {
            if (!error)
            {
                if (!active.load())
                {
                    session = std::make_shared<Session>(*this, std::move(socket), ++nextClient);
                    active.store(session->id);
                    session->Read();
                }
                // An additional controller is closed by the socket destructor.
                Accept();
            }
        });
    }
};

Transport::Transport() : _impl(std::make_unique<Impl>()) { }
Transport::~Transport() { Stop(); }

bool Transport::Start(uint16_t port)
{
    try
    {
        TCP::endpoint endpoint(boost::asio::ip::address_v4::loopback(), port);
        _impl->acceptor.open(endpoint.protocol());
        _impl->acceptor.set_option(TCP::acceptor::reuse_address(true));
        _impl->acceptor.bind(endpoint);
        _impl->acceptor.listen();
        _impl->Accept();
        _impl->worker = std::thread([this] { _impl->io.run(); });
        return true;
    }
    catch (std::exception const& error)
    {
        LOG_ERROR("module.pythonapi", "Cannot start Python API: {}", error.what());
        return false;
    }
}

void Transport::Stop()
{
    if (!_impl->worker.joinable())
        return;
    boost::asio::post(_impl->io, [this]
    {
        _impl->acceptor.close();
        if (_impl->session)
            _impl->session->Close();
        _impl->io.stop();
    });
    _impl->worker.join();
}

uint64_t Transport::Client() const { return _impl->active.load(); }

std::vector<Command> Transport::Drain()
{
    std::vector<Command> result;
    std::lock_guard lock(_impl->mutex);
    result.swap(_impl->commands);
    return result;
}

void Transport::Reply(uint64_t client, std::string message)
{
    boost::asio::post(_impl->io, [this, client, message = std::move(message)]() mutable
    {
        if (_impl->active.load() == client && _impl->session)
            _impl->session->Write(std::move(message));
    });
}
}

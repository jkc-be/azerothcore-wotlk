/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Journal.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <regex>
#include <sstream>
#include <system_error>

namespace Alles::Telemetry
{
namespace
{
std::string Stem(std::string const& name)
{
    static std::string const suffix = ".ndjson";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        return name.substr(0, name.size() - suffix.size());
    return name;
}

uint32_t HighestSegment(std::filesystem::path const& directory, std::string const& stem)
{
    uint32_t highest = 0;
    std::regex const pattern("^" + std::regex_replace(stem, std::regex("[.^$|()\\[\\]{}*+?\\\\]"), "\\$&")
        + "\\.(\\d{6})\\.ndjson$");
    std::error_code error;
    for (auto const& entry : std::filesystem::directory_iterator(directory, error))
    {
        std::smatch match;
        auto const filename = entry.path().filename().string();
        if (std::regex_match(filename, match, pattern))
            highest = std::max(highest, uint32_t(std::stoul(match[1].str())));
    }
    return highest;
}
} // namespace

Journal::Journal(std::filesystem::path directory, std::string name, uint64_t segmentBytes, std::size_t queueLimit)
    : _directory(std::move(directory)), _name(std::move(name)), _segmentBytes(segmentBytes), _queueLimit(queueLimit)
{
    std::error_code error;
    auto const path = _directory / _name;
    _bytes = std::filesystem::is_regular_file(path, error) ? std::filesystem::file_size(path, error) : 0;
    if (error)
        _bytes = 0;
    _segments = HighestSegment(_directory, Stem(_name));
    _segmentCount = _segments;
    _stream.open(path, std::ios::app);
    if (!_stream)
        _failed = true;
    else
        _thread = std::thread([this] { Run(); });
}

Journal::~Journal()
{
    {
        std::lock_guard lock(_mutex);
        _stop = true;
    }
    _wake.notify_all();
    if (_thread.joinable())
        _thread.join();
}

bool Journal::Append(std::string line)
{
    if (_failed)
    {
        ++_dropped;
        return false;
    }
    {
        std::lock_guard lock(_mutex);
        if (_stop || _queue.size() >= _queueLimit)
        {
            ++_dropped;
            return false;
        }
        _queue.push_back(std::move(line));
    }
    _wake.notify_one();
    return true;
}

void Journal::Flush()
{
    std::unique_lock lock(_mutex);
    _drained.wait(lock, [this] { return (_queue.empty() && !_writing) || _failed || !_thread.joinable(); });
}

JournalStatus Journal::Status() const
{
    return {_records.load(), _dropped.load(), _written.load(), _segmentCount.load(), _failed.load()};
}

void Journal::Run()
{
    while (true)
    {
        std::vector<std::string> batch;
        {
            std::unique_lock lock(_mutex);
            _wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return _stop || !_queue.empty(); });
            batch.assign(std::make_move_iterator(_queue.begin()), std::make_move_iterator(_queue.end()));
            _queue.clear();
            _writing = !batch.empty();
            if (batch.empty() && _stop)
                return;
        }
        if (!batch.empty())
        {
            try
            {
                Write(batch);
            }
            catch (std::exception const&)
            {
                _failed = true;
                _dropped += batch.size();
            }
        }
        {
            std::lock_guard lock(_mutex);
            _writing = false;
        }
        _drained.notify_all();
        if (_failed)
        {
            std::lock_guard lock(_mutex);
            _dropped += _queue.size();
            _queue.clear();
            if (_stop)
                return;
        }
    }
}

void Journal::Write(std::vector<std::string> const& batch)
{
    for (auto const& line : batch)
    {
        _stream << line << '\n';
        _bytes += line.size() + 1;
    }
    _stream.flush();
    if (!_stream)
        throw std::runtime_error("telemetry journal write failed");
    _records += batch.size();
    _written = _bytes;
    if (_segmentBytes && _bytes >= _segmentBytes)
        Rotate();
}

// Only whole lines move: rotation happens after a flushed batch, never inside one.
void Journal::Rotate()
{
    _stream.close();
    std::ostringstream segment;
    segment << Stem(_name) << '.' << std::setw(6) << std::setfill('0') << ++_segments << ".ndjson";
    std::filesystem::rename(_directory / _name, _directory / segment.str());
    _stream.open(_directory / _name, std::ios::app);
    _bytes = 0;
    _written = 0;
    _segmentCount = _segments;
    if (!_stream)
        throw std::runtime_error("telemetry journal rotation failed");
}

std::size_t ArchivePreviousRun(std::filesystem::path const& directory, std::string label)
{
    std::erase_if(label, [](unsigned char c) { return !std::isalnum(c) && c != '-' && c != '_' && c != '.'; });
    if (label.empty() || label == "." || label == "..")
        label = "previous";
    std::error_code error;
    std::vector<std::filesystem::path> found;
    for (auto const& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (!entry.is_regular_file(error))
            continue;
        auto const name = entry.path().filename().string();
        bool const journal = name.ends_with(".ndjson")
            && (name.starts_with("events") || name.starts_with("snapshots") || name == "rollup.ndjson"
                || name == "milestones.ndjson" || name == "progression.ndjson");
        if (journal || name == "initial.json" || name == "latest.json" || name == "manifest.json")
            found.push_back(entry.path());
    }
    if (found.empty())
        return 0;
    auto target = directory / "archive" / label;
    for (unsigned attempt = 1; std::filesystem::exists(target, error) && attempt < 1000; ++attempt)
        target = directory / "archive" / (label + "-" + std::to_string(attempt));
    if (!std::filesystem::create_directories(target, error) && error)
        return 0;
    std::filesystem::permissions(target, std::filesystem::perms::owner_all, error);
    std::size_t moved = 0;
    for (auto const& path : found)
    {
        std::filesystem::rename(path, target / path.filename(), error);
        if (!error)
            ++moved;
    }
    return moved;
}

std::string PreviousRunLabel(std::filesystem::path const& directory)
{
    std::ifstream manifest(directory / "manifest.json");
    std::string text((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
    std::smatch match;
    if (std::regex_search(text, match, std::regex("\"run\"\\s*:\\s*\"([^\"]{1,64})\"")))
        return match[1].str();
    return "previous-" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
}

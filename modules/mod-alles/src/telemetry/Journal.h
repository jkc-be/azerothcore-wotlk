/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_TELEMETRY_JOURNAL_H
#define MOD_ALLES_TELEMETRY_JOURNAL_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Alles::Telemetry
{
struct JournalStatus
{
    uint64_t records = 0;
    uint64_t dropped = 0;
    uint64_t bytes = 0;
    uint32_t segments = 0;
    bool failed = false;
};

// Append-only NDJSON file written by its own thread from a bounded queue. Rotation mirrors the Observatory
// journal: once a flushed batch takes the file past `segmentBytes` it is renamed `stem.NNNNNN.ndjson` and a
// fresh file continues, so the bridge can retain a bounded window and prune older segments. Telemetry is
// best effort: a full queue or a failing disk drops records and counts them; gameplay never waits on it.
class Journal
{
public:
    // A nonempty manifest also publishes latest.json from each flushed batch, on the same writer thread.
    Journal(std::filesystem::path directory, std::string name, uint64_t segmentBytes, std::size_t queueLimit = 4096,
        std::string manifest = "");
    ~Journal();
    Journal(Journal const&) = delete;
    Journal& operator=(Journal const&) = delete;

    // Any thread. False when the bounded queue is full or the writer has failed.
    bool Append(std::string line);
    // Blocks until every accepted line has been handed to the file (tests and shutdown).
    void Flush();
    JournalStatus Status() const;

private:
    void Run();
    void Write(std::vector<std::string> const& batch);
    void Rotate();

    std::filesystem::path const _directory;
    std::string const _name;
    uint64_t const _segmentBytes;
    std::size_t const _queueLimit;
    std::string const _manifest;
    std::ofstream _stream;
    uint64_t _bytes = 0;
    uint32_t _segments = 0;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::condition_variable _drained;
    std::deque<std::string> _queue;
    bool _stop = false;
    bool _writing = false;
    std::atomic<uint64_t> _records{0};
    std::atomic<uint64_t> _dropped{0};
    std::atomic<uint64_t> _written{0};
    std::atomic<uint32_t> _segmentCount{0};
    std::atomic<bool> _failed{false};
    std::thread _thread;
};

// Moves a previous run's journals, snapshots and the bridge's derived files into `archive/<label>/` so a
// new run starts from an empty directory. Returns how many entries moved; failures leave files in place.
std::size_t ArchivePreviousRun(std::filesystem::path const& directory, std::string label);
// The run recorded in the directory's manifest.json, or a timestamp when there is none.
std::string PreviousRunLabel(std::filesystem::path const& directory);
}

#endif

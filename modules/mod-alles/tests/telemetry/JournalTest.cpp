/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "telemetry/Journal.h"
#include "gtest/gtest.h"
#include <fstream>
#include <unistd.h>

namespace Alles::Telemetry
{
namespace
{
std::filesystem::path FreshDirectory(char const* name)
{
    auto path = std::filesystem::temp_directory_path()
        / ("alles-telemetry-" + std::to_string(getpid()) + "-" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<std::string> Lines(std::filesystem::path const& path)
{
    std::vector<std::string> lines;
    std::ifstream input(path);
    for (std::string line; std::getline(input, line);)
        lines.push_back(line);
    return lines;
}
} // namespace

TEST(AllesTelemetryJournal, AppendsWholeLinesRotatesAfterFlushedBatchesAndContinuesNumbering)
{
    auto const directory = FreshDirectory("journal");
    std::string const line(30, 'x');
    {
        Journal journal(directory, "events.ndjson", 64);
        for (unsigned index = 0; index < 3; ++index)
        {
            ASSERT_TRUE(journal.Append(line));
            journal.Flush();
        }
        auto const status = journal.Status();
        EXPECT_EQ(status.records, 3u);
        EXPECT_EQ(status.dropped, 0u);
        EXPECT_EQ(status.segments, 1u);
        EXPECT_FALSE(status.failed);
    }
    EXPECT_EQ(Lines(directory / "events.000001.ndjson").size(), 3u);
    EXPECT_TRUE(Lines(directory / "events.ndjson").empty());
    {
        Journal journal(directory, "events.ndjson", 64);
        for (unsigned index = 0; index < 3; ++index)
            journal.Append(line);
    }
    EXPECT_EQ(Lines(directory / "events.000002.ndjson").size(), 3u);
    EXPECT_TRUE(std::filesystem::exists(directory / "events.000001.ndjson"));
    std::filesystem::remove_all(directory);
}

TEST(AllesTelemetryJournal, UnwritableTargetCountsDropsInsteadOfThrowing)
{
    auto const directory = FreshDirectory("journal-missing");
    Journal journal(directory / "missing", "events.ndjson", 0);
    EXPECT_FALSE(journal.Append("{}"));
    auto const status = journal.Status();
    EXPECT_TRUE(status.failed);
    EXPECT_EQ(status.dropped, 1u);
    EXPECT_EQ(status.records, 0u);
    journal.Flush();
    std::filesystem::remove_all(directory);
}

TEST(AllesTelemetryJournal, ArchivesThePreviousRunAndLeavesOtherFilesAlone)
{
    auto const directory = FreshDirectory("archive");
    for (auto const* name : {"events.ndjson", "events.000001.ndjson", "snapshots.ndjson", "rollup.ndjson",
             "progression.ndjson", "milestones.ndjson", "events-rollup.ndjson", "latest.json", "initial.json",
             "notes.txt"})
        std::ofstream(directory / name) << "{}\n";
    std::ofstream(directory / "manifest.json") << "{\"schema\":1,\"run\":\"alles-42/../x\"}\n";
    EXPECT_EQ(PreviousRunLabel(directory), "alles-42/../x");
    EXPECT_EQ(ArchivePreviousRun(directory, PreviousRunLabel(directory)), 10u);
    auto const archived = directory / "archive" / "alles-42..x";
    EXPECT_TRUE(std::filesystem::exists(archived / "events.000001.ndjson"));
    EXPECT_TRUE(std::filesystem::exists(archived / "manifest.json"));
    EXPECT_FALSE(std::filesystem::exists(directory / "events.ndjson"));
    EXPECT_TRUE(std::filesystem::exists(directory / "notes.txt"));
    EXPECT_EQ(ArchivePreviousRun(directory, "again"), 0u);
    EXPECT_TRUE(PreviousRunLabel(directory).starts_with("previous-"));
    std::filesystem::remove_all(directory);
}
}

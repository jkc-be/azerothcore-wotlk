/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "telemetry/Recorder.h"
#include "gtest/gtest.h"
#include <fstream>
#include <unistd.h>

namespace Alles::Telemetry
{
namespace
{
ActorKey const Humana{ActorKind::Player, 7};
ActorKey const Humanb{ActorKind::Player, 8};

std::filesystem::path FreshDirectory(char const* name)
{
    auto path = std::filesystem::temp_directory_path()
        / ("alles-recorder-" + std::to_string(getpid()) + "-" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<boost::json::object> Records(Recorder& recorder, std::filesystem::path const& directory)
{
    recorder.Flush();
    std::vector<boost::json::object> records;
    std::ifstream input(directory / "events.ndjson");
    for (std::string line; std::getline(input, line);)
        records.push_back(boost::json::parse(line).as_object());
    return records;
}

std::vector<std::string> Kinds(std::vector<boost::json::object> const& records)
{
    std::vector<std::string> kinds;
    for (auto const& record : records)
        kinds.emplace_back(record.at("kind").as_string());
    return kinds;
}

LiveEvent Event(uint32_t guidLow, std::string kind, uint64_t value, std::string detail, uint64_t realMs)
{
    LiveEvent event;
    event.guidLow = guidLow;
    event.id = BotIdentity({ActorKind::Player, guidLow});
    event.kind = std::move(kind);
    event.value = value;
    event.detail = std::move(detail);
    event.context = "ctx";
    event.realMs = realMs;
    return event;
}

Memory MakeMemory(uint64_t id, FormationMode formation = FormationMode::Fallback)
{
    Memory memory;
    memory.id = id;
    memory.kind = MemoryKind::HeardStatement;
    memory.subject.name = "Hogger";
    memory.source.name = "Humanb";
    memory.claim = "Hogger is near the mill";
    memory.confidence = 0.5;
    memory.salience = 0.5;
    memory.formation = formation;
    return memory;
}

OwnerStatus Ready(uint64_t generation, uint64_t committed = 0, bool saveFailed = false)
{
    return {ActorState::Ready, generation, 1, committed, committed, saveFailed, false, 0};
}
} // namespace

TEST(AllesTelemetryRecorder, CountsAiUpdatesAndProgressionForConfiguredOwnersOnly)
{
    auto const directory = FreshDirectory("counters");
    Recorder recorder(directory, "run-1", {Humana, Humanb}, 0, 1000);
    EXPECT_TRUE(recorder.Live(Event(7, "ai_update", 0, "", 1500)));
    EXPECT_TRUE(recorder.Live(Event(7, "ai_update", 0, "", 2500)));
    EXPECT_TRUE(recorder.Live(Event(7, "xp", 30, "", 2600)));
    EXPECT_TRUE(recorder.Live(Event(7, "death", 0, "", 2700)));
    EXPECT_TRUE(recorder.Live(Event(7, "quest_reward", 176, "", 2800)));
    EXPECT_TRUE(recorder.Live(Event(99, "ai_update", 0, "", 2900)));
    EXPECT_TRUE(recorder.Live(Event(99, "xp", 500, "", 2900)));
    auto const counters = recorder.Counters(Humana);
    ASSERT_TRUE(counters);
    EXPECT_EQ(counters->aiUpdates, 2u);
    EXPECT_EQ(counters->lastAiMs, 2500u);
    EXPECT_EQ(counters->xp, 30u);
    EXPECT_EQ(counters->deaths, 1u);
    EXPECT_EQ(counters->quests, 1u);
    EXPECT_FALSE(recorder.Counters({ActorKind::Player, 99}));
    EXPECT_FALSE(recorder.Counters({ActorKind::CreatureSpawn, 7}));
    EXPECT_EQ(recorder.ActiveBots({7, 8}, 5000), 1u);
    EXPECT_EQ(recorder.ActiveBots({7, 8}, 20000), 0u);
    EXPECT_EQ(recorder.Totals().xp, 30u);
    EXPECT_EQ(recorder.Totals().deaths, 1u);
    EXPECT_EQ(recorder.SimMs(2500), 1500u);
    EXPECT_EQ(recorder.SimMs(10), 0u);
    // AI updates are counted only; progression is journaled in Observatory record shape, in arrival order.
    EXPECT_EQ(recorder.Drain(), 3u);
    auto const records = Records(recorder, directory);
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(Kinds(records), (std::vector<std::string>{"xp", "death", "quest_reward"}));
    EXPECT_EQ(records[0].at("run").as_string(), "run-1");
    EXPECT_EQ(records[0].at("seq").to_number<uint64_t>(), 1u);
    EXPECT_EQ(records[2].at("seq").to_number<uint64_t>(), 3u);
    EXPECT_EQ(records[0].at("simMs").to_number<uint64_t>(), 1600u);
    EXPECT_EQ(records[0].at("bot").as_string(), BotIdentity(Humana));
    EXPECT_EQ(records[0].at("value").to_number<uint64_t>(), 30u);
    EXPECT_EQ(records[0].at("context").as_string(), "ctx");
    EXPECT_EQ(records[0].at("map").to_number<uint64_t>(), 0u);
    EXPECT_EQ(recorder.Status().at("liveDropped").to_number<uint64_t>(), 0u);
    std::filesystem::remove_all(directory);
}

TEST(AllesTelemetryRecorder, OfflineOwnersKeepProgressWithoutCountingAsActive)
{
    auto const directory = FreshDirectory("offline");
    Recorder recorder(directory, "run-1", {Humana, Humanb}, 0, 1000);
    recorder.Live(Event(7, "ai_update", 0, "", 1500));
    recorder.Live(Event(7, "xp", 30, "", 1500));
    recorder.Live(Event(8, "ai_update", 0, "", 1500));
    EXPECT_EQ(recorder.ActiveBots({7, 8}, 1600), 2u);
    EXPECT_EQ(recorder.ActiveBots({8}, 1600), 1u);
    EXPECT_EQ(recorder.ActiveBots({}, 1600), 0u);
    EXPECT_EQ(recorder.ActiveBots({99}, 1600), 0u);
    EXPECT_EQ(recorder.Totals().xp, 30u);
    EXPECT_EQ(recorder.Counters(Humana)->xp, 30u);
    std::filesystem::remove_all(directory);
}

TEST(AllesTelemetryRecorder, RemembersTheLastCompletedActionAndBoundsTheLiveQueue)
{
    auto const directory = FreshDirectory("actions");
    Recorder recorder(directory, "run-1", {Humana}, 0, 0);
    recorder.Live(Event(7, "bot_action", 0, "loot", 100));
    recorder.Live(Event(7, "bot_action", 0, "move to travel target", 200));
    auto const counters = recorder.Counters(Humana);
    ASSERT_TRUE(counters);
    EXPECT_EQ(counters->actions, 2u);
    EXPECT_EQ(counters->lastAction, "move to travel target");
    EXPECT_EQ(counters->lastActionMs, 200u);
    unsigned dropped = 0;
    for (unsigned index = 0; index < 5000; ++index)
        if (!recorder.Live(Event(7, "bot_action", 0, "spam", 300)))
            ++dropped;
    EXPECT_EQ(dropped, 5000u + 2u - 4096u);
    EXPECT_EQ(recorder.Counters(Humana)->actions, 5002u);
    EXPECT_EQ(recorder.Status().at("liveDropped").to_number<uint64_t>(), dropped);
    EXPECT_EQ(recorder.Drain(100), 100u);
    EXPECT_EQ(recorder.Drain(5000), 4096u - 100u);
    EXPECT_EQ(recorder.Drain(), 0u);
    EXPECT_EQ(Records(recorder, directory).size(), 4096u);
    std::filesystem::remove_all(directory);
}

TEST(AllesTelemetryRecorder, EmitsNewAndRevisedMemoriesOnceAndNeverTheLoadedBaseline)
{
    auto const directory = FreshDirectory("memories");
    Recorder recorder(directory, "run-1", {Humana}, 0, 0);
    OwnerSnapshot snapshot;
    snapshot.owner = Humana;
    snapshot.memories = {MakeMemory(1), MakeMemory(2)};
    recorder.RecordOwner(Humana, std::nullopt, nullptr, 10);
    recorder.RecordOwner(Humana, Ready(1), &snapshot, 20);
    recorder.RecordOwner(Humana, Ready(1), &snapshot, 30);
    auto records = Records(recorder, directory);
    ASSERT_EQ(Kinds(records), (std::vector<std::string>{"alles_owner"}));
    EXPECT_EQ(records[0].at("detail").as_string(), "ready");
    EXPECT_EQ(records[0].at("simMs").to_number<uint64_t>(), 20u);

    snapshot.memories.push_back(MakeMemory(3, FormationMode::Model));
    recorder.RecordOwner(Humana, Ready(1), &snapshot, 40);
    recorder.RecordOwner(Humana, Ready(1), &snapshot, 50);
    records = Records(recorder, directory);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1].at("kind").as_string(), "alles_memory");
    EXPECT_EQ(records[1].at("value").to_number<uint64_t>(), 3u);
    EXPECT_EQ(records[1].at("context").as_string(), "model");
    EXPECT_EQ(records[1].at("simMs").to_number<uint64_t>(), 40u);
    EXPECT_FALSE(records[1].at("detail").as_string().empty());

    snapshot.memories[1].contentRevision = 2;
    recorder.RecordOwner(Humana, Ready(1, 5), &snapshot, 60);
    records = Records(recorder, directory);
    ASSERT_EQ(records.size(), 4u);
    EXPECT_EQ(records[2].at("kind").as_string(), "alles_save");
    EXPECT_EQ(records[2].at("detail").as_string(), "committed");
    EXPECT_EQ(records[2].at("value").to_number<uint64_t>(), 5u);
    EXPECT_EQ(records[3].at("kind").as_string(), "alles_memory");
    EXPECT_EQ(records[3].at("value").to_number<uint64_t>(), 2u);
    EXPECT_EQ(records[3].at("context").as_string(), "revised");

    // A reload replays persisted memories: a new generation resets the baseline silently.
    snapshot.memories.push_back(MakeMemory(4));
    recorder.RecordOwner(Humana, Ready(2, 5), &snapshot, 70);
    EXPECT_EQ(Records(recorder, directory).size(), 4u);
    snapshot.memories.push_back(MakeMemory(5));
    recorder.RecordOwner(Humana, Ready(2, 5, true), &snapshot, 80);
    recorder.RecordOwner(Humana, std::nullopt, nullptr, 90);
    records = Records(recorder, directory);
    ASSERT_EQ(records.size(), 7u);
    EXPECT_EQ(records[4].at("kind").as_string(), "alles_save");
    EXPECT_EQ(records[4].at("detail").as_string(), "failed");
    EXPECT_EQ(records[5].at("kind").as_string(), "alles_memory");
    EXPECT_EQ(records[5].at("value").to_number<uint64_t>(), 5u);
    EXPECT_EQ(records[6].at("kind").as_string(), "alles_owner");
    EXPECT_EQ(records[6].at("detail").as_string(), "unloaded");
    std::filesystem::remove_all(directory);
}

TEST(AllesTelemetryRecorder, DescribesPerceptionsSpeechAndInterpreterChanges)
{
    auto const directory = FreshDirectory("described");
    Recorder recorder(directory, "run-1", {Humana}, 0, 0);
    Perception heard;
    heard.kind = PerceptionKind::Speech;
    heard.source.name = "Humanb";
    heard.text = "Hogger is near the mill";
    heard.place = "Northshire";
    recorder.RecordPerception(Humana, heard, true, 100);
    heard.comprehended = false;
    heard.text.clear();
    recorder.RecordPerception(Humana, heard, false, 110);
    Perception death;
    death.kind = PerceptionKind::WitnessedDeath;
    death.subject.name = "Humanb";
    death.source.name = "Hogger";
    recorder.RecordPerception(Humana, death, true, 120);
    recorder.RecordSpeech(Humana, "I saw Humanb die.", true, 130);
    recorder.RecordSpeech(Humana, "I saw Humanb die.", false, 140);
    recorder.RecordInterpreter(true, 3, 150);
    recorder.RecordInterpreter(true, 3, 160);
    recorder.RecordInterpreter(true, 4, 170);
    recorder.RecordInterpreter(false, 4, 180);
    recorder.Record(Humana, "alles_conversation", 1, "reply to Humanb: \"Hello\"", "delivered", 190);
    recorder.RecordSnapshot("{\"seq\":1}");
    auto const records = Records(recorder, directory);
    ASSERT_EQ(records.size(), 9u);
    EXPECT_EQ(records[8].at("kind").as_string(), "alles_conversation");
    EXPECT_EQ(records[8].at("bot").as_string(), BotIdentity(Humana));
    EXPECT_EQ(records[8].at("context").as_string(), "delivered");
    EXPECT_EQ(records[0].at("kind").as_string(), "alles_perception");
    EXPECT_EQ(records[0].at("detail").as_string(), "\"Hogger is near the mill\" from Humanb at Northshire");
    EXPECT_EQ(records[0].at("context").as_string(), "speech");
    EXPECT_EQ(records[0].at("value").to_number<uint64_t>(), 1u);
    EXPECT_EQ(records[1].at("detail").as_string(), "unintelligible speech from Humanb at Northshire");
    EXPECT_EQ(records[1].at("value").to_number<uint64_t>(), 0u);
    EXPECT_EQ(records[2].at("detail").as_string(), "Humanb died, killed by Hogger");
    EXPECT_EQ(records[2].at("context").as_string(), "witnessed_death");
    EXPECT_EQ(records[3].at("kind").as_string(), "alles_said");
    EXPECT_EQ(records[3].at("context").as_string(), "delivered");
    EXPECT_EQ(records[4].at("context").as_string(), "no listener");
    EXPECT_EQ(records[5].at("kind").as_string(), "alles_worker");
    EXPECT_EQ(records[5].at("detail").as_string(), "connected");
    EXPECT_EQ(records[6].at("kind").as_string(), "alles_request");
    EXPECT_EQ(records[6].at("value").to_number<uint64_t>(), 4u);
    EXPECT_EQ(records[7].at("detail").as_string(), "disconnected");
    EXPECT_EQ(records[7].at("bot").as_string(), "");
    std::ifstream snapshots(directory / "snapshots.ndjson");
    std::string line;
    ASSERT_TRUE(std::getline(snapshots, line));
    EXPECT_EQ(line, "{\"seq\":1}");
    EXPECT_EQ(recorder.Status().at("snapshots").as_object().at("records").to_number<uint64_t>(), 1u);
    std::filesystem::remove_all(directory);
}

TEST(AllesTelemetryRecorder, TruncationNeverSplitsAMultiByteCharacter)
{
    EXPECT_EQ(TruncateUtf8("h\xC3\xA9llo", 2), "h");
    EXPECT_EQ(TruncateUtf8("h\xC3\xA9llo", 3), "h\xC3\xA9");
    EXPECT_EQ(TruncateUtf8("hello", 10), "hello");
    EXPECT_EQ(BotIdentity({ActorKind::CreatureSpawn, 7}), "");
    EXPECT_FALSE(BotIdentity(Humana).empty());
}
}

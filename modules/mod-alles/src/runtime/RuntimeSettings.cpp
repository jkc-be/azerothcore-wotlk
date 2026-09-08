/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "RuntimeSettings.h"
#include "Config.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <fstream>
#include <filesystem>

namespace Alles
{
std::optional<std::set<ActorKey>> ParseOwners(std::string_view text, std::size_t cap)
{
    if (!cap || text.empty() || text.size() > 16384)
        return std::nullopt;
    std::set<ActorKey> owners;
    while (!text.empty())
    {
        auto const comma = text.find(',');
        auto entry = text.substr(0, comma);
        while (!entry.empty() && entry.front() == ' ')
            entry.remove_prefix(1);
        while (!entry.empty() && entry.back() == ' ')
            entry.remove_suffix(1);
        ActorKey owner;
        if (entry.starts_with("player:"))
        {
            owner.kind = ActorKind::Player;
            entry.remove_prefix(7);
        }
        else if (entry.starts_with("creature:"))
        {
            owner.kind = ActorKind::CreatureSpawn;
            entry.remove_prefix(9);
        }
        else
            return std::nullopt;
        if (entry.empty())
            return std::nullopt;
        auto const [end, error] = std::from_chars(entry.data(), entry.data() + entry.size(), owner.id);
        if (error != std::errc{} || end != entry.data() + entry.size() || !IsValidActor(owner) ||
            owners.size() >= cap || !owners.insert(owner).second)
            return std::nullopt;
        if (comma == std::string_view::npos)
            break;
        text.remove_prefix(comma + 1);
        if (text.empty())
            return std::nullopt;
    }
    return owners;
}

RuntimeSettings ReadRuntimeSettings()
{
    RuntimeSettings settings;
    auto const owners = ParseOwners(sConfigMgr->GetOption<std::string>("Alles.Owners", ""), 64);
    if (!owners)
        throw std::invalid_argument("Alles.Owners requires 1-64 unique typed entries, for example player:42,player:43");
    settings.owners = *owners;
    settings.limits.owners = settings.owners.size();
    for (auto const owner : settings.owners)
        if (owner.kind != ActorKind::Player || owner.id > std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("First-light runtime supports player owners; NPC registration follows in W7");
    auto const scheduling = sConfigMgr->GetOption<std::string>("Alles.SchedulingProfile", "pilot");
    bool const simulation = sConfigMgr->GetOption<bool>("Observatory.Enable", false);
    if ((simulation && scheduling != "simulation") || (!simulation && scheduling != "pilot"))
        throw std::invalid_argument("Use the simulation scheduling profile only with isolated Observatory mode");
    auto const mode = sConfigMgr->GetOption<std::string>("Alles.Worker.Mode", "inprocess-fake");
    if (mode != "inprocess-fake" && mode != "bridge")
        throw std::invalid_argument("Alles.Worker.Mode must be inprocess-fake or bridge");
    settings.external = mode == "bridge";
    settings.objectives = sConfigMgr->GetOption<bool>("Alles.Objectives.Enable", false);
    if (settings.external)
    {
        auto port = sConfigMgr->GetOption<uint32_t>("Alles.Worker.Port", 8779);
        settings.bridge.profile = sConfigMgr->GetOption<std::string>("Alles.Worker.Profile", "");
        settings.bridge.model = sConfigMgr->GetOption<std::string>("Alles.Worker.Model", "");
        settings.bridge.ledger = sConfigMgr->GetOption<std::string>("Alles.Interpreter.TrialLedger", "");
        settings.bridge.maxRequests = sConfigMgr->GetOption<uint32_t>("Alles.Interpreter.TrialMaxRequests", 100);
        auto budgetMode = sConfigMgr->GetOption<std::string>("Alles.Interpreter.BudgetMode", "limited");
        if (budgetMode != "trial" && budgetMode != "rolling" && budgetMode != "limited" && budgetMode != "unlimited")
            throw std::invalid_argument(
                "Alles.Interpreter.BudgetMode must be limited, unlimited or trial (rolling is a legacy alias)");
        settings.bridge.budgetMode = budgetMode;
        if (budgetMode == "rolling" || budgetMode == "limited")
        {
            settings.bridge.maxRequests = 0;
            settings.bridge.requestsPerMinute =
                sConfigMgr->GetOption<uint32_t>("Alles.Interpreter.RequestsPerMinute", 30);
            if (!settings.bridge.requestsPerMinute || settings.bridge.requestsPerMinute > 100000)
                throw std::invalid_argument("Alles.Interpreter.RequestsPerMinute must be 1-100000");
        }
        settings.conversation = sConfigMgr->GetOption<bool>("Alles.Conversation.Enable", false);
        auto tokenFile = sConfigMgr->GetOption<std::string>("Alles.Worker.TokenFile", "");
        std::ifstream input(tokenFile);
        std::getline(input, settings.bridge.token);
        if (!port || port > 65535 || settings.bridge.profile.size() != 64 || settings.bridge.model.empty() ||
            settings.bridge.model.size() > 128 || settings.bridge.token.size() < 32 ||
            settings.bridge.token.size() > 128 || settings.bridge.ledger.empty() ||
            (budgetMode == "trial" && !settings.bridge.maxRequests))
            throw std::invalid_argument("Invalid Alles bridge port, profile, token file, model or trial ledger");
        settings.bridge.port = uint16_t(port);
        if (budgetMode == "unlimited")
            settings.bridge.maxRequests = 0;
        settings.bridge.policyFile = sConfigMgr->GetOption<std::string>(
            "Alles.Interpreter.PolicyFile", "");
        if (settings.bridge.policyFile.empty())
            settings.bridge.policyFile = settings.bridge.ledger + ".policy.json";
        auto controlFile = sConfigMgr->GetOption<std::string>("Alles.Interpreter.ControlTokenFile", "");
        if (!controlFile.empty())
        {
            std::ifstream control(controlFile);
            std::getline(control, settings.bridge.controlToken);
            if (settings.bridge.controlToken.size() < 32 || settings.bridge.controlToken.size() > 128 ||
                settings.bridge.controlToken == settings.bridge.token)
                throw std::invalid_argument("Interpreter controls need a separate 32-128 byte token");
        }
        auto& policy = settings.bridge.policy;
        policy.concurrentJobs = sConfigMgr->GetOption<uint32_t>("Alles.Interpreter.ConcurrentJobs", 1);
        policy.waitingPerActor = sConfigMgr->GetOption<uint32_t>("Alles.Interpreter.WaitingPerActor", 2);
        policy.waitingGlobal = sConfigMgr->GetOption<uint32_t>("Alles.Interpreter.WaitingGlobal", 10);
        policy.bytesPerActor = sConfigMgr->GetOption<uint32_t>("Alles.Interpreter.BytesPerActor", 49152);
        policy.bytesGlobal = sConfigMgr->GetOption<uint32_t>("Alles.Interpreter.BytesGlobal", 245760);
        settings.bridge.concurrentCalls = sConfigMgr->GetOption<uint32_t>("Alles.Interpreter.ConcurrentCalls", 1);
        if (!Interpreter::ValidPolicy(policy) || !settings.bridge.concurrentCalls ||
            settings.bridge.concurrentCalls > 32)
            throw std::invalid_argument("Invalid interpreter queue or concurrency bounds");

    }
    settings.telemetryDirectory = sConfigMgr->GetOption<std::string>("Alles.Telemetry.Directory", "");
    if (simulation && !settings.telemetryDirectory.empty())
        throw std::invalid_argument("Simulation uses Observatory.Directory; leave Alles.Telemetry.Directory empty");
    if (!settings.telemetryDirectory.empty() && !std::filesystem::is_directory(settings.telemetryDirectory))
        throw std::invalid_argument("Alles telemetry directory must already exist and be private");
    settings.telemetrySegmentBytes =
        sConfigMgr->GetOption<uint32_t>("Alles.Telemetry.JournalSegmentBytes", 64u * 1024 * 1024);
    if (settings.telemetrySegmentBytes && settings.telemetrySegmentBytes < 1024 * 1024)
        throw std::invalid_argument("Alles.Telemetry.JournalSegmentBytes must be 0 or at least 1 MiB");

    settings.limits.memories = sConfigMgr->GetOption<uint32_t>("Alles.Memory.MaxMemories", 256);
    settings.limits.perceptions = sConfigMgr->GetOption<uint32_t>("Alles.Memory.MaxPerceptions", 128);
    auto const hearsay = sConfigMgr->GetOption<std::string>("Alles.Memory.HearsayCap", "0.6");
    auto const [hearsayEnd, hearsayError] =
        std::from_chars(hearsay.data(), hearsay.data() + hearsay.size(), settings.memory.hearsayCap);
    if (hearsayError != std::errc{} || hearsayEnd != hearsay.data() + hearsay.size())
        throw std::invalid_argument("Alles.Memory.HearsayCap must be a finite decimal probability");
    settings.memory.salienceHalfLifeMs =
        uint64_t(sConfigMgr->GetOption<uint32_t>("Alles.Memory.HalfLifeSeconds", 86400)) * 1000;
    settings.witnessRange = sConfigMgr->GetOption<float>("Alles.WitnessRange", 40.0f);
    settings.speech = sConfigMgr->GetOption<bool>("Alles.Speech.Enable", true);
    settings.withholdFake = sConfigMgr->GetOption<bool>("Alles.Worker.FakeWithhold", false);
    settings.shutdownBudgetMs = uint64_t(sConfigMgr->GetOption<uint32_t>("Alles.Shutdown.DrainSeconds", 10)) * 1000;
    settings.speechCooldownMs = uint64_t(sConfigMgr->GetOption<uint32_t>("Alles.Speech.CooldownSeconds", 30)) * 1000;
    settings.speechRepeatMs = uint64_t(sConfigMgr->GetOption<uint32_t>("Alles.Speech.RepeatSeconds", 300)) * 1000;
    if (!IsValidPolicy(settings.memory) || !std::isfinite(settings.witnessRange) || settings.witnessRange <= 0 ||
        settings.witnessRange > 200 || !settings.limits.memories || settings.limits.memories > 256 ||
        !settings.limits.perceptions || settings.limits.perceptions > 128 || !settings.shutdownBudgetMs ||
        settings.shutdownBudgetMs > 60000 || settings.speechCooldownMs < 5000 ||
        settings.speechRepeatMs < settings.speechCooldownMs)
        throw std::invalid_argument("Alles memory, witness, speech, or shutdown limits are outside pilot bounds");
    return settings;
}
} // namespace Alles

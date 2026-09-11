/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "CommandsParse.h"
#include "Runtime.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "WorldSession.h"
#include <sstream>

namespace
{
using namespace Alles;
using namespace Acore::ChatCommands;

bool Error(ChatHandler* handler, std::string_view message)
{
    handler->SendSysMessage(message);
    handler->SetSentErrorMessage(true);
    return false;
}

bool IsDiagnosticCaller(ChatHandler* handler)
{
    if (auto const* session = handler->GetSession(); session && session->GetSecurity() < SEC_GAMEMASTER)
        return Error(handler, "Alles diagnostics require GM security.");
    return true;
}

Runtime* CommandRuntime(ChatHandler* handler)
{
    auto* runtime = ActiveRuntime();
    if (!runtime)
    {
        Error(handler, "Alles is disabled or unavailable.");
        return nullptr;
    }
    if (!runtime->IsMainThread())
    {
        Error(handler, "Alles commands are unavailable outside the world thread.");
        return nullptr;
    }
    return runtime;
}

char const* KindName(ActorKind kind)
{
    return kind == ActorKind::Player ? "player" : "creature";
}

char const* StateName(ActorState state)
{
    switch (state)
    {
        case ActorState::Loading: return "loading";
        case ActorState::Ready: return "ready";
        case ActorState::Closing: return "closing";
    }
    return "unknown";
}

class AllesCommandScript final : public CommandScript
{
public:
    AllesCommandScript() : CommandScript("alles_commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable const allesCommands =
        {
            {"recall", Recall, SEC_PLAYER, Console::No},
            {"status", Status, SEC_GAMEMASTER, Console::Yes},
            {"flush", Flush, SEC_GAMEMASTER, Console::Yes},
            {"motives", Motives, SEC_GAMEMASTER, Console::Yes},
            {"motive", Motive, SEC_GAMEMASTER, Console::Yes},
            {"ambition", Ambition, SEC_GAMEMASTER, Console::Yes},
            {"effect", Effect, SEC_GAMEMASTER, Console::Yes}
        };
        return {{"alles", allesCommands}};
    }

private:
    static bool Motives(ChatHandler* handler, char const* args)
    {
        auto const parsed = Commands::ParseArguments(args);
        auto const owner = parsed ? Commands::Owner(*parsed) : std::nullopt;
        if (!IsDiagnosticCaller(handler) || !owner)
            return Error(handler, "Usage: .alles motives player id");
        auto* runtime = CommandRuntime(handler);
        if (!runtime)
            return false;
        handler->SendSysMessage(runtime->SatisfactionStatus(*owner));
        return true;
    }

    static bool Motive(ChatHandler* handler, char const* args)
    {
        if (!IsDiagnosticCaller(handler) || !args || std::string_view(args).size() > 256)
            return false;
        std::istringstream input(args);
        std::string kind, rawId, motive, extra;
        double weight = 0, depletion = 0, satiation = 0;
        if (!(input >> kind >> rawId >> motive >> weight >> depletion >> satiation) || kind != "player")
            return Error(handler,
                "Usage: .alles motive player id motive weight depletion_per_hour satiation [urgency]");
        std::optional<double> urgency;
        input >> std::ws;
        if (!input.eof())
        {
            double value = 0;
            if (!(input >> value) || (input >> extra))
                return Error(handler, "Urgency must be a finite number from 0 to 10.");
            urgency = value;
        }
        auto const id = Commands::ParsePositiveId(rawId);
        auto* runtime = CommandRuntime(handler);
        if (!id || !runtime
            || !runtime->SetMotive({ActorKind::Player, *id}, motive, weight, depletion, satiation, {}, urgency))
            return Error(handler, "Motive rejected: use a ready brain owner, a named motive, weight/depletion 0-10 "
                "and satiation 0-1. At least one motive must have positive weight.");
        handler->SendSysMessage("Motive updated. Current fulfillment was preserved. "
            "Use .alles flush for a save receipt.");
        return true;
    }

    static bool Ambition(ChatHandler* handler, char const* args)
    {
        if (!IsDiagnosticCaller(handler) || !args || std::string_view(args).size() > 256)
            return false;
        std::istringstream input(args);
        std::string kind, rawId, motive, extra;
        double weight = 0, scale = 0;
        if (!(input >> kind >> rawId >> motive >> weight >> scale) || (input >> extra) || kind != "player")
            return Error(handler, "Usage: .alles ambition player id motive weight scale");
        auto const id = Commands::ParsePositiveId(rawId);
        auto* runtime = CommandRuntime(handler);
        if (!id || !runtime || !runtime->SetMotive({ActorKind::Player, *id}, motive, weight, 0, 0, scale))
            return Error(handler, "Ambition rejected: use a ready brain owner, weight 0-10 and positive scale.");
        handler->SendSysMessage("Continuing ambition updated. Observed value preserved. Use .alles flush to save.");
        return true;
    }

    static bool Effect(ChatHandler* handler, char const* args)
    {
        if (!IsDiagnosticCaller(handler) || !args || std::string_view(args).size() > 256)
            return false;
        std::istringstream input(args);
        std::string kind, rawId, activity, motive, extra;
        double effect = 0;
        if (!(input >> kind >> rawId >> activity >> motive >> effect) || (input >> extra) || kind != "player")
            return Error(handler, "Usage: .alles effect player id activity motive effect");
        auto const id = Commands::ParsePositiveId(rawId);
        auto* runtime = CommandRuntime(handler);
        if (!id || !runtime || !runtime->SetEffect({ActorKind::Player, *id}, activity, motive, effect))
            return Error(handler, "Effect rejected: use a ready brain owner, an installed activity, an existing "
                "motive and a finite effect in its units (needs -1 to 1, ambitions up to 1e12).");
        handler->SendSysMessage("Activity effect updated. Use .alles flush for a save receipt.");
        return true;
    }

    static bool Recall(ChatHandler* handler, char const* args)
    {
        auto const parsed = Commands::ParseArguments(args);
        auto const count = parsed ? Commands::RecallCount(*parsed) : std::nullopt;
        if (!count)
            return Error(handler, "Usage: .alles recall [1-20]");

        auto const* session = handler->GetSession();
        auto const* player = session ? session->GetPlayer() : nullptr;
        if (!player)
            return Error(handler, "Recall requires your logged-in character.");
        auto* runtime = CommandRuntime(handler);
        if (!runtime)
            return false;
        ActorKey const owner{ActorKind::Player, player->GetGUID().GetCounter()};
        if (!runtime->Contains(owner))
            return Error(handler, "Your character is not configured for alles memory.");
        auto const status = runtime->Status(owner);
        if (!status || status->state != ActorState::Ready)
            return Error(handler, "Your memories are not ready yet.");

        auto const memories = runtime->Recall(*player, *count);
        if (memories.empty())
            handler->SendSysMessage("No recall is available for your character at the moment.");
        for (auto const& memory : memories)
            handler->SendSysMessage(memory, true);
        return true;
    }

    static bool Status(ChatHandler* handler, char const* args)
    {
        if (!IsDiagnosticCaller(handler))
            return false;
        auto const parsed = Commands::ParseArguments(args);
        if (!parsed)
            return Error(handler, "Usage: .alles status [player|creature id]");
        auto const owner = Commands::Owner(*parsed);
        if (parsed->count != 0 && !owner)
            return Error(handler, "Usage: .alles status [player|creature id]");
        auto* runtime = CommandRuntime(handler);
        if (!runtime)
            return false;
        if (!owner)
        {
            handler->PSendSysMessage("Alles active: dropped={} unsafe_packets={}",
                runtime->Dropped(), runtime->UnsafePackets());
            return true;
        }
        if (!runtime->Contains(*owner))
            return Error(handler, "That typed owner is not configured for alles memory.");

        auto const status = runtime->Status(*owner);
        if (!status)
        {
            handler->PSendSysMessage("Alles {} {}: state=unloaded committed_revision=unavailable",
                KindName(owner->kind), owner->id);
            return true;
        }
        if (status->state == ActorState::Loading)
        {
            handler->PSendSysMessage(
                "Alles {} {}: state=loading generation={} attachment={} committed_revision=pending "
                "saving={} save_failed={} dropped_perceptions={}",
                KindName(owner->kind), owner->id, status->generation, status->attachment,
                status->saving, status->saveFailed, status->droppedPerceptions);
            return true;
        }
        handler->PSendSysMessage(
            "Alles {} {}: state={} generation={} attachment={} revision={} committed_revision={} "
            "saving={} save_failed={} dropped_perceptions={}",
            KindName(owner->kind), owner->id, StateName(status->state), status->generation, status->attachment,
            status->revision, status->committedRevision, status->saving, status->saveFailed,
            status->droppedPerceptions);
        return true;
    }

    static bool Flush(ChatHandler* handler, char const* args)
    {
        if (!IsDiagnosticCaller(handler))
            return false;
        auto const parsed = Commands::ParseArguments(args);
        auto const owner = parsed ? Commands::Owner(*parsed) : std::nullopt;
        if (!owner)
            return Error(handler, "Usage: .alles flush player|creature id");
        auto* runtime = CommandRuntime(handler);
        if (!runtime)
            return false;
        if (!runtime->Contains(*owner))
            return Error(handler, "That typed owner is not configured for alles memory.");
        auto const before = runtime->Status(*owner);
        if (!before || before->state == ActorState::Loading)
            return Error(handler, "Flush unavailable: the owner's snapshot is not loaded yet.");

        auto const revision = runtime->Flush(*owner);
        if (!revision || *revision == 0)
            return Error(handler, "Flush unavailable; no commit has been acknowledged.");
        auto const status = runtime->Status(*owner);
        if (!status || status->state == ActorState::Loading)
            return Error(handler, "Flush state unavailable; no commit has been acknowledged.");
        bool const committed = status->committedRevision >= *revision;
        handler->PSendSysMessage(
            "Alles {} {}: requested_revision={} committed_revision={} result={} save_failed={}",
            KindName(owner->kind), owner->id, *revision, status->committedRevision,
            committed ? "committed" : "pending", status->saveFailed);
        if (!committed)
            handler->SendSysMessage("Wait for .alles status to report committed_revision >= requested_revision.");
        return true;
    }
};
}

void AddAllesCommandScripts()
{
    new AllesCommandScript();
}

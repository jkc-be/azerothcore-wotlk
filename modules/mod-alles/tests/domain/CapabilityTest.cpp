/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Capability.h"
#include "gtest/gtest.h"

namespace Alles
{
TEST(AllesCapability, RequiresSuppliedReferencesAndCurrentOwnerObjectiveRevision)
{
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({"pursue_quest", true, false, false, "Accepted quest",
        "Observed quest credit and reward", "Release execution ownership"}));
    CapabilityContext context{{ActorKind::Player, 42}, 3, 8, 2, true, {100}, {}, {}};
    CapabilityRequest request{1, context.owner, 3, 8, 2, "pursue_quest", 100, 0, {}};
    EXPECT_TRUE(registry.Validate(request, context).empty());
    request.quest = 101;
    EXPECT_EQ(registry.Validate(request, context), "reference_not_supplied");
    request.quest = 100;
    request.place = 1;
    EXPECT_EQ(registry.Validate(request, context), "invalid_parameters");
    request.place = 0;
    request.revision = 1;
    EXPECT_EQ(registry.Validate(request, context), "stale_objective_or_actor");
    request.revision = 2;
    request.actorGeneration = 2;
    EXPECT_EQ(registry.Validate(request, context), "stale_objective_or_actor");
    request.actorGeneration = 3;
    request.owner.kind = ActorKind::CreatureSpawn;
    EXPECT_EQ(registry.Validate(request, context), "stale_objective_or_actor");
    request.owner = context.owner;
    context.autonomous = false;
    EXPECT_EQ(registry.Validate(request, context), "control_handoff");
    context.autonomous = true;
    request.capability = "teleport";
    EXPECT_EQ(registry.Validate(request, context), "unavailable_capability");
    request.contractVersion = 2;
    EXPECT_EQ(registry.Validate(request, context), "unsupported_planning_contract");
}

TEST(AllesCapability, RegistrationCannotSilentlyReplaceAnAdapter)
{
    CapabilityRegistry registry;
    CapabilitySpec spec{"pursue_quest", true, false, false, "Accepted quest",
        "Observed reward", "Release execution ownership"};
    EXPECT_TRUE(registry.Register(spec));
    EXPECT_FALSE(registry.Register(spec));
    spec.name = "raw command";
    EXPECT_FALSE(registry.Register(spec));
    spec.name = "travel";
    spec.cancellation.clear();
    EXPECT_FALSE(registry.Register(spec));
}
}

/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Memory.h"
#include "gtest/gtest.h"
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
using namespace Alles;

Perception Heard(std::string text)
{
    Perception perception;
    perception.source = {ActorKey{ActorKind::Player, 17}, "Humanc"};
    perception.text = std::move(text);
    return perception;
}

std::string Repeat(std::string const& character, std::size_t count)
{
    std::string text;
    for (std::size_t index = 0; index < count; ++index)
        text += character;
    return text;
}

TEST(AllesMemoryTest, StrictUnicodeCountsScalarsInsteadOfBytes)
{
    EXPECT_TRUE(IsBoundedText("", 0));
    EXPECT_TRUE(IsBoundedText("A\xC3\xA9\xE7\x8B\xBC\xF0\x9F\x90\xBA", 4));
    EXPECT_FALSE(IsBoundedText("A\xC3\xA9\xE7\x8B\xBC\xF0\x9F\x90\xBA", 3));

    std::string const invalid[] =
    {
        std::string("a\0b", 3),
        "\x80",                 // Stray continuation.
        "\xC0\xAF",            // Overlong ASCII.
        "\xE0\x80\xAF",        // Overlong three-byte sequence.
        "\xF0\x80\x80\xAF",    // Overlong four-byte sequence.
        "\xED\xA0\x80",        // UTF-16 surrogate.
        "\xF4\x90\x80\x80",    // Above U+10FFFF.
        "\xF5\x80\x80\x80",    // Invalid leading byte.
        "\xE7\x8B",            // Truncated scalar.
        "\xC3x"                // Invalid continuation.
    };
    for (auto const& text : invalid)
        EXPECT_FALSE(IsBoundedText(text, 100));
}

TEST(AllesMemoryTest, PerceptionsAndStoredMemoriesAcceptExactUnicodeBounds)
{
    auto perception = Heard(Repeat("\xE7\x8B\xBC", 512));
    perception.source.name = Repeat("\xC3\xA9", 100);
    perception.subject.name = Repeat("\xF0\x9F\x90\xBA", 100);
    EXPECT_TRUE(GatePerception(perception));
    auto memory = FormFallback(perception, {}, 0);
    EXPECT_TRUE(IsValidMemory(memory));

    perception.text += "x";
    EXPECT_FALSE(GatePerception(perception));
    EXPECT_THROW(FormFallback(perception, {}, 0), std::invalid_argument);
    memory.claim += "x";
    EXPECT_FALSE(IsValidMemory(memory));

    perception.text = "A wolf passed.";
    perception.source.name += "x";
    EXPECT_FALSE(GatePerception(perception));
    perception.source.name = "Humanc";
    perception.subject.name += "x";
    EXPECT_FALSE(GatePerception(perception));
}

TEST(AllesMemoryTest, LanguageGateDestroysPlaintextBeforeRetention)
{
    auto perception = Heard("Humanb hid the treasure beneath the abbey.");
    perception.comprehended = false;
    perception.language = 1;
    EXPECT_TRUE(GatePerception(perception));
    EXPECT_TRUE(perception.text.empty());
    EXPECT_EQ(perception.language, 1u);
    EXPECT_EQ(perception.source.name, "Humanc");

    auto memory = FormFallback(perception, {}, 100);
    EXPECT_EQ(memory.kind, MemoryKind::UnintelligibleSpeech);
    EXPECT_EQ(memory.claim.find("treasure"), std::string::npos);
    EXPECT_EQ(RenderMemory(memory).find("treasure"), std::string::npos);
    EXPECT_FALSE(memory.reportedDepth.has_value());
}

TEST(AllesMemoryTest, FallbackRepeatsTheLanguageGateForUntrustedInput)
{
    auto perception = Heard("I saw Humanb hide the treasure.");
    perception.comprehended = false;
    auto memory = FormFallback(perception, {}, 100);
    EXPECT_EQ(memory.kind, MemoryKind::UnintelligibleSpeech);
    EXPECT_EQ(memory.claim, "I heard speech I could not understand.");
    EXPECT_TRUE(memory.attribution.empty());
    EXPECT_FALSE(memory.reportedDepth.has_value());
    EXPECT_EQ(RenderMemory(memory).find("treasure"), std::string::npos);

    // Comprehension acquired later cannot recover the erased observation.
    ASSERT_TRUE(GatePerception(perception));
    perception.comprehended = true;
    auto later = FormFallback(perception, {}, 200);
    EXPECT_EQ(later.claim.find("treasure"), std::string::npos);
}

TEST(AllesMemoryTest, AudibleFirsthandClaimRemainsListenerHearsay)
{
    auto perception = Heard("I saw Humanb die");
    auto memory = FormFallback(perception, {}, 100);
    EXPECT_EQ(memory.kind, MemoryKind::HeardStatement);
    EXPECT_EQ(memory.source, perception.source);
    EXPECT_EQ(memory.attribution, "Humanc");
    EXPECT_EQ(memory.claim, "Humanb die");
    ASSERT_TRUE(memory.reportedDepth.has_value());
    EXPECT_EQ(*memory.reportedDepth, 1u);
    EXPECT_LE(memory.confidence, MemoryPolicy{}.hearsayCap);
    EXPECT_EQ(RenderMemory(memory), "Humanc told me Humanb die");
}

TEST(AllesMemoryTest, NamedAttributionIsDistinctFromImmediateSource)
{
    auto perception = Heard("Humand told me Humanb died");
    auto memory = FormFallback(perception, {}, 100);
    EXPECT_EQ(memory.source, perception.source);
    EXPECT_EQ(memory.attribution, "Humand");
    EXPECT_EQ(memory.claim, "Humanb died");
    ASSERT_TRUE(memory.reportedDepth.has_value());
    EXPECT_EQ(*memory.reportedDepth, 2u);
    EXPECT_EQ(RenderMemory(memory), "Humanc told me Humand reported that Humanb died");
}

TEST(AllesMemoryTest, MissingOrUnrecognizedAttributionLeavesDepthUnknown)
{
    auto heard = FormFallback(Heard("I heard Humanb died"), {}, 100);
    EXPECT_EQ(heard.claim, "Humanb died");
    EXPECT_TRUE(heard.attribution.empty());
    EXPECT_FALSE(heard.reportedDepth.has_value());

    std::string const opaque[] =
    {
        "Humanb died",
        "Humand, perhaps, told me Humanb died.",
        "They say Humanb died",
        "I saw ",
        "I heard ",
        "Humand told me "
    };
    for (auto const& text : opaque)
    {
        auto memory = FormFallback(Heard(text), {}, 100);
        EXPECT_EQ(memory.kind, MemoryKind::HeardStatement);
        EXPECT_EQ(memory.claim, text);
        EXPECT_TRUE(memory.attribution.empty());
        EXPECT_FALSE(memory.reportedDepth.has_value());
    }
}

TEST(AllesMemoryTest, HeardConfidenceObeysLocalCapForEveryAudibleForm)
{
    MemoryPolicy policy;
    policy.hearsayCap = 0.25;
    std::string const statements[] = {"I saw Humanb die", "Humand told me Humanb died", "I heard Humanb died"};
    for (auto const& text : statements)
    {
        auto perception = Heard(text);
        auto player = FormFallback(perception, policy, 100);
        perception.source.actor = ActorKey{ActorKind::CreatureSpawn, 17};
        auto creature = FormFallback(perception, policy, 100);
        EXPECT_LE(player.confidence, policy.hearsayCap);
        EXPECT_DOUBLE_EQ(player.confidence, creature.confidence);
        EXPECT_EQ(player.claim, creature.claim);
        EXPECT_EQ(player.reportedDepth, creature.reportedDepth);
    }
}

TEST(AllesMemoryTest, HiddenKillerDoesNotAppearInWitnessMemoryOrRetelling)
{
    Perception death;
    death.kind = PerceptionKind::WitnessedDeath;
    death.subject = {ActorKey{ActorKind::Player, 18}, "Humanb"};
    auto memory = FormFallback(death, {}, 100);
    EXPECT_EQ(memory.kind, MemoryKind::WitnessedDeath);
    EXPECT_EQ(memory.claim, "Humanb died");
    EXPECT_EQ(memory.source, Reference{});
    EXPECT_EQ(RenderMemory(memory), "I saw that Humanb died");

    death.source = {ActorKey{ActorKind::CreatureSpawn, 18}, "Young Wolf"};
    auto visible = FormFallback(death, {}, 100);
    EXPECT_EQ(visible.claim, "Humanb died after being attacked by Young Wolf");
    EXPECT_EQ(visible.source, death.source);
    EXPECT_NE(visible.source.actor, visible.subject.actor);
}

TEST(AllesMemoryTest, IncrementalAndOfflineDecayAgreeAndSameTimestampIsIdempotent)
{
    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 1000;
    auto incremental = FormFallback(Heard("Humanb died"), policy, 100);
    auto offline = incremental;
    ASSERT_TRUE(DecayMemory(incremental, policy, 1100));
    EXPECT_NEAR(incremental.salience, 0.35, 1e-12);
    ASSERT_TRUE(DecayMemory(incremental, policy, 2100));
    ASSERT_TRUE(DecayMemory(offline, policy, 2100));
    EXPECT_NEAR(incremental.salience, 0.175, 1e-12);
    EXPECT_DOUBLE_EQ(incremental.salience, offline.salience);
    EXPECT_EQ(incremental.claim, offline.claim);
    EXPECT_EQ(incremental.contentRevision, offline.contentRevision);

    auto const salience = incremental.salience;
    auto const revision = incremental.contentRevision;
    EXPECT_FALSE(DecayMemory(incremental, policy, 2100));
    EXPECT_FALSE(DecayMemory(incremental, policy, 2000));
    EXPECT_DOUBLE_EQ(incremental.salience, salience);
    EXPECT_EQ(incremental.contentRevision, revision);
    EXPECT_EQ(incremental.decayGameTimeMs, 2100u);
}

TEST(AllesMemoryTest, ProvenanceErodesFromStoredTextReferencesAndSpeech)
{
    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 1000;
    auto perception = Heard("Humand told me Humanb died");
    perception.subject = {ActorKey{ActorKind::Player, 18}, "Humanb"};
    auto memory = FormFallback(perception, policy, 100);
    ASSERT_TRUE(DecayMemory(memory, policy, 2100));
    EXPECT_EQ(memory.subject, Reference{});
    EXPECT_EQ(memory.source, Reference{});
    EXPECT_TRUE(memory.attribution.empty());
    EXPECT_FALSE(memory.reportedDepth.has_value());
    EXPECT_EQ(memory.contentRevision, 2u);
    for (auto const* name : {"Humanb", "Humanc", "Humand"})
    {
        EXPECT_EQ(memory.claim.find(name), std::string::npos);
        EXPECT_EQ(RenderMemory(memory).find(name), std::string::npos);
    }

    ASSERT_TRUE(DecayMemory(memory, policy, 3100));
    EXPECT_EQ(memory.contentRevision, 2u);
}

TEST(AllesMemoryTest, RehearsalCannotRestoreProvenanceOrIncreaseConfidence)
{
    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 1000;
    auto memory = FormFallback(Heard("Humand told me Humanb died"), policy, 100);
    auto const confidence = memory.confidence;
    RehearseMemory(memory, policy, 2100, 0.5);
    EXPECT_NEAR(memory.salience, 0.675, 1e-12);
    EXPECT_DOUBLE_EQ(memory.confidence, confidence);
    EXPECT_EQ(memory.source, Reference{});
    EXPECT_TRUE(memory.attribution.empty());
    EXPECT_FALSE(memory.reportedDepth.has_value());
    EXPECT_EQ(RenderMemory(memory).find("Humand"), std::string::npos);

    auto const claim = memory.claim;
    RehearseMemory(memory, policy, 2100, 1);
    EXPECT_DOUBLE_EQ(memory.salience, 1);
    EXPECT_DOUBLE_EQ(memory.confidence, confidence);
    EXPECT_EQ(memory.claim, claim);
    EXPECT_EQ(memory.recalledGameTimeMs, 2100u);
    RehearseMemory(memory, policy, 2000, 0);
    EXPECT_EQ(memory.recalledGameTimeMs, 2100u);
}

TEST(AllesMemoryTest, AlreadyErodedLoadedClaimStillClearsResidualSubject)
{
    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 1000;
    auto memory = FormFallback(Heard("Humanb died"), policy, 100);
    memory.claim = "something, but I no longer remember what or from whom";
    memory.source = {};
    memory.subject = {ActorKey{ActorKind::Player, 18}, "Humanb"};
    ASSERT_TRUE(IsValidMemory(memory));
    ASSERT_TRUE(DecayMemory(memory, policy, 2100));
    EXPECT_EQ(memory.subject, Reference{});
    EXPECT_EQ(memory.contentRevision, 2u);
}

TEST(AllesMemoryTest, InvalidPolicyAndNonFiniteMemoryValuesAreRejected)
{
    auto perception = Heard("Humanb died");
    auto memory = FormFallback(perception, {}, 100);
    memory.confidence = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(IsValidMemory(memory));
    memory.confidence = 0.5;
    memory.salience = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(IsValidMemory(memory));

    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 0;
    EXPECT_FALSE(IsValidPolicy(policy));
    EXPECT_THROW(FormFallback(perception, policy, 100), std::invalid_argument);
    EXPECT_THROW(DecayMemory(memory, policy, 200), std::invalid_argument);
    EXPECT_THROW(RehearseMemory(memory, {}, 200, -0.1), std::invalid_argument);
}
}

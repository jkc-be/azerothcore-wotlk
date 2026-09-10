/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "storage/PlanningCodec.h"
#include "storage/SnapshotStatements.h"
#include "bridge/Wire.h"
#include "gtest/gtest.h"
#include <limits>
#include <stdexcept>

namespace Alles::Storage
{
namespace
{
constexpr ActorKey Owner{ActorKind::Player, 42};

PlanningSnapshot Fixture()
{
    ObjectiveBook book;
    auto const* objective = book.ProposeQuest(100, "Earn the reward", "My accepted quest");
    QuestProgress progress;
    progress.inLog = true;
    progress.counters[0] = 2;
    book.Activate(objective->id, objective->revision, progress, 1000, 5);
    PrivateKnowledge knowledge;
    knowledge.Seed(1, false, true);
    knowledge.Visit(9, "Northshire Valley", 1000, true);
    knowledge.Hear({ActorKey{ActorKind::Player, uint64_t(1) << 40}, "Speaker"},
        {Activity::Work, 0, 87, {}}, "You may find work in Goldshire", 2000, 0.4);
    return {Owner, 1, book.Capture(), knowledge.Capture()};
}
}

TEST(AllesPlanningCodec, RoundTripRetainsSemanticIntentPrivateProvenanceAndSixtyFourBitIds)
{
    auto const snapshot = Fixture();
    auto const encoded = EncodePlanning(snapshot);
    auto const decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    EXPECT_EQ(encoded.find("lastSampleMs"), std::string::npos);
    EXPECT_EQ(encoded.find("token"), std::string::npos);
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(decoded->objectives));
    ASSERT_NE(restored.Current(), nullptr);
    EXPECT_EQ(restored.Current()->state, ObjectiveState::Waiting);
    EXPECT_EQ(restored.Current()->step, ObjectiveStep::Wait);
    EXPECT_EQ(restored.Current()->lastSampleMs, 0u);
    EXPECT_EQ(restored.Current()->attempts, 1u);
    EXPECT_EQ(restored.Current()->checkpoint.counters[0], 2u);
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Restore(decoded->knowledge));
    EXPECT_FALSE(knowledge.Seed(2, false, true));
    EXPECT_EQ(knowledge.Capture(), snapshot.knowledge);
}

TEST(AllesPlanningCodec, SatisfactionRoundTripLegacyDefaultsAndMalformedRejection)
{
    auto snapshot = Fixture();
    SatisfactionModel model;
    ASSERT_TRUE(model.SetDimension("craftsmanship", {2.5, 0.1, 0.4, 0.5}));
    ASSERT_TRUE(model.Observe(1000, 0, {{"companionship", 0.2}}));
    snapshot.satisfaction = model.Capture();
    auto const encoded = EncodePlanning(snapshot);
    auto decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    SatisfactionModel restored;
    ASSERT_TRUE(restored.Restore(decoded->satisfaction));
    EXPECT_FALSE(restored.Observe(1000, 0, {{"companionship", 0.2}}));
    auto older = Bridge::Parse(encoded).as_object();
    older["version"] = 10;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(older), Owner));
    older.erase("satisfaction");
    decoded = DecodePlanning(boost::json::serialize(older), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->satisfaction, DefaultSatisfaction());
    for (unsigned invalid = 0; invalid < 6; ++invalid)
    {
        auto value = Bridge::Parse(encoded).as_object();
        auto& state = value.at("satisfaction").as_object();
        auto& dimensions = state.at("dimensions").as_array();
        auto& dimension = dimensions.front().as_object();
        switch (invalid)
        {
            case 0: state["revision"] = 0; break;
            case 1: dimension["weight"] = -1; break;
            case 2: dimension["fulfillment"] = 1.1; break;
            case 3: dimension["id"] = "invalid id"; break;
            case 4: dimensions.push_back(dimensions.front()); break;
            case 5: state["rewardOverride"] = 1; break;
        }
        EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner));
    }
}

TEST(AllesPlanningCodec, PersonalContactsAndLearnedActivityOutcomesRemainOwnerBound)
{
    auto snapshot = Fixture();
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Restore(snapshot.knowledge));
    ActorKey const companion{ActorKind::Player, 43};
    ASSERT_TRUE(knowledge.RememberContact({{companion, "Companion"}, 9, {0, 1, -8949, -132, 84, 1000}}));
    EXPECT_FALSE(knowledge.RememberContact({{companion, "Companion"}, 9, {0, 1, 0, 0, 0, 999}}));
    snapshot.knowledge = knowledge.Capture();
    SatisfactionModel model;
    ASSERT_TRUE(model.Learn("visit_companion", false, 120000));
    ASSERT_TRUE(model.ActivityReceipt("rest", 1000));
    snapshot.satisfaction = model.Capture();
    auto const decoded = DecodePlanning(EncodePlanning(snapshot), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    snapshot.knowledge.contacts.emplace(Owner, KnownContact{{Owner, "Self"}, 9, {0, 1, 0, 0, 0, 1000}});
    EXPECT_FALSE(IsValidPlanningSnapshot(snapshot));
}

TEST(AllesPlanningCodec, RepairLocationsRoundTripWithoutInventingLegacyKnowledgeOrLiveAuthority)
{
    auto snapshot = Fixture();
    snapshot.knowledge.places.at(9).repair = RepairLocation{0, 1, -8901.25f, -125.5f, 83.125f, 2000};
    auto const encoded = EncodePlanning(snapshot);
    auto const decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    auto older = Bridge::Parse(encoded).as_object();
    older["version"] = 9;
    older.erase("satisfaction");
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(older), Owner));
    for (auto& place : older.at("places").as_array())
        place.as_object().erase("repair");
    auto const legacy = DecodePlanning(boost::json::serialize(older), Owner);
    ASSERT_TRUE(legacy);
    EXPECT_FALSE(legacy->knowledge.places.at(9).repair);
    for (unsigned invalid = 0; invalid < 5; ++invalid)
    {
        auto value = Bridge::Parse(encoded).as_object();
        auto& place = value.at("places").as_array()[0].as_object();
        auto& repair = place.at("repair").as_object();
        switch (invalid)
        {
            case 0: repair["vendorGuid"] = 999; break;
            case 1: repair["x"] = 40000; break;
            case 2: repair["phase"] = 0; break;
            case 3: repair["observedMs"] = 0; break;
            case 4: place["visitedMs"] = 0; break;
        }
        EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner));
    }
}

TEST(AllesPlanningCodec, ResourcePreparationRetainsDeadlineChargeAndRejectsPrivilegedOrImpossibleState)
{
    auto snapshot = Fixture();
    ObjectiveBook book;
    ASSERT_TRUE(book.Restore(snapshot.objectives));
    auto const id = book.Current()->id;
    ASSERT_TRUE(book.Block(id, Obstruction::Supplies, "Critically damaged equipped items", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    ASSERT_TRUE(book.ProposeRepair(id));
    ASSERT_TRUE(book.BeginRepair(id, book.Find(id)->revision, 3000));
    ASSERT_TRUE(book.PreparationTransaction(id, 3001));
    snapshot.objectives = book.Capture();
    auto const encoded = EncodePlanning(snapshot);
    auto decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(decoded->objectives));
    EXPECT_FALSE(restored.PreparationTransaction(id, 4000));
    EXPECT_EQ(restored.Preparing()->preparation->deadlineMs, 123000u);
    auto legacy = Bridge::Parse(encoded).as_object();
    legacy["version"] = 7;
    legacy.erase("satisfaction");
    auto& oldPreparation = legacy.at("objectives").as_array()[0].as_object().at("preparation").as_object();
    for (auto field : {"kind", "item", "count", "attemptsInCircumstances", "ownMoney", "fundsAtAttempt", "fundsKnown",
        "earnedMoney"})
        oldPreparation.erase(field);
    auto upgraded = DecodePlanning(boost::json::serialize(legacy), Owner);
    ASSERT_TRUE(upgraded);
    EXPECT_EQ(*upgraded, snapshot);
    for (unsigned invalid = 0; invalid < 4; ++invalid)
    {
        auto value = Bridge::Parse(encoded).as_object();
        auto& preparation = value.at("objectives").as_array()[0].as_object().at("preparation").as_object();
        switch (invalid)
        {
            case 0: preparation["vendorGuid"] = 999; break;
            case 1: preparation["deadlineMs"] = 999999; break;
            case 2: preparation["transactions"] = 4; break;
            case 3: preparation["state"] = 99; break;
        }
        EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner)) << invalid;
    }
    auto older = Bridge::Parse(EncodePlanning(Fixture())).as_object();
    older["version"] = 6;
    older.erase("satisfaction");
    for (auto& value : older.at("objectives").as_array())
        value.as_object().erase("preparation");
    decoded = DecodePlanning(boost::json::serialize(older), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, Fixture());
}

TEST(AllesPlanningCodec, SupplyIntentRetainsRequiredItemAndFundsWithoutSavingLiveMerchantAuthority)
{
    auto snapshot = Fixture();
    ObjectiveBook book;
    ASSERT_TRUE(book.Restore(snapshot.objectives));
    auto const id = book.Current()->id;
    ASSERT_TRUE(book.ProposeSupplies(id, 200, 5));
    ASSERT_TRUE(book.ObservePreparationFunds(id, 1000, 2000));
    ASSERT_TRUE(book.BeginSupplyPurchase(id, book.Find(id)->revision, 2001));
    ASSERT_TRUE(book.PreparationTransaction(id, 2002));
    ASSERT_TRUE(book.PreparationIncome(id, 1000, 1050));
    snapshot.objectives = book.Capture();
    auto const encoded = EncodePlanning(snapshot);
    auto decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    EXPECT_EQ(encoded.find("vendor"), std::string::npos);
    EXPECT_EQ(encoded.find("price"), std::string::npos);
    auto legacy = Bridge::Parse(encoded).as_object();
    legacy["version"] = 8;
    legacy.erase("satisfaction");
    auto& oldPreparation = legacy.at("objectives").as_array()[0].as_object().at("preparation").as_object();
    oldPreparation.erase("earnedMoney");
    auto upgraded = DecodePlanning(boost::json::serialize(legacy), Owner);
    ASSERT_TRUE(upgraded);
    EXPECT_EQ(upgraded->objectives.objectives.at(id).preparation->earnedMoney, 0u);
    EXPECT_EQ(upgraded->objectives.objectives.at(id).preparation->ownMoney, 1050u);
    EXPECT_EQ(upgraded->objectives.objectives.at(id).preparation->transactions, 1u);
    auto invalid = Bridge::Parse(encoded).as_object();
    auto& preparation = invalid.at("objectives").as_array()[0].as_object().at("preparation").as_object();
    preparation["item"] = 0;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(invalid), Owner));
    preparation["item"] = 200;
    preparation["kind"] = 0;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(invalid), Owner));
    preparation["kind"] = 1;
    preparation["fundsKnown"] = false;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(invalid), Owner));
}

TEST(AllesPlanningCodec, RejectsForeignOwnersUnknownVersionsFieldsAndDuplicateKeys)
{
    auto const encoded = EncodePlanning(Fixture());
    EXPECT_FALSE(DecodePlanning(encoded, {ActorKind::CreatureSpawn, Owner.id}));
    EXPECT_FALSE(DecodePlanning(encoded, {ActorKind::Player, Owner.id + 1}));
    auto value = Bridge::Parse(encoded).as_object();
    value["version"] = 14;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner));
    value["version"] = 13;
    value["movementHandle"] = 123;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner));
    EXPECT_FALSE(DecodePlanning("{\"version\":1," + encoded.substr(1), Owner));
    EXPECT_FALSE(DecodePlanning("{} trailing", Owner));
    EXPECT_FALSE(DecodePlanning(std::string(MaxPlanningBytes + 1, ' '), Owner));
}

TEST(AllesPlanningCodec, RejectsOverflowUnknownStatesDuplicateIdsAndPrivilegedExecutionFields)
{
    auto const encoded = EncodePlanning(Fixture());
    for (unsigned invalid = 0; invalid < 5; ++invalid)
    {
        auto value = Bridge::Parse(encoded).as_object();
        auto& objectives = value.at("objectives").as_array();
        auto& objective = objectives[0].as_object();
        if (invalid == 0)
            objective["quest"] = uint64_t(std::numeric_limits<uint32_t>::max()) + 1;
        else if (invalid == 1)
            objective["state"] = 255;
        else if (invalid == 2)
        {
            auto duplicate = objectives[0];
            objectives.push_back(std::move(duplicate));
        }
        else if (invalid == 3)
            objective["lastSampleMs"] = 9000;
        else
            objective["revision"] = -1;
        EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner)) << invalid;
    }
}

TEST(AllesPlanningCodec, CorruptKnowledgeDoesNotBecomeEmptyOrCertainState)
{
    auto const encoded = EncodePlanning(Fixture());
    auto value = Bridge::Parse(encoded).as_object();
    value["reports"].as_array()[0].as_object()["confidence"] = 1.0;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner));
    value = Bridge::Parse(encoded).as_object();
    value["seedVersion"] = 2;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner));
    value = Bridge::Parse(encoded).as_object();
    value["places"].as_array()[0].as_object()["minimumLevel"] = 81;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner));
}

TEST(AllesPlanningCodec, PreparedStatementOwnsPayloadAfterItsSourceIsDestroyed)
{
    CharacterDatabasePreparedStatement statement(CHAR_REP_ALLES_PLANNING, 3);
    {
        auto snapshot = Fixture();
        BindPlanning(statement, snapshot);
    }
    auto const& parameters = statement.GetParameters();
    ASSERT_EQ(parameters.size(), 3u);
    EXPECT_EQ(std::get<uint8_t>(parameters[0].data), uint8_t(ActorKind::Player));
    EXPECT_EQ(std::get<uint64_t>(parameters[1].data), Owner.id);
    auto const decoded = DecodePlanning(std::get<std::string>(parameters[2].data), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, Fixture());
}

TEST(AllesPlanningCodec, CollectionLimitsStillRoundTripWithWorstCaseStringEscaping)
{
    PlanningSnapshot snapshot{Owner, 1, {}, {}};
    snapshot.objectives.nextId = 33;
    for (uint64_t id = 1; id < snapshot.objectives.nextId; ++id)
    {
        Objective objective;
        objective.id = id;
        objective.quest = uint32_t(id);
        objective.state = ObjectiveState::Deferred;
        objective.outcome = std::string(512, '\1');
        objective.reason = objective.outcome;
        objective.approach = std::string(64, '\1');
        objective.evidence.assign(16, std::numeric_limits<uint64_t>::max());
        auto& cooperation = objective.cooperation;
        cooperation = {CooperationState::Deferred, Owner, Owner, id, uint32_t(id), 9,
            2, 1000, 2000, 602000, std::string(512, '\1'), {}};
        for (uint64_t peer = Owner.id; peer < Owner.id + 5; ++peer)
        {
            ActorKey actor{ActorKind::Player, peer};
            cooperation.agreements.emplace(actor, CompanionAgreement{{actor, std::string(100, '\1')},
                std::string(512, '\1'), 1000});
            if (actor != Owner)
                cooperation.agreements.at(actor).reportedBy = Reference{Owner, std::string(100, '\1')};
        }
        snapshot.objectives.objectives.emplace(id, std::move(objective));
    }
    snapshot.knowledge.nextReport = 65;
    for (uint32_t id = 1; id <= 64; ++id)
    {
        KnownPlace place;
        place.area = id;
        place.name = std::string(100, '\1');
        place.direction = place.name;
        snapshot.knowledge.places.emplace(id, std::move(place));
        LearnedReport report;
        report.id = id;
        report.source = {Owner, std::string(100, '\1')};
        report.text = std::string(512, '\1');
        snapshot.knowledge.reports.emplace(id, std::move(report));
    }
    ASSERT_TRUE(IsValidPlanningSnapshot(snapshot));
    auto const encoded = EncodePlanning(snapshot);
    EXPECT_GT(encoded.size(), 1024u * 1024u);
    EXPECT_LE(encoded.size(), MaxPlanningBytes);
    EXPECT_THROW(Bridge::Parse(encoded), std::invalid_argument);
    auto const decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
}

TEST(AllesPlanningCodec, ExplorationArrivalAndDiscoveredWorkSurviveTheSemanticSnapshot)
{
    ObjectiveBook book;
    auto const* place = book.ProposePlace(87, "Find work in Goldshire", "Explore known surroundings");
    ASSERT_NE(place, nullptr);
    book.ActivatePlace(place->id, place->revision, 1000, 5);
    book.ObservePlace(place->id, 87, 42, ObjectiveStep::Attempt, 2000);
    auto const id = place->id;
    PlanningSnapshot snapshot{Owner, 1, book.Capture(), {}};
    auto const decoded = DecodePlanning(EncodePlanning(snapshot), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->objectives.objectives.at(id).arrivedMs, 2000u);
    EXPECT_EQ(decoded->objectives.objectives.at(id).discoveredQuest, 42u);
    EXPECT_EQ(decoded->objectives.objectives.at(id).state, ObjectiveState::Completed);
    EXPECT_EQ(decoded->objectives.objectives.at(id).place, 87u);
    EXPECT_EQ(decoded->objectives.objectives.at(id).quest, 0u);
}

TEST(AllesPlanningCodec, QuestionStateRoundTripsAndVersionOneUpgradesWithoutInventingAnAttempt)
{
    auto snapshot = Fixture();
    auto const id = snapshot.objectives.objectives.begin()->first;
    auto& objective = snapshot.objectives.objectives.at(id);
    objective.information = {InformationStatus::Awaiting, 1, 3000, 123000, 3001, 0, "Where could I find work?"};
    auto const encoded = EncodePlanning(snapshot);
    auto const decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    auto old = Bridge::Parse(encoded).as_object();
    old["version"] = 1;
    old.erase("satisfaction");
    for (auto& value : old.at("objectives").as_array())
    {
        value.as_object().erase("information");
        value.as_object().erase("plannedMs");
        value.as_object().erase("cooperation");
        value.as_object().erase("request");
        value.as_object().erase("preparation");
    }
    auto upgraded = DecodePlanning(boost::json::serialize(old), Owner);
    ASSERT_TRUE(upgraded);
    EXPECT_EQ(upgraded->objectives.objectives.at(id).information, InformationSearch{});
    EXPECT_EQ(upgraded->objectives.objectives.at(id).checkpoint, objective.checkpoint);
    EXPECT_EQ(upgraded->knowledge, snapshot.knowledge);
    old["version"] = 2;
    old.erase("satisfaction");
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(old), Owner));
    auto invalid = Bridge::Parse(encoded).as_object();
    invalid.at("objectives").as_array()[0].as_object().at("information").as_object()["attempts"] = 3;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(invalid), Owner));
}
TEST(AllesPlanningCodec, PreferenceRoundTripsAndVersionTwoUpgradesWithoutInventingOne)
{
    auto snapshot = Fixture();
    auto& objective = snapshot.objectives.objectives.begin()->second;
    objective.plannedMs = 3000;
    auto encoded = EncodePlanning(snapshot);
    auto decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    auto previous = Bridge::Parse(encoded).as_object();
    previous["version"] = 2;
    previous.erase("satisfaction");
    for (auto& value : previous.at("objectives").as_array())
    {
        value.as_object().erase("plannedMs");
        value.as_object().erase("cooperation");
        value.as_object().erase("request");
        value.as_object().erase("preparation");
    }
    decoded = DecodePlanning(boost::json::serialize(previous), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->objectives.objectives.begin()->second.plannedMs, 0u);
    EXPECT_EQ(decoded->objectives.objectives.begin()->second.checkpoint, objective.checkpoint);
    previous["version"] = 3;
    previous.erase("satisfaction");
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(previous), Owner));
}

TEST(AllesPlanningCodec, HumanRequestsRetainSourceAndDeadlineWithoutInventingAnExecutionHandle)
{
    ObjectiveBook book;
    auto id = book.Request({RequestAction::Follow, {ActorKey{ActorKind::Player, 9}, "Traveler"},
        "Please come with me", "", 1000, 121000});
    ASSERT_TRUE(id);
    ASSERT_TRUE(book.ObserveRequest(*id, {true, false, true, false}, 2000));
    PlanningSnapshot snapshot{Owner, 1, book.Capture(), {}};
    auto encoded = EncodePlanning(snapshot);
    auto decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    EXPECT_EQ(encoded.find("movementHandle"), std::string::npos);
    EXPECT_EQ(encoded.find("session"), std::string::npos);
    snapshot.owner = {ActorKind::Player, 9};
    EXPECT_FALSE(IsValidPlanningSnapshot(snapshot));
    for (unsigned invalid = 0; invalid < 4; ++invalid)
    {
        auto value = Bridge::Parse(encoded).as_object();
        auto& request = value.at("objectives").as_array()[0].as_object().at("request").as_object();
        switch (invalid)
        {
            case 0: request["expiresMs"] = 9999999; break;
            case 1: request["action"] = 99; break;
            case 2: request["liveTarget"] = 7; break;
            case 3: request["source"] = nullptr; break;
        }
        EXPECT_FALSE(DecodePlanning(boost::json::serialize(value), Owner)) << invalid;
    }
    auto older = Bridge::Parse(EncodePlanning(Fixture())).as_object();
    older["version"] = 5;
    older.erase("satisfaction");
    for (auto& objective : older.at("objectives").as_array())
    {
        objective.as_object().erase("request");
        objective.as_object().erase("preparation");
    }
    decoded = DecodePlanning(boost::json::serialize(older), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, Fixture());
}

TEST(AllesPlanningCodec, SatisfactionReceiptsAndAuthoredEffectsSurviveSerialization)
{
    auto snapshot = Fixture();
    auto& objective = snapshot.objectives.objectives.begin()->second;
    QuestProgress receipt;
    receipt.counters[0] = 7;
    receipt.rewarded = true;
    objective.satisfactionReceipt = receipt;
    objective.assessedAttempts = objective.attempts;
    SatisfactionModel model;
    ASSERT_TRUE(model.SetDimension("purpose", {2, 0, 0, 1}));
    ASSERT_TRUE(model.SetActivity("visit_companion", {{"companionship", 0.2}, {"purpose", 0.3}}));
    ASSERT_TRUE(model.Learn("visit_companion", false, 45000));
    ASSERT_TRUE(model.ActivityReceipt("rest", 10000));
    ASSERT_TRUE(model.LearnTravel("route_test", false, 30000, 10000));
    snapshot.satisfaction = model.Capture();
    auto const encoded = EncodePlanning(snapshot);
    auto decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    for (unsigned invalid = 0; invalid < 3; ++invalid)
    {
        auto malformed = snapshot;
        switch (invalid)
        {
            case 0: malformed.satisfaction.activities["rest"]["absent_dimension"] = 0.2; break;
            case 1: malformed.satisfaction.experiences["rest"] = {2, 3, 10000}; break;
            case 2: malformed.objectives.objectives.begin()->second.satisfactionReceipt->inLog = true; break;
        }
        EXPECT_FALSE(IsValidPlanningSnapshot(malformed));
    }
}

TEST(AllesPlanningCodec, AmbitionsContextualLearningAndLegacyNeedsRoundTrip)
{
    auto snapshot = Fixture();
    SatisfactionModel model;
    ASSERT_TRUE(model.SetDimension("wealth", {4, 20000, 0, 0, MotivationCurve::Growth, 1000}));
    ASSERT_TRUE(model.LearnOutcome("pursue_quest", "forest", true, 120000, {{"wealth", 300}}));
    ASSERT_TRUE(model.LearnOutcome("pursue_quest", "forest", false, 30000, {{"wealth", -50}}));
    snapshot.satisfaction = model.Capture();
    snapshot.satisfaction.horizonMs = 900000;
    auto decoded = DecodePlanning(EncodePlanning(snapshot), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    SatisfactionModel restored;
    ASSERT_TRUE(restored.Restore(decoded->satisfaction));
    EXPECT_EQ(restored.ExpectedEffects("pursue_quest", "forest"),
        model.ExpectedEffects("pursue_quest", "forest"));
    EXPECT_EQ(restored.ExpectedEffects("pursue_quest", "forest", false),
        model.ExpectedEffects("pursue_quest", "forest", false));

    auto legacy = Bridge::Parse(EncodePlanning(Fixture())).as_object();
    legacy["version"] = 11;
    auto& state = legacy.at("satisfaction").as_object();
    state.erase("contexts");
    state.erase("horizonMs");
    for (auto& dimension : state.at("dimensions").as_array())
    {
        dimension.as_object().erase("curve");
        dimension.as_object().erase("scale");
        dimension.as_object().erase("urgency");
    }
    decoded = DecodePlanning(boost::json::serialize(legacy), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->satisfaction, DefaultSatisfaction());
    for (unsigned invalid = 0; invalid < 4; ++invalid)
    {
        auto data = Bridge::Parse(EncodePlanning(snapshot)).as_object();
        auto& motives = data.at("satisfaction").as_object();
        switch (invalid)
        {
            case 0: motives.at("dimensions").as_array()[0].as_object()["curve"] = 9; break;
            case 1: motives.at("dimensions").as_array()[0].as_object()["scale"] = 0; break;
            case 2: motives["horizonMs"] = 0; break;
            case 3: motives.at("contexts").as_array()[0].as_object()["id"] = "absent:forest"; break;
        }
        EXPECT_FALSE(DecodePlanning(boost::json::serialize(data), Owner));
    }
}

TEST(AllesPlanningCodec, NeedUrgencyPersistsAndVersionTwelveKeepsItsExistingPreferences)
{
    auto snapshot = Fixture();
    snapshot.satisfaction.dimensions.at("security").urgency = 4;
    auto const encoded = EncodePlanning(snapshot);
    auto decoded = DecodePlanning(encoded, Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    auto legacy = Bridge::Parse(encoded).as_object();
    legacy["version"] = 12;
    for (auto& dimension : legacy.at("satisfaction").as_object().at("dimensions").as_array())
        dimension.as_object().erase("urgency");
    decoded = DecodePlanning(boost::json::serialize(legacy), Owner);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->satisfaction, DefaultSatisfaction());
    auto invalid = Bridge::Parse(encoded).as_object();
    invalid.at("satisfaction").as_object().at("dimensions").as_array()[0].as_object()["urgency"] = 11;
    EXPECT_FALSE(DecodePlanning(boost::json::serialize(invalid), Owner));
}

}

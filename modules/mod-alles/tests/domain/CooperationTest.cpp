/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "domain/Cooperation.h"
#include "domain/ControlMode.h"
#include "domain/Objective.h"
#include "storage/PlanningCodec.h"
#include "bridge/Wire.h"
#include "QuestObjectiveControl.h"
#include "gtest/gtest.h"

namespace Alles
{
namespace
{
constexpr ActorKey Leader{ActorKind::Player, 1};
constexpr ActorKey Peer{ActorKind::Player, 2};

CompanionAgreement Agreement(ActorKey actor)
{
    return {{actor, "Player " + std::to_string(actor.id)}, "I agree to work on this quest together.", 0};
}

Cooperation Recruiting()
{
    Cooperation state;
    EXPECT_TRUE(BeginRecruitment(state, Leader, 1, 7, 9, Agreement(Leader), 1000));
    return state;
}

PartyObservation Ready()
{
    return {99, Leader, true, false,
        {{Leader, true, true, true, true, true, false}, {Peer, true, true, true, true, true, false}}};
}
}

TEST(AllesCooperation, AgreementRequiresActualMembershipRendezvousAndReadinessBeforeWork)
{
    auto state = Recruiting();
    ASSERT_TRUE(AgreeCompanion(state, Agreement(Peer), 2000));
    EXPECT_EQ(state.state, CooperationState::Agreed);
    EXPECT_FALSE(ObserveCooperation(state, {}, 2001));
    EXPECT_EQ(state.state, CooperationState::Agreed);
    auto party = Ready();
    party.members[1].inRendezvous = false;
    ASSERT_TRUE(ObserveCooperation(state, party, 3000));
    EXPECT_EQ(state.state, CooperationState::Rendezvous);
    party.members[1].inRendezvous = true;
    party.members[1].eligible = false;
    ObserveCooperation(state, party, 3001);
    EXPECT_EQ(state.state, CooperationState::Rendezvous);
    party.members[1].eligible = true;
    party.members[1].ready = false;
    ObserveCooperation(state, party, 3002);
    EXPECT_EQ(state.state, CooperationState::Rendezvous);
    party.members[1].ready = true;
    ASSERT_TRUE(ObserveCooperation(state, party, 3003));
    EXPECT_EQ(state.state, CooperationState::Working);
    EXPECT_TRUE(IsValidCooperation(state));
}

TEST(AllesCooperation, LeadersRewardNeverCompletesAnotherParticipantsWork)
{
    auto state = Recruiting();
    ASSERT_TRUE(AgreeCompanion(state, Agreement(Peer), 2000));
    auto party = Ready();
    ObserveCooperation(state, party, 3000);
    party.members[0].finished = true;
    ObserveCooperation(state, party, 4000);
    EXPECT_EQ(state.state, CooperationState::Working);
    party.members[1].finished = true;
    ASSERT_TRUE(ObserveCooperation(state, party, 5000));
    EXPECT_EQ(state.state, CooperationState::Completed);
    ReconcileCooperation(state, 6000);
    EXPECT_EQ(state.state, CooperationState::Agreed);
    party.members[1].finished = false;
    ObserveCooperation(state, party, 6001);
    EXPECT_EQ(state.state, CooperationState::Working);
}

TEST(AllesCooperation, RequiredPartySizeAndActualMembershipPrecedeWork)
{
    auto state = Recruiting();
    ActorKey const third{ActorKind::Player, 3};
    ASSERT_TRUE(AgreeCompanion(state, Agreement(Peer), 2000));
    ASSERT_TRUE(AgreeCompanion(state, Agreement(third), 2001));
    auto party = Ready();
    party.requiredMembers = 3;
    ObserveCooperation(state, party, 3000);
    EXPECT_EQ(state.state, CooperationState::Rendezvous);
    EXPECT_FALSE(CooperativeGuide(state, party));
    party.members.push_back({third, true, true, true, true, true, false});
    ObserveCooperation(state, party, 4000);
    EXPECT_EQ(state.state, CooperationState::Working);
    EXPECT_EQ(CooperativeGuide(state, party), Leader);
}

TEST(AllesCooperation, NextWorkingMemberFollowsIndividualCreditThenEachOutstandingReward)
{
    auto state = Recruiting();
    ASSERT_TRUE(AgreeCompanion(state, Agreement(Peer), 2000));
    auto party = Ready();
    ObserveCooperation(state, party, 3000);
    EXPECT_EQ(CooperativeGuide(state, party), Leader);
    party.members[0].readyToReward = true;
    EXPECT_EQ(CooperativeGuide(state, party), Peer);
    party.members[1].readyToReward = true;
    EXPECT_EQ(CooperativeGuide(state, party), Leader);
    party.members[0].finished = true;
    EXPECT_EQ(CooperativeGuide(state, party), Peer);
    EXPECT_EQ(state.state, CooperationState::Working);
    party.members[1].finished = true;
    EXPECT_FALSE(CooperativeGuide(state, party));
    ObserveCooperation(state, party, 4000);
    EXPECT_EQ(state.state, CooperationState::Completed);
    EXPECT_FALSE(ObserveCooperation(state, {}, 5000));
    EXPECT_EQ(state.state, CooperationState::Completed); // Ordinary disband does not undo the observed outcome.
}

TEST(AllesCooperation, RecoveryAndCatchingUpDoNotForgetAnEstablishedRendezvous)
{
    auto state = Recruiting();
    ASSERT_TRUE(AgreeCompanion(state, Agreement(Peer), 2000));
    auto party = Ready();
    ObserveCooperation(state, party, 3000);
    auto const deadline = state.deadlineMs;
    party.members[1].ready = false;
    party.members[1].inRendezvous = false;
    ObserveCooperation(state, party, 4000);
    EXPECT_EQ(state.state, CooperationState::Working);
    EXPECT_EQ(state.deadlineMs, deadline);
    party.members[1].ready = true;
    party.members[1].inRendezvous = true;
    ObserveCooperation(state, party, 5000);
    EXPECT_EQ(state.state, CooperationState::Working);
    EXPECT_EQ(state.deadlineMs, deadline);
    EXPECT_TRUE(DeferCooperation(state, "No measured route advancement", 6000));
    EXPECT_EQ(state.state, CooperationState::Deferred);
    EXPECT_EQ(state.attempts, 1u);
    EXPECT_EQ(state.agreements.size(), 2u);
    EXPECT_FALSE(DeferCooperation(state, "Do not refresh the retry window", 7000));
    EXPECT_EQ(state.reconsiderMs, 606000u);
}

TEST(AllesCooperation, MissingDeclinedDepartedAndUnagreedMembersCannotSupplyReadiness)
{
    for (unsigned scenario = 0; scenario < 5; ++scenario)
    {
        auto state = Recruiting();
        ASSERT_TRUE(AgreeCompanion(state, Agreement(Peer), 2000));
        auto party = Ready();
        ObserveCooperation(state, party, 3000);
        switch (scenario)
        {
            case 0: party.members.pop_back(); break;
            case 1: party.members[1].online = false; break;
            case 2: party.ordinaryParty = false; break;
            case 3: party.members[1].actor.id = 88; break;
            case 4: party.group = 0; break;
        }
        ObserveCooperation(state, party, 3001);
        EXPECT_NE(state.state, CooperationState::Working);
        EXPECT_NE(state.state, CooperationState::Completed);
        EXPECT_TRUE(IsValidCooperation(state));
    }
    auto state = Recruiting();
    ObserveCooperation(state, {}, 121000);
    EXPECT_EQ(state.state, CooperationState::Deferred);
    EXPECT_FALSE(AgreeCompanion(state, Agreement(Peer), 121001));
}

TEST(AllesCooperation, RecruitmentAttemptsRemainBoundedAcrossSavedIntentions)
{
    auto state = Recruiting();
    ObserveCooperation(state, {}, 121000);
    EXPECT_FALSE(BeginRecruitment(state, Leader, 1, 7, 9, Agreement(Leader), 122000));
    ASSERT_TRUE(BeginRecruitment(state, Leader, 1, 7, 9, Agreement(Leader), 721000));
    EXPECT_EQ(state.attempts, 2u);
    ObserveCooperation(state, {}, 841000);
    EXPECT_FALSE(BeginRecruitment(state, Leader, 1, 7, 9, Agreement(Leader), 1441000));
    EXPECT_FALSE(BeginRecruitment(state, Peer, 1, 7, 9, Agreement(Peer), 1441000));
    EXPECT_TRUE(IsValidCooperation(state));
}

TEST(AllesCooperation, ConsentAndPartySizeAreBoundedAndHumanControlCancelsCooperation)
{
    auto state = Recruiting();
    for (uint64_t id = 2; id <= 5; ++id)
        EXPECT_TRUE(AgreeCompanion(state, Agreement({ActorKind::Player, id}), 2000));
    EXPECT_FALSE(AgreeCompanion(state, Agreement({ActorKind::Player, 6}), 2000));
    EXPECT_FALSE(AgreeCompanion(state, Agreement(Peer), 2001));
    auto party = Ready();
    party.ownerControlled = true;
    ObserveCooperation(state, party, 2001);
    EXPECT_EQ(state.state, CooperationState::Cancelled);
    EXPECT_TRUE(IsValidCooperation(state));
    Cooperation follower;
    ASSERT_TRUE(AcceptCooperation(follower, Peer, 2, 7, 9, Agreement(Peer), Agreement(Leader), 2000));
    EXPECT_EQ(follower.owner, Peer);
    EXPECT_EQ(follower.leader, Leader);
    EXPECT_EQ(follower.state, CooperationState::Agreed);
    EXPECT_FALSE(ObserveCooperation(follower, {}, 2001));
}

TEST(AllesCooperation, PersistenceBindsAgreementToOwnerAndObjectiveAndDoesNotRestoreALiveGroup)
{
    ObjectiveBook book;
    auto const* objective = book.ProposeQuest(7, "Earn my reward", "My own accepted quest");
    ASSERT_NE(objective, nullptr);
    auto state = Recruiting();
    ASSERT_TRUE(AgreeCompanion(state, Agreement(Peer), 2000));
    ObserveCooperation(state, Ready(), 3000);
    ASSERT_TRUE(book.SetCooperation(objective->id, objective->revision, state));
    EXPECT_FALSE(book.SetCooperation(objective->id, objective->revision, {}));
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Seed(1, false, true));
    PlanningSnapshot snapshot{Leader, 1, book.Capture(), knowledge.Capture()};
    auto encoded = Storage::EncodePlanning(snapshot);
    auto decoded = Storage::DecodePlanning(encoded, Leader);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    EXPECT_EQ(encoded.find("groupId"), std::string::npos);
    EXPECT_EQ(encoded.find("invitation"), std::string::npos);
    snapshot.owner = Peer;
    EXPECT_FALSE(IsValidPlanningSnapshot(snapshot));
    auto previous = Bridge::Parse(encoded).as_object();
    previous["version"] = 3;
    for (auto& value : previous.at("objectives").as_array())
    {
        value.as_object().erase("cooperation");
        value.as_object().erase("request");
        value.as_object().erase("preparation");
    }
    decoded = Storage::DecodePlanning(boost::json::serialize(previous), Leader);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->objectives.objectives.begin()->second.cooperation, Cooperation{});
    auto invalid = Bridge::Parse(encoded).as_object();
    auto& agreements = invalid.at("objectives").as_array()[0].as_object().at("cooperation")
        .as_object().at("agreements").as_array();
    agreements.push_back(agreements[0]);
    EXPECT_FALSE(Storage::DecodePlanning(boost::json::serialize(invalid), Leader));
}

TEST(AllesCooperation, DeliveredLeadersRosterRetainsProvenanceAndStillRequiresEveryActualMember)
{
    Cooperation state;
    ASSERT_TRUE(AcceptCooperation(state, Peer, 1, 7, 9, Agreement(Peer), Agreement(Leader), 2000));
    auto const self = state.agreements.at(Peer);
    auto const leader = state.agreements.at(Leader);
    ActorKey const third{ActorKind::Player, 3};
    std::vector<Reference> roster{leader.person, self.person, Agreement(third).person};
    std::string const statement = "Our agreed party: Player 1, Player 2, Player 3.";
    ASSERT_TRUE(AdoptCooperativeRoster(state, leader.person, roster, statement, 3000));
    EXPECT_EQ(state.agreements.at(Peer), self);
    EXPECT_EQ(state.agreements.at(Leader), leader);
    EXPECT_EQ(state.agreements.at(third).reportedBy, leader.person);
    EXPECT_EQ(state.agreements.at(third).statement, statement);
    EXPECT_EQ(state.agreements.at(third).agreedMs, 3000u);
    auto const before = state;
    ASSERT_TRUE(AdoptCooperativeRoster(state, leader.person, roster, statement, 4000));
    EXPECT_EQ(state, before); // Repetition is not another agreement or fresher evidence.
    ObserveCooperation(state, {}, 4000);
    EXPECT_EQ(state.state, CooperationState::Agreed);
    auto party = Ready();
    party.requiredMembers = 3;
    ObserveCooperation(state, party, 4001);
    EXPECT_EQ(state.state, CooperationState::Rendezvous);
    party.members.push_back({third, true, true, true, true, true, false});
    ObserveCooperation(state, party, 4002);
    EXPECT_EQ(state.state, CooperationState::Working);
    ASSERT_TRUE(AdoptCooperativeRoster(state, leader.person, roster, statement, 4003));
    roster.push_back(Agreement({ActorKind::Player, 4}).person);
    EXPECT_FALSE(AdoptCooperativeRoster(state, leader.person, roster, statement + " Player 4", 4004));
}

TEST(AllesCooperation, RosterRejectsForeignSourcesMissingNamesAndMembershipReplacementAtomically)
{
    Cooperation original;
    ASSERT_TRUE(AcceptCooperation(original, Peer, 1, 7, 9, Agreement(Peer), Agreement(Leader), 2000));
    for (unsigned invalid = 0; invalid < 9; ++invalid)
    {
        auto state = original;
        auto source = Agreement(Leader).person;
        std::vector<Reference> roster{source, Agreement(Peer).person, Agreement({ActorKind::Player, 3}).person};
        std::string statement = "Our party: Player 1, Player 2, Player 3.";
        uint64_t now = 3000;
        switch (invalid)
        {
            case 0: source = Agreement(Peer).person; break;
            case 1: roster.erase(roster.begin()); break;
            case 2: roster.erase(roster.begin() + 1); break;
            case 3: roster.push_back(roster.back()); break;
            case 4: roster.back().actor->kind = ActorKind::CreatureSpawn; break;
            case 5: statement = "Our party: Player 1, Player 2."; break;
            case 6: roster[1].name = "Someone else"; statement += " Someone else"; break;
            case 7: now = state.deadlineMs; break;
            case 8: now = state.startedMs - 1; break;
        }
        EXPECT_FALSE(AdoptCooperativeRoster(state, source, roster, statement, now)) << invalid;
        EXPECT_EQ(state, original) << invalid;
    }
    auto relayed = Agreement({ActorKind::Player, 3});
    relayed.reportedBy = Agreement(Leader).person;
    auto recruiting = Recruiting();
    EXPECT_FALSE(AgreeCompanion(recruiting, relayed, 3000));
}

TEST(AllesCooperation, RelayedAgreementsRoundTripAndOlderDirectAgreementsUpgrade)
{
    ObjectiveBook book;
    auto const* objective = book.ProposeQuest(7, "Help with this quest", "A delivered request");
    ASSERT_NE(objective, nullptr);
    Cooperation state;
    ASSERT_TRUE(AcceptCooperation(state, Peer, objective->id, 7, 9, Agreement(Peer), Agreement(Leader), 2000));
    ASSERT_TRUE(AdoptCooperativeRoster(state, Agreement(Leader).person,
        {Agreement(Leader).person, Agreement(Peer).person, Agreement({ActorKind::Player, 3}).person},
        "Our party: Player 1, Player 2, Player 3.", 3000));
    ASSERT_TRUE(book.SetCooperation(objective->id, objective->revision, state));
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Seed(1, false, true));
    PlanningSnapshot snapshot{Peer, 1, book.Capture(), knowledge.Capture()};
    auto encoded = Storage::EncodePlanning(snapshot);
    auto decoded = Storage::DecodePlanning(encoded, Peer);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
    auto invalid = Bridge::Parse(encoded).as_object();
    auto& agreements = invalid.at("objectives").as_array()[0].as_object().at("cooperation")
        .as_object().at("agreements").as_array();
    agreements[2].as_object().at("reportedBy").as_object().at("actor").as_object()["id"] = Peer.id;
    EXPECT_FALSE(Storage::DecodePlanning(boost::json::serialize(invalid), Peer));

    snapshot.objectives.objectives.begin()->second.cooperation.agreements.erase({ActorKind::Player, 3});
    auto old = Bridge::Parse(Storage::EncodePlanning(snapshot)).as_object();
    old["version"] = 4;
    old.at("objectives").as_array()[0].as_object().erase("request");
    old.at("objectives").as_array()[0].as_object().erase("preparation");
    for (auto& item : old.at("objectives").as_array()[0].as_object().at("cooperation")
        .as_object().at("agreements").as_array())
        item.as_object().erase("reportedBy");
    decoded = Storage::DecodePlanning(boost::json::serialize(old), Peer);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, snapshot);
}

TEST(AllesControlMode, BotMasterInSamePartyRemainsAutonomousWhileHumanChainsAndExternalControlDoNot)
{
    ControlRecord bot{true, true, false, false, 0, true, {}};
    std::map<ActorKey, ControlRecord> chain{{Leader, bot}, {Peer, bot}};
    EXPECT_EQ(ClassifyControl(Peer, chain), ControlMode::AutonomousSolo);
    chain[Peer].master = Leader;
    EXPECT_EQ(ClassifyControl(Peer, chain), ControlMode::Human);
    chain[Leader].group = chain[Peer].group = 9;
    EXPECT_EQ(ClassifyControl(Peer, chain), ControlMode::AutonomousParty);
    chain[Leader].botSession = false;
    EXPECT_EQ(ClassifyControl(Peer, chain), ControlMode::Human);
    chain[Leader].botSession = true;
    chain[Leader].external = true;
    EXPECT_EQ(ClassifyControl(Peer, chain), ControlMode::Human);
    chain[Leader].external = false;
    chain[Peer].ordinaryParty = false;
    EXPECT_EQ(ClassifyControl(Peer, chain), ControlMode::UnsupportedGroup);
    chain[Peer].ordinaryParty = true;
    chain[Leader].available = false;
    EXPECT_EQ(ClassifyControl(Peer, chain), ControlMode::Unavailable);
    chain[Leader].available = true;
    chain[Leader].master = Peer;
    EXPECT_EQ(ClassifyControl(Peer, chain), ControlMode::Human);
}

TEST(AllesCooperativeExecution, PartyCapabilityRequiresItsExactAgreedQuestAndEnoughActualReadyMembers)
{
    QuestObjectiveControl control;
    control.cooperativeQuest = 7;
    control.partyMembers = 5;
    EXPECT_FALSE(control.PartyReady(7, 2));
    control.plannerAttached = true;
    EXPECT_TRUE(control.PartyReady(7, 5));
    EXPECT_FALSE(control.PartyReady(8, 2));
    EXPECT_FALSE(control.PartyReady(7, 6));
    control.partyMembers = 1;
    EXPECT_FALSE(control.PartyReady(7, 1));
    control.partyMembers = 3;
    EXPECT_TRUE(control.PartyReady(7, 2));
}
}

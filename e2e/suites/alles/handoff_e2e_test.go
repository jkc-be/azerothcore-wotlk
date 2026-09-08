//go:build e2e

package alles_test

import (
	"context"
	"encoding/json"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

type savedQuestIntention struct {
	ID, Revision, ActiveWithoutProgressMs uint64
	Quest, Attempts, GainedCredit         uint32
	State, Step                           uint8
	Checkpoint                            struct {
		Counters                               [10]uint32
		InLog, ReadyToReward, Rewarded, Failed bool
	}
}

func readSavedPlanningPayload(t *testing.T, guest *e2eharness.ScenarioBot, owner uint64) []byte {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	var data []byte
	if err := guest.CharDB.QueryRowContext(ctx,
		"SELECT payload FROM alles_planning WHERE owner_kind=0 AND owner_id=?", owner).Scan(&data); err != nil {
		e2eharness.HarnessFailf(t, "read acknowledged planning payload: %v", err)
	}
	var snapshot struct {
		Owner struct {
			Kind uint8
			ID   uint64
		}
		Revision   uint64
		Objectives []savedQuestIntention
	}
	if len(data) > 2*1024*1024 || json.Unmarshal(data, &snapshot) != nil ||
		snapshot.Owner.Kind != 0 || snapshot.Owner.ID != owner || snapshot.Revision == 0 {
		e2eharness.HarnessFailf(t, "invalid acknowledged owner planning payload")
	}
	return data
}

func readSavedQuestIntention(t *testing.T, guest *e2eharness.ScenarioBot, owner, id uint64) savedQuestIntention {
	t.Helper()
	var snapshot struct {
		Objectives []savedQuestIntention
	}
	if err := json.Unmarshal(readSavedPlanningPayload(t, guest, owner), &snapshot); err != nil {
		e2eharness.HarnessFailf(t, "decode acknowledged quest intentions: %v", err)
	}
	for _, objective := range snapshot.Objectives {
		if objective.ID == id {
			return objective
		}
	}
	e2eharness.Assertf(t, "retained quest intention %d disappeared from saved planning", id)
	return savedQuestIntention{}
}

// A real human's normal party invitation must take control without discarding the bot's saved quest intention.
// The objective must stop charging active work while controlled and resume after the human leaves the party.
func TestAlles_HumanInvitationSuspendsAndResumesQuest(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"alles", "serial", "med"}, Runtime: "med", Category: "alles"})
	f, guest := objectiveScene(t)
	run, start := readObjectiveBot(t, f)
	var intention objectiveView
	for _, objective := range start.Planning.Objectives {
		if objective.Quest == f.Quest && objective.State == "active" && objective.Step == "travel" {
			intention = objective
		}
	}
	if f.Quest == 0 || intention.ID == 0 || start.Planning.Engine != "new_rpg" || guest.World.GroupState().InGroup {
		e2eharness.Preconditionf(t, "fixture needs a solo bot actively traveling for an incomplete quest and an ungrouped human")
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	var groups int
	err := guest.CharDB.QueryRowContext(ctx, "SELECT COUNT(*) FROM group_member WHERE memberGuid=?", f.Target.GUID).
		Scan(&groups)
	cancel()
	if err != nil || groups != 0 {
		e2eharness.Preconditionf(t, "target must be ungrouped: %v", err)
	}
	watchObjectiveBot(t, guest, start)
	flushActor(t, guest, f.Target.GUID)
	before := readSavedQuestIntention(t, guest, f.Target.GUID, intention.ID)
	if before.State != 1 || !before.Checkpoint.InLog || before.Checkpoint.ReadyToReward ||
		before.Checkpoint.Failed || before.Checkpoint.Rewarded {
		e2eharness.Preconditionf(t, "quest changed before the human invitation; use a quiet route away from credit and turn-ins")
	}
	t.Cleanup(func() {
		if !guest.World.IsStopped() && guest.World.GroupState().InGroup {
			guest.LeaveGroup(t)
			guest.WaitNotInGroup(t, 5*time.Second)
		}
	})
	if err := guest.World.GroupInvite(f.Target.Name); err != nil {
		e2eharness.HarnessFailf(t, "send normal human party invitation: %v", err)
	}
	party := guest.WaitGroupList(t, true, 2, 15*time.Second)
	if party.LeaderGUID != f.Actor.GUID || party.GroupType != 0 || party.MemberCount != 2 || len(party.Members) != 1 ||
		party.Members[0].GUID != f.Target.GUID || party.Members[0].Name != f.Target.Name {
		e2eharness.Assertf(t, "human invitation did not form the expected two-member party")
	}
	if !eventually(10*time.Second, func() bool {
		currentRun, bot := readObjectiveBot(t, f)
		if currentRun != run {
			e2eharness.HarnessFailf(t, "world restarted during human handoff")
		}
		for _, objective := range bot.Planning.Objectives {
			if objective.ID == intention.ID && bot.Planning.Engine == "human_control_or_incompatible_engine" &&
				objective.State == "waiting" {
				return true
			}
		}
		return false
	}) {
		e2eharness.Assertf(t, "accepted human invitation did not suspend the quest planner")
	}
	flushActor(t, guest, f.Target.GUID)
	paused := readSavedQuestIntention(t, guest, f.Target.GUID, intention.ID)
	if paused.State != 2 || paused.Step != 5 || paused.Quest != before.Quest ||
		paused.Attempts != before.Attempts || paused.GainedCredit != before.GainedCredit || paused.Checkpoint != before.Checkpoint {
		e2eharness.Assertf(t, "human handoff lost the saved quest, changed credit or restarted the attempt")
	}
	// Observe several fresh ticks while the human remains in control; a saved intention alone is not suspension.
	until := time.Now().Add(5 * time.Second)
	if !eventually(8*time.Second, func() bool {
		currentRun, bot := readObjectiveBot(t, f)
		if currentRun != run || bot.Planning.Engine != "human_control_or_incompatible_engine" {
			e2eharness.Assertf(t, "planner reclaimed the bot while the human still led its party")
		}
		return !time.Now().Before(until)
	}) {
		e2eharness.Assertf(t, "human handoff observation window did not finish")
	}
	flushActor(t, guest, f.Target.GUID)
	held := readSavedQuestIntention(t, guest, f.Target.GUID, intention.ID)
	if held.State != paused.State || held.Attempts != paused.Attempts || held.Checkpoint != paused.Checkpoint ||
		held.ActiveWithoutProgressMs != paused.ActiveWithoutProgressMs {
		e2eharness.Assertf(t, "human-controlled time changed quest credit, attempts or the active-work clock")
	}
	guest.LeaveGroup(t)
	guest.WaitNotInGroup(t, 5*time.Second)
	if !eventually(20*time.Second, func() bool {
		currentRun, bot := readObjectiveBot(t, f)
		if currentRun != run {
			e2eharness.HarnessFailf(t, "world restarted before autonomous resumption")
		}
		for _, objective := range bot.Planning.Objectives {
			if objective.ID == intention.ID && bot.Planning.Engine == "new_rpg" && objective.State == "active" {
				return objective.Attempts == before.Attempts
			}
		}
		return false
	}) {
		e2eharness.Assertf(t, "bot did not resume its retained quest attempt after the human left")
	}
	if !savedQuestPresent(t, guest, f, f.Quest, false) {
		e2eharness.Assertf(t, "resumed intention has neither a saved quest nor its legitimate reward")
	}
	t.Logf("PASS human=%d invited bot=%d; quest=%d intention=%d suspended with saved credit and resumed attempt=%d",
		f.Actor.GUID, f.Target.GUID, f.Quest, intention.ID, before.Attempts)
}

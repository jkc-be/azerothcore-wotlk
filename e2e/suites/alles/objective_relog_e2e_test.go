//go:build e2e

package alles_test

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// Normal client takeover suspends a bot's quest, then logout must evict its owner. A new heard observation
// after fresh login forces a new snapshot transaction, proving the planning payload came back through loading.
// Autonomous resumption after human party control is a separate oracle in the handoff keeper.
func TestAlles_ObjectiveSurvivesEvictionAndClientRelog(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"alles", "serial", "med"}, Runtime: "med", Category: "alles"})
	f, guest := objectiveScene(t)
	_, start := readObjectiveBot(t, f)
	var intention objectiveView
	for _, objective := range start.Planning.Objectives {
		if objective.Quest == f.Quest && objective.State == "active" && objective.Step == "travel" {
			intention = objective
		}
	}
	if !f.ClientTakeover || f.Quest == 0 || intention.ID == 0 || start.Planning.Engine != "new_rpg" {
		e2eharness.Preconditionf(t, "requires clientTakeover=true and a solo bot traveling for an incomplete quest")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	var race, class, otherOnline, groups int
	err := guest.AuthDB.QueryRowContext(ctx, "SELECT id FROM account WHERE username=?", f.Target.Account).
		Scan(&f.Target.accountID)
	if err == nil {
		err = guest.CharDB.QueryRowContext(ctx,
			"SELECT race,class FROM characters WHERE guid=? AND account=? AND name=? AND online=1",
			f.Target.GUID, f.Target.accountID, f.Target.Name).Scan(&race, &class)
	}
	if err == nil {
		err = guest.CharDB.QueryRowContext(ctx,
			"SELECT COUNT(*) FROM characters WHERE account=? AND guid<>? AND online<>0",
			f.Target.accountID, f.Target.GUID).Scan(&otherOnline)
	}
	if err == nil {
		err = guest.CharDB.QueryRowContext(ctx, "SELECT COUNT(*) FROM group_member WHERE memberGuid=?", f.Target.GUID).
			Scan(&groups)
	}
	cancel()
	if err != nil || race != int(e2eharness.RaceHuman) || class != int(e2eharness.ClassWarrior) ||
		otherOnline != 0 || groups != 0 {
		e2eharness.Preconditionf(t, "takeover needs the exact ungrouped Human Warrior and no online account siblings: %v", err)
	}
	flushActor(t, guest, f.Target.GUID)
	before := readSavedQuestIntention(t, guest, f.Target.GUID, intention.ID)
	if before.State != 1 || !before.Checkpoint.InLog || before.Checkpoint.ReadyToReward ||
		before.Checkpoint.Rewarded || before.Checkpoint.Failed {
		e2eharness.Preconditionf(t, "use a quiet route away from combat, credit triggers and turn-ins")
	}
	// SecureLogin's normal CMSG_PLAYER_LOGIN path logs out this exact independent bot before client entry.
	target := loginFixtureAccount(t, guest.AuthDB, guest.CharDB, f.Target, false)
	if !eventually(15*time.Second, func() bool {
		return field(status(t, guest, f.Target.GUID), "state") == "ready"
	}) {
		e2eharness.Assertf(t, "taken-over owner did not become ready")
	}
	flushActor(t, guest, f.Target.GUID)
	paused := readSavedQuestIntention(t, guest, f.Target.GUID, intention.ID)
	if paused.State != 2 || paused.Step != 5 || paused.Quest != before.Quest ||
		paused.Attempts != before.Attempts || paused.Checkpoint != before.Checkpoint ||
		paused.GainedCredit != before.GainedCredit {
		e2eharness.Assertf(t, "normal client takeover failed to retain and suspend the quest attempt")
	}
	expected := readSavedPlanningPayload(t, guest, f.Target.GUID)
	var knowledge struct {
		Places []json.RawMessage
	}
	if err := json.Unmarshal(expected, &knowledge); err != nil || len(knowledge.Places) == 0 {
		e2eharness.Preconditionf(t, "fixture must have private place knowledge before relog: %v", err)
	}
	generation := number(t, status(t, guest, f.Target.GUID), "generation")
	if err := target.World.SendLogout(); err != nil {
		e2eharness.HarnessFailf(t, "target graceful logout: %v", err)
	}
	if err := target.World.WaitForLogout(30 * time.Second); err != nil {
		e2eharness.Assertf(t, "target graceful logout did not complete: %v", err)
	}
	if target.World.SessionPhase() != client.PhaseLogout {
		e2eharness.Assertf(t, "target disconnected without SMSG_LOGOUT_COMPLETE")
	}
	target.Close()
	if !eventually(15*time.Second, func() bool {
		return field(status(t, guest, f.Target.GUID), "state") == "unloaded"
	}) {
		e2eharness.Assertf(t, "owner did not evict after logout; fixture must prevent automatic bot relog during this window")
	}
	target = loginFixtureAccount(t, guest.AuthDB, guest.CharDB, f.Target, false)
	var loadedRevision uint64
	if !eventually(15*time.Second, func() bool {
		line := status(t, guest, f.Target.GUID)
		if field(line, "state") != "ready" || number(t, line, "generation") <= generation {
			return false
		}
		loadedRevision = number(t, line, "revision")
		return true
	}) {
		e2eharness.Assertf(t, "relog did not load a new owner generation")
	}
	x, y, z, _, worldMap := target.World.Position()
	guest.Teleport(t, x+2, y, z, uint32(worldMap))
	commands(t, guest, ownerPrefix(f.Target.GUID), ".gm off", ".gm visible on", ".gm chat off",
		fmt.Sprintf(".alles status player %d", f.Target.GUID))
	target.WaitUnitGUID(t, guest.GUID, 10*time.Second)
	marker := fmt.Sprintf("relog%d", time.Now().UnixNano())
	seen, _, stop := watchSay(target.World, guest.GUID, marker)
	defer stop()
	say(t, guest, "I heard that travelers gather near Goldshire. "+marker)
	waitSay(t, seen)
	// Flush drains ingress. Require this new input in the acknowledged transaction, not just an old planning row.
	if revision := flushActor(t, guest, f.Target.GUID); revision <= loadedRevision {
		e2eharness.Assertf(t, "post-relog observation did not create a new persistent revision")
	}
	ctx, cancel = context.WithTimeout(context.Background(), 5*time.Second)
	var heard int
	err = guest.CharDB.QueryRowContext(ctx,
		`SELECT (SELECT COUNT(*) FROM alles_perception WHERE owner_kind=0 AND owner_id=?
		 AND source_kind=0 AND source_id=? AND INSTR(gated_text,?)>0) +
		 (SELECT COUNT(*) FROM alles_memory WHERE owner_kind=0 AND owner_id=?
		 AND source_kind=0 AND source_id=? AND INSTR(claim,?)>0)`,
		f.Target.GUID, guest.GUID, marker, f.Target.GUID, guest.GUID, marker).Scan(&heard)
	cancel()
	if err != nil {
		e2eharness.HarnessFailf(t, "read newly acknowledged heard input: %v", err)
	}
	if heard == 0 {
		e2eharness.Assertf(t, "fresh snapshot did not retain the witnessed post-relog input")
	}
	if !bytes.Equal(expected, readSavedPlanningPayload(t, guest, f.Target.GUID)) {
		e2eharness.Assertf(t, "eviction/relog changed the saved planning payload under client control")
	}
	if !savedQuestPresent(t, guest, f, f.Quest, false) {
		e2eharness.Assertf(t, "retained intention lost its actual saved quest")
	}
	t.Logf("PASS owner=%d quest=%d intention=%d survived eviction, fresh login and a new snapshot transaction",
		f.Target.GUID, f.Quest, intention.ID)
}

//go:build e2e

package alles_test

import (
	"context"
	"fmt"
	"regexp"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// Autonomous cooperation must gather through ordinary movement and earn each participant's own reward.
// The pre-provisioned exclusive cohort starts ungrouped. Only the invisible observer is moved by this test.
func TestAlles_CooperationGathersAndRewardsEachMember(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"alles", "serial", "long"}, Runtime: "long", Category: "alles"})
	f, guest := objectiveScene(t)
	if f.Quest == 0 || len(f.Companions) < 1 || len(f.Companions) > 4 {
		e2eharness.Preconditionf(t, "cooperation fixture requires one group quest and 1–4 companions")
	}
	members := append([]actorFixture{f.Target}, f.Companions...)
	authDB, _ := e2eharness.OpenTestDBs(t)
	seen := map[uint64]bool{}
	previous := map[uint64]objectiveBotView{}
	baseline := map[uint64]uint32{}
	run := ""
	for _, member := range members {
		if seen[member.GUID] || member.GUID == 0 || member.GUID > 0xffffffff || member.GUID == f.Actor.GUID ||
			!regexp.MustCompile(`^RNDBOT[0-9]+$`).MatchString(member.Account) ||
			!regexp.MustCompile(`^[A-Za-z]{2,12}$`).MatchString(member.Name) {
			e2eharness.Preconditionf(t, "distinct exact disposable bot identities required")
		}
		seen[member.GUID] = true
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		var account, actualAccount uint32
		var name string
		var online, groups int
		err := authDB.QueryRowContext(ctx, "SELECT id FROM account WHERE username=?", member.Account).Scan(&account)
		if err == nil {
			err = guest.CharDB.QueryRowContext(ctx, "SELECT account,name,online FROM characters WHERE guid=?", member.GUID).
				Scan(&actualAccount, &name, &online)
		}
		if err == nil {
			err = guest.CharDB.QueryRowContext(ctx, "SELECT COUNT(*) FROM group_member WHERE memberGuid=?", member.GUID).
				Scan(&groups)
		}
		cancel()
		if err != nil || account != actualAccount || name != member.Name || online != 1 || groups != 0 {
			e2eharness.Preconditionf(t, "exact ungrouped live companion %d required: %v", member.GUID, err)
		}
		viewFixture := f
		viewFixture.Target = member
		currentRun, view := readObjectiveBot(t, viewFixture)
		if run != "" && run != currentRun {
			e2eharness.Preconditionf(t, "cohort must share one live run")
		}
		run = currentRun
		previous[member.GUID] = view
		for _, objective := range view.Planning.Objectives {
			if objective.Quest == f.Quest {
				baseline[member.GUID] = objective.GainedCredit
				if objective.State == "completed" || objective.Cooperation.State != "none" {
					e2eharness.Preconditionf(t, "cohort must precede recruitment and quest completion")
				}
			}
		}
	}
	leaderStart := previous[f.Target.GUID]
	separated := false
	for _, member := range f.Companions {
		view := previous[member.GUID]
		separated = separated || view.Map == leaderStart.Map &&
			e2eharness.Distance3D(view.X, view.Y, view.Z, leaderStart.X, leaderStart.Y, leaderStart.Z) >= 100
	}
	if !separated {
		e2eharness.Preconditionf(t, "at least one companion must need a real rendezvous of 100 yards or more")
	}
	groupObserved, visibleGathering := false, false
	if !eventually(12*time.Minute, func() bool {
		allWorking, allCompleted := true, true
		for _, member := range members {
			viewFixture := f
			viewFixture.Target = member
			currentRun, view := readObjectiveBot(t, viewFixture)
			if currentRun != run {
				e2eharness.HarnessFailf(t, "world restarted during cooperation")
			}
			old := previous[member.GUID]
			if view.sampleMs > old.sampleMs {
				elapsed := float64(view.sampleMs-old.sampleMs) / 1000
				distance := e2eharness.Distance3D(view.X, view.Y, view.Z, old.X, old.Y, old.Z)
				if view.Map != old.Map || float64(distance) > 35*elapsed+15 {
					e2eharness.Assertf(t, "cooperative actor %d jumped map or exceeded ordinary movement speed", member.GUID)
				}
				previous[member.GUID] = view
			}
			working, completed := false, false
			for _, objective := range view.Planning.Objectives {
				if objective.Quest != f.Quest || objective.Cooperation.Leader != f.Target.GUID {
					continue
				}
				working = objective.Cooperation.State == "working" && objective.Cooperation.Agreements == uint32(len(members))
				completed = objective.State == "completed" && objective.Cooperation.State == "completed" &&
					objective.GainedCredit > baseline[member.GUID]
			}
			allWorking = allWorking && working
			allCompleted = allCompleted && completed
		}
		leader := previous[f.Target.GUID]
		watchObjectiveBot(t, guest, leader)
		if allWorking && !groupObserved {
			commands(t, guest, ownerPrefix(f.Target.GUID), ".saveall", fmt.Sprintf(".alles status player %d", f.Target.GUID))
			var groupID uint64
			for _, member := range members {
				ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
				var actualGroup, actualLeader uint64
				var groupType int
				err := guest.CharDB.QueryRowContext(ctx,
					"SELECT g.guid,g.leaderGuid,g.groupType FROM groups g JOIN group_member m ON m.guid=g.guid WHERE m.memberGuid=?",
					member.GUID).Scan(&actualGroup, &actualLeader, &groupType)
				cancel()
				if err != nil || actualLeader != f.Target.GUID || groupType != 0 || (groupID != 0 && groupID != actualGroup) {
					e2eharness.Assertf(t, "cooperative claim lacks one actual ordinary party for %d: %v", member.GUID, err)
				}
				groupID = actualGroup
				guest.WaitUnitGUID(t, member.GUID, 10*time.Second)
			}
			groupObserved = true
		}
		if groupObserved {
			allVisible := true
			for _, member := range members {
				object := guest.World.GetObject(member.GUID)
				if object == nil || !object.HasKnownPosition() {
					allVisible = false
					break
				}
			}
			visibleGathering = visibleGathering || allVisible
		}
		return allCompleted
	}) {
		e2eharness.Assertf(t, "cooperation did not earn new credit and complete every participant's intention")
	}
	if !groupObserved || !visibleGathering {
		e2eharness.Assertf(t, "completion lacks actual membership and client-visible gathering")
	}
	for _, member := range members {
		viewFixture := f
		viewFixture.Target = member
		if !savedQuestPresent(t, guest, viewFixture, f.Quest, true) {
			e2eharness.Assertf(t, "participant %d has no saved reward for quest %d", member.GUID, f.Quest)
		}
	}
	t.Logf("PASS cooperative quest=%d members=%d; ordinary gathering, actual party and every saved reward",
		f.Quest, len(members))
}

//go:build e2e

package alles_test

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"regexp"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

type objectiveFixture struct {
	Version            int
	Disposable         bool
	ClientTakeover     bool
	Actor              actorFixture
	Target             actorFixture
	Telemetry          string
	Origin             uint32
	Destination        uint32
	Quest              uint32
	Repairer           uint64
	RepairItem         uint32
	RecallRepair       bool
	SupplyItem         uint32
	FundingItem        uint32
	FundingQuest       uint32
	SupplyVendor       uint32
	Companions         []actorFixture
	Brain              bool
	TravelStarted      bool
	CombatInterruption bool
}

type objectiveView struct {
	ID, ArrivedMs                 uint64
	Quest, Place, DiscoveredQuest uint32
	GainedCredit                  uint32
	Attempts                      uint32
	State, Step, Obstruction      string
	Preparation                   *struct {
		State, Capability      string
		Attempts, Transactions uint32
		SpentMoney             uint64
		EarnedMoney            uint64
	}
	Cooperation struct {
		State      string
		Quest      uint32
		Leader     uint64
		Agreements uint32
	}
}

type objectiveBotView struct {
	sampleMs int64
	GUID     uint64
	Money    uint64
	Name     string
	Map      uint32
	X, Y, Z  float32
	Planning struct {
		Engine     string
		Objectives []objectiveView
		Body       struct {
			Attached                   bool
			Objective                  uint64
			Skill, State, Interruption string
			Route                      struct {
				Advances, Failures uint32
			}
		}
		Survey struct {
			ActiveMs              uint64
			EmptyScans, Positions uint32
		}
	}
}

func objectiveScene(t *testing.T) (objectiveFixture, *e2eharness.ScenarioBot) {
	t.Helper()
	path := os.Getenv("E2E_ALLES_OBJECTIVE_FIXTURE")
	if path == "" {
		t.Skip("requires E2E_ALLES_OBJECTIVE_FIXTURE and an exclusive disposable objective cohort")
	}
	var f objectiveFixture
	data, err := os.ReadFile(path)
	if err != nil || len(data) > 16384 {
		e2eharness.Preconditionf(t, "objective fixture read: %v", err)
	}
	if err := json.Unmarshal(data, &f); err != nil || f.Version != 1 || !f.Disposable ||
		!regexp.MustCompile(`^ALLESE2E[A-Z0-9]{1,9}$`).MatchString(f.Actor.Account) ||
		!regexp.MustCompile(`^RNDBOT[0-9]+$`).MatchString(f.Target.Account) ||
		!regexp.MustCompile(`^[A-Za-z]{2,12}$`).MatchString(f.Target.Name) ||
		f.Target.GUID == 0 || f.Target.GUID > 0xffffffff || f.Target.GUID == f.Actor.GUID || f.Telemetry == "" {
		e2eharness.Preconditionf(t, "invalid objective fixture: %v", err)
	}
	authDB, charDB := e2eharness.OpenTestDBs(t)
	checkFixtureIdentity(t, authDB, charDB, &f.Actor)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var account, actualAccount uint32
	var name string
	var online int
	err = authDB.QueryRowContext(ctx, "SELECT id FROM account WHERE username=?", f.Target.Account).Scan(&account)
	if err == nil {
		err = charDB.QueryRowContext(ctx, "SELECT account,name,online FROM characters WHERE guid=?", f.Target.GUID).
			Scan(&actualAccount, &name, &online)
	}
	if err != nil || account != actualAccount || name != f.Target.Name || online != 1 {
		e2eharness.Preconditionf(t, "exact live objective bot mismatch: %v", err)
	}
	guest := loginFixture(t, authDB, charDB, f.Actor)
	commands(t, guest, ownerPrefix(f.Target.GUID), ".gm on", ".gm visible off",
		fmt.Sprintf(".alles status player %d", f.Target.GUID))
	return f, guest
}

func readObjectiveBot(t *testing.T, f objectiveFixture) (string, objectiveBotView) {
	t.Helper()
	var snapshot struct {
		Run             string
		PublishedUnixMs int64
		Bots            []objectiveBotView
	}
	data, err := os.ReadFile(f.Telemetry)
	if err != nil || len(data) > 8*1024*1024 || json.Unmarshal(data, &snapshot) != nil ||
		math.Abs(float64(time.Now().UnixMilli()-snapshot.PublishedUnixMs)) > 3000 {
		e2eharness.HarnessFailf(t, "fresh objective telemetry required: %v", err)
	}
	for _, bot := range snapshot.Bots {
		if bot.GUID == f.Target.GUID && bot.Name == f.Target.Name {
			bot.sampleMs = snapshot.PublishedUnixMs
			return snapshot.Run, bot
		}
	}
	e2eharness.HarnessFailf(t, "exact objective bot disappeared from telemetry")
	return "", objectiveBotView{}
}

func watchObjectiveBot(t *testing.T, guest *e2eharness.ScenarioBot, bot objectiveBotView) {
	t.Helper()
	x, y, z, _, worldMap := guest.World.Position()
	distance := e2eharness.Distance3D(x, y, z, bot.X, bot.Y, bot.Z)
	if uint32(worldMap) == bot.Map && distance > 35 && distance <= 100 && guest.World.GetObject(bot.GUID) != nil {
		// The pinned client's near-teleport pruning uses cached spline origins. Repeated short teleports can
		// prune a moving player still known to the server, which then has no reason to resend its create block.
		// Follow an already visible target with ordinary movement instead; only the observer moves here.
		orientation := float32(math.Atan2(float64(bot.Y-y), float64(bot.X-x)))
		if err := guest.World.MoveForwardAt(x, y, z, orientation); err != nil {
			e2eharness.HarnessFailf(t, "observer follow start: %v", err)
		}
		defer guest.World.MoveStop()
		start := time.Now()
		tick := time.NewTicker(200 * time.Millisecond)
		defer tick.Stop()
		for at := range tick.C {
			travel := float32(at.Sub(start).Seconds()) * 7
			fraction := min(travel/distance, 1)
			if err := guest.World.SendMovementHeartbeatAt(x+(bot.X-x)*fraction, y+(bot.Y-y)*fraction,
				z+(bot.Z-z)*fraction, orientation); err != nil {
				e2eharness.HarnessFailf(t, "observer follow movement: %v", err)
			}
			if fraction == 1 {
				return
			}
		}
	}
	if uint32(worldMap) != bot.Map || distance > 35 {
		guest.Teleport(t, bot.X+2, bot.Y, bot.Z, bot.Map)
		guest.WaitUnitGUID(t, bot.GUID, 10*time.Second)
	}
}

func savedQuestPresent(t *testing.T, guest *e2eharness.ScenarioBot, f objectiveFixture,
	quest uint32, rewarded bool) bool {
	t.Helper()
	commands(t, guest, ownerPrefix(f.Target.GUID), ".saveall",
		fmt.Sprintf(".alles status player %d", f.Target.GUID))
	return eventually(5*time.Second, func() bool {
		ctx, cancel := context.WithTimeout(context.Background(), time.Second)
		defer cancel()
		var count int
		query := `SELECT COUNT(*) FROM character_queststatus_rewarded WHERE guid=? AND quest=?`
		args := []any{f.Target.GUID, quest}
		if !rewarded {
			query = `SELECT
			 (SELECT COUNT(*) FROM character_queststatus WHERE guid=? AND quest=?) +
			 (SELECT COUNT(*) FROM character_queststatus_rewarded WHERE guid=? AND quest=?)`
			args = append(args, f.Target.GUID, quest)
		}
		if err := guest.CharDB.QueryRowContext(ctx, query, args...).Scan(&count); err != nil && err != sql.ErrNoRows {
			e2eharness.HarnessFailf(t, "saved quest oracle: %v", err)
		}
		return count > 0
	})
}

// Alles objectives: leaving a searched area must lead to real arrival and a newly accepted quest.
// Only the disposable observer teleports. The autonomous target executes all travel and interaction itself.
func TestAlles_ExplorationAcquiresWork(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"alles", "serial", "long"}, Runtime: "long", Category: "alles"})
	f, guest := objectiveScene(t)
	run, start := readObjectiveBot(t, f)
	var local objectiveView
	var intention objectiveView
	for _, objective := range start.Planning.Objectives {
		if objective.Place == f.Origin && objective.Quest == 0 && objective.State == "active" {
			local = objective
		}
		if f.TravelStarted && objective.Place == f.Destination && objective.Quest == 0 &&
			objective.State == "active" && objective.ArrivedMs == 0 {
			intention = objective
		}
	}
	if f.Origin == 0 || f.Destination == 0 || f.Origin == f.Destination ||
		(!f.TravelStarted && local.ID == 0) || (f.TravelStarted && intention.ID == 0) ||
		start.Planning.Engine != "new_rpg" {
		e2eharness.Preconditionf(t, "fixture must start local investigation or its explicitly configured active journey")
	}
	if (f.Brain && !start.Planning.Body.Attached) || (f.CombatInterruption && !f.Brain) {
		e2eharness.Preconditionf(t, "brain fixture requires an attached body; combat interruption requires brain mode")
	}
	watchObjectiveBot(t, guest, start)
	visibleTravel := false
	searched := f.TravelStarted
	interrupted, resumed, routeAdvanced := false, false, false
	previous := start
	var acquired uint32
	if !eventually(10*time.Minute, func() bool {
		currentRun, bot := readObjectiveBot(t, f)
		if currentRun != run {
			e2eharness.HarnessFailf(t, "world restarted during exploration")
		}
		if f.Brain && !bot.Planning.Body.Attached {
			e2eharness.Assertf(t, "Alles lost body ownership during autonomous exploration")
		}
		body := bot.Planning.Body
		if intention.ID != 0 && body.Objective == intention.ID {
			if body.Interruption == "combat" {
				interrupted = true
			}
			if interrupted && body.Skill == "travel" && body.State == "running" && body.Interruption == "none" {
				resumed = true
			}
			routeAdvanced = routeAdvanced || body.Route.Advances > 0
		}
		if bot.sampleMs > previous.sampleMs {
			elapsed := float64(bot.sampleMs-previous.sampleMs) / 1000
			if bot.Map != previous.Map || float64(e2eharness.Distance3D(bot.X, bot.Y, bot.Z,
				previous.X, previous.Y, previous.Z)) > 35*elapsed+15 {
				e2eharness.Assertf(t, "exploration used a map jump or movement exceeding ordinary travel speed")
			}
			previous = bot
		}
		watchObjectiveBot(t, guest, bot)
		if object := guest.World.GetObject(bot.GUID); object != nil && object.HasKnownPosition() {
			x, y, z := object.InterpolatedPosition()
			visibleTravel = visibleTravel || e2eharness.Distance3D(start.X, start.Y, start.Z, x, y, z) >= 50
		}
		for _, objective := range bot.Planning.Objectives {
			if objective.ID == local.ID && objective.State == "deferred" &&
				bot.Planning.Survey.ActiveMs >= 120000 && bot.Planning.Survey.EmptyScans >= 3 &&
				bot.Planning.Survey.Positions >= 3 {
				searched = true
			}
			if searched && intention.ID == 0 && objective.Place == f.Destination && objective.Quest == 0 &&
				objective.State == "active" && objective.ArrivedMs == 0 {
				intention = objective
			}
			if objective.ID == intention.ID && objective.State == "completed" && objective.ArrivedMs != 0 {
				acquired = objective.DiscoveredQuest
				return acquired != 0
			}
		}
		return false
	}) {
		e2eharness.Assertf(t, "exploration did not arrive and acquire work in the configured area")
	}
	if !visibleTravel || !savedQuestPresent(t, guest, f, acquired, false) {
		e2eharness.Assertf(t, "exploration claim lacks client-visible travel or an actually saved new quest")
	}
	if f.Brain && !routeAdvanced {
		e2eharness.Assertf(t, "body never measured route advancement for the committed journey")
	}
	if f.CombatInterruption && (!interrupted || !resumed) {
		e2eharness.Assertf(t, "fixture did not observe combat interruption and resumption of the same travel intention")
	}
	t.Logf("PASS bot=%d place=%d objective=%d acquiredQuest=%d", f.Target.GUID, f.Destination, intention.ID, acquired)
}

// Alles objectives: counter progress and a completed intention must be backed by a saved quest reward.
func TestAlles_ObjectiveEarnsQuestReward(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"alles", "serial", "long"}, Runtime: "long", Category: "alles"})
	f, guest := objectiveScene(t)
	run, start := readObjectiveBot(t, f)
	quest := f.Quest
	var blocked objectiveView
	var requiredMoney uint64
	if f.FundingQuest != 0 {
		quest = f.FundingQuest
		for _, objective := range start.Planning.Objectives {
			if objective.Quest == f.Quest && objective.State == "deferred" && objective.Obstruction == "supplies" {
				blocked = objective
			}
		}
		if f.Quest == quest || blocked.ID == 0 || start.Planning.Engine != "earning_quest_money" {
			e2eharness.Preconditionf(t, "funding fixture needs an earning quest and a distinct retained unpaid turn-in")
		}
		worldDB, err := e2eharness.OpenWorldDB()
		if err != nil {
			e2eharness.Preconditionf(t, "quest template connection: %v", err)
		}
		defer worldDB.Close()
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		var cost, reward int64
		err = worldDB.QueryRowContext(ctx, "SELECT RewardMoney FROM quest_template WHERE ID=?", f.Quest).Scan(&cost)
		if err == nil {
			err = worldDB.QueryRowContext(ctx, "SELECT RewardMoney FROM quest_template WHERE ID=?", quest).Scan(&reward)
		}
		cancel()
		if err != nil || cost >= 0 || reward <= 0 || start.Money >= uint64(-cost) {
			e2eharness.Preconditionf(t, "funding fixture requires an unaffordable turn-in and a genuinely paying source: %v", err)
		}
		requiredMoney = uint64(-cost)
		flushActor(t, guest, f.Target.GUID)
		retained := readSavedQuestIntention(t, guest, f.Target.GUID, blocked.ID)
		if !retained.Checkpoint.InLog || retained.Checkpoint.Rewarded || retained.Checkpoint.Failed {
			e2eharness.Preconditionf(t, "unpaid parent quest must remain accepted and unrewarded")
		}
	}
	var intention objectiveView
	for _, objective := range start.Planning.Objectives {
		if objective.Quest == quest && objective.State == "active" {
			intention = objective
		}
	}
	if quest == 0 || intention.ID == 0 || (start.Planning.Engine != "new_rpg" && f.FundingQuest == 0) {
		e2eharness.Preconditionf(t, "fixture must start an active quest requiring further combat/loot credit")
	}
	funded := f.FundingQuest == 0
	if !eventually(10*time.Minute, func() bool {
		currentRun, bot := readObjectiveBot(t, f)
		if currentRun != run {
			e2eharness.HarnessFailf(t, "world restarted during quest execution")
		}
		watchObjectiveBot(t, guest, bot)
		funded = funded || (f.FundingQuest != 0 && bot.Money >= requiredMoney && bot.Money > start.Money)
		for _, objective := range bot.Planning.Objectives {
			if objective.ID == intention.ID && objective.State == "completed" {
				return objective.GainedCredit > intention.GainedCredit
			}
		}
		return false
	}) || !savedQuestPresent(t, guest, f, quest, true) {
		e2eharness.Assertf(t, "quest objective lacks new credit and an authoritative saved reward")
	}
	if f.FundingQuest != 0 {
		if !funded {
			e2eharness.Assertf(t, "paying quest completed without an observed sufficient actual balance for the retained turn-in")
		}
		if !eventually(2*time.Minute, func() bool {
			currentRun, bot := readObjectiveBot(t, f)
			if currentRun != run {
				e2eharness.HarnessFailf(t, "world restarted during the funded turn-in")
			}
			watchObjectiveBot(t, guest, bot)
			for _, objective := range bot.Planning.Objectives {
				if objective.ID == blocked.ID && objective.State == "completed" {
					return true
				}
			}
			return false
		}) || !savedQuestPresent(t, guest, f, f.Quest, true) {
			e2eharness.Assertf(t, "actual earned funds did not lead to the retained turn-in's saved reward")
		}
		t.Logf("PASS paying quest=%d funded retained quest=%d; actual balance covered required money=%d",
			quest, f.Quest, requiredMoney)
	}
	t.Logf("PASS bot=%d quest=%d objective=%d saved reward", f.Target.GUID, quest, intention.ID)
}

// Alles readiness: a quest outside the executor's level range is deferred and retained without repeated attempts.
// GM setup changes only the exact disposable target's level; the planner must discover and act on the obstruction.
func TestAlles_QuestReadinessDefersWithoutAbandoning(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"alles", "serial", "med"}, Runtime: "med", Category: "alles"})
	f, guest := objectiveScene(t)
	run, start := readObjectiveBot(t, f)
	var intention objectiveView
	for _, objective := range start.Planning.Objectives {
		if objective.Quest == f.Quest && objective.State == "active" && objective.Step == "travel" {
			intention = objective
		}
	}
	if f.Quest == 0 || intention.ID == 0 || start.Planning.Engine != "new_rpg" {
		e2eharness.Preconditionf(t, "fixture needs an active incomplete solo quest while traveling safely")
	}
	worldDB, err := e2eharness.OpenWorldDB()
	if err != nil {
		e2eharness.Preconditionf(t, "quest definition connection: %v", err)
	}
	defer worldDB.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var questLevel, questType, suggestedPlayers int
	err = worldDB.QueryRowContext(ctx, "SELECT QuestLevel,QuestInfoID,SuggestedGroupNum FROM quest_template WHERE ID=?",
		f.Quest).Scan(&questLevel, &questType, &suggestedPlayers)
	if err != nil || questLevel < 5 || questType != 0 || suggestedPlayers > 1 {
		e2eharness.Preconditionf(t, "fixture needs a normal solo quest at level >=5: %v", err)
	}
	watchObjectiveBot(t, guest, start)
	guest.WaitUnitGUID(t, f.Target.GUID, 10*time.Second)
	unit := guest.World.GetObject(f.Target.GUID)
	if unit == nil {
		e2eharness.Preconditionf(t, "target left visibility before level setup")
	}
	originalLevel := unit.Level()
	if originalLevel <= 1 || int(originalLevel)+3 < questLevel || e2eharness.UnitInCombat(guest.World, f.Target.GUID) {
		e2eharness.Preconditionf(t, "target must begin out of combat within the quest executor's supported level range")
	}
	setLevel := func(level uint32) {
		commands(t, guest, ownerPrefix(f.Target.GUID), fmt.Sprintf(".character level %s %d", f.Target.Name, level),
			fmt.Sprintf(".alles status player %d", f.Target.GUID))
		if !eventually(5*time.Second, func() bool {
			_, bot := readObjectiveBot(t, f)
			watchObjectiveBot(t, guest, bot)
			object := guest.World.GetObject(f.Target.GUID)
			return object != nil && object.Level() == level
		}) {
			e2eharness.Preconditionf(t, "disposable target level %d was not observed by the client", level)
		}
	}
	t.Cleanup(func() {
		setLevel(originalLevel)
		commands(t, guest, ownerPrefix(f.Target.GUID), ".saveall",
			fmt.Sprintf(".alles status player %d", f.Target.GUID))
	})
	setLevel(1)
	var deferredAt int64
	var alternative bool
	if !eventually(45*time.Second, func() bool {
		currentRun, bot := readObjectiveBot(t, f)
		if currentRun != run {
			e2eharness.HarnessFailf(t, "world restarted during quest deferral")
		}
		watchObjectiveBot(t, guest, bot)
		found := false
		for _, objective := range bot.Planning.Objectives {
			if objective.ID != intention.ID {
				alternative = alternative || objective.State == "active"
				continue
			}
			found = true
			if deferredAt == 0 {
				if objective.State == "deferred" && objective.Obstruction == "strength" &&
					objective.Attempts == intention.Attempts {
					deferredAt = bot.sampleMs
				}
			} else if objective.State != "deferred" || objective.Obstruction != "strength" ||
				objective.Attempts != intention.Attempts {
				e2eharness.Assertf(t, "unresolved strength obstruction retried or lost its deferred intention: %+v", objective)
			}
		}
		if !found {
			e2eharness.Assertf(t, "blocked quest disappeared from the objective book")
		}
		return deferredAt != 0 && bot.sampleMs-deferredAt >= 30000 && alternative
	}) || !savedQuestPresent(t, guest, f, f.Quest, false) {
		e2eharness.Assertf(t, "quest was not retained with strength deferral while another intention became active")
	}
	ctx, cancel = context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var retained, rewarded int
	err = guest.CharDB.QueryRowContext(ctx, `SELECT
	 (SELECT COUNT(*) FROM character_queststatus WHERE guid=? AND quest=?),
	 (SELECT COUNT(*) FROM character_queststatus_rewarded WHERE guid=? AND quest=?)`,
		f.Target.GUID, f.Quest, f.Target.GUID, f.Quest).Scan(&retained, &rewarded)
	if err != nil || retained != 1 || rewarded != 0 {
		e2eharness.Assertf(t, "deferred quest needs saved log retention without a reward: retained=%d rewarded=%d err=%v",
			retained, rewarded, err)
	}
	t.Logf("PASS bot=%d quest=%d objective=%d deferred and retained without another attempt", f.Target.GUID,
		f.Quest, intention.ID)
}

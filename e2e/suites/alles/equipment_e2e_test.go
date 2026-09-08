//go:build e2e

package alles_test

import (
	"context"
	"encoding/json"
	"fmt"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// Alles preparation: ordinary approach and repair must improve the same equipped item using real money.
// The fixture supplies damaged disposable gear; the test never grants durability, money or quest credit.
func TestAlles_EquipmentPreparationPaysForRealRepair(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"alles", "serial", "med"}, Runtime: "med", Category: "alles"})
	f, guest := objectiveScene(t)
	run, start := readObjectiveBot(t, f)
	var intention objectiveView
	for _, objective := range start.Planning.Objectives {
		if objective.Quest == f.Quest && objective.Preparation != nil && objective.Preparation.State == "active" {
			intention = objective
		}
	}
	if intention.ID == 0 || f.Repairer == 0 || f.RepairItem == 0 ||
		(start.Planning.Engine != "new_rpg" && start.Planning.Engine != "preparing_equipment" &&
			start.Planning.Engine != "returning_to_known_repairer") {
		e2eharness.Preconditionf(t, "fixture needs active repair preparation, repairer GUID and equipped repairItem GUID")
	}
	watchObjectiveBot(t, guest, start)
	if f.RecallRepair {
		flushActor(t, guest, f.Target.GUID)
		var saved struct {
			Places []struct {
				Repair *struct {
					Map        uint32
					X, Y, Z    float32
					ObservedMs uint64
				}
			}
		}
		if err := json.Unmarshal(readSavedPlanningPayload(t, guest, f.Target.GUID), &saved); err != nil {
			e2eharness.HarnessFailf(t, "read personally remembered repair location: %v", err)
		}
		known := 0
		for _, place := range saved.Places {
			if place.Repair == nil || place.Repair.Map != start.Map {
				continue
			}
			site := place.Repair
			known++
			gap := e2eharness.Distance3D(start.X, start.Y, start.Z, site.X, site.Y, site.Z)
			if site.ObservedMs == 0 || gap < 90 || gap > 450 {
				e2eharness.Preconditionf(t, "recall fixture needs an existing personal repair location 90-450 yards away")
			}
			guest.Teleport(t, site.X, site.Y, site.Z, site.Map)
		}
		if known != 1 {
			e2eharness.Preconditionf(t, "recall fixture needs exactly one personally known repair location on this map")
		}
	}
	guest.WaitUnitGUID(t, f.Repairer, 5*time.Second)
	repairer := guest.World.GetObject(f.Repairer)
	if repairer == nil || repairer.Value(client.UnitNPCFlags)&0x1000 == 0 || !repairer.HasKnownPosition() {
		e2eharness.Preconditionf(t, "repairer must be a client-visible NPC with UNIT_NPC_FLAG_REPAIR")
	}
	rx, ry, rz := repairer.InterpolatedPosition()
	distance := e2eharness.Distance3D(start.X, start.Y, start.Z, rx, ry, rz)
	if (!f.RecallRepair && (distance < 15 || distance > 60)) || (f.RecallRepair && distance < 75) {
		e2eharness.Preconditionf(t, "fixture must start 15-60 yards from a visible repairer, or beyond 75 for recall")
	}
	if f.RecallRepair {
		x, y, z, _, _ := guest.World.Position()
		if e2eharness.Distance3D(x, y, z, rx, ry, rz) > 10 {
			e2eharness.Preconditionf(t, "fixture repairer must still be beside the bot's personally remembered location")
		}
		watchObjectiveBot(t, guest, start)
	}
	readGear := func(save bool) (uint32, uint32, uint64) {
		if save {
			commands(t, guest, ownerPrefix(f.Target.GUID), ".saveall",
				fmt.Sprintf(".alles status player %d", f.Target.GUID))
		}
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		var entry, durability uint32
		var money uint64
		err := guest.CharDB.QueryRowContext(ctx, `SELECT i.itemEntry,i.durability,c.money
		 FROM character_inventory v JOIN item_instance i ON i.guid=v.item JOIN characters c ON c.guid=v.guid
		 WHERE v.guid=? AND v.item=? AND v.bag=0 AND v.slot<19`, f.Target.GUID, f.RepairItem).
			Scan(&entry, &durability, &money)
		if err != nil {
			e2eharness.HarnessFailf(t, "saved exact equipped item and money: %v", err)
		}
		return entry, durability, money
	}
	entry, beforeDurability, beforeMoney := readGear(true)
	worldDB, err := e2eharness.OpenWorldDB()
	if err != nil {
		e2eharness.Preconditionf(t, "item template connection: %v", err)
	}
	defer worldDB.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var maximum uint32
	err = worldDB.QueryRowContext(ctx, "SELECT MaxDurability FROM item_template WHERE entry=?", entry).Scan(&maximum)
	if err != nil || maximum == 0 || uint64(beforeDurability)*5 > uint64(maximum) || beforeMoney == 0 {
		e2eharness.Preconditionf(t, "fixture needs critical equipped durability and enough own repair money: %v", err)
	}
	var approached bool
	returned := !f.RecallRepair || start.Planning.Engine == "returning_to_known_repairer"
	var spent uint64
	previous := start
	if !eventually(130*time.Second, func() bool {
		currentRun, bot := readObjectiveBot(t, f)
		if currentRun != run {
			e2eharness.HarnessFailf(t, "world restarted during preparation")
		}
		if bot.sampleMs > previous.sampleMs {
			elapsed := float64(bot.sampleMs-previous.sampleMs) / 1000
			if bot.Map != previous.Map || float64(e2eharness.Distance3D(bot.X, bot.Y, bot.Z,
				previous.X, previous.Y, previous.Z)) > 35*elapsed+15 {
				e2eharness.Assertf(t, "equipment preparation exceeded ordinary movement speed")
			}
			previous = bot
		}
		watchObjectiveBot(t, guest, bot)
		returned = returned || bot.Planning.Engine == "returning_to_known_repairer"
		if unit := guest.World.GetObject(f.Target.GUID); unit != nil && unit.HasKnownPosition() {
			x, y, z := unit.InterpolatedPosition()
			approached = approached || e2eharness.Distance3D(x, y, z, rx, ry, rz) <= 7
		}
		for _, objective := range bot.Planning.Objectives {
			if objective.ID == intention.ID && objective.Preparation != nil && objective.Preparation.State == "completed" &&
				objective.Preparation.SpentMoney > intention.Preparation.SpentMoney {
				spent = objective.Preparation.SpentMoney - intention.Preparation.SpentMoney
				return approached && returned && objective.Preparation.Transactions > intention.Preparation.Transactions
			}
		}
		return false
	}) {
		e2eharness.Assertf(t, "no client-observed approach and paid equipment preparation completion")
	}
	afterEntry, afterDurability, afterMoney := readGear(true)
	if !eventually(5*time.Second, func() bool {
		afterEntry, afterDurability, afterMoney = readGear(false)
		return afterEntry == entry && afterDurability > beforeDurability && uint64(afterDurability)*5 > uint64(maximum) &&
			afterMoney < beforeMoney && beforeMoney-afterMoney == spent
	}) {
		e2eharness.Assertf(t, "repair lacks real saved durability and expense: durability=%d->%d money=%d->%d expense=%d",
			beforeDurability, afterDurability, beforeMoney, afterMoney, spent)
	}
	t.Logf("PASS bot=%d objective=%d item=%d repaired with own expense=%d", f.Target.GUID, intention.ID, f.RepairItem, spent)
}

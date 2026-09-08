//go:build e2e

package alles_test

import (
	"context"
	"fmt"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// Alles supplies: a normal received vendor offer must produce actual quest items paid for with own money.
// Freeze only stabilizes disposable setup. The product path starts on unfreeze; no items or money are granted.
func TestAlles_SupplyPreparationBuysRequiredItems(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"alles", "serial", "med"}, Runtime: "med", Category: "alles"})
	f, guest := objectiveScene(t)
	if f.Quest == 0 || f.SupplyItem == 0 || f.SupplyVendor == 0 {
		e2eharness.Preconditionf(t, "fixture requires quest, supplyItem entry and a compatible supplyVendor entry")
	}
	run, start := readObjectiveBot(t, f)
	watchObjectiveBot(t, guest, start)
	setFrozen := func(frozen bool) {
		if err := guest.World.SetTarget(f.Target.GUID); err != nil {
			e2eharness.HarnessFailf(t, "select exact disposable target: %v", err)
		}
		command := ".unfreeze "
		if frozen {
			command = ".freeze "
		}
		commands(t, guest, ownerPrefix(f.Target.GUID), command+f.Target.Name,
			fmt.Sprintf(".alles status player %d", f.Target.GUID))
		if !eventually(5*time.Second, func() bool { return guest.UnitHasAura(f.Target.GUID, 9454) == frozen }) {
			e2eharness.Preconditionf(t, "freeze setup state was not observed by the client")
		}
	}
	t.Cleanup(func() { setFrozen(false) })
	setFrozen(true)
	frozenAt := time.Now().UnixMilli()
	if !eventually(5*time.Second, func() bool {
		_, start = readObjectiveBot(t, f)
		return start.sampleMs >= frozenAt
	}) {
		e2eharness.HarnessFailf(t, "no fresh frozen position")
	}
	var intention objectiveView
	for _, objective := range start.Planning.Objectives {
		if objective.Quest == f.Quest && objective.State == "active" {
			intention = objective
		}
	}
	if intention.ID == 0 || start.Planning.Engine != "new_rpg" || (start.Money == 0 && f.FundingItem == 0) {
		e2eharness.Preconditionf(t, "fixture needs an active incomplete quest, healthy equipment and funds or expendable junk")
	}
	guest.Teleport(t, start.X, start.Y, start.Z, start.Map)
	guest.WaitUnitGUID(t, f.Target.GUID, 5*time.Second)
	worldDB, err := e2eharness.OpenWorldDB()
	if err != nil {
		e2eharness.Preconditionf(t, "world fixture connection: %v", err)
	}
	defer worldDB.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var required, offers int
	err = worldDB.QueryRowContext(ctx, `SELECT
	 CASE WHEN RequiredItemId1=? THEN RequiredItemCount1 ELSE 0 END +
	 CASE WHEN RequiredItemId2=? THEN RequiredItemCount2 ELSE 0 END +
	 CASE WHEN RequiredItemId3=? THEN RequiredItemCount3 ELSE 0 END +
	 CASE WHEN RequiredItemId4=? THEN RequiredItemCount4 ELSE 0 END +
	 CASE WHEN RequiredItemId5=? THEN RequiredItemCount5 ELSE 0 END +
	 CASE WHEN RequiredItemId6=? THEN RequiredItemCount6 ELSE 0 END FROM quest_template WHERE ID=?`,
		f.SupplyItem, f.SupplyItem, f.SupplyItem, f.SupplyItem, f.SupplyItem, f.SupplyItem, f.Quest).Scan(&required)
	if err == nil {
		err = worldDB.QueryRowContext(ctx, `SELECT COUNT(*) FROM npc_vendor v JOIN item_template i ON i.entry=v.item
		 WHERE v.entry=? AND v.item=? AND v.ExtendedCost=0 AND i.BuyPrice>0`, f.SupplyVendor, f.SupplyItem).Scan(&offers)
	}
	if err != nil || required == 0 || offers == 0 {
		e2eharness.Preconditionf(t, "fixture lacks a required quest item with an ordinary paid vendor offer: %v", err)
	}
	readInventory := func() (int, uint64) {
		ctx, cancel := context.WithTimeout(context.Background(), time.Second)
		defer cancel()
		var count int
		var money uint64
		err := guest.CharDB.QueryRowContext(ctx, `SELECT COALESCE((SELECT SUM(i.count)
		 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid=c.guid AND i.itemEntry=?),0),
		 c.money FROM characters c WHERE c.guid=?`, f.SupplyItem, f.Target.GUID).Scan(&count, &money)
		if err != nil {
			e2eharness.HarnessFailf(t, "saved own inventory and currency: %v", err)
		}
		return count, money
	}
	commands(t, guest, ownerPrefix(f.Target.GUID), ".saveall",
		fmt.Sprintf(".alles status player %d", f.Target.GUID))
	if !eventually(5*time.Second, func() bool {
		count, money := readInventory()
		return count == 0 && money == start.Money
	}) {
		e2eharness.Preconditionf(t, "fixture needs zero owned copies and an acknowledged saved currency baseline")
	}
	readFundingItem := func() (uint32, uint32) {
		ctx, cancel := context.WithTimeout(context.Background(), time.Second)
		defer cancel()
		var entry, count uint32
		err := guest.CharDB.QueryRowContext(ctx, `SELECT COALESCE(MAX(i.itemEntry),0), COALESCE(SUM(i.count),0)
		 FROM character_inventory v JOIN item_instance i ON i.guid=v.item
		 WHERE v.guid=? AND i.guid=? AND ((v.bag=0 AND v.slot>=23 AND v.slot<39) OR v.bag IN
		 (SELECT item FROM character_inventory WHERE guid=? AND bag=0 AND slot>=19 AND slot<23))`,
			f.Target.GUID, f.FundingItem, f.Target.GUID).Scan(&entry, &count)
		if err != nil {
			e2eharness.HarnessFailf(t, "saved funding stack: %v", err)
		}
		return entry, count
	}
	var fundingCount uint32
	if f.FundingItem != 0 {
		entry, count := readFundingItem()
		fundingCount = count
		var eligible int
		err := worldDB.QueryRowContext(ctx, `SELECT COUNT(*) FROM item_template WHERE entry=? AND Quality=0
		 AND class=15 AND InventoryType=0 AND startquest=0 AND SellPrice>0 AND MaxDurability=0
		 AND spellid_1=0 AND spellid_2=0 AND spellid_3=0 AND spellid_4=0 AND spellid_5=0`, entry).Scan(&eligible)
		if err != nil || eligible != 1 || count == 0 || start.Money != 0 || entry == f.SupplyItem {
			e2eharness.Preconditionf(t, "funded fixture needs zero money and an owned expendable junk stack: %v", err)
		}
	}
	// Spawn's harness cleanup first removes same-entry neighbours: refuse unless there are none to preserve.
	spawns, err := e2eharness.ListCreatureSpawnsNear(worldDB, f.SupplyVendor, start.Map, start.X, start.Y, 110)
	if err != nil || len(spawns) != 0 || len(guest.UnitsByEntry(110, f.SupplyVendor)) != 0 {
		e2eharness.Preconditionf(t, "supply vendor entry must have no existing nearby spawns: %v", err)
	}
	guest.Spawn(t, f.SupplyVendor, 10*time.Second) // Registers cleanup for the exact fixture spawn.
	setFrozen(false)
	var spent, earned uint64
	if !eventually(45*time.Second, func() bool {
		currentRun, bot := readObjectiveBot(t, f)
		if currentRun != run {
			e2eharness.HarnessFailf(t, "world restarted during supply purchase")
		}
		for _, objective := range bot.Planning.Objectives {
			if objective.ID == intention.ID && objective.Preparation != nil &&
				objective.Preparation.Capability == "buy_quest_supplies" && objective.Preparation.State == "completed" {
				spent = objective.Preparation.SpentMoney
				earned = objective.Preparation.EarnedMoney
				return spent > 0 && (f.FundingItem == 0 || earned > 0) &&
					objective.Preparation.Transactions > 0 && objective.GainedCredit > intention.GainedCredit
			}
		}
		return false
	}) {
		e2eharness.Assertf(t, "no paid supply preparation with actual new quest credit")
	}
	commands(t, guest, ownerPrefix(f.Target.GUID), ".saveall",
		fmt.Sprintf(".alles status player %d", f.Target.GUID))
	if !eventually(5*time.Second, func() bool {
		count, money := readInventory()
		if f.FundingItem != 0 {
			_, remaining := readFundingItem()
			if remaining >= fundingCount {
				return false
			}
		}
		return count >= required && start.Money+earned >= spent && start.Money+earned-spent == money
	}) {
		e2eharness.Assertf(t, "supply purchase lacks saved required items and matching real own-money expense")
	}
	t.Logf("PASS bot=%d quest=%d item=%d acquired with income=%d expense=%d",
		f.Target.GUID, f.Quest, f.SupplyItem, earned, spent)
}

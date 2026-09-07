//go:build e2e

package alles_test

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"regexp"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// Regression: ordinary local chat from a privileged account uses SMSG_GM_MESSAGECHAT;
// an online human outside Alles.Owners must still count as an audience for autonomous speech.
func TestAlles_OnlineHumanAudience(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"med", "alles", "serial"}, Runtime: "med", Category: "alles"})
	path := os.Getenv("E2E_ALLES_AUDIENCE_FIXTURE")
	if path == "" {
		t.Skip("requires E2E_ALLES_AUDIENCE_FIXTURE with a disposable unmanaged human and isolated live bot")
	}
	var fixture struct {
		Version    int
		Disposable bool
		Actor      actorFixture
		Target     struct {
			Name string
			GUID uint64
		}
		Telemetry   string
		OtherOwners []uint64
	}
	b, err := os.ReadFile(path)
	if err != nil || len(b) > 16384 {
		e2eharness.Preconditionf(t, "audience fixture: %v", err)
	}
	if err := json.Unmarshal(b, &fixture); err != nil || fixture.Version != 1 || !fixture.Disposable ||
		fixture.Actor.Role != "audience" || fixture.Actor.GUID == 0 || fixture.Target.GUID == 0 ||
		fixture.Actor.GUID == fixture.Target.GUID || len(fixture.OtherOwners) == 0 ||
		!regexp.MustCompile(`^ALLESE2E[A-Z0-9]{1,9}$`).MatchString(fixture.Actor.Account) ||
		!regexp.MustCompile(`^[A-Za-z]{2,12}$`).MatchString(fixture.Target.Name) {
		e2eharness.Preconditionf(t, "invalid audience fixture: %v", err)
	}
	authDB, charDB := e2eharness.OpenTestDBs(t)
	checkFixtureIdentity(t, authDB, charDB, &fixture.Actor)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	var privileged, overrides int
	err = authDB.QueryRowContext(ctx,
		`SELECT (SELECT COUNT(*) FROM account_access WHERE id=? AND gmlevel=3 AND RealmID=-1),
		 (SELECT COUNT(*) FROM rbac_account_permissions WHERE accountId=? AND permissionId=372)`,
		fixture.Actor.accountID, fixture.Actor.accountID).Scan(&privileged, &overrides)
	cancel()
	if err != nil || privileged != 1 || overrides != 0 {
		e2eharness.Preconditionf(t, "audience requires default GM chat permission: %v", err)
	}
	guest := loginFixture(t, authDB, charDB, fixture.Actor)
	target := fixture.Target.GUID
	commands(t, guest, ownerPrefix(target), ".gm off", ".gm visible on", ".gm chat off",
		fmt.Sprintf(".appear %s", fixture.Target.Name), fmt.Sprintf(".alles status player %d", target))
	if field(status(t, guest, target), "state") != "ready" {
		e2eharness.Preconditionf(t, "target owner is not ready")
	}
	// Telemetry supplies current world positions, not lagging characters-table coordinates.
	// Require every other configured owner to be >60 yards away or confirmed offline.
	isolated := func() bool {
		var snapshot struct {
			PublishedUnixMs int64
			Bots            []struct {
				GUID    uint64
				Map     uint32
				X, Y, Z float64
			}
		}
		b, err := os.ReadFile(fixture.Telemetry)
		if err != nil || json.Unmarshal(b, &snapshot) != nil ||
			math.Abs(float64(time.Now().UnixMilli()-snapshot.PublishedUnixMs)) > 3000 {
			return false
		}
		idx := -1
		for i, bot := range snapshot.Bots {
			if bot.GUID == target {
				idx = i
			}
		}
		if idx < 0 {
			return false
		}
		p := snapshot.Bots[idx]
		for _, guid := range fixture.OtherOwners {
			found := false
			for _, q := range snapshot.Bots {
				if q.GUID != guid {
					continue
				}
				found = true
				if q.Map == p.Map && math.Sqrt(math.Pow(p.X-q.X, 2)+math.Pow(p.Y-q.Y, 2)+math.Pow(p.Z-q.Z, 2)) <= 60 {
					return false
				}
			}
			if !found {
				ctx, cancel := context.WithTimeout(context.Background(), time.Second)
				var online int
				err := charDB.QueryRowContext(ctx, "SELECT online FROM characters WHERE guid=?", guid).Scan(&online)
				cancel()
				if err != nil || online != 0 {
					return false
				}
			}
		}
		return true
	}
	if !isolated() {
		e2eharness.Preconditionf(t, "target must have no nearby configured audience")
	}
	seen, _, stop := watchSay(guest.World, target, "")
	defer stop()
	marker := fmt.Sprintf("HELP audience %d", time.Now().UnixNano())
	for _, channel := range []uint32{client.ChatMsgSay, client.ChatMsgYell} {
		text := fmt.Sprintf("%s channel %d", marker, channel)
		if err := guest.World.SendChatMessage(channel, client.LangCommon, text); err != nil {
			e2eharness.HarnessFailf(t, "send local chat: %v", err)
		}
	}
	// Fence input and persistence with the target owner's explicit module watermark.
	lines := commands(t, guest, ownerPrefix(target), fmt.Sprintf(".alles flush player %d", target))
	requested := number(t, lines[len(lines)-1], "requested_revision")
	if !eventually(15*time.Second, func() bool {
		return number(t, status(t, guest, target), "committed_revision") >= requested
	}) {
		e2eharness.Assertf(t, "target did not commit the hearing snapshot")
	}
	ctx, cancel = context.WithTimeout(context.Background(), 5*time.Second)
	var heard, enrolled int
	err = charDB.QueryRowContext(ctx,
		`SELECT (SELECT COUNT(*) FROM alles_perception WHERE owner_kind=0 AND owner_id=?
		 AND source_kind=0 AND source_id=? AND INSTR(gated_text,?)>0) +
		 (SELECT COUNT(*) FROM alles_memory WHERE owner_kind=0 AND owner_id=?
		 AND source_kind=0 AND source_id=? AND INSTR(claim,?)>0)`,
		target, guest.GUID, marker, target, guest.GUID, marker).Scan(&heard)
	if err == nil {
		err = charDB.QueryRowContext(ctx, "SELECT COUNT(*) FROM alles_actor WHERE owner_kind=0 AND owner_id=?",
			guest.GUID).Scan(&enrolled)
	}
	cancel()
	if err != nil {
		e2eharness.HarnessFailf(t, "read acknowledged hearing snapshot: %v", err)
	}
	if heard != 2 || enrolled != 0 {
		e2eharness.Assertf(t, "expected both local messages without enrolling human: heard=%d enrolled=%d", heard, enrolled)
	}
	t.Logf("PASS both privileged local messages persisted; human %d remains unmanaged", guest.GUID)
	nextFollow := time.Now()
	if !eventually(65*time.Second, func() bool {
		select {
		case <-seen:
			if !isolated() {
				e2eharness.Preconditionf(t, "another configured audience approached during speech")
			}
			return true
		default:
		}
		// Autonomous bots keep moving. Move only our disposable observer to stay in hearing range.
		if !time.Now().Before(nextFollow) {
			commands(t, guest, ownerPrefix(target), fmt.Sprintf(".appear %s", fixture.Target.Name),
				fmt.Sprintf(".alles status player %d", target))
			nextFollow = time.Now().Add(3 * time.Second)
		}
		return false
	}) {
		e2eharness.Assertf(t, "unmanaged human received no autonomous bot SAY within two speech intervals")
	}
	t.Logf("PASS privileged SAY/YELL captured by %d; autonomous SAY delivered to unmanaged human %d", target, guest.GUID)
}

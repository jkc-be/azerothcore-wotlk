//go:build e2e

package alles_test

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// Natural player speech must produce contextual model replies and validated temporary movement,
// with no magic chat word or bot-master assignment. This opt-in fixture temporarily summons exact bots.
func TestAlles_NaturalConversationAndFollow(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"med", "alles", "serial"}, Runtime: "med", Category: "alles"})
	path := os.Getenv("E2E_ALLES_CONVERSATION_FIXTURE")
	if path == "" {
		t.Skip("requires E2E_ALLES_CONVERSATION_FIXTURE and an exclusive local bot cohort")
	}
	var f struct {
		Version    int
		Disposable bool
		Actor      actorFixture
		Bots       []actorFixture
		Telemetry  string
	}
	b, err := os.ReadFile(path)
	if err != nil || len(b) > 16384 {
		e2eharness.Preconditionf(t, "fixture read: %v", err)
	}
	if err := json.Unmarshal(b, &f); err != nil || f.Version != 1 || !f.Disposable || len(f.Bots) != 2 ||
		!regexp.MustCompile(`^ALLESE2E[A-Z0-9]{1,9}$`).MatchString(f.Actor.Account) {
		e2eharness.Preconditionf(t, "invalid conversation fixture: %v", err)
	}
	authDB, charDB := e2eharness.OpenTestDBs(t)
	checkFixtureIdentity(t, authDB, charDB, &f.Actor)
	for _, bot := range f.Bots {
		if bot.GUID == 0 || !regexp.MustCompile(`^RNDBOT[0-9]+$`).MatchString(bot.Account) ||
			!regexp.MustCompile(`^[A-Za-z]{2,12}$`).MatchString(bot.Name) {
			e2eharness.Preconditionf(t, "invalid disposable bot identity")
		}
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		var accountID, actualAccount uint32
		var name string
		var online int
		err := authDB.QueryRowContext(ctx, "SELECT id FROM account WHERE username=?", bot.Account).Scan(&accountID)
		if err == nil {
			err = charDB.QueryRowContext(ctx, "SELECT account,name,online FROM characters WHERE guid=?", bot.GUID).
				Scan(&actualAccount, &name, &online)
		}
		cancel()
		if err != nil || actualAccount != accountID || name != bot.Name || online != 1 {
			e2eharness.Preconditionf(t, "exact live disposable bot mismatch: %v", err)
		}
	}
	guest := loginFixture(t, authDB, charDB, f.Actor)
	barrier := ownerPrefix(f.Bots[0].GUID)
	command := func(cmds ...string) {
		cmds = append(cmds, fmt.Sprintf(".alles status player %d", f.Bots[0].GUID))
		t.Logf("setup %v: %v", cmds, commands(t, guest, barrier, cmds...))
	}
	command(".gm off", ".gm visible on", ".gm chat off")
	guest.Teleport(t, -8950, -130, 83.5, 0)
	command()
	var summoned []actorFixture
	t.Cleanup(func() {
		// Stop through the player path first. Normal logout also releases any remaining follow leases.
		if !guest.World.IsStopped() {
			say(t, guest, "Please stop following me and return to your own tasks.")
			for _, bot := range summoned {
				command(".recall " + bot.Name)
			}
		}
	})
	for _, bot := range f.Bots {
		command(".summon " + bot.Name)
		summoned = append(summoned, bot)
	}
	// A same-session command barrier does not acknowledge another player's teleport completion.
	for _, bot := range f.Bots {
		guest.WaitUnitGUID(t, bot.GUID, 10*time.Second)
		if !eventually(10*time.Second, func() bool {
			object := guest.World.GetObject(bot.GUID)
			if object == nil || !object.HasKnownPosition() {
				return false
			}
			x, y, z, _, _ := guest.World.Position()
			bx, by, bz := object.InterpolatedPosition()
			return e2eharness.Distance3D(x, y, z, bx, by, bz) < 15
		}) {
			e2eharness.Preconditionf(t, "summoned bot did not reach observer: %s", bot.Name)
		}
		object := guest.World.GetObject(bot.GUID)
		x, y, z, _, _ := guest.World.Position()
		bx, by, bz := object.InterpolatedPosition()
		t.Logf("visible %s at %.1f %.1f %.1f; human %.1f %.1f %.1f", bot.Name, bx, by, bz, x, y, z)
	}
	var mu sync.Mutex
	var heard []chatLine
	stop := guest.World.AddPacketHook(func(op uint16, data []byte) {
		if line, ok := parseChat(op, data); ok && (line.kind == 1 || line.kind == 6) {
			for _, bot := range f.Bots {
				if line.source == bot.GUID {
					mu.Lock()
					heard = append(heard, line)
					mu.Unlock()
				}
			}
		}
	})
	defer stop()
	channel := client.ChatMsgSay
	turn := func(message string, targets []actorFixture, contains string) {
		mu.Lock()
		heard = nil
		mu.Unlock()
		if err := guest.World.SendChatMessage(channel, client.LangCommon, message); err != nil {
			e2eharness.HarnessFailf(t, "send conversation: %v", err)
		}
		if !eventually(35*time.Second, func() bool {
			mu.Lock()
			defer mu.Unlock()
			for _, bot := range targets {
				found := false
				for _, line := range heard {
					if line.source == bot.GUID && strings.Contains(strings.ToLower(line.text), strings.ToLower(contains)) {
						found = true
					}
				}
				if !found {
					return false
				}
			}
			return true
		}) {
			mu.Lock()
			lines := append([]chatLine(nil), heard...)
			mu.Unlock()
			e2eharness.Assertf(t, "missing contextual replies to %q: %+v", message, lines)
		}
		mu.Lock()
		t.Logf("player=%q replies=%+v", message, heard)
		mu.Unlock()
	}
	read := func() (uint64, map[uint64][2]float64) {
		var snapshot struct {
			PublishedUnixMs int64
			Conversation    struct {
				Enabled   bool
				Following uint64
			}
			Bots []struct {
				GUID uint64
				X, Y float64
			}
		}
		data, err := os.ReadFile(f.Telemetry)
		if err != nil || json.Unmarshal(data, &snapshot) != nil || !snapshot.Conversation.Enabled ||
			time.Now().UnixMilli()-snapshot.PublishedUnixMs > 3000 {
			e2eharness.Preconditionf(t, "fresh conversation telemetry unavailable")
		}
		positions := map[uint64][2]float64{}
		for _, bot := range snapshot.Bots {
			positions[bot.GUID] = [2]float64{bot.X, bot.Y}
		}
		return snapshot.Conversation.Following, positions
	}
	turn(f.Bots[0].Name+", would you walk along with me for a little while?", f.Bots[:1], "")
	if !eventually(8*time.Second, func() bool { n, _ := read(); return n == 1 }) {
		e2eharness.Assertf(t, "named request did not start exactly one follow")
	}
	// Moving the observer tests actual follow motion, rather than accepting a verbal promise as success.
	x, y, z, _, _ := guest.World.Position()
	if err := guest.World.MoveForwardAt(x, y, z, 0); err != nil {
		e2eharness.HarnessFailf(t, "start observer movement: %v", err)
	}
	movement := time.NewTicker(250 * time.Millisecond)
	defer movement.Stop()
	for step := 1; step <= 16; step++ {
		<-movement.C
		if err := guest.World.SendMovementHeartbeatAt(x+float32(step)*1.25, y, z, 0); err != nil {
			e2eharness.HarnessFailf(t, "observer movement: %v", err)
		}
	}
	if err := guest.World.MoveStopAt(x+20, y, z, 0); err != nil {
		e2eharness.HarnessFailf(t, "stop observer movement: %v", err)
	}
	if !eventually(15*time.Second, func() bool {
		_, positions := read()
		for _, bot := range f.Bots[:1] {
			p, ok := positions[bot.GUID]
			if !ok || math.Hypot(p[0]-float64(x+20), p[1]-float64(y)) > 7 {
				return false
			}
		}
		return true
	}) {
		e2eharness.Assertf(t, "bots promised to follow but did not approach the moving player")
	}
	turn(f.Bots[0].Name+", please stop accompanying me and return to your own tasks.", f.Bots[:1], "")
	if !eventually(8*time.Second, func() bool { n, _ := read(); return n == 0 }) {
		e2eharness.Assertf(t, "natural stop request did not release the follow before its deadline")
	}

	channel = client.ChatMsgYell
	nicknames := []string{"Marmalade", "Dandelion", "Hazelnut", "Buttercup", "Peppermint", "Bluebell"}
	nickname := nicknames[time.Now().UnixNano()%int64(len(nicknames))]
	turn(f.Bots[1].Name+", please call me "+nickname+" while we talk.", f.Bots[1:], "")
	turn(f.Bots[1].Name+", which name should you use for me now?", f.Bots[1:], nickname)
	channel = client.ChatMsgSay
	turn(f.Bots[1].Name+", what nickname did I ask you to use?", f.Bots[1:], nickname)
	// Continue observing after the addressed reply to catch an unwanted second respondent.
	if eventually(4*time.Second, func() bool {
		mu.Lock()
		defer mu.Unlock()
		for _, line := range heard {
			if line.source == f.Bots[0].GUID && strings.Contains(line.text, nickname) {
				return true
			}
		}
		return false
	}) {
		e2eharness.Assertf(t, "unaddressed bot answered a directly named question")
	}
	// A temporary unowned creature provides an exact, disposable engaged threat.
	guest.CombatReady(t)
	before := map[uint64]bool{}
	for _, object := range guest.World.GetNearbyUnits(40) {
		before[object.GUID] = true
	}
	command(".npc add temp 113")
	var threat uint64
	if !eventually(5*time.Second, func() bool {
		for _, object := range guest.World.GetNearbyUnits(20) {
			if object.Entry == 113 && !before[object.GUID] {
				threat = object.GUID
				return true
			}
		}
		return false
	}) {
		e2eharness.Preconditionf(t, "temporary threat did not enter object cache")
	}
	t.Cleanup(func() {
		if !guest.World.IsStopped() && guest.World.GetObject(threat) != nil {
			_ = guest.World.SetTarget(threat)
			command(".npc delete")
		}
	})
	attacks := make(chan uint64, 4)
	unhook := guest.World.AddPacketHook(func(op uint16, data []byte) {
		if op == client.SmsgAttackStart && len(data) >= 16 && binary.LittleEndian.Uint64(data[8:16]) == threat {
			source := binary.LittleEndian.Uint64(data[:8])
			for _, bot := range f.Bots {
				if source == bot.GUID {
					select {
					case attacks <- source:
					default:
					}
				}
			}
		}
	})
	defer unhook()
	guest.Engage(t, threat, 8*time.Second)
	turn(f.Bots[0].Name+", please help me fight this boar attacking me.", f.Bots[:1], "")
	select {
	case source := <-attacks:
		t.Logf("PASS model-assisted attack start from bot %d against temporary threat %x", source, threat)
	case <-time.After(5 * time.Second):
		e2eharness.Assertf(t, "assist promise produced no bot attack against the engaged threat")
	}

	t.Log("PASS natural SAY/YELL, distinct addressed respondents, history, physical follow/stop and NPC assist")
}

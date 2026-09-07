//go:build e2e

package alles_test

import (
	"bufio"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// PR #19: actual conversation delivery must reach the journal even with ambient speech disabled.
// The disposable human visits a live bot; the bot's position and strategies are never changed by setup.
func TestAlles_ConversationJournal(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"med", "alles", "serial"}, Runtime: "med", Category: "alles"})
	path := os.Getenv("E2E_ALLES_AUDIENCE_FIXTURE")
	if path == "" {
		t.Skip("requires E2E_ALLES_AUDIENCE_FIXTURE, conversation enabled, ambient speech disabled, and journaling")
	}
	var f struct {
		Version    int
		Disposable bool
		Actor      actorFixture
		Target     struct {
			Name string
			GUID uint64
		}
		Telemetry string
	}
	b, err := os.ReadFile(path)
	if err != nil || len(b) > 16384 {
		e2eharness.Preconditionf(t, "journal fixture read: %v", err)
	}
	if err := json.Unmarshal(b, &f); err != nil || f.Version != 1 || !f.Disposable ||
		!regexp.MustCompile(`^ALLESE2E[A-Z0-9]{1,9}$`).MatchString(f.Actor.Account) ||
		!regexp.MustCompile(`^[A-Za-z]{2,12}$`).MatchString(f.Target.Name) || f.Target.GUID == 0 {
		e2eharness.Preconditionf(t, "invalid journal fixture: %v", err)
	}
	authDB, charDB := e2eharness.OpenTestDBs(t)
	checkFixtureIdentity(t, authDB, charDB, &f.Actor)
	guest := loginFixture(t, authDB, charDB, f.Actor)
	commands(t, guest, ownerPrefix(f.Target.GUID), ".gm off", ".gm visible on", ".gm chat off",
		fmt.Sprintf(".alles status player %d", f.Target.GUID))
	var snapshot struct {
		Run             string
		PublishedUnixMs int64
		Conversation    struct{ Enabled bool }
		Journal         json.RawMessage
		Bots            []struct {
			GUID    uint64
			ID      string
			Name    string
			Map     uint32
			X, Y, Z float64
		}
	}
	readSnapshot := func() {
		b, err := os.ReadFile(f.Telemetry)
		if err != nil || json.Unmarshal(b, &snapshot) != nil ||
			math.Abs(float64(time.Now().UnixMilli()-snapshot.PublishedUnixMs)) > 3000 ||
			!snapshot.Conversation.Enabled || len(snapshot.Journal) == 0 {
			e2eharness.Preconditionf(t, "fresh conversation/journal telemetry required: %v", err)
		}
	}
	readSnapshot()
	run := snapshot.Run
	targetID := ""
	visit := func() {
		readSnapshot()
		for _, bot := range snapshot.Bots {
			if bot.GUID == f.Target.GUID && bot.Name == f.Target.Name {
				targetID = bot.ID
				guest.Teleport(t, float32(bot.X+2), float32(bot.Y), float32(bot.Z), bot.Map)
				guest.WaitUnitGUID(t, bot.GUID, 10*time.Second)
				guest.AssertNear(t, float32(bot.X), float32(bot.Y), float32(bot.Z), 15)
				return
			}
		}
		e2eharness.Preconditionf(t, "exact target bot absent from live telemetry")
	}
	for _, channel := range []uint32{client.ChatMsgSay, client.ChatMsgYell} {
		visit()
		journal := filepath.Join(filepath.Dir(f.Telemetry), "events.ndjson")
		file, err := os.Open(journal)
		if err != nil {
			e2eharness.Preconditionf(t, "open live event journal: %v", err)
		}
		info, err := file.Stat()
		if err != nil {
			file.Close()
			e2eharness.HarnessFailf(t, "journal stat: %v", err)
		}
		if _, err := file.Seek(info.Size(), 0); err != nil {
			file.Close()
			e2eharness.HarnessFailf(t, "journal seek: %v", err)
		}
		defer file.Close()
		replies := make(chan string, 8)
		cancel := guest.World.AddPacketHook(func(op uint16, data []byte) {
			if line, ok := parseChat(op, data); ok && line.source == f.Target.GUID && uint32(line.kind) == channel {
				select {
				case replies <- line.text:
				default:
				}
			}
		})
		defer cancel()
		if err := guest.World.SendChatMessage(channel, client.LangCommon, f.Target.Name+", please wave to me."); err != nil {
			e2eharness.HarnessFailf(t, "send conversation: %v", err)
		}
		var reply string
		select {
		case reply = <-replies:
		case <-time.After(45 * time.Second):
			e2eharness.Assertf(t, "no bot conversation reply")
		}
		cancel()
		// Preserve incomplete lines between reads of the writer's live journal.
		reader := bufio.NewReader(file)
		partial := ""
		var matched string
		if !eventually(5*time.Second, func() bool {
			for {
				line, err := reader.ReadString('\n')
				partial += line
				if !strings.HasSuffix(partial, "\n") {
					return false
				}
				var event struct {
					Run, Kind, Bot, Detail, Context string
					Value                           uint64
				}
				if json.Unmarshal([]byte(partial), &event) != nil {
					e2eharness.HarnessFailf(t, "invalid event JSON")
				}
				partial = ""
				kind := "say"
				if channel == client.ChatMsgYell {
					kind = "yell"
				}
				if event.Run == run && event.Bot == targetID && event.Kind == "alles_conversation" && event.Detail == reply &&
					strings.Contains(event.Context, fmt.Sprintf("human=%d ", f.Actor.GUID)) &&
					strings.Contains(event.Context, "channel="+kind+" action=wave actionOk=true job=conversation-") &&
					event.Value == 1 {
					matched = event.Context
					return true
				}
				if err != nil {
					return false
				}
			}
		}) {
			e2eharness.Assertf(t, "delivered reply/action missing from journal: %q", reply)
		}
		t.Logf("PASS channel=%d bot=%d reply=%q journal=%s", channel, f.Target.GUID, reply, matched)
		file.Close()
	}
}

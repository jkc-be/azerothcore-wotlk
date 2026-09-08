//go:build e2e

package alles_test

import (
	"context"
	"encoding/binary"
	"fmt"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
)

type chatLine struct {
	kind   byte
	source uint64
	text   string
}

// Core ChatHandler::BuildChatPacket: SYSTEM and player SAY/YELL share this fixed layout.
// Match decoded system text, not an arbitrary byte substring that ordinary player chat could impersonate.
func parseChat(op uint16, data []byte) (chatLine, bool) {
	if op != client.SmsgMessageChat || len(data) < 31 || (data[0] != 0 && data[0] != 1 && data[0] != 6) {
		return chatLine{}, false
	}
	size := uint64(binary.LittleEndian.Uint32(data[25:29]))
	if size < 1 || size > 8192 || size+30 != uint64(len(data)) || data[28+size] != 0 {
		return chatLine{}, false
	}
	line := chatLine{data[0], binary.LittleEndian.Uint64(data[5:13]), string(data[29 : 28+size])}
	if !utf8.ValidString(line.text) || strings.ContainsRune(line.text, '\x00') {
		return chatLine{}, false
	}
	if line.kind == 0 && line.source != 0 {
		return chatLine{}, false
	}
	return line, true
}

func ownerPrefix(guid uint64) string {
	return fmt.Sprintf("Alles player %d:", guid)
}

// Command lists are ordered on one session. The final command supplies an explicit system-message barrier;
// unrelated SAY packets cannot acknowledge a recall/flush/status command.
func commands(t *testing.T, bot *e2eharness.ScenarioBot, prefix string, cmds ...string) []string {
	t.Helper()
	lines := make(chan string, 128)
	var overflow atomic.Bool
	cancel := bot.World.AddPacketHook(func(op uint16, data []byte) {
		if line, ok := parseChat(op, data); ok && line.kind == 0 {
			select {
			case lines <- line.text:
			default:
				overflow.Store(true)
			}
		}
	})
	defer cancel()
	for _, cmd := range cmds {
		if err := bot.World.SendGMCommand(cmd); err != nil {
			e2eharness.HarnessFailf(t, "alles command send: %v", err)
		}
	}
	deadline := time.NewTimer(5 * time.Second)
	defer deadline.Stop()
	var result []string
	for {
		select {
		case line := <-lines:
			if overflow.Load() || len(result) >= 128 {
				e2eharness.HarnessFailf(t, "alles system-message observation overflow")
			}
			result = append(result, line)
			if strings.HasPrefix(line, prefix) {
				return result
			}
			if strings.Contains(line, "not configured for alles") ||
				strings.Contains(line, "disabled or unavailable") ||
				strings.HasPrefix(line, "Flush unavailable") {
				e2eharness.Preconditionf(t, "alles fixture command rejected: %s", line)
			}
		case <-deadline.C:
			e2eharness.HarnessFailf(t, "missing alles command barrier %q; received %v", prefix, result)
		}
	}
}

func status(t *testing.T, bot *e2eharness.ScenarioBot, guid uint64) string {
	t.Helper()
	lines := commands(t, bot, ownerPrefix(guid), fmt.Sprintf(".alles status player %d", guid))
	return lines[len(lines)-1]
}

func eventually(timeout time.Duration, predicate func() bool) bool {
	until := time.Now().Add(timeout)
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for {
		if !time.Now().Before(until) {
			return false
		}
		if predicate() {
			return true
		}
		select {
		case <-deadline.C:
			return false
		case <-tick.C:
		}
	}
}

func waitRecall(t *testing.T, bot *e2eharness.ScenarioBot, marker string) {
	t.Helper()
	if !eventually(65*time.Second, func() bool {
		lines := commands(t, bot, ownerPrefix(bot.GUID), ".alles recall 20",
			fmt.Sprintf(".alles status player %d", bot.GUID))
		for _, line := range lines[:len(lines)-1] {
			if strings.Contains(line, marker) {
				return true
			}
		}
		return false
	}) {
		e2eharness.Assertf(t, "heard marker did not enter %s's self recall before its bounded deadline", bot.Role)
	}
}

func number(t *testing.T, line, name string) uint64 {
	t.Helper()
	value, err := strconv.ParseUint(field(line, name), 10, 64)
	if err != nil {
		e2eharness.HarnessFailf(t, "missing/invalid %s in module response: %s", name, line)
	}
	return value
}

// The explicit module watermark is required before every memory/perception DB oracle.
func flushOwner(t *testing.T, bot *e2eharness.ScenarioBot) uint64 {
	t.Helper()
	return flushActor(t, bot, bot.GUID)
}

func flushActor(t *testing.T, bot *e2eharness.ScenarioBot, owner uint64) uint64 {
	t.Helper()
	// The same-session module response fences .save without relying on the harness's fixed save delay.
	save := ".save"
	if owner != bot.GUID {
		save = ".saveall"
	}
	lines := commands(t, bot, ownerPrefix(owner), save, fmt.Sprintf(".alles flush player %d", owner))
	requested := number(t, lines[len(lines)-1], "requested_revision")
	if requested == 0 {
		e2eharness.Assertf(t, "fixture's admitted observation has no persistent revision")
	}
	if !eventually(15*time.Second, func() bool {
		line := status(t, bot, owner)
		if failed := field(line, "save_failed"); failed == "true" || failed == "1" {
			e2eharness.Assertf(t, "alles snapshot save failed: %s", line)
		}
		if field(line, "state") != "ready" {
			e2eharness.Assertf(t, "active fixture owner left ready state during module flush: %s", line)
		}
		return number(t, line, "committed_revision") >= requested
	}) {
		e2eharness.Assertf(t, "alles owner %d never acknowledged revision %d", owner, requested)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var committed uint64
	if err := bot.CharDB.QueryRowContext(ctx,
		"SELECT committed_revision FROM alles_actor WHERE owner_kind=0 AND owner_id=?", owner).
		Scan(&committed); err != nil {
		e2eharness.HarnessFailf(t, "read acknowledged actor revision: %v", err)
	}
	if committed < requested {
		e2eharness.Assertf(t, "module acknowledged %d but DB contains only %d", requested, committed)
	}
	return requested
}

func watchSay(w *client.WorldClient, source uint64, marker string) (<-chan struct{}, *atomic.Uint64, func()) {
	seen := make(chan struct{}, 1)
	count := new(atomic.Uint64)
	cancel := w.AddPacketHook(func(op uint16, data []byte) {
		line, ok := parseChat(op, data)
		if ok && line.kind == 1 && line.source == source && strings.Contains(line.text, marker) {
			count.Add(1)
			select {
			case seen <- struct{}{}:
			default:
			}
		}
	})
	return seen, count, cancel
}

func say(t *testing.T, bot *e2eharness.ScenarioBot, text string) {
	t.Helper()
	if err := bot.World.SendChatMessage(client.ChatMsgSay, client.LangCommon, text); err != nil {
		e2eharness.HarnessFailf(t, "fixture SAY: %v", err)
	}
}

func waitSay(t *testing.T, seen <-chan struct{}) {
	t.Helper()
	select {
	case <-seen:
	case <-time.After(5 * time.Second):
		e2eharness.Preconditionf(t, "ordinary player SAY was not delivered to the nearby client")
	}
}

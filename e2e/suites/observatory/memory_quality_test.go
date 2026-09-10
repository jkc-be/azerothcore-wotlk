//go:build e2e

package observatory_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// PR #28: ordinary greetings must not become high-salience recursive reports. This exclusive disposable
// fixture temporarily enables interactive GM observation; it is not a throughput comparison run.
func TestObservatory_GreetingsDoNotBecomeRecursiveNews(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"observatory", "alles", "serial"}, Category: "observatory", Runtime: "med"})
	path := os.Getenv("E2E_OBSERVATORY_MOTIVATIONS_FIXTURE")
	if path == "" {
		t.Skip("requires an exclusive disposable managed cohort; see e2e/README.md")
	}
	var fixture struct {
		Disposable bool
		Name       string
		GUID       uint64
	}
	data, err := os.ReadFile(path)
	if err != nil || json.Unmarshal(data, &fixture) != nil || !fixture.Disposable || fixture.GUID == 0 {
		e2eharness.Preconditionf(t, "invalid memory fixture: %v", err)
	}
	token, err := os.ReadFile(os.Getenv("E2E_OBSERVATORY_TOKEN_FILE"))
	if err != nil {
		e2eharness.Preconditionf(t, "token: %v", err)
	}
	a := api{url: strings.TrimRight(os.Getenv("E2E_OBSERVATORY_URL"), "/"),
		token: strings.TrimSpace(string(token)), http: &http.Client{Timeout: 5 * time.Second}}
	initial := a.frame(t)
	if !initial.Allowed || initial.Observers != 0 {
		e2eharness.Preconditionf(t, "exclusive observer-enabled realm required")
	}
	seq := a.observerMode(t, initial, 2, http.StatusAccepted)
	a.wait(t, "interactive chat fixture", func(s snapshot) bool { return s.ControlSeq >= seq && s.ObserverMode == 2 })
	t.Cleanup(func() {
		seq := a.observerMode(t, a.frame(t), initial.ObserverMode, http.StatusAccepted)
		a.wait(t, "restore observer mode", func(s snapshot) bool {
			return s.ControlSeq >= seq && s.ObserverMode == initial.ObserverMode
		})
	})
	authDB, charDB := e2eharness.OpenTestDBs(t)
	id := e2eharness.MakeBotIdents("Memtalk", 1)[0]
	if err := e2eharness.EnsureAccount(authDB, id.Account, "test"); err != nil {
		e2eharness.Preconditionf(t, "account: %v", err)
	}
	if err := e2eharness.SetGM(authDB, id.Account, 3); err != nil {
		e2eharness.Preconditionf(t, "GM: %v", err)
	}
	t.Cleanup(func() {
		_, err := authDB.Exec("DELETE aa FROM account_access aa JOIN account a ON a.id=aa.id WHERE a.username=?", id.Account)
		if err != nil {
			t.Errorf("cleanup GM: %v", err)
		}
	})
	observer := loginObserver(t, authDB, id)
	t.Cleanup(func() {
		observer.Close()
		a.wait(t, "observer disconnected", func(s snapshot) bool { return s.Observers == 0 })
	})
	send := func(text string) {
		if err := observer.SendChatMessage(client.ChatMsgSay, client.LangCommon, text); err != nil {
			e2eharness.HarnessFailf(t, "send ordinary chat: %v", err)
		}
	}
	var echoes atomic.Uint32
	cancel := observer.AddPacketHook(func(op uint16, b []byte) {
		if op == client.SmsgMessageChat && bytes.Contains(b, []byte("told me")) &&
			bytes.Contains(b, []byte("good to see you")) {
			echoes.Add(1)
		}
	})
	defer cancel()
	approach := func() {
		send(".appear " + fixture.Name)
		chat(t, observer, fmt.Sprintf(".alles status player %d", fixture.GUID), "state=ready")
	}
	send(".gm off")
	send(".gm visible on")
	approach()
	greeting := "Hello, " + fixture.Name + ". It is good to see you."
	send(greeting)
	send("I saw a violet banner fall beside the abbey.")
	started := a.frame(t).SimMs
	deadline := time.NewTimer(2 * time.Minute)
	defer deadline.Stop()
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	repeated, retained := false, false
	var firstID uint64
	for {
		frame := a.frame(t)
		require(t, frame.Run == initial.Run, "realm changed during memory check")
		var count int
		var memoryID uint64
		var salience float64
		var formation uint32
		err := charDB.QueryRow(`SELECT COUNT(*), COALESCE(MIN(memory_id),0), COALESCE(MAX(salience),0),
		 COALESCE(MAX(formation_mode),0) FROM alles_memory WHERE owner_kind=0 AND owner_id=?
		 AND source_kind=0 AND source_id=? AND claim=?`, fixture.GUID, observer.CharGUID(), greeting).
			Scan(&count, &memoryID, &salience, &formation)
		if err != nil {
			e2eharness.HarnessFailf(t, "committed memory: %v", err)
		}
		if count > 0 {
			require(t, count == 1 && salience <= 0.05 && formation == 2,
				"greeting duplicated or promoted: count=%d salience=%g formation=%d", count, salience, formation)
			if !retained {
				firstID, retained = memoryID, true
			}
			require(t, firstID == memoryID, "repeat replaced the original memory")
		}
		require(t, echoes.Load() == 0, "ordinary chat received a recursive greeting report")
		if retained && !repeated && frame.SimMs >= started+35000 {
			approach()
			send(greeting) // Beyond ingress deduplication: exercises memory consolidation.
			repeated = true
		}
		if repeated && frame.SimMs >= started+75000 {
			var useful int
			err := charDB.QueryRow(`SELECT COUNT(*) FROM alles_memory WHERE owner_kind=0 AND owner_id=?
			 AND source_kind=0 AND source_id=? AND reported_depth=1 AND salience>0.05
			 AND confidence<=0.6 AND claim NOT LIKE '%reported that%' AND claim NOT LIKE 'I saw %'`,
				fixture.GUID, observer.CharGUID()).Scan(&useful)
			require(t, err == nil && useful > 0, "substantive report did not remain attributed hearsay: %v", err)
			var pending int
			err = charDB.QueryRow(`SELECT COUNT(*) FROM alles_perception WHERE owner_kind=0 AND owner_id=?
			 AND source_kind=0 AND source_id=? AND gated_text=?`, fixture.GUID, observer.CharGUID(), greeting).Scan(&pending)
			require(t, err == nil && pending == 0 && count == 1, "repeat remains pending or memory was lost: %v", err)
			t.Logf("PASS: greeting persisted once as low-salience reflex memory %d; "+
				"useful report retained and no recursive news over 75 game seconds", memoryID)
			return
		}
		select {
		case <-deadline.C:
			e2eharness.Assertf(t, "greeting memory observation timed out: retained=%t repeated=%t", retained, repeated)
		case <-tick.C:
		}
	}
}

// A controlled death in an exclusive disposable realm validates the runtime-to-storage learning path.
func TestObservatory_PersonalDeathRetainsLearningAfterRecovery(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"observatory", "alles", "serial"}, Category: "observatory", Runtime: "med"})
	path := os.Getenv("E2E_OBSERVATORY_MOTIVATIONS_FIXTURE")
	if path == "" {
		t.Skip("requires an exclusive disposable managed cohort; see e2e/README.md")
	}
	var fixture struct {
		Disposable bool
		Name       string
		GUID       uint64
	}
	data, err := os.ReadFile(path)
	if err != nil || json.Unmarshal(data, &fixture) != nil || !fixture.Disposable || fixture.GUID == 0 {
		e2eharness.Preconditionf(t, "invalid memory fixture: %v", err)
	}
	token, err := os.ReadFile(os.Getenv("E2E_OBSERVATORY_TOKEN_FILE"))
	if err != nil {
		e2eharness.Preconditionf(t, "token: %v", err)
	}
	a := api{url: strings.TrimRight(os.Getenv("E2E_OBSERVATORY_URL"), "/"),
		token: strings.TrimSpace(string(token)), http: &http.Client{Timeout: 5 * time.Second}}
	initial := a.frame(t)
	if !initial.Allowed || initial.Observers != 0 {
		e2eharness.Preconditionf(t, "exclusive observer-enabled realm required")
	}
	seq := a.observerMode(t, initial, 2, http.StatusAccepted)
	a.wait(t, "interactive chat fixture", func(s snapshot) bool { return s.ControlSeq >= seq && s.ObserverMode == 2 })
	t.Cleanup(func() {
		seq := a.observerMode(t, a.frame(t), initial.ObserverMode, http.StatusAccepted)
		a.wait(t, "restore observer mode", func(s snapshot) bool {
			return s.ControlSeq >= seq && s.ObserverMode == initial.ObserverMode
		})
	})
	authDB, charDB := e2eharness.OpenTestDBs(t)
	id := e2eharness.MakeBotIdents("Harm", 1)[0]
	if err := e2eharness.EnsureAccount(authDB, id.Account, "test"); err != nil {
		e2eharness.Preconditionf(t, "account: %v", err)
	}
	if err := e2eharness.SetGM(authDB, id.Account, 3); err != nil {
		e2eharness.Preconditionf(t, "GM: %v", err)
	}
	t.Cleanup(func() {
		_, err := authDB.Exec("DELETE aa FROM account_access aa JOIN account a ON a.id=aa.id WHERE a.username=?", id.Account)
		if err != nil {
			t.Errorf("cleanup GM: %v", err)
		}
	})
	observer := loginObserver(t, authDB, id)
	t.Cleanup(func() {
		observer.Close()
		a.wait(t, "observer disconnected", func(s snapshot) bool { return s.Observers == 0 })
	})
	send := func(text string) {
		if err := observer.SendChatMessage(client.ChatMsgSay, client.LangCommon, text); err != nil {
			e2eharness.HarnessFailf(t, "send ordinary chat: %v", err)
		}
	}

	send(".gm off")
	send(".gm visible on")
	send(".appear " + fixture.Name)
	chat(t, observer, fmt.Sprintf(".alles status player %d", fixture.GUID), "state=ready")
	type experience struct {
		Samples, Successes uint64
		FailureEffects     map[string]struct{ Mean float64 }
	}
	type botView struct {
		GUID     uint64
		Activity string
		Health   float64
		Deaths   uint64
		Planning struct {
			Objectives   []struct{ State string }
			Satisfaction struct{ Contexts map[string]experience }
		}
	}
	read := func() botView {
		var frame struct {
			Run  string
			Bots []botView
		}
		a.call(t, "/api/snapshot", nil, http.StatusOK, &frame)
		require(t, frame.Run == initial.Run, "realm changed during personal-consequence check")
		for _, bot := range frame.Bots {
			if bot.GUID == fixture.GUID {
				return bot
			}
		}
		t.Fatal("managed actor missing")
		return botView{}
	}
	deadline := time.NewTimer(4 * time.Minute)
	defer deadline.Stop()
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	var before botView
	for {
		before = read()
		active := false
		for _, objective := range before.Planning.Objectives {
			active = active || objective.State == "active"
		}
		if active && before.Health > 0 && before.Activity != "dead" {
			break
		}
		select {
		case <-deadline.C:
			t.Fatal("no live activity available for controlled personal consequence")
		case <-tick.C:
		}
	}
	// Only this explicitly disposable managed actor is affected. Ordinary death and resurrection hooks
	// must record the consequence; the test never writes a fabricated experience or memory into SQL.
	send(".appear " + fixture.Name)
	chat(t, observer, fmt.Sprintf(".alles status player %d", fixture.GUID), "state=ready")
	require(t, observer.SetTarget(fixture.GUID) == nil, "select disposable actor")
	send(".die")
	dead, recovered := false, false
	for {
		view := read()
		dead = dead || view.Deaths > before.Deaths || view.Health == 0
		if dead && view.Health > 0 && view.Activity != "dead" {
			recovered = true
		}
		learned := ""
		for key, experience := range view.Planning.Satisfaction.Contexts {
			previous := before.Planning.Satisfaction.Contexts[key]
			if experience.Samples-experience.Successes > previous.Samples-previous.Successes &&
				experience.FailureEffects["security"].Mean < -0.5 {
				learned = key
			}
		}
		var deaths, invalid, duplicates int
		err := charDB.QueryRow(`SELECT COUNT(*) FROM alles_memory WHERE owner_kind=0 AND owner_id=?
		 AND kind=4 AND salience>0.5 AND source_kind=0 AND source_id=?`, fixture.GUID, observer.CharGUID()).Scan(&deaths)
		require(t, err == nil, "read committed own-death memory: %v", err)
		err = charDB.QueryRow(`SELECT COUNT(*) FROM alles_memory WHERE owner_kind=0 AND owner_id=?
		 AND kind=5 AND (encounters=0 OR last_seen_game_time_ms=0)`, fixture.GUID).Scan(&invalid)
		require(t, err == nil && invalid == 0, "familiarity metadata missing: %v", err)
		err = charDB.QueryRow(`SELECT COUNT(*) FROM (SELECT subject_kind,subject_id FROM alles_memory
		 WHERE owner_kind=0 AND owner_id=? AND kind=5 AND subject_id IS NOT NULL
		 GROUP BY subject_kind,subject_id HAVING COUNT(*)>1) AS repeated`, fixture.GUID).Scan(&duplicates)
		require(t, err == nil && duplicates == 0, "persistent actor has duplicate familiarity records: %v", err)
		if recovered && learned != "" && deaths > 0 {
			var payload string
			err = charDB.QueryRow("SELECT payload FROM alles_planning WHERE owner_kind=0 AND owner_id=?", fixture.GUID).
				Scan(&payload)
			var saved struct {
				Satisfaction struct {
					Contexts []struct {
						ID                 string
						Samples, Successes uint64
					}
				}
			}
			require(t, err == nil && json.Unmarshal([]byte(payload), &saved) == nil, "read durable learning: %v", err)
			for _, context := range saved.Satisfaction.Contexts {
				if context.ID == learned && context.Samples > context.Successes {
					t.Logf("PASS: actual death, recovery, retained own-death memory and durable context %s; "+
						"no duplicate identities or incomplete familiarity metadata", learned)
					return
				}
			}
		}
		select {
		case <-deadline.C:
			t.Fatalf("personal consequence missing: dead=%t recovered=%t context=%q committedDeaths=%d",
				dead, recovered, learned, deaths)
		case <-tick.C:
		}
	}
}

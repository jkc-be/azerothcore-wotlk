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
	t.Cleanup(func() { a.observerMode(t, a.frame(t), initial.ObserverMode, http.StatusAccepted) })
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
	t.Cleanup(observer.Close)
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

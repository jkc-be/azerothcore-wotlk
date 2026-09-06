//go:build e2e

package observatory_test

import (
	"bytes"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
	_ "github.com/go-sql-driver/mysql"
)

func require(t *testing.T, condition bool, format string, args ...any) {
	t.Helper()
	if !condition {
		e2eharness.Assertf(t, format, args...)
	}
}

type snapshot struct {
	ExpectedBots int    `json:"expectedBots"`
	Run          string `json:"run"`
	Seq          uint64 `json:"seq"`
	SimMs        uint64 `json:"simMs"`
	Observers    int    `json:"observers"`
	Allowed      bool   `json:"observersAllowed"`
	Speed        int    `json:"requestedSpeed"`
	Paused       bool   `json:"paused"`
	Fault        string `json:"fault"`
	ControlError string `json:"controlError"`
	ControlSeq   uint64 `json:"controlSeq"`
	Bots         []struct {
		Name string `json:"name"`
	} `json:"bots"`
}

type api struct {
	url, token string
	http       *http.Client
}

func (a api) call(t *testing.T, path string, body any, want int, out any) {
	t.Helper()
	method := "GET"
	var data []byte
	if body != nil {
		method = "POST"
		data, _ = json.Marshal(body)
	}
	req, err := http.NewRequest(method, a.url+path, bytes.NewReader(data))
	if err != nil {
		e2eharness.HarnessFailf(t, "request: %v", err)
	}
	req.Header.Set("Authorization", "Bearer "+a.token)
	req.Header.Set("Content-Type", "application/json")
	res, err := a.http.Do(req)
	if err != nil {
		e2eharness.HarnessFailf(t, "HTTP: %v", err)
	}
	defer res.Body.Close()
	b, err := io.ReadAll(res.Body)
	if err != nil {
		e2eharness.HarnessFailf(t, "HTTP body: %v", err)
	}
	require(t, res.StatusCode == want, "HTTP status=%d want=%d: %s", res.StatusCode, want, b)
	if out != nil {
		if err := json.Unmarshal(b, out); err != nil {
			e2eharness.HarnessFailf(t, "JSON: %v", err)
		}
	}
}
func (a api) frame(t *testing.T) snapshot {
	t.Helper()
	var s snapshot
	a.call(t, "/api/snapshot", nil, 200, &s)
	require(t, s.Fault == "", "simulation fault: %s", s.Fault)
	return s
}
func (a api) wait(t *testing.T, label string, pred func(snapshot) bool) snapshot {
	t.Helper()
	deadline := time.NewTimer(20 * time.Second)
	defer deadline.Stop()
	tick := time.NewTicker(50 * time.Millisecond)
	defer tick.Stop()
	for {
		s := a.frame(t)
		if pred(s) {
			return s
		}
		select {
		case <-deadline.C:
			require(t, false, "timeout %s: %+v", label, s)
		case <-tick.C:
		}
	}
}
func (a api) control(t *testing.T, s snapshot, speed int, paused bool, want int) uint64 {
	t.Helper()
	var r struct {
		Sequence uint64 `json:"sequence"`
	}
	a.call(t, "/api/control", map[string]any{"run": s.Run, "speed": speed, "paused": paused}, want, &r)
	return r.Sequence
}
func chat(t *testing.T, w *client.WorldClient, command, contains string) {
	t.Helper()
	done := make(chan struct{}, 1)
	cancel := w.AddPacketHook(func(op uint16, b []byte) {
		if op == client.SmsgMessageChat && bytes.Contains(b, []byte(contains)) {
			select {
			case done <- struct{}{}:
			default:
			}
		}
	})
	defer cancel()
	if err := w.SendChatMessage(client.ChatMsgSay, client.LangCommon, command); err != nil {
		e2eharness.HarnessFailf(t, "send chat: %v", err)
	}
	select {
	case <-done:
	case <-time.After(15 * time.Second):
		require(t, false, "no %q response to %q", contains, command)
	}
}

// AzerothGhost v1.0.8 sends the OS FourCC with a leading NUL; AC reads an empty OS.
// Set only the freshly created test account's OS after SRP, before realm authentication, so Warden's OS
// check cannot mask the GM admission oracle. Do not change Warden or real accounts to work around it.
func realmClient(t *testing.T, db *sql.DB, user string) (*client.WorldClient, string) {
	t.Helper()
	auth := client.NewAuthClient(user, "test")
	realms, err := auth.Authenticate(e2eharness.AuthAddr)
	if err != nil || len(realms) == 0 {
		e2eharness.Preconditionf(t, "SRP: %v", err)
	}
	if _, err := db.Exec("UPDATE account SET os='Win' WHERE username=?", user); err != nil {
		e2eharness.Preconditionf(t, "test client OS metadata: %v", err)
	}
	w := client.NewWorldClient(strings.ToUpper(user), auth.SessionKey(), func(string, ...interface{}) {})
	w.SetCharRace(e2eharness.RaceHuman)
	t.Cleanup(w.Close)
	return w, realms[0].Address
}

func loginObserver(t *testing.T, db *sql.DB, id e2eharness.BotIdent) *client.WorldClient {
	t.Helper()
	w, addr := realmClient(t, db, id.Account)
	chars := make(chan []client.CharEnumEntry, 2)
	created := make(chan byte, 1)
	w.OnCharList = func(c []client.CharEnumEntry) {
		select {
		case chars <- c:
		default:
		}
	}
	w.OnCharCreateResult = func(b []byte) {
		if len(b) > 0 {
			select {
			case created <- b[0]:
			default:
			}
		}
	}
	if err := w.Connect(addr); err != nil {
		e2eharness.Preconditionf(t, "world connect: %v", err)
	}
	go func() { _ = w.Run() }()
	if err := w.WaitForSessionPhase(client.PhaseAuthed, 10*time.Second); err != nil {
		e2eharness.Assertf(t, "GM realm login: %v", err)
	}
	if err := w.SendReadyForAccountDataTimes(); err != nil {
		e2eharness.HarnessFailf(t, "ready: %v", err)
	}
	if err := w.CreateCharacter(id.CharName, 1, 1, 0, 1, 1, 1, 1, 1, 0); err != nil {
		e2eharness.HarnessFailf(t, "create: %v", err)
	}
	select {
	case code := <-created:
		require(t, code == 0x2f, "character create: 0x%02x", code)
	case <-time.After(10 * time.Second):
		e2eharness.Assertf(t, "character creation timed out")
	}
	if err := w.RequestCharList(); err != nil {
		e2eharness.HarnessFailf(t, "enum: %v", err)
	}
	select {
	case list := <-chars:
		require(t, len(list) == 1, "expected unique test character, got %d", len(list))
		if err := w.LoginCharacter(list[0].GUID); err != nil {
			e2eharness.HarnessFailf(t, "character login: %v", err)
		}
	case <-time.After(10 * time.Second):
		e2eharness.Assertf(t, "character enum timed out")
	}
	if err := w.WaitForLogin(20 * time.Second); err != nil {
		e2eharness.Assertf(t, "GM enters world: %v", err)
	}
	return w
}

// Requires exclusive disposable Observatory.Enable=1 + AllowGmObservers=1, at least two online bots.
// Direct Session login is deliberate: default scenario setup sends GM mutations that this realm must deny.
// Oracle: native POV works while the connection lease enforces 1x; ordinary accounts and mutations are rejected.
func TestObservatory_GmPovConnectionLock(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"observatory", "serial"}, Category: "observatory", Runtime: "med"})
	if os.Getenv("E2E_OBSERVATORY_URL") == "" {
		t.Skip("dedicated observatory test; set E2E_OBSERVATORY_URL and E2E_OBSERVATORY_TOKEN_FILE")
	}
	token, err := os.ReadFile(os.Getenv("E2E_OBSERVATORY_TOKEN_FILE"))
	if err != nil {
		e2eharness.Preconditionf(t, "token: %v", err)
	}
	a := api{strings.TrimRight(os.Getenv("E2E_OBSERVATORY_URL"), "/"), strings.TrimSpace(string(token)), &http.Client{Timeout: 5 * time.Second}}
	initial := a.frame(t)
	if !initial.Allowed || initial.Observers != 0 || len(initial.Bots) < 2 {
		e2eharness.Preconditionf(t, "need observer admission, no clients and two bots")
	}
	db, err := e2eharness.OpenAuthDB()
	if err != nil {
		e2eharness.Preconditionf(t, "auth DB: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	ids := e2eharness.MakeBotIdents("Pov", 3)
	for i, id := range ids {
		if err := e2eharness.EnsureAccount(db, id.Account, "test"); err != nil {
			e2eharness.Preconditionf(t, "test account: %v", err)
		}
		if i != 1 {
			if err := e2eharness.SetGM(db, id.Account, 2); err != nil {
				e2eharness.Preconditionf(t, "GM: %v", err)
			}
		}
		t.Cleanup(func() {
			// Retain disposable character rows as evidence; remove access so test GMs cannot be reused.
			if _, err := db.Exec("DELETE aa FROM account_access aa JOIN account a ON a.id=aa.id WHERE a.username=?", id.Account); err != nil {
				t.Errorf("cleanup access: %v", err)
			}
		})
	}
	// SRP succeeds for an ordinary account, but realm authentication must explicitly reject it.
	normal, addr := realmClient(t, db, ids[1].Account)
	reply := make(chan byte, 1)
	cancel := normal.AddPacketHook(func(op uint16, b []byte) {
		if op == client.SmsgAuthResponse && len(b) > 0 {
			select {
			case reply <- b[0]:
			default:
			}
		}
	})
	defer cancel()
	if err := normal.Connect(addr); err != nil {
		e2eharness.Preconditionf(t, "ordinary connect: %v", err)
	}
	go func() { _ = normal.Run() }()
	select {
	case code := <-reply:
		require(t, code == 0x0e, "expected AUTH_REJECT (0x0e), got 0x%02x", code)
	case <-time.After(10 * time.Second):
		require(t, false, "no realm rejection")
	}
	normal.Close()

	// Connecting while paused must unpause the existing world to service native login.
	seq := a.control(t, initial, 5, true, 202)
	a.wait(t, "pause", func(s snapshot) bool { return s.ControlSeq == seq && s.Paused })
	observer := loginObserver(t, db, ids[0])
	locked := a.wait(t, "GM lease", func(s snapshot) bool { return s.Observers == 1 && s.Speed == 1 && !s.Paused })
	for _, speed := range []int{2, 5, 10} {
		a.control(t, locked, speed, false, 400)
	}
	a.control(t, locked, 1, true, 400)
	// Exercise the world's second validation layer independently of the bridge's fresh-snapshot guard.
	spool := os.Getenv("E2E_OBSERVATORY_SPOOL")
	if spool == "" {
		e2eharness.Preconditionf(t, "set E2E_OBSERVATORY_SPOOL for the mailbox race proof")
	}
	sequence := locked.ControlSeq + 1
	mailbox := fmt.Sprintf("%s %d 10 1 %d\n", locked.Run, sequence, locked.ExpectedBots)
	tmp := filepath.Join(spool, "gm-pov-test-control.tmp")
	if err := os.WriteFile(tmp, []byte(mailbox), 0600); err != nil {
		e2eharness.HarnessFailf(t, "mailbox: %v", err)
	}
	if err := os.Rename(tmp, filepath.Join(spool, "control.txt")); err != nil {
		e2eharness.HarnessFailf(t, "mailbox rename: %v", err)
	}
	a.wait(t, "world rejects stale bridge request", func(s snapshot) bool {
		return s.ControlSeq == sequence && s.ControlError != "" && s.Speed == 1 && !s.Paused
	})
	chat(t, observer, ".pov list", "RPOV\tPLAYER|")
	target := locked.Bots[0].Name
	chat(t, observer, ".pov watch "+target, "RPOV\tSTATE|"+target+"|")
	chat(t, observer, ".pov watch "+locked.Bots[1].Name, "RPOV\tSTATE|"+locked.Bots[1].Name+"|")
	chat(t, observer, ".modify money 999999", "permits /pov only")
	chat(t, observer, ".pov stop", "RPOV\tSTOP")
	s := a.frame(t)
	require(t, s.Observers == 1 && s.Speed == 1, "POV stop released lease")
	// A forged client heartbeat and another GM command must not mutate saved observer state.
	x, y, z, _, mapID := observer.Position()
	money := observer.Money()
	if err := observer.SendMovementHeartbeatAt(x+2, y, z, 0); err != nil {
		e2eharness.HarnessFailf(t, "movement packet: %v", err)
	}
	chat(t, observer, ".die", "permits /pov only")
	chat(t, observer, ".pov list", "RPOV\tEND|")
	require(t, observer.Health() > 0 && observer.Money() == money, "observer gameplay state changed")
	// A second GM stays connected while the first leaves, then drops abruptly while watching.
	second := loginObserver(t, db, ids[2])
	a.wait(t, "two leases", func(s snapshot) bool { return s.Observers == 2 })
	chat(t, second, ".pov watch "+target, "RPOV\tSTATE|"+target+"|")
	// Measure through fresh authoritative snapshots; allow 20%% quantization/telemetry jitter, no catch-up burst.
	start := a.frame(t)
	wall := time.Now()
	end := a.wait(t, "1x advances", func(s snapshot) bool { return s.SimMs >= start.SimMs+1500 })
	ratio := float64(end.SimMs-start.SimMs) / float64(time.Since(wall).Milliseconds())
	require(t, ratio <= 1.2, "observer simulation accelerated: %.3fx", ratio)
	if err := observer.SendLogout(); err != nil {
		e2eharness.HarnessFailf(t, "logout: %v", err)
	}
	if err := observer.WaitForLogout(10 * time.Second); err != nil {
		require(t, false, "logout: %v", err)
	}
	charDB, err := e2eharness.OpenCharDB()
	if err != nil {
		e2eharness.HarnessFailf(t, "character DB: %v", err)
	}
	defer charDB.Close()
	var sx, sy, sz float64
	var smap, smoney uint32
	if err := charDB.QueryRow("SELECT position_x,position_y,position_z,map,money FROM characters WHERE guid=?", observer.CharGUID()).Scan(&sx, &sy, &sz, &smap, &smoney); err != nil {
		e2eharness.HarnessFailf(t, "logout save: %v", err)
	}
	require(t, math.Abs(sx-float64(x)) < .1 && math.Abs(sy-float64(y)) < .1 && math.Abs(sz-float64(z)) < .1 && smap == mapID && smoney == money, "blocked movement/money reached logout save")
	// Character selection still owns the world connection.
	s = a.frame(t)
	a.control(t, s, 2, false, 400)
	observer.Close()
	remaining := a.wait(t, "one lease remains", func(s snapshot) bool { return s.Observers == 1 })
	a.control(t, remaining, 2, false, 400)
	second.Close()
	unlocked := a.wait(t, "disconnect releases lease", func(s snapshot) bool { return s.Observers == 0 })
	seq = a.control(t, unlocked, 2, false, 202)
	a.wait(t, "acceleration unlocked", func(s snapshot) bool { return s.ControlSeq == seq && s.Speed == 2 })
	seq = a.control(t, unlocked, initial.Speed, initial.Paused, 202)
	a.wait(t, "restore controls", func(s snapshot) bool {
		return s.ControlSeq == seq && s.Speed == initial.Speed && s.Paused == initial.Paused
	})
	t.Logf("PASS native GM login/watch/switch/stop, ordinary rejection, blocked movement/money, stale mailbox rejection, 1x/pause lock, two GMs, character selection, abrupt disconnect; measured %.3fx", ratio)
}

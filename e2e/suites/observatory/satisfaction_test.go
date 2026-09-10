//go:build e2e

package observatory_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// A freshly seeded, exhausted owner with a strong rest preference must physically rest and persist
// only observed recovery. The client watches; it never directs the bot or changes its fulfillment.
func TestObservatory_SatisfactionProducesObservedRest(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"observatory", "alles", "serial"}, Category: "observatory", Runtime: "med"})
	path := os.Getenv("E2E_OBSERVATORY_SATISFACTION_FIXTURE")
	if path == "" {
		t.Skip("requires a fresh disposable satisfaction fixture; see e2e/README.md")
	}
	var fixture struct {
		Disposable bool
		Name       string
		GUID       uint64
	}
	data, err := os.ReadFile(path)
	if err != nil || json.Unmarshal(data, &fixture) != nil || !fixture.Disposable || fixture.GUID == 0 {
		e2eharness.Preconditionf(t, "invalid satisfaction fixture: %v", err)
	}
	token, err := os.ReadFile(os.Getenv("E2E_OBSERVATORY_TOKEN_FILE"))
	if err != nil {
		e2eharness.Preconditionf(t, "token: %v", err)
	}
	a := api{url: strings.TrimRight(os.Getenv("E2E_OBSERVATORY_URL"), "/"), token: strings.TrimSpace(string(token)), http: &http.Client{Timeout: 5 * time.Second}}
	initial := a.frame(t)
	if !initial.Allowed || initial.Observers != 0 || initial.SimMs > 30000 {
		e2eharness.Preconditionf(t, "requires a fresh realm with no observers before its first recovery")
	}
	authDB, charDB := e2eharness.OpenTestDBs(t)
	id := e2eharness.MakeBotIdents("Sat", 1)[0]
	if err := e2eharness.EnsureAccount(authDB, id.Account, "test"); err != nil {
		e2eharness.Preconditionf(t, "account: %v", err)
	}
	if err := e2eharness.SetGM(authDB, id.Account, 2); err != nil {
		e2eharness.Preconditionf(t, "GM: %v", err)
	}
	t.Cleanup(func() {
		if _, err := authDB.Exec("DELETE aa FROM account_access aa JOIN account a ON a.id=aa.id WHERE a.username=?", id.Account); err != nil {
			t.Errorf("cleanup GM: %v", err)
		}
	})
	observer := loginObserver(t, authDB, id)
	t.Cleanup(func() { observer.Close() })
	chat(t, observer, ".pov watch "+fixture.Name, "RPOV\tSTATE|"+fixture.Name+"|")
	var frame struct {
		Run, Fault string
		Bots       []struct {
			GUID     uint64
			Name     string
			Planning struct {
				Objectives []struct {
					ID, ActivityMs, CompletedMs uint64
					Purpose, State              string
				}
				Satisfaction struct {
					NextRestMs uint64
					Dimensions []struct {
						ID          string
						Fulfillment float64
					}
				}
			}
		}
	}
	seen := false
	var startX, startY float32
	var completedID uint64
	deadline := time.NewTimer(2 * time.Minute)
	defer deadline.Stop()
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for completedID == 0 {
		a.call(t, "/api/snapshot", nil, 200, &frame)
		require(t, frame.Run == initial.Run && frame.Fault == "", "realm changed or faulted")
		for _, bot := range frame.Bots {
			if bot.GUID != fixture.GUID || bot.Name != fixture.Name {
				continue
			}
			for _, intention := range bot.Planning.Objectives {
				if intention.Purpose != "rest" {
					continue
				}
				if object := observer.GetObject(fixture.GUID); object != nil && object.HasKnownPosition() {
					x, y, _ := object.InterpolatedPosition()
					if !seen {
						startX, startY, seen = x, y, true
					}
					require(t, math.Hypot(float64(x-startX), float64(y-startY)) < 5, "resting character traveled")
				}
				if intention.State == "completed" && bot.Planning.Satisfaction.NextRestMs > intention.CompletedMs {
					require(t, seen && intention.ActivityMs == 60000, "rest lacks client visibility or observed duration")
					matched := false
					for _, dimension := range bot.Planning.Satisfaction.Dimensions {
						if dimension.ID == "rest" {
							matched = true
							require(t, dimension.Fulfillment >= 0.45 && dimension.Fulfillment <= 0.51, "expected observed recovery from seeded 0.1: %f", dimension.Fulfillment)
						}
					}
					require(t, matched, "rest dimension missing")
					completedID = intention.ID
				}
			}
		}
		select {
		case <-deadline.C:
			e2eharness.Assertf(t, "preferred local rest did not produce observed recovery")
		case <-tick.C:
		}
	}
	// Wait for normal persistence. A telemetry read cannot serve as a save receipt.
	persisted := false
	until := time.NewTimer(40 * time.Second)
	defer until.Stop()
	for !persisted {
		var payload []byte
		err := charDB.QueryRow("SELECT payload FROM alles_planning WHERE owner_kind=0 AND owner_id=?", fixture.GUID).Scan(&payload)
		if err == nil {
			var saved struct {
				Satisfaction struct{ NextRestMs uint64 }
				Objectives   []struct {
					ID       uint64
					Activity *struct{ ObservedMs, CreditedMs, CompletedMs uint64 }
				}
			}
			if json.Unmarshal(payload, &saved) != nil {
				e2eharness.Assertf(t, "invalid persisted planning payload")
			}
			for _, intention := range saved.Objectives {
				if intention.ID == completedID && intention.Activity != nil {
					evidence := intention.Activity
					persisted = evidence.ObservedMs == 60000 && evidence.CreditedMs == 60000 && evidence.CompletedMs != 0 && saved.Satisfaction.NextRestMs > evidence.CompletedMs
				}
			}
		}
		select {
		case <-until.C:
			e2eharness.Assertf(t, "observed rest receipt was not committed: %v", err)
		case <-tick.C:
		}
	}
	t.Logf("PASS %s: client-observed stationary rest, grounded fulfillment and committed anti-replay receipt", fixture.Name)
}

// A seeded last-observed meeting site can justify travel without quest rewards. Ordinary SAY must reach
// the intended companion, and removing/rejoining the owner must retain the receipt without another gain.
func TestObservatory_SatisfactionSocialTravelAndReload(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"observatory", "alles", "serial"}, Category: "observatory", Runtime: "med"})
	path := os.Getenv("E2E_OBSERVATORY_SATISFACTION_FIXTURE")
	if path == "" {
		t.Skip("requires an exclusive two-bot satisfaction fixture")
	}
	var fixture struct {
		Disposable          bool
		Name, Companion     string
		GUID, CompanionGUID uint64
	}
	data, err := os.ReadFile(path)
	if err != nil || json.Unmarshal(data, &fixture) != nil || !fixture.Disposable || fixture.CompanionGUID == 0 {
		e2eharness.Preconditionf(t, "requires the seeded companion fixture: %v", err)
	}
	token, err := os.ReadFile(os.Getenv("E2E_OBSERVATORY_TOKEN_FILE"))
	if err != nil {
		e2eharness.Preconditionf(t, "token: %v", err)
	}
	a := api{url: strings.TrimRight(os.Getenv("E2E_OBSERVATORY_URL"), "/"), token: strings.TrimSpace(string(token)), http: &http.Client{Timeout: 5 * time.Second}}
	initial := a.wait(t, "previous observer release", func(s snapshot) bool { return s.Observers == 0 })
	if initial.ExpectedBots != 2 {
		e2eharness.Preconditionf(t, "exclusive two-bot fixture required")
	}
	seq := a.observerMode(t, initial, 2, 200)
	a.wait(t, "GM fixture mode", func(s snapshot) bool { return s.ControlSeq >= seq && s.ObserverMode == 2 })
	t.Cleanup(func() { s := a.frame(t); a.observerMode(t, s, initial.ObserverMode, 200) })
	authDB, _ := e2eharness.OpenTestDBs(t)
	id := e2eharness.MakeBotIdents("Satvis", 1)[0]
	if err := e2eharness.EnsureAccount(authDB, id.Account, "test"); err != nil {
		e2eharness.Preconditionf(t, "account: %v", err)
	}
	if err := e2eharness.SetGM(authDB, id.Account, 3); err != nil {
		e2eharness.Preconditionf(t, "GM: %v", err)
	}
	t.Cleanup(func() {
		authDB.Exec("DELETE aa FROM account_access aa JOIN account a ON a.id=aa.id WHERE a.username=?", id.Account)
	})
	observer := loginObserver(t, authDB, id)
	t.Cleanup(func() { observer.Close() })
	chat(t, observer, ".pov watch "+fixture.Name, "RPOV\tSTATE|"+fixture.Name+"|")
	heard := make(chan struct{}, 1)
	cancel := observer.AddPacketHook(func(op uint16, b []byte) {
		if op == client.SmsgMessageChat && bytes.Contains(b, []byte("Hello, "+fixture.Companion+". It is good to see you.")) {
			select {
			case heard <- struct{}{}:
			default:
			}
		}
	})
	defer cancel()
	chat(t, observer, fmt.Sprintf(".alles motive player %d companionship 10 0.3 1", fixture.GUID), "Motive updated")
	chat(t, observer, fmt.Sprintf(".alles motive player %d rest 0 0.6 1", fixture.GUID), "Motive updated")
	type view struct {
		GUID     uint64
		Planning struct {
			Body         struct{ Generation uint64 }
			Satisfaction struct {
				NextSocialMs uint64
				Dimensions   []struct {
					ID          string
					Fulfillment float64
				}
			}
			Objectives []struct {
				ID, CompletedMs uint64
				Purpose, State  string
			}
		}
	}
	sample := func() view {
		var frame struct {
			Run, Fault string
			Bots       []view
		}
		a.call(t, "/api/snapshot", nil, 200, &frame)
		require(t, frame.Run == initial.Run && frame.Fault == "", "realm changed or faulted")
		for _, bot := range frame.Bots {
			if bot.GUID == fixture.GUID {
				return bot
			}
		}
		return view{}
	}
	fulfillment := func(v view) float64 {
		for _, dimension := range v.Planning.Satisfaction.Dimensions {
			if dimension.ID == "companionship" {
				return dimension.Fulfillment
			}
		}
		return -1
	}
	before := sample()
	if fulfillment(before) < 0 || before.Planning.Satisfaction.NextSocialMs != 0 {
		e2eharness.Preconditionf(t, "expected unmet social activity")
	}
	var x0, y0 float32
	seen, moved := false, false
	var completed view
	deadline := time.NewTimer(2 * time.Minute)
	defer deadline.Stop()
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for completed.GUID == 0 {
		if obj := observer.GetObject(fixture.GUID); obj != nil && obj.HasKnownPosition() {
			x, y, _ := obj.InterpolatedPosition()
			if !seen {
				x0, y0, seen = x, y, true
			}
			moved = moved || math.Hypot(float64(x-x0), float64(y-y0)) > 20
		}
		v := sample()
		for _, o := range v.Planning.Objectives {
			if o.Purpose == "companionship" && o.State == "completed" && v.Planning.Satisfaction.NextSocialMs > o.CompletedMs {
				completed = v
			}
		}
		select {
		case <-deadline.C:
			e2eharness.Assertf(t, "social travel did not reach its evidenced companion; seen=%t moved=%t", seen, moved)
		case <-tick.C:
		}
	}
	require(t, moved, "social intention did not produce client-observed travel")
	require(t, fulfillment(completed) > fulfillment(before)+0.25, "interaction did not produce observed companionship")
	select {
	case <-heard:
	case <-time.After(3 * time.Second):
		e2eharness.Assertf(t, "normal social greeting never reached the observing client")
	}
	chat(t, observer, ".pov stop", "RPOV\tSTOP")
	// Population control goes through the real world admission/logout path, keeping the simulation clock alive.
	control := func(count int) {
		var result struct{ Sequence uint64 }
		a.call(t, "/api/control", map[string]any{"run": initial.Run, "speed": 1, "paused": false, "bots": count}, 200, &result)
		a.wait(t, "population change", func(s snapshot) bool { return s.ControlSeq >= result.Sequence && len(s.Bots) == count })
	}
	control(0)
	control(2)
	var restored view
	a.wait(t, "owner reloaded", func(s snapshot) bool {
		restored = sample()
		return restored.GUID == fixture.GUID && restored.Planning.Body.Generation != 0 &&
			restored.Planning.Body.Generation != completed.Planning.Body.Generation && fulfillment(restored) >= 0
	})
	require(t, restored.GUID == fixture.GUID && restored.Planning.Body.Generation != completed.Planning.Body.Generation,
		"source owner did not load a new generation after ordinary logout/rejoin")
	require(t, restored.Planning.Satisfaction.NextSocialMs == completed.Planning.Satisfaction.NextSocialMs,
		"social cooldown did not survive owner reload")
	require(t, fulfillment(restored) <= fulfillment(completed)+0.005 && fulfillment(restored) > fulfillment(completed)-0.03,
		"reload replayed fulfillment or invented offline activity: before=%f after=%f", fulfillment(completed), fulfillment(restored))
	t.Logf("PASS %s: walked to %s, ordinary greeting, grounded fulfillment and receipt preserved through owner reload", fixture.Name, fixture.Companion)
}

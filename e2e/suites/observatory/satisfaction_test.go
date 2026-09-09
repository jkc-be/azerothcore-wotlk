//go:build e2e

package observatory_test

import (
	"encoding/json"
	"math"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"

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

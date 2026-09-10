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

// PR: https://github.com/jkc-be/azerothcore-wotlk/pull/28
// An exclusive fresh cohort uses individual motivations, acts through the ordinary client-visible body,
// and persists measured outcomes. No preference, money or equipment is changed by the observing client.
func TestObservatory_IndividualMotivationsLearnObservedOutcomes(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"observatory", "alles", "serial"}, Category: "observatory", Runtime: "med"})
	path := os.Getenv("E2E_OBSERVATORY_MOTIVATIONS_FIXTURE")
	if path == "" {
		t.Skip("requires an exclusive fresh cohort with individual motivations; see e2e/README.md")
	}
	var fixture struct {
		Disposable bool
		Name       string
		GUID       uint64
	}
	data, err := os.ReadFile(path)
	if err != nil || json.Unmarshal(data, &fixture) != nil || !fixture.Disposable || fixture.GUID == 0 {
		e2eharness.Preconditionf(t, "invalid motivation fixture: %v", err)
	}
	token, err := os.ReadFile(os.Getenv("E2E_OBSERVATORY_TOKEN_FILE"))
	if err != nil {
		e2eharness.Preconditionf(t, "token: %v", err)
	}
	a := api{url: strings.TrimRight(os.Getenv("E2E_OBSERVATORY_URL"), "/"),
		token: strings.TrimSpace(string(token)), http: &http.Client{Timeout: 5 * time.Second}}
	initial := a.frame(t)
	if !initial.Allowed || initial.Observers != 0 {
		e2eharness.Preconditionf(t, "requires an exclusive observer-enabled realm")
	}
	authDB, charDB := e2eharness.OpenTestDBs(t)
	id := e2eharness.MakeBotIdents("Motiv", 1)[0]
	if err := e2eharness.EnsureAccount(authDB, id.Account, "test"); err != nil {
		e2eharness.Preconditionf(t, "account: %v", err)
	}
	if err := e2eharness.SetGM(authDB, id.Account, 2); err != nil {
		e2eharness.Preconditionf(t, "GM: %v", err)
	}
	t.Cleanup(func() {
		if _, err := authDB.Exec("DELETE aa FROM account_access aa JOIN account a ON a.id=aa.id WHERE a.username=?",
			id.Account); err != nil {
			t.Errorf("cleanup GM: %v", err)
		}
	})
	observer := loginObserver(t, authDB, id)
	t.Cleanup(func() { observer.Close() })
	chat(t, observer, ".pov watch "+fixture.Name, "RPOV\tSTATE|"+fixture.Name+"|")
	type dimension struct {
		ID, Curve                           string
		Fulfillment, Weight, Scale, Urgency float64
	}
	type effect struct {
		Samples uint64
		Mean    float64
	}
	type experience struct {
		Samples        uint64
		Effects        map[string]effect
		FailureEffects map[string]effect
	}
	var frame struct {
		Run, Fault string
		Bots       []struct {
			GUID         uint64
			Name         string
			Money        float64
			ControlGroup bool
			Planning     struct {
				Satisfaction struct {
					Dimensions  []dimension
					Experiences map[string]experience
					Contexts    map[string]experience
				}
			}
		}
	}
	seen, moved, matchedMoney := false, false, false
	var x0, y0 float32
	var retained uint64
	var weights map[string]float64
	deadline := time.NewTimer(4 * time.Minute)
	defer deadline.Stop()
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for {
		a.call(t, "/api/snapshot", nil, 200, &frame)
		require(t, frame.Run == initial.Run && frame.Fault == "", "realm changed or faulted")
		profiles := map[float64]bool{}
		for _, bot := range frame.Bots {
			if bot.ControlGroup {
				continue
			}
			for _, motive := range bot.Planning.Satisfaction.Dimensions {
				if motive.ID == "security" || motive.ID == "rest" {
					require(t, motive.Urgency > 0, "survival/recovery urgency is absent")
				}
				if motive.ID == "wealth" {
					require(t, motive.Curve == "growth" && motive.Scale > 0, "wealth ambition is absent")
					profiles[motive.Weight] = true
				}
			}
			if bot.GUID != fixture.GUID || bot.Name != fixture.Name {
				continue
			}
			weights = map[string]float64{}
			for _, motive := range bot.Planning.Satisfaction.Dimensions {
				weights[motive.ID] = motive.Weight
				if motive.ID == "wealth" && motive.Fulfillment == bot.Money {
					matchedMoney = true
				}
			}
			if object := observer.GetObject(bot.GUID); object != nil && object.HasKnownPosition() {
				x, y, _ := object.InterpolatedPosition()
				if !seen {
					x0, y0, seen = x, y, true
				}
				moved = moved || math.Hypot(float64(x-x0), float64(y-y0)) > 5
			}
			retained = 0
			for _, outcome := range bot.Planning.Satisfaction.Contexts {
				for _, effects := range []map[string]effect{outcome.Effects, outcome.FailureEffects} {
					if effects["wealth"].Samples > 0 && effects["equipment"].Samples > 0 {
						retained += outcome.Samples
					}
				}
			}
		}
		if moved && matchedMoney && retained > 0 && len(profiles) > 1 {
			var payload []byte
			err := charDB.QueryRow("SELECT payload FROM alles_planning WHERE owner_kind=0 AND owner_id=?",
				fixture.GUID).Scan(&payload)
			if err == nil {
				var saved struct {
					Version      int
					Satisfaction struct {
						Dimensions []struct {
							ID     string
							Weight float64
						}
						Contexts []struct {
							Effects        map[string]effect
							FailureEffects map[string]effect
						}
					}
				}
				require(t, json.Unmarshal(payload, &saved) == nil, "invalid committed planning payload")
				persisted := false
				for _, outcome := range saved.Satisfaction.Contexts {
					persisted = persisted || outcome.Effects["wealth"].Samples > 0 ||
						outcome.FailureEffects["wealth"].Samples > 0
				}
				if saved.Version == 13 && persisted {
					for _, motive := range saved.Satisfaction.Dimensions {
						require(t, weights[motive.ID] == motive.Weight, "individual preference changed on save")
					}
					t.Logf("PASS %s: native movement, distinct personal priorities, actual wealth and committed learned outcomes",
						fixture.Name)
					return
				}
			}
		}
		select {
		case <-deadline.C:
			e2eharness.Assertf(t, "missing observed learning: seen=%t moved=%t wealth=%t attempts=%d profiles=%d",
				seen, moved, matchedMoney, retained, len(profiles))
		case <-tick.C:
		}
	}
}

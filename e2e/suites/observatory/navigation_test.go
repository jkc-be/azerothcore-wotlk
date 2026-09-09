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

// An exclusive, freshly seeded disposable realm places the two managed bots at the reported failure
// positions, with active quest/place intentions. GM clients only watch; the bodies must do the work.
func TestObservatory_NavigationRecoveryAndAreaSearch(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"observatory", "navigation", "serial"},
		Category: "observatory", Runtime: "med"})
	path := os.Getenv("E2E_OBSERVATORY_NAVIGATION_FIXTURE")
	if path == "" {
		t.Skip("requires a fresh disposable navigation fixture; see e2e/README.md")
	}
	var fixture struct {
		Disposable bool
		Bots       []struct {
			Name         string
			GUID         uint64
			Quest, Place uint32
		}
	}
	data, err := os.ReadFile(path)
	if err != nil || json.Unmarshal(data, &fixture) != nil || !fixture.Disposable || len(fixture.Bots) != 2 {
		e2eharness.Preconditionf(t, "invalid navigation fixture: %v", err)
	}
	token, err := os.ReadFile(os.Getenv("E2E_OBSERVATORY_TOKEN_FILE"))
	if err != nil {
		e2eharness.Preconditionf(t, "token: %v", err)
	}
	a := api{url: strings.TrimRight(os.Getenv("E2E_OBSERVATORY_URL"), "/"),
		token: strings.TrimSpace(string(token)), http: &http.Client{Timeout: 5 * time.Second}}
	initial := a.frame(t)
	if !initial.Allowed || initial.Observers != 0 || initial.SimMs > 60000 {
		e2eharness.Preconditionf(t, "requires a newly started realm with no observers")
	}
	db, err := e2eharness.OpenAuthDB()
	if err != nil {
		e2eharness.Preconditionf(t, "auth: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	ids := e2eharness.MakeBotIdents("Nav", 2)
	type objective struct {
		ID                            uint64
		Quest, Place, DiscoveredQuest uint32
		State, Step                   string
	}
	type botView struct {
		GUID     uint64
		Name     string
		Planning struct {
			Objectives []objective
			Body       struct {
				Objective uint64
				Skill     string
			}
		}
	}
	for i, target := range fixture.Bots {
		if err := e2eharness.EnsureAccount(db, ids[i].Account, "test"); err != nil {
			e2eharness.Preconditionf(t, "account: %v", err)
		}
		if err := e2eharness.SetGM(db, ids[i].Account, 2); err != nil {
			e2eharness.Preconditionf(t, "GM: %v", err)
		}
		account := ids[i].Account
		t.Cleanup(func() {
			_, err := db.Exec("DELETE aa FROM account_access aa JOIN account a ON a.id=aa.id WHERE a.username=?", account)
			if err != nil {
				t.Errorf("cleanup GM: %v", err)
			}
		})
		observer := loginObserver(t, db, ids[i])
		chat(t, observer, ".pov watch "+target.Name, "RPOV\tSTATE|"+target.Name+"|")
		t.Run(target.Name, func(t *testing.T) {
			t.Parallel() // Two read-only POVs observe the same exclusive fixture from its first seconds.
			deadline := time.NewTimer(3 * time.Minute)
			defer deadline.Stop()
			tick := time.NewTicker(250 * time.Millisecond)
			defer tick.Stop()
			var startX, startY float32
			seen, moved, investigating := false, false, false
			returns := 0
			lastSkill := ""
			for {
				var frame struct {
					Run, Fault string
					Bots       []botView
				}
				a.call(t, "/api/snapshot", nil, 200, &frame)
				require(t, frame.Run == initial.Run && frame.Fault == "", "realm changed or faulted")
				if object := observer.GetObject(target.GUID); object != nil && object.HasKnownPosition() {
					x, y, _ := object.InterpolatedPosition()
					if !seen {
						startX, startY, seen = x, y, true
					}
					moved = moved || math.Hypot(float64(x-startX), float64(y-startY)) > 20
				}
				for _, bot := range frame.Bots {
					if bot.GUID != target.GUID {
						continue
					}
					body := bot.Planning.Body
					if body.Objective == 1 {
						if body.Skill == "investigate" {
							investigating = true
						}
						if investigating && body.Skill == "travel" && lastSkill != "travel" {
							returns++
						}
						lastSkill = body.Skill
					}
					require(t, returns <= 1, "area search repeatedly returned to travel: %d", returns)
					for _, objective := range bot.Planning.Objectives {
						if objective.ID != 1 {
							continue
						}
						if objective.State == "completed" {
							require(t, seen && moved, "no client-observed movement during recovery")
							require(t, objective.Quest == target.Quest && objective.Place == target.Place, "fixture intention changed")
							if target.Place != 0 {
								require(t, objective.DiscoveredQuest != 0, "search completed without accepting work")
							}
							t.Logf("PASS %s: client-observed movement, objective completed, travel reversals=%d", target.Name, returns)
							return
						}
					}
				}
				select {
				case <-deadline.C:
					e2eharness.Assertf(t, "%s did not complete the seeded intention; seen=%t moved=%t reversals=%d",
						target.Name, seen, moved, returns)
				case <-tick.C:
				}
			}
		})
	}
}

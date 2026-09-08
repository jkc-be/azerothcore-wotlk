//go:build e2e

package alles_test

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"io"
	"os"
	"regexp"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	_ "github.com/go-sql-driver/mysql"
)

type actorFixture struct {
	Role      string `json:"role"`
	Account   string `json:"account"`
	Name      string `json:"name"`
	GUID      uint64 `json:"guid"`
	accountID uint32
}

type fixture struct {
	Version    int            `json:"version"`
	Disposable bool           `json:"disposable"`
	Formation  string         `json:"formation"`
	Actors     []actorFixture `json:"actors"`
}

// NewScenario creates random GUIDs, which cannot be in Alles.Owners before startup.
// This opt-in fixture therefore authenticates only the exact pre-provisioned disposable accounts.
func loadFixture(t *testing.T) fixture {
	t.Helper()
	path := os.Getenv("E2E_ALLES_FIXTURE")
	if path == "" {
		t.Skip("alles requires E2E_ALLES_FIXTURE and an exclusive configured disposable realm")
	}
	f, err := os.Open(path)
	if err != nil {
		e2eharness.Preconditionf(t, "alles fixture: %v", err)
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, 16385))
	if err != nil || len(b) > 16384 {
		e2eharness.Preconditionf(t, "alles fixture exceeds bounds or cannot be read: %v", err)
	}
	var result fixture
	decoder := json.NewDecoder(bytes.NewReader(b))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&result); err != nil {
		e2eharness.Preconditionf(t, "alles fixture JSON: %v", err)
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		e2eharness.Preconditionf(t, "alles fixture must contain exactly one JSON object")
	}
	if result.Version != 1 || !result.Disposable || len(result.Actors) != 3 ||
		(result.Formation != "fake" && result.Formation != "fallback") {
		e2eharness.Preconditionf(t,
			"alles fixture requires version 1, disposable=true, three actors and a formation mode")
	}
	accounts := make(map[string]bool)
	guids := make(map[uint64]bool)
	roles := map[string]bool{"speaker": false, "listener": false, "distant": false}
	for _, actor := range result.Actors {
		used, known := roles[actor.Role]
		if !known || used || accounts[actor.Account] || guids[actor.GUID] || actor.GUID == 0 ||
			actor.GUID > 0xffffffff || !regexp.MustCompile(`^ALLESE2E[A-Z0-9]{1,9}$`).MatchString(actor.Account) ||
			!regexp.MustCompile(`^[A-Za-z]{2,12}$`).MatchString(actor.Name) {
			e2eharness.Preconditionf(t, "invalid or duplicate alles fixture identity/role")
		}
		roles[actor.Role], accounts[actor.Account], guids[actor.GUID] = true, true, true
	}
	return result
}

func checkFixtureIdentity(t *testing.T, authDB, charDB *sql.DB, actor *actorFixture) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := authDB.QueryRowContext(ctx, "SELECT id FROM account WHERE username=?", actor.Account).
		Scan(&actor.accountID); err != nil {
		e2eharness.Preconditionf(t, "pre-provisioned fixture account: %v", err)
	}
	var count int
	var name string
	var race, class, online int
	if err := charDB.QueryRowContext(ctx, "SELECT COUNT(*) FROM characters WHERE account=?", actor.accountID).
		Scan(&count); err != nil || count != 1 {
		e2eharness.Preconditionf(t, "fixture account must own exactly one character: count=%d err=%v", count, err)
	}
	if err := charDB.QueryRowContext(ctx,
		"SELECT name, race, class, online FROM characters WHERE guid=? AND account=?", actor.GUID, actor.accountID).
		Scan(&name, &race, &class, &online); err != nil || name != actor.Name ||
		race != int(e2eharness.RaceHuman) || class != int(e2eharness.ClassWarrior) || online != 0 {
		e2eharness.Preconditionf(t, "fixture must be the exact offline Human Warrior: role=%s err=%v", actor.Role, err)
	}
	if actor.Role == "speaker" {
		// Player::Say selects SMSG_GM_MESSAGECHAT from permission 372, even with .gm chat off.
		// Keep the GM security needed for setup/status, but require the disposable sender's preexisting deny.
		var denied, granted int
		err := authDB.QueryRowContext(ctx,
			`SELECT COUNT(CASE WHEN granted=0 AND realmId=-1 THEN 1 END),
			 COUNT(CASE WHEN granted<>0 THEN 1 END)
			 FROM rbac_account_permissions WHERE accountId=? AND permissionId=372`, actor.accountID).
			Scan(&denied, &granted)
		if err != nil || denied != 1 || granted != 0 {
			e2eharness.Preconditionf(t,
				"speaker requires pre-provisioned global RBAC deny 372 with no conflicting grant: %v", err)
		}
	}
}

func loginFixture(t *testing.T, authDB, charDB *sql.DB, actor actorFixture) *e2eharness.ScenarioBot {
	t.Helper()
	return loginFixtureAccount(t, authDB, charDB, actor, true)
}

// The objective takeover fixture may have offline siblings in its numbered bot account.
// Always select the exact authenticated character; ordinary fixtures still require a single character.
func loginFixtureAccount(t *testing.T, authDB, charDB *sql.DB, actor actorFixture,
	singleCharacter bool) *e2eharness.ScenarioBot {
	t.Helper()
	auth := client.NewAuthClient(actor.Account, e2eharness.DefaultPassword)
	realms, err := auth.Authenticate(e2eharness.AuthAddr)
	if err != nil || len(realms) != 1 {
		e2eharness.Preconditionf(t, "alles requires one reachable fixture realm: %v", err)
	}
	// The pinned v1.0.8 auth client sends a leading-NUL OS FourCC. Match the existing observatory fixture's
	// account-scoped workaround after SRP; keep Warden enabled and never update accounts outside this manifest.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	_, err = authDB.ExecContext(ctx, "UPDATE account SET os='Win' WHERE id=? AND username=?",
		actor.accountID, actor.Account)
	cancel()
	if err != nil {
		e2eharness.Preconditionf(t, "fixture client OS metadata: %v", err)
	}
	w := client.NewWorldClient(actor.Account, auth.SessionKey(), func(string, ...interface{}) {})
	w.SetCharRace(e2eharness.RaceHuman)
	t.Cleanup(w.Close)
	characters := make(chan []client.CharEnumEntry, 1)
	var enumOverflow atomic.Bool
	w.OnCharList = func(entries []client.CharEnumEntry) {
		select {
		case characters <- append([]client.CharEnumEntry(nil), entries...):
		default:
			enumOverflow.Store(true)
		}
	}
	if err := w.Connect(realms[0].Address); err != nil {
		e2eharness.Preconditionf(t, "fixture world connection: %v", err)
	}
	go func() { _ = w.Run() }()
	if err := w.WaitForSessionPhase(client.PhaseAuthed, 15*time.Second); err != nil {
		e2eharness.Preconditionf(t, "fixture realm authentication: %v", err)
	}
	if err := w.SendReadyForAccountDataTimes(); err != nil {
		e2eharness.HarnessFailf(t, "fixture ready: %v", err)
	}
	if err := w.RequestCharList(); err != nil {
		e2eharness.HarnessFailf(t, "fixture character enum: %v", err)
	}
	select {
	case entries := <-characters:
		matches := 0
		for _, entry := range entries {
			if entry.GUID == actor.GUID && entry.Name == actor.Name && entry.Race == e2eharness.RaceHuman &&
				entry.Class == e2eharness.ClassWarrior {
				matches++
			}
		}
		if enumOverflow.Load() || (singleCharacter && len(entries) != 1) || matches != 1 {
			e2eharness.Preconditionf(t, "realm returned a different fixture character")
		}
	case <-time.After(15 * time.Second):
		e2eharness.Preconditionf(t, "fixture character enum timed out")
	}
	if err := w.LoginCharacter(actor.GUID); err != nil {
		e2eharness.HarnessFailf(t, "fixture character login: %v", err)
	}
	if err := w.WaitForLogin(30 * time.Second); err != nil {
		e2eharness.Preconditionf(t, "fixture world entry: %v", err)
	}
	if err := w.WaitForSessionPhase(client.PhaseInWorld, 10*time.Second); err != nil {
		e2eharness.Preconditionf(t, "fixture never entered the gameplay session phase: %v", err)
	}
	bot := &e2eharness.ScenarioBot{
		Session: &e2eharness.Session{World: w, GUID: actor.GUID, Name: actor.Name, User: actor.Account},
		AuthDB:  authDB,
		CharDB:  charDB,
		Role:    actor.Role,
		Ident: e2eharness.BotIdent{
			Account:  actor.Account,
			CharName: actor.Name,
			Race:     e2eharness.RaceHuman,
			Class:    e2eharness.ClassWarrior,
		},
	}
	t.Cleanup(func() {
		if !w.IsStopped() {
			if err := w.SendLogout(); err != nil {
				t.Errorf("harness: fixture logout could not be sent: %v", err)
			} else if err := w.WaitForLogout(30 * time.Second); err != nil {
				t.Errorf("harness: fixture logout did not complete: %v", err)
			} else if w.SessionPhase() != client.PhaseLogout {
				t.Errorf("harness: fixture stopped without SMSG_LOGOUT_COMPLETE")
			}
		}
	})
	t.Logf("fixture entered: role=%s name=%s guid=%d", actor.Role, actor.Name, actor.GUID)
	return bot
}

func field(line, name string) string {
	for _, value := range strings.Fields(line) {
		if strings.HasPrefix(value, name+"=") {
			return strings.TrimPrefix(value, name+"=")
		}
	}
	return ""
}

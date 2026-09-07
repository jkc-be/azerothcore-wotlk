//go:build e2e

package alles_test

import (
	"context"
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"math"
	"testing"
	"time"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

type heardMemory struct {
	ID, ContentRevision, Formed    uint64
	Kind, Formation                uint32
	SourceKind, SourceID, Depth    sql.NullInt64
	SourceName, Claim, Attribution string
	Confidence                     float64
}

func readHeard(t *testing.T, bot *e2eharness.ScenarioBot, speaker uint64, marker string) heardMemory {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	rows, err := bot.CharDB.QueryContext(ctx,
		`SELECT memory_id, content_revision, formed_game_time_ms, kind, formation_mode,
		 source_kind, source_id, reported_depth, source_name, claim, attribution, confidence
		 FROM alles_memory WHERE owner_kind=0 AND owner_id=? AND source_kind=0 AND source_id=?
		 AND INSTR(claim, ?) > 0 LIMIT 2`, bot.GUID, speaker, marker)
	if err != nil {
		e2eharness.HarnessFailf(t, "read acknowledged heard memory: %v", err)
	}
	defer rows.Close()
	var result heardMemory
	if !rows.Next() {
		if err := rows.Err(); err != nil {
			e2eharness.HarnessFailf(t, "heard memory rows: %v", err)
		}
		e2eharness.Assertf(t, "self-recalled heard memory is absent from the acknowledged snapshot")
	}
	if err := rows.Scan(&result.ID, &result.ContentRevision, &result.Formed, &result.Kind, &result.Formation,
		&result.SourceKind, &result.SourceID, &result.Depth, &result.SourceName, &result.Claim,
		&result.Attribution, &result.Confidence); err != nil {
		e2eharness.HarnessFailf(t, "heard memory decode: %v", err)
	}
	if rows.Next() {
		e2eharness.Assertf(t, "one emitted fixture claim produced duplicate retained memories")
	}
	if err := rows.Err(); err != nil {
		e2eharness.HarnessFailf(t, "heard memory iteration: %v", err)
	}
	return result
}

func assertNoDistantInput(t *testing.T, bot *e2eharness.ScenarioBot, speaker uint64, marker string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var count uint64
	err := bot.CharDB.QueryRowContext(ctx,
		`SELECT
		 (SELECT COUNT(*) FROM alles_memory WHERE owner_kind=0 AND owner_id=?
		  AND source_kind=0 AND source_id=? AND INSTR(claim, ?) > 0) +
		 (SELECT COUNT(*) FROM alles_perception WHERE owner_kind=0 AND owner_id=?
		  AND source_kind=0 AND source_id=? AND INSTR(gated_text, ?) > 0)`,
		bot.GUID, speaker, marker, bot.GUID, speaker, marker).Scan(&count)
	if err != nil {
		e2eharness.HarnessFailf(t, "read distant acknowledged snapshot: %v", err)
	}
	if count != 0 {
		e2eharness.Assertf(t, "distant actor retained %d unseen memories/perceptions", count)
	}
}

func distantPad(t *testing.T, near e2eharness.Position3) e2eharness.Position3 {
	t.Helper()
	for _, candidate := range e2eharness.IsolationPads {
		p := candidate.Pos
		if p.Map == near.Map && e2eharness.Distance3D(p.X, p.Y, p.Z, near.X, near.Y, near.Z) >= 500 {
			return p
		}
	}
	e2eharness.Preconditionf(t, "no existing isolation pad at least 500 yards away on the same map")
	return near
}

// mod-alles W2/W3/W6: ordinary heard text becomes locally attributed hearsay and survives actual cache eviction
// plus CMSG_PLAYER_LOGIN. A same-map distant actor must retain neither the line nor a pending perception of it.
// This is a deterministic persistence oracle; the six-actor watched death-to-rumor scene remains a separate gate.
func TestAlles_HeardMemorySurvivesRelog(t *testing.T) {
	meta.Begin(t, meta.TestMeta{Tags: []string{"med", "alles", "serial"}, Runtime: "med", Category: "alles"})
	f := loadFixture(t)
	authDB, charDB := e2eharness.OpenTestDBs(t)
	for i := range f.Actors {
		checkFixtureIdentity(t, authDB, charDB, &f.Actors[i])
	}
	var bots []*e2eharness.ScenarioBot
	var listenerFixture actorFixture
	for _, actor := range f.Actors {
		bots = append(bots, loginFixture(t, authDB, charDB, actor))
		if actor.Role == "listener" {
			listenerFixture = actor
		}
	}
	speaker := e2eharness.ByRole(t, bots, "speaker")
	listener := e2eharness.ByRole(t, bots, "listener")
	distant := e2eharness.ByRole(t, bots, "distant")
	pad := e2eharness.PackagePad(t)
	for _, bot := range bots {
		bot.TeleportPad(t, pad)
		bot.AssertNearPad(t, pad, 5)
		bot.CombatReady(t)
		bot.GM(t, ".gm visible on")
		// The explicit status response also fences that session's preceding setup commands.
		if !eventually(15*time.Second, func() bool { return field(status(t, bot, bot.GUID), "state") == "ready" }) {
			e2eharness.Preconditionf(t, "configured alles owner %d never became ready", bot.GUID)
		}
	}
	listener.WaitUnitGUID(t, speaker.GUID, 10*time.Second)
	distant.WaitUnitGUID(t, speaker.GUID, 10*time.Second)
	var nonce [8]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		e2eharness.HarnessFailf(t, "fixture nonce: %v", err)
	}
	marker := "alles" + hex.EncodeToString(nonce[:])
	// The warmup proves both receivers work before the distant actor moves. It also gives its later flush a
	// real, nonzero revision; an empty/unloaded owner cannot falsely acknowledge persistence with revision zero.
	control := marker + "control"
	nearSeen, _, cancelNear := watchSay(listener.World, speaker.GUID, control)
	farSeen, _, cancelFar := watchSay(distant.World, speaker.GUID, control)
	defer cancelNear()
	defer cancelFar()
	say(t, speaker, "I saw the "+control+" banner.")
	waitSay(t, nearSeen)
	waitSay(t, farSeen)
	cancelNear()
	cancelFar()
	waitRecall(t, listener, control)
	waitRecall(t, distant, control)
	distantLocation := distantPad(t, pad)
	distant.TeleportPad(t, distantLocation)
	distant.AssertNearPad(t, distantLocation, 5)
	// Recheck client position/phase through a command barrier before sending the challenged line.
	status(t, distant, distant.GUID)
	target := marker + "report"
	delivered, _, cancelDelivered := watchSay(listener.World, speaker.GUID, target)
	_, distantPackets, cancelDistant := watchSay(distant.World, speaker.GUID, target)
	defer cancelDelivered()
	defer cancelDistant()
	say(t, speaker, "I saw the "+target+" banner fall.")
	waitSay(t, delivered)
	waitRecall(t, listener, target)
	flushOwner(t, listener)
	before := readHeard(t, listener, speaker.GUID, target)
	expectedFormation := uint32(1) // FormationMode::InProcessFake
	if f.Formation == "fallback" {
		expectedFormation = 3 // FormationMode::Fallback
	}
	if before.Kind != 0 || before.Formation != expectedFormation || before.ID == 0 || before.Formed == 0 ||
		!before.SourceKind.Valid || before.SourceKind.Int64 != 0 || !before.SourceID.Valid ||
		before.SourceID.Int64 != int64(speaker.GUID) || before.SourceName != speaker.Name ||
		before.Attribution != speaker.Name || !before.Depth.Valid || before.Depth.Int64 != 1 ||
		math.IsNaN(before.Confidence) || math.IsInf(before.Confidence, 0) ||
		before.Confidence <= 0 || before.Confidence > 0.6 {
		e2eharness.Assertf(t,
			"heard claim lost local source/kind/attribution/confidence or expected formation: %+v", before)
	}
	flushOwner(t, distant)
	assertNoDistantInput(t, distant, speaker.GUID, target)
	if distantPackets.Load() != 0 {
		e2eharness.Assertf(t, "same-map distant client received the challenged SAY packet")
	}
	generation := number(t, status(t, listener, listener.GUID), "generation")
	if err := listener.World.SendLogout(); err != nil {
		e2eharness.HarnessFailf(t, "listener logout: %v", err)
	}
	if err := listener.World.WaitForLogout(30 * time.Second); err != nil {
		e2eharness.Assertf(t, "listener graceful logout did not complete: %v", err)
	}
	if listener.World.SessionPhase() != client.PhaseLogout {
		e2eharness.Assertf(t, "listener disconnected without the server acknowledging graceful logout")
	}
	listener.Close()
	if !eventually(15*time.Second, func() bool {
		return field(status(t, speaker, listener.GUID), "state") == "unloaded"
	}) {
		e2eharness.Assertf(t, "clean logged-out owner did not evict; persistence cannot be proved by retained RAM")
	}
	listener = loginFixture(t, authDB, charDB, listenerFixture)
	if !eventually(15*time.Second, func() bool {
		line := status(t, listener, listener.GUID)
		return field(line, "state") == "ready" && number(t, line, "generation") > generation
	}) {
		e2eharness.Assertf(t, "listener did not load a new owner generation after cache eviction")
	}
	waitRecall(t, listener, target)
	flushOwner(t, listener)
	after := readHeard(t, listener, speaker.GUID, target)
	if before != after {
		e2eharness.Assertf(t,
			"relog changed persistent memory identity/provenance: before=%+v after=%+v", before, after)
	}
	listener.AssertWorldAlive(t)
	t.Logf("PASS heard memory %d survived eviction/relog; source=%d confidence=%g formation=%d; distant actor unaware",
		before.ID, speaker.GUID, before.Confidence, before.Formation)
}

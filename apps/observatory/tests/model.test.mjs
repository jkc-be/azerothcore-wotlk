import test from "node:test";
import assert from "node:assert/strict";
import {
  summarize,
  visibleBots,
  worldToScreen,
  fitView,
  duration,
  retainedHistory,
  observationAge,
} from "../web/model.js";
const bots = [
  { id: "a", map: 0, zone: 12, level: 2, earnedXp: 120, questCompletions: 2, deaths: 1, x: 100, y: 50 },
  { id: "b", map: 1, zone: 14, level: 3, earnedXp: 250, questCompletions: 3, deaths: 0, x: 200, y: 150 },
];
test("progression uses cumulative authoritative counters across level XP resets", () => {
  const result = summarize({ simMs: 80000, bots });
  assert.equal(result.xp, 370);
  assert.equal(result.quests, 5);
  assert.deepEqual(result.levels, { "Level 2": 1, "Level 3": 1 });
});
test("separate maps cannot accidentally share a coordinate plane", () => {
  assert.deepEqual(visibleBots({ bots }, "0", "all"), [bots[0]]);
  assert.deepEqual(visibleBots({ bots }, "0", "14"), []);
});
test("view fitting centers world coordinates with WoW north/east axes", () => {
  const fit = fitView(bots);
  assert.deepEqual(worldToScreen({ x: fit.x, y: fit.y }, { ...fit, scale: 1 }, 800, 600), { x: 400, y: 300 });
  assert.deepEqual(worldToScreen({ x: fit.x + 10, y: fit.y + 10 }, { ...fit, scale: 1 }, 800, 600), {
    x: 390,
    y: 290,
  });
});
test("simulated time labels retain whole days", () => assert.equal(duration(86400000), "24h 0m 0s"));

test("run totals retain departed bots' progression", () => {
  const snapshot = { simMs: 100, bots: [], runTotals: { xp: 1200, quests: 3, deaths: 2 } };
  assert.deepEqual(summarize(snapshot), { simMs: 100, xp: 1200, quests: 3, deaths: 2, levels: {}, zones: {} });
});

test("Python history preserves unknown progression and restores only this run in sequence", () => {
  const frame = {
    run: "python",
    seq: 4,
    simMs: 100,
    source: "python-api",
    runTotals: { kills: 3, deaths: 1, levelGains: 0 },
    bots: [
      {
        id: "42",
        map: 0,
        instance: 2,
        zone: null,
        level: 1,
        health: 50,
        maxHealth: 100,
        alive: true,
        combat: true,
        deaths: 0,
      },
    ],
  };
  const result = summarize(frame);
  assert.equal(result.xp, null);
  assert.equal(result.quests, null);
  assert.equal(result.health, 50);
  assert.equal(result.kills, 3);
  assert.equal(result.deaths, 1);
  assert.equal(result.observed, 1);
  const history = retainedHistory(
    [{ ...frame, seq: 5 }, { ...frame, run: "old" }, frame, { ...frame, seq: 2, simMs: 50 }],
    frame,
  );
  assert.deepEqual(
    history.map((point) => point.simMs),
    [50, 100],
  );
  assert.equal(visibleBots(frame, "0", "all", "1").length, 0);
  assert.equal(visibleBots(frame, "0", "all", "2").length, 1);
  assert.equal(observationAge({ observedUnixMs: 1000 }, 5000), 4000);
});

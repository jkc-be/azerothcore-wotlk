import test from "node:test";
import assert from "node:assert/strict";
import { summarize, visibleBots, worldToScreen, fitView, duration } from "../web/model.js";
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
  assert.deepEqual(worldToScreen({ x: fit.x + 10, y: fit.y + 10 }, { ...fit, scale: 1 }, 800, 600), { x: 390, y: 290 });
});
test("simulated time labels retain whole days", () => assert.equal(duration(86400000), "24h 0m 0s"));

test("run totals retain departed bots' progression", () => {
  const snapshot = { simMs: 100, bots: [], runTotals: { xp: 1200, quests: 3, deaths: 2 } };
  assert.deepEqual(summarize(snapshot), { simMs: 100, xp: 1200, quests: 3, deaths: 2, levels: {}, zones: {} });
});

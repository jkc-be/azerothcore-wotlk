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
  rate,
  series,
  zoneTable,
  leaderboard,
  stalledBots,
  healthStats,
  questStats,
  gearCount,
  formatMoney,
  clock,
  formatClock,
  alerts,
  recordTrails,
  bucketHistory,
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
  const point = summarize(snapshot);
  assert.deepEqual([point.xp, point.quests, point.deaths, point.levels, point.zones], [1200, 3, 2, {}, {}]);
  assert.equal(point.meanHealth, null);
  assert.equal(point.money, null);
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

test("summaries expose simulation health and cohort composition for the dashboard", () => {
  const snapshot = {
    simMs: 5000,
    realMs: 1000,
    seq: 9,
    onlineBots: 2,
    activeBots: 1,
    expectedBots: 3,
    requestedSpeed: 5,
    achievedSpeed: 4.5,
    backlogMs: 120,
    maxTickUs: 6200,
    bots: [
      {
        ...bots[0],
        activity: "combat",
        health: 50,
        maxHealth: 100,
        money: 150,
        quests: [{ id: 1, state: 1, objectives: [1, 0, 0, 0] }],
      },
      { ...bots[1], activity: "moving", health: 100, maxHealth: 100, money: 50, quests: [] },
    ],
  };
  const point = summarize(snapshot);
  assert.deepEqual(point.activity, { idle: 0, moving: 1, combat: 1, casting: 0, dead: 0 });
  assert.equal(point.meanHealth, 75);
  assert.equal(point.money, 200);
  assert.equal(point.tickMs, 6.2);
  assert.equal(point.questsActive, 1);
  assert.equal(point.meanLevel, 2.5);
  assert.deepEqual([point.minLevel, point.maxLevel], [2, 3]);
  const dense = Array.from({ length: 10 }, (_, index) => ({
    simMs: index,
    meanLevel: index,
    minLevel: 1,
    maxLevel: 9,
    activity: { idle: index % 2, moving: 1, combat: 0, casting: 0, dead: 0 },
  }));
  const bucketed = bucketHistory(dense, 5);
  assert.equal(bucketed.length, 5);
  assert.deepEqual([bucketed[0].simMs, bucketed[0].meanLevel, bucketed[0].activity.idle], [1, 0.5, 0.5]);
  assert.equal(bucketHistory(dense, 20), dense);
  assert.equal(point.inWorld, 2);
});

test("rates use the trailing window on the history's time basis and never a false zero", () => {
  const history = [
    { simMs: 0, xp: 0 },
    { simMs: 600000, xp: 100 },
    { simMs: 1200000, xp: 400 },
  ];
  assert.equal(rate(history, "xp", 600000).perHour, 1800);
  assert.equal(rate(history, "xp", 3600000).perHour, 1200);
  assert.equal(rate([{ simMs: 0, xp: 0 }], "xp", 1000), null);
  assert.equal(
    rate(
      [
        { simMs: 0, xp: null },
        { simMs: 10, xp: null },
      ],
      "xp",
      1000,
    ),
    null,
  );
  assert.deepEqual(series(history, "xp", 2), [100, 400]);
});

test("composition helpers rank and group the current cohort", () => {
  const cohort = [
    {
      id: "a",
      name: "Zed",
      level: 3,
      earnedXp: 10,
      deaths: 0,
      health: 10,
      maxHealth: 100,
      map: 0,
      zone: 12,
      activity: "combat",
      lastAiMs: 900,
      gear: [0, 4, 5],
      quests: [{ id: 1, state: 0, objectives: [0, 0, 0, 0], items: [2, 0] }],
    },
    {
      id: "b",
      name: "Amy",
      level: 3,
      earnedXp: 10,
      deaths: 2,
      health: 0,
      maxHealth: 100,
      map: 0,
      zone: 12,
      activity: "dead",
      lastAiMs: 20000,
    },
    {
      id: "c",
      name: "Bob",
      level: 7,
      earnedXp: 90,
      deaths: 1,
      health: 90,
      maxHealth: 100,
      map: 1,
      zone: 14,
      activity: "idle",
      lastAiMs: 20000,
    },
  ];
  assert.deepEqual(zoneTable(cohort)[0], { map: 0, zone: 12, count: 2, combat: 1 });
  assert.deepEqual(
    leaderboard(cohort, "earnedXp", 2).map((bot) => bot.name),
    ["Bob", "Amy"],
  );
  assert.deepEqual(
    stalledBots({ simMs: 20000, bots: cohort }).map(({ bot }) => bot.name),
    ["Zed"],
  );
  assert.deepEqual(healthStats(cohort), { mean: 100 / 3, min: 0, low: 1, dead: 1 });
  assert.deepEqual(questStats(cohort), { active: 1, complete: 0, progressing: 1, bots: 1 });
  assert.equal(gearCount(cohort[0]), 2);
  assert.equal(formatMoney(123456), "12g 34s");
  assert.equal(formatMoney(456), "4s 56c");
  assert.equal(formatMoney(undefined), "unknown");
  assert.deepEqual(clock(90061000), { days: 1, time: "01:01:01" });
});

test("alerts describe run state without inventing health", () => {
  const healthy = alerts({ requestedSpeed: 5, achievedSpeed: 4.9, ready: true });
  assert.deepEqual(healthy, []);
  const troubled = alerts(
    { fault: "population_timeout", overloaded: true, requestedSpeed: 10, achievedSpeed: 3, controlError: "GM POV" },
    { stale: true, gaps: 2, silentSince: 4 },
  ).map((alert) => alert.level);
  assert.deepEqual(troubled, ["danger", "danger", "warn", "warn", "warn", "info"]);
  const behind = alerts({ requestedSpeed: 10, achievedSpeed: 3, backlogMs: 1500 });
  assert.deepEqual(
    behind.map((alert) => alert.level),
    ["warn"],
  );
  assert.match(behind[0].text, /3.00× of the requested 10×/);
  assert.equal(
    alerts({ requestedSpeed: 1 }, { gaps: 1 })[0].text,
    "1 snapshot gap this session, coalesced by the writer.",
  );
  assert.equal(formatClock(90061000), "1d 01:01:01");
  assert.equal(formatClock(61000), "00:01:01");
  assert.equal(alerts({ source: "python-api", overloaded: true }).length, 0);
});

test("trails keep bounded per-bot samples keyed by simulated time", () => {
  const trails = new Map();
  const frame = (simMs) => ({
    simMs,
    bots: [{ id: "a", map: 0, instance: 0, x: simMs, y: 0, health: 50, maxHealth: 100, level: 1 }],
  });
  recordTrails(trails, frame(1), 2);
  recordTrails(trails, frame(1), 2);
  recordTrails(trails, frame(2), 2);
  recordTrails(trails, frame(3), 2);
  assert.deepEqual(
    trails.get("a").map((sample) => sample.x),
    [2, 3],
  );
  assert.equal(trails.get("a")[0].health, 50);
});

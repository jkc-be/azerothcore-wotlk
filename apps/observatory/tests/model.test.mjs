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
  longTermSeries,
  formatBytes,
  describeEvent,
  isAllesEvent,
  filterMemories,
  relativeTime,
  currentObjective,
  objectiveTable,
  objectiveTally,
  worldTalk,
  runStatistics,
  attention,
  budgetState,
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

test("long-term points join event counts by bucket as records per simulated minute", () => {
  const snapshots = [
    { bucket: 0, simMs: 1000, meanLevel: 1 },
    { bucket: 300000, simMs: 301000, meanLevel: 2 },
    { bucket: 600000, simMs: 601000, meanLevel: 3, buckets: 2 },
  ];
  const events = [
    { bucket: 0, kinds: { xp: 10, position: 3000 } },
    { bucket: 600000, buckets: 2, kinds: { death: 5, quest_reward: 5, melee_swing: 600 } },
  ];
  const joined = longTermSeries(snapshots, events, 300000);
  assert.deepEqual(
    joined.map((point) => point.records),
    [{ progression: 2, trace: 600 }, null, { progression: 1, trace: 60 }],
  );
  assert.equal(joined[2].meanLevel, 3);
});

test("byte counts read as compact decimal units", () => {
  assert.equal(formatBytes(0), "0 B");
  assert.equal(formatBytes(1536), "1.5 kB");
  assert.equal(formatBytes(20310314350), "20.3 GB");
  assert.equal(formatBytes(587476670), "587 MB");
  assert.equal(formatBytes(null), "unknown");
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

test("ordinary live telemetry leaves unmeasured cumulative totals unknown", () => {
  const snapshot = {source: "alles-live", simMs: 1000, bots: [
    {id: "one", level: 2, health: 30, maxHealth: 40, activity: "moving", money: 12},
  ]};
  const result = summarize(snapshot);
  assert.equal(result.xp, null);
  assert.equal(result.quests, null);
  assert.equal(result.deaths, null);
  assert.equal(result.money, 12);
  assert.equal(result.memories, null);
  assert.equal(result.modelMemories, null);
  assert.equal(result.workerConnected, null);
});

test("live realm summaries total only what the bots report and carry the interpreter figures", () => {
  const snapshot = {
    source: "alles-live",
    simMs: 5000,
    activeBots: 2,
    onlineBots: 3,
    interpreter: {
      connected: true,
      usedRequests: 40,
      maxRequests: 100,
      remainingRequests: 60,
      modelMemories: 12,
      fallbackMemories: 3,
      invalidResults: 1,
    },
    bots: [
      { id: "a", level: 3, memoryCount: 10, pendingPerceptions: 2, earnedXp: 50, deaths: 0, questCompletions: 1 },
      { id: "b", level: 4, memoryCount: 6, pendingPerceptions: 0, earnedXp: 20, deaths: 1, questCompletions: 0 },
    ],
  };
  const result = summarize(snapshot);
  assert.equal(result.memories, 16);
  assert.equal(result.pendingPerceptions, 2);
  assert.equal(result.modelMemories, 12);
  assert.equal(result.fallbackMemories, 3);
  assert.equal(result.invalidResults, 1);
  assert.equal(result.usedRequests, 40);
  assert.equal(result.remainingRequests, 60);
  assert.equal(result.workerConnected, 1);
  assert.equal(result.xp, 70);
  assert.equal(result.quests, 1);
  assert.equal(result.deaths, 1);
  assert.equal(result.active, 2);
  assert.equal(result.conversationReplies, null);
  const talking = summarize({
    ...snapshot,
    conversation: { enabled: true, queuedTurns: 2, pendingReplies: 1, replies: 66, actions: 19, following: 0, omitted: 3 },
  });
  assert.equal(talking.conversationReplies, 66);
  assert.equal(talking.conversationActions, 19);
  assert.equal(talking.conversationPending, 3);
  // One reporting bot is enough for a total; a bot without the counter contributes nothing, not zero.
  const partial = summarize({ simMs: 1, bots: [{ id: "a", level: 1, deaths: 2 }, { id: "b", level: 1 }] });
  assert.equal(partial.deaths, 2);
  assert.equal(partial.xp, null);
});

test("alles journal records are described in their own words", () => {
  assert.equal(
    describeEvent({ kind: "alles_memory", value: 3, detail: "Hogger is near the mill.", context: "model" }),
    "memory formed by model: Hogger is near the mill.",
  );
  assert.equal(describeEvent({ kind: "alles_memory", value: 3, detail: "text", context: "revised" }), "memory 3 revised: text");
  assert.equal(
    describeEvent({ kind: "alles_perception", value: 0, detail: '"hello" from Humanb', context: "speech" }),
    'perceived speech: "hello" from Humanb (not retained)',
  );
  assert.equal(describeEvent({ kind: "alles_said", value: 1, detail: "I saw Humanb die." }), "said “I saw Humanb die.”");
  assert.equal(describeEvent({ kind: "alles_said", value: 0, detail: "x" }), "said “x” (nobody in range)");
  assert.equal(describeEvent({ kind: "alles_worker", value: 1, detail: "connected" }), "interpreter worker connected");
  assert.equal(describeEvent({ kind: "alles_request", value: 41 }), "pilot request charged, 41 used");
  assert.equal(describeEvent({ kind: "alles_save", value: 5, detail: "committed" }), "memory save committed, revision 5");
  assert.equal(describeEvent({ kind: "alles_owner", value: 1, detail: "ready" }), "memory store ready");
  assert.equal(describeEvent({ kind: "bot_action", detail: "loot", context: "loot" }), "action: loot");
  assert.equal(describeEvent({ kind: "bot_action", detail: "loot", context: "gather" }), "action: loot [gather]");
  assert.equal(describeEvent({ kind: "xp", value: 120, context: "attack" }), "xp: 120 [attack]");
  assert.ok(isAllesEvent({ kind: "alles_said" }));
  assert.ok(!isAllesEvent({ kind: "xp" }));
  assert.ok(!isAllesEvent({}));
});

test("live realm alerts name the worker, the budget, the journal and stale telemetry", () => {
  const base = {
    source: "alles-live",
    simMs: 1,
    bots: [],
    interpreter: { connected: false, usedRequests: 100, maxRequests: 100 },
    journal: { events: { dropped: 2 }, liveDropped: 1 },
  };
  const texts = alerts(base).map((alert) => alert.text);
  assert.equal(texts.length, 3);
  assert.match(texts[0], /Interpreter worker not connected/);
  assert.match(texts[1], /budget used \(100 of 100\)/);
  assert.match(texts[2], /3 journal records dropped/);
  const stale = alerts({ ...base, telemetryStale: true }, { stale: true });
  assert.equal(stale[0].level, "danger");
  assert.match(stale[0].text, /World telemetry is stale/);
  const rolling = alerts({
    source: "alles-live",
    bots: [],
    interpreter: { connected: true, budgetMode: "rolling", usedRequests: 100, maxRequests: 100 },
  });
  assert.deepEqual(rolling, []);
  const faults = alerts({
    source: "alles-live",
    bots: [],
    interpreter: { connected: true, ledgerFault: true },
    alles: { lifecycleFault: true },
    journal: { events: { failed: true } },
  });
  assert.deepEqual(
    faults.map((alert) => alert.level),
    ["danger", "danger", "warn"],
  );
});

test("memory inspector filters on every visible field and orders by the chosen key", () => {
  const memories = [
    { id: 1, text: "I saw that Mangy Wolf died", kind: "witnessed death", formation: "model", attribution: "",
      subject: { name: "Mangy Wolf" }, source: { name: "Humana" }, salience: 0.4, confidence: 0.9,
      formedUnixMs: 300, recalledUnixMs: 0 },
    { id: 2, text: "Humanb told me the mill burned", kind: "heard statement", formation: "fallback",
      attribution: "Humand", subject: { name: "" }, source: { name: "Humanb" }, salience: 0.9, confidence: 0.5,
      formedUnixMs: 100, recalledUnixMs: 250 },
    { id: 3, text: "I met Humane", kind: "met", formation: "reflex", attribution: "", subject: { name: "Humane" },
      source: { name: "" }, salience: 0.9, confidence: 1, formedUnixMs: 200, recalledUnixMs: 0 },
  ];
  const ids = (list) => list.map((memory) => memory.id);
  assert.deepEqual(ids(filterMemories(memories)), [3, 2, 1]);
  assert.deepEqual(ids(filterMemories(memories, "", "confidence")), [3, 1, 2]);
  assert.deepEqual(ids(filterMemories(memories, "", "newest")), [1, 3, 2]);
  assert.deepEqual(ids(filterMemories(memories, "", "recalled")), [2, 3, 1]);
  assert.deepEqual(ids(filterMemories(memories, " WOLF ")), [1]);
  assert.deepEqual(ids(filterMemories(memories, "humand")), [2]);
  assert.deepEqual(ids(filterMemories(memories, "reflex")), [3]);
  assert.deepEqual(ids(filterMemories(memories, "nothing")), []);
  assert.equal(memories[0].id, 1, "the caller's list is not reordered");
});

test("relative time is coarse and never negative", () => {
  const now = 10 * 86400 * 1000;
  assert.equal(relativeTime(null, now), "unknown");
  assert.equal(relativeTime(0, now), "unknown");
  assert.equal(relativeTime(now + 5000, now), "just now");
  assert.equal(relativeTime(now - 30 * 1000, now), "just now");
  assert.equal(relativeTime(now - 5 * 60 * 1000, now), "5 min ago");
  assert.equal(relativeTime(now - (3 * 3600 + 7 * 60) * 1000, now), "3 h 7 min ago");
  assert.equal(relativeTime(now - (2 * 86400 + 5 * 3600) * 1000, now), "2 d 5 h ago");
});

// ------------------------------------------------------------------------------------------- world state

const planner = (name, objectives, extra = {}) => ({
  id: name,
  name,
  health: 100,
  maxHealth: 100,
  planning: { engine: "new_rpg", objectives, reports: [] },
  ...extra,
});
const objective = (id, state, extra = {}) => ({
  id,
  state,
  step: "travel",
  approach: "pursue_quest",
  obstruction: "none",
  attempts: 1,
  ...extra,
});

test("the objective a bot is working on outranks the ones it has finished", () => {
  const bot = planner("a", [objective(1, "completed"), objective(2, "active"), objective(3, "proposed")]);
  assert.equal(currentObjective(bot).id, 2);
});

test("with nothing active the newest proposal says what a bot is about to do", () => {
  const bot = planner("a", [objective(1, "completed"), objective(4, "proposed"), objective(7, "proposed")]);
  assert.equal(currentObjective(bot).id, 7);
});

test("a world build without planning has no objective to report", () => {
  assert.equal(currentObjective({ id: "a", name: "a" }), null);
  assert.deepEqual(objectiveTable([{ id: "a", name: "a" }]), []);
});

test("obstructed objectives sort above stuck ones, and stuck above the rest", () => {
  const rows = objectiveTable([
    planner("calm", [objective(1, "active")]),
    planner("stuck", [objective(2, "active", { activeWithoutProgressMs: 90000 })]),
    planner("blocked", [objective(3, "active", { obstruction: "navigation" })]),
  ]);
  assert.deepEqual(
    rows.map((row) => row.bot.name),
    ["blocked", "stuck", "calm"],
  );
});

test("the tally counts every objective a bot has held, commonest state first", () => {
  const tally = objectiveTally([
    planner("a", [objective(1, "completed"), objective(2, "completed"), objective(3, "active")]),
    planner("b", [objective(4, "completed")]),
  ]);
  assert.equal(tally.total, 4);
  assert.deepEqual(tally.states, [
    ["completed", 3],
    ["active", 1],
  ]);
});

test("identical advice from one source is a single lead held by several bots", () => {
  const report = { source: "Humanb", text: "Try Goldshire.", confidence: 0.4, receivedMs: 10, usefulVisits: 1 };
  const bots = [
    planner("a", [], { planning: { objectives: [], reports: [report] } }),
    planner("b", [], { planning: { objectives: [], reports: [{ ...report, receivedMs: 20, usefulVisits: 0 }] } }),
  ];
  const { leads } = worldTalk(bots);
  assert.equal(leads.length, 1);
  assert.deepEqual(leads[0].holders, ["a", "b"]);
  assert.equal(leads[0].useful, 1);
  assert.equal(leads[0].receivedMs, 20);
});

test("a question a bot is still waiting on is listed with its status", () => {
  const asking = planner("a", [
    objective(1, "active", { information: { status: "lead_received", attempts: 2, question: "Where is it?" } }),
  ]);
  const quiet = planner("b", [objective(2, "active", { information: { status: "none", question: "" } })]);
  const { asks } = worldTalk([asking, quiet]);
  assert.deepEqual(asks, [{ id: "a", name: "a", status: "lead_received", question: "Where is it?", attempts: 2 }]);
});

test("run statistics rate flows by elapsed time and leave standing figures alone", () => {
  const rows = runStatistics({
    simMs: 1800000,
    runTotals: { xp: 600, quests: 4, deaths: 1 },
    bots: [{ actions: 10, memoryCount: 100, money: 50 }],
  });
  const row = (label) => rows.find((entry) => entry.label === label);
  assert.equal(row("XP earned").value, 600);
  assert.equal(row("XP earned").perHour, 1200);
  assert.equal(row("Memories held").value, 100);
  assert.equal(row("Memories held").perHour, null);
  assert.equal(row("AI updates"), undefined);
});

test("a healthy run needs no attention", () => {
  const notes = attention({
    simMs: 1000,
    expectedBots: 1,
    onlineBots: 1,
    bots: [{ id: "a", name: "a", health: 100, maxHealth: 100, memoryState: "ready" }],
    interpreter: { connected: true },
  });
  assert.deepEqual(notes, []);
});

test("attention names the bots each note is about, most serious first", () => {
  const notes = attention({
    simMs: 1000,
    expectedBots: 2,
    onlineBots: 2,
    bots: [
      { id: "a", name: "a", health: 0, maxHealth: 100, saveFailed: true },
      { id: "b", name: "b", health: 100, maxHealth: 100, droppedPerceptions: 3 },
    ],
    interpreter: {},
  });
  assert.equal(notes[0].level, "danger");
  assert.ok(notes.every((note) => note.bots.every((bot) => bot.id && bot.name)));
  const dropped = notes.find((note) => note.text.includes("dropping perceptions"));
  assert.deepEqual(dropped.bots, [{ id: "b", name: "b" }]);
});

test("a short cohort and a spent request budget are notes about the run, not about a bot", () => {
  const notes = attention({
    simMs: 1000,
    expectedBots: 5,
    onlineBots: 3,
    bots: [],
    interpreter: { connected: true, usedRequests: 200, maxRequests: 100 },
  });
  assert.ok(notes.some((note) => note.text === "3 of 5 bots online" && !note.bots.length));
  assert.ok(notes.some((note) => note.text.includes("budget spent") && !note.bots.length));
});

// -------------------------------------------------------------------------------------- request budget

test("an unlimited world enforces no cap, however far the request count runs past maxRequests", () => {
  const budget = budgetState({ budgetMode: "unlimited", usedRequests: 2196, maxRequests: 100 });
  assert.equal(budget.mode, "unlimited");
  assert.equal(budget.capped, false);
  assert.equal(budget.exhausted, false);
});

test("a trial budget is spent once the count reaches its cap", () => {
  assert.equal(budgetState({ budgetMode: "trial", usedRequests: 99, maxRequests: 100 }).exhausted, false);
  assert.equal(budgetState({ budgetMode: "trial", usedRequests: 100, maxRequests: 100 }).exhausted, true);
});

test("a rolling budget is spent when nothing is left this minute, not when the total is high", () => {
  const rolling = { budgetMode: "rolling", usedRequests: 9000, requestsPerMinute: 60 };
  assert.equal(budgetState({ ...rolling, remainingRequests: 0 }).exhausted, true);
  assert.equal(budgetState({ ...rolling, remainingRequests: 12 }).exhausted, false);
});

test("a rolling budget takes its cap from requestsPerMinute, which is the only field the world fills", () => {
  // maxRequests is absent on a rolling world; reading it would report an uncapped budget and hide the meter.
  const budget = budgetState({ budgetMode: "rolling", usedRequests: 9000, requestsPerMinute: 60, remainingRequests: 5 });
  assert.equal(budget.max, 60);
  assert.equal(budget.capped, true);
});

test("a missing remaining count is derived from the cap rather than read as unspent", () => {
  assert.equal(budgetState({ budgetMode: "rolling", usedRequests: 60, requestsPerMinute: 60 }).remaining, 0);
  assert.equal(budgetState({ budgetMode: "rolling", usedRequests: 60, requestsPerMinute: 60 }).exhausted, true);
});

test("an unlimited world raises no budget alert while the model is still answering", () => {
  const list = alerts({
    source: "alles-live",
    simMs: 1000,
    bots: [],
    interpreter: { connected: true, budgetMode: "unlimited", usedRequests: 2196, maxRequests: 100 },
  });
  assert.equal(
    list.some((alert) => alert.text.includes("budget")),
    false,
  );
});

test("a spent trial budget is still reported", () => {
  const list = alerts({
    source: "alles-live",
    simMs: 1000,
    bots: [],
    interpreter: { connected: true, budgetMode: "trial", usedRequests: 100, maxRequests: 100 },
  });
  assert.ok(list.some((alert) => alert.text.includes("Pilot request budget used (100 of 100)")));
});

test("attention does not warn about a budget the world is not enforcing", () => {
  const uncapped = attention({
    simMs: 1000,
    bots: [],
    interpreter: { connected: true, budgetMode: "unlimited", usedRequests: 2196, maxRequests: 100 },
  });
  const capped = attention({
    simMs: 1000,
    bots: [],
    interpreter: { connected: true, budgetMode: "trial", usedRequests: 100, maxRequests: 100 },
  });
  assert.deepEqual(uncapped, []);
  assert.ok(capped.some((note) => note.text.includes("budget spent")));
});

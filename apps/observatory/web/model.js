export const ACTIVITIES = ["idle", "moving", "combat", "casting", "dead"];

// Trace-only journal kinds. Keep in sync with TRACE_KINDS in bridge.py.
export const TRACE_KINDS = new Set([
  "position",
  "melee_swing",
  "aura_tick",
  "damage_input",
  "damage",
  "periodic_damage",
  "cast_start",
  "cast_finish",
  "cast_cancel",
  "cooldown",
  "regeneration",
  "health_set",
  "power_set",
  "creature_death",
  "creature_respawn",
]);

export function duration(ms) {
  const seconds = Math.floor(ms / 1000);
  return `${Math.floor(seconds / 3600)}h ${Math.floor(seconds / 60) % 60}m ${seconds % 60}s`;
}

export function clock(ms) {
  const seconds = Math.floor(ms / 1000);
  const days = Math.floor(seconds / 86400);
  const pad = (value) => String(value).padStart(2, "0");
  return {
    days,
    time: `${pad(Math.floor(seconds / 3600) % 24)}:${pad(Math.floor(seconds / 60) % 60)}:${pad(seconds % 60)}`,
  };
}

export function formatMoney(copper) {
  if (copper == null || !Number.isFinite(copper)) return "unknown";
  const gold = Math.floor(copper / 10000),
    silver = Math.floor(copper / 100) % 100,
    bronze = copper % 100;
  return gold ? `${gold}g ${silver}s` : silver ? `${silver}s ${bronze}c` : `${bronze}c`;
}

export function formatNumber(value, digits = 0) {
  if (value == null || !Number.isFinite(value)) return "unknown";
  return value.toLocaleString(undefined, { maximumFractionDigits: digits, minimumFractionDigits: digits });
}

export function healthPercent(bot) {
  return (100 * (bot.health ?? 0)) / Math.max(1, bot.maxHealth ?? 1);
}

export function activityMix(bots) {
  const mix = Object.fromEntries(ACTIVITIES.map((activity) => [activity, 0]));
  for (const bot of bots) mix[ACTIVITIES.includes(bot.activity) ? bot.activity : "idle"] += 1;
  return mix;
}

export function isAllesEvent(event) {
  return typeof event?.kind === "string" && event.kind.startsWith("alles_");
}

// One line per journal record for the feeds and the inspector; alles kinds carry their own wording.
export function describeEvent(event) {
  const what = event.detail || (event.value ? formatNumber(event.value) : "");
  const spell = event.spell ? `, spell ${event.spell}` : "";
  const context = event.context && event.context !== event.detail ? ` [${event.context}]` : "";
  switch (event.kind) {
    case "bot_action":
      return `action: ${event.detail || "unnamed"}${context}`;
    case "alles_perception":
      return `perceived ${event.context || "something"}: ${event.detail}${event.value ? "" : " (not retained)"}`;
    case "alles_memory":
      return event.context === "revised"
        ? `memory ${event.value} revised: ${event.detail}`
        : `memory formed by ${event.context || "unknown"}: ${event.detail}`;
    case "alles_said":
      return `said “${event.detail}”${event.value ? "" : " (nobody in range)"}`;
    case "alles_owner":
      return `memory store ${event.detail}`;
    case "alles_save":
      return `memory save ${event.detail}, revision ${formatNumber(event.value)}`;
    case "alles_worker":
      return `interpreter worker ${event.detail}`;
    case "alles_request":
      return `pilot request charged, ${formatNumber(event.value)} used`;
    case "alles_conversation":
      return `conversation ${event.detail || ""}${context}`.trim();
    default:
      return `${event.kind.replaceAll("_", " ")}${what ? `: ${what}` : ""}${spell}${context}`;
  }
}

export function summarize(snapshot) {
  const bots = snapshot.bots;
  const interpreter = snapshot.interpreter || {};
  const conversation = snapshot.conversation || {};
  // A counter nobody reports is unknown, never zero: an ordinary realm only measures what its world build taps.
  const total = (key) =>
    bots.some((bot) => bot[key] != null) ? bots.reduce((sum, bot) => sum + (bot[key] ?? 0), 0) : null;
  const result = {
    simMs: snapshot.simMs,
    realMs: snapshot.realMs ?? null,
    seq: snapshot.seq ?? null,
    xp: total("earnedXp"),
    quests: total("questCompletions"),
    deaths: total("deaths"),
    memories: total("memoryCount"),
    pendingPerceptions: total("pendingPerceptions"),
    modelMemories: interpreter.modelMemories ?? null,
    fallbackMemories: interpreter.fallbackMemories ?? null,
    invalidResults: interpreter.invalidResults ?? null,
    usedRequests: interpreter.usedRequests ?? null,
    remainingRequests: interpreter.remainingRequests ?? null,
    workerConnected: interpreter.connected == null ? null : Number(Boolean(interpreter.connected)),
    conversationReplies: conversation.replies ?? null,
    conversationActions: conversation.actions ?? null,
    conversationPending:
      conversation.pendingReplies == null ? null : (conversation.pendingReplies ?? 0) + (conversation.queuedTurns ?? 0),
    levels: {},
    zones: {},
    activity: activityMix(bots),
    inWorld: bots.length,
    online: snapshot.onlineBots ?? null,
    active: snapshot.activeBots ?? null,
    expected: snapshot.expectedBots ?? null,
    requestedSpeed: snapshot.requestedSpeed ?? null,
    achievedSpeed: snapshot.achievedSpeed ?? null,
    backlogMs: snapshot.backlogMs ?? null,
    tickMs: snapshot.maxTickUs == null ? null : snapshot.maxTickUs / 1000,
    paused: Boolean(snapshot.paused),
    meanHealth: bots.length ? bots.reduce((sum, bot) => sum + healthPercent(bot), 0) / bots.length : null,
    money: bots.some((bot) => bot.money != null) ? bots.reduce((sum, bot) => sum + (bot.money ?? 0), 0) : null,
    questsActive: bots.reduce((sum, bot) => sum + (bot.quests?.length ?? 0), 0),
    meanLevel: bots.length ? bots.reduce((sum, bot) => sum + bot.level, 0) / bots.length : null,
    minLevel: bots.length ? Math.min(...bots.map((bot) => bot.level)) : null,
    maxLevel: bots.length ? Math.max(...bots.map((bot) => bot.level)) : null,
  };
  for (const bot of bots) {
    const level = `Level ${bot.level}`;
    const zone = `${bot.map}/${bot.zone}`;
    result.levels[level] = (result.levels[level] || 0) + 1;
    result.zones[zone] = (result.zones[zone] || 0) + 1;
  }
  if (snapshot.runTotals) Object.assign(result, snapshot.runTotals);
  if (snapshot.source === "python-api") {
    result.xp = result.quests = null;
    result.observed = bots.length;
    result.combat = bots.filter((bot) => bot.combat).length;
    result.alive = bots.filter((bot) => bot.alive).length;
    result.health = bots.length ? bots.reduce((sum, bot) => sum + healthPercent(bot), 0) / bots.length : null;
  }
  return result;
}

// Progression rate over the trailing window, expressed per hour of the history's time basis.
export function rate(history, key, windowMs) {
  if (history.length < 2) return null;
  const last = history.at(-1);
  if (last[key] == null) return null;
  let first = history[0];
  for (let index = history.length - 2; index >= 0; index -= 1) {
    if (history[index][key] == null) break;
    first = history[index];
    if (last.simMs - history[index].simMs >= windowMs) break;
  }
  const deltaMs = last.simMs - first.simMs;
  if (deltaMs <= 0 || first[key] == null) return null;
  return { perHour: ((last[key] - first[key]) * 3600000) / deltaMs, delta: last[key] - first[key], deltaMs };
}

// Average history into at most `buckets` points so small panels show trends rather than per-snapshot jitter.
export function bucketHistory(history, buckets = 120) {
  if (history.length <= buckets) return history;
  const size = Math.ceil(history.length / buckets);
  const result = [];
  for (let start = 0; start < history.length; start += size) {
    const group = history.slice(start, start + size);
    const mean = (values) => {
      const known = values.filter((value) => value != null);
      return known.length ? known.reduce((sum, value) => sum + value, 0) / known.length : null;
    };
    result.push({
      simMs: group.at(-1).simMs,
      meanLevel: mean(group.map((point) => point.meanLevel)),
      minLevel: mean(group.map((point) => point.minLevel)),
      maxLevel: mean(group.map((point) => point.maxLevel)),
      activity: Object.fromEntries(
        ACTIVITIES.map((activity) => [activity, mean(group.map((point) => point.activity?.[activity] ?? 0))]),
      ),
    });
  }
  return result;
}

export function series(history, key, limit = 120) {
  return history.slice(-limit).map((point) => (point[key] == null ? null : point[key]));
}

// Long-term tier: the bridge's snapshot buckets carry the chart keys; event buckets add journal records
// per simulated minute. Points are joined on their bucket start, so folded responses still line up.
export function longTermSeries(snapshots, events, bucketMs) {
  const counts = new Map(events.map((point) => [point.bucket, point]));
  return snapshots.map((point) => {
    const found = counts.get(point.bucket);
    const minutes = found ? ((found.buckets ?? 1) * bucketMs) / 60000 : 0;
    let progression = 0,
      trace = 0;
    for (const [kind, count] of Object.entries(found?.kinds ?? {})) {
      if (TRACE_KINDS.has(kind)) trace += count;
      else progression += count;
    }
    return {
      ...point,
      records: minutes ? { progression: progression / minutes, trace: trace / minutes } : null,
    };
  });
}

export function formatBytes(bytes) {
  if (bytes == null) return "unknown";
  const units = ["B", "kB", "MB", "GB", "TB"];
  let value = bytes,
    unit = 0;
  while (value >= 1000 && unit < units.length - 1) {
    value /= 1000;
    unit += 1;
  }
  return `${unit ? value.toFixed(value >= 100 ? 0 : 1) : value} ${units[unit]}`;
}

export function zoneTable(bots) {
  const counts = new Map();
  for (const bot of bots) {
    const key = `${bot.map}/${bot.zone ?? "?"}`;
    const entry = counts.get(key) || { map: bot.map, zone: bot.zone ?? null, count: 0, combat: 0 };
    entry.count += 1;
    if (bot.activity === "combat" || bot.combat) entry.combat += 1;
    counts.set(key, entry);
  }
  return [...counts.values()].sort((a, b) => b.count - a.count);
}

export function leaderboard(bots, key, limit = 5, ascending = false) {
  return bots
    .filter((bot) => bot[key] != null)
    .sort((a, b) => (ascending ? a[key] - b[key] : b[key] - a[key]) || a.name.localeCompare(b.name))
    .slice(0, limit);
}

// The world counts a bot as active when its AI updated within 10 simulated seconds; report the others.
export function stalledBots(snapshot, thresholdMs = 10000) {
  return snapshot.bots
    .filter((bot) => bot.lastAiMs != null && snapshot.simMs - bot.lastAiMs > thresholdMs)
    .map((bot) => ({ bot, silentMs: snapshot.simMs - bot.lastAiMs }))
    .sort((a, b) => b.silentMs - a.silentMs);
}

export function healthStats(bots) {
  if (!bots.length) return { mean: null, min: null, low: 0, dead: 0 };
  const percents = bots.map(healthPercent);
  return {
    mean: percents.reduce((sum, value) => sum + value, 0) / percents.length,
    min: Math.min(...percents),
    low: percents.filter((value) => value > 0 && value < 35).length,
    dead: bots.filter((bot) => bot.health === 0 || bot.alive === false).length,
  };
}

export function questStats(bots) {
  const result = { active: 0, complete: 0, progressing: 0, bots: 0 };
  for (const bot of bots) {
    const quests = bot.quests || [];
    if (quests.length) result.bots += 1;
    for (const quest of quests) {
      result.active += 1;
      if (quest.state & 1) result.complete += 1;
      else if ((quest.objectives || []).some(Boolean) || (quest.items || []).some(Boolean)) result.progressing += 1;
    }
  }
  return result;
}

export function gearCount(bot) {
  return (bot.gear || []).filter(Boolean).length;
}

export function formatClock(ms) {
  const { days, time } = clock(ms);
  return days ? `${days}d ${time}` : time;
}

export function alerts(state, { stale = false, gaps = 0, silentSince = null } = {}) {
  const list = [];
  const python = state.source === "python-api";
  const backlog = state.backlogMs == null ? "" : `${(state.backlogMs / 1000).toFixed(1)} s`;
  if (state.fault)
    list.push({ level: "danger", text: `Run frozen: ${state.fault}. Export the run and inspect the server.` });
  if (state.completed)
    list.push({
      level: "info",
      text: python ? "Feed closed, showing recorded history." : "Simulated duration complete.",
    });
  if (stale)
    list.push({
      level: "danger",
      text: state.telemetryStale
        ? "World telemetry is stale: the realm stopped publishing samples. Showing the last one."
        : "No snapshot for 3 s. Check the bridge.",
    });
  if (state.controlError) list.push({ level: "warn", text: `Control rejected: ${state.controlError}` });
  if (state.source === "alles-live") {
    const worker = state.interpreter || {};
    const budget = budgetState(worker);
    if (worker.connected === false)
      list.push({ level: "warn", text: "Interpreter worker not connected: perceptions form by template fallback." });
    if (worker.ledgerFault)
      list.push({ level: "danger", text: "Provider ledger fault: no further model requests are permitted." });
    else if (budget.exhausted && budget.mode === "trial")
      list.push({
        level: "info",
        text: `Pilot request budget used (${budget.used} of ${budget.max}); memories form by template fallback.`,
      });
    if (state.alles?.lifecycleFault)
      list.push({ level: "danger", text: "Lifecycle ingress overflow: interpretation and speech are suspended." });
    if (state.journal?.events?.failed)
      list.push({ level: "warn", text: "Telemetry journal failed on disk; the event feed is incomplete." });
    else if (state.journal?.events?.dropped || state.journal?.liveDropped)
      list.push({
        level: "info",
        text: `${formatNumber((state.journal.events?.dropped ?? 0) + (state.journal.liveDropped ?? 0))} journal records dropped under load.`,
      });
  }
  if (!python) {
    if (state.overloaded) list.push({ level: "warn", text: `Overloaded: ${backlog} of simulated steps queued.` });
    if (state.populationPending)
      list.push({
        level: "info",
        text:
          `Population adjusting: ${state.onlineBots} of ${state.expectedBots} bots online` +
          `${state.paused ? ", waiting for Resume" : ""}.`,
      });
    if (state.observers)
      list.push({
        level: state.observerMode === 2 ? "warn" : "info",
        text:
          `${state.observers} GM observer${state.observers === 1 ? "" : "s"} connected, speed locked at 1×` +
          (state.observerMode === 2
            ? "; full GM control is enabled, so this run is not a clean comparison."
            : state.observerMode === 1
              ? "; observers may move."
              : "."),
      });
    if (state.baseline) list.push({ level: "info", text: "Real-time baseline: 1× without pause only." });
    // The speed instrument already shows the shortfall while overloaded; one alert is enough.
    if (
      !state.overloaded &&
      !state.paused &&
      !state.completed &&
      state.achievedSpeed != null &&
      state.requestedSpeed > 1 &&
      state.achievedSpeed < 0.9 * state.requestedSpeed
    )
      list.push({
        level: "warn",
        text: `Running at ${state.achievedSpeed.toFixed(2)}× of the requested ${state.requestedSpeed}×.`,
      });
    if (state.ready === false) list.push({ level: "info", text: "Waiting for the complete initial cohort." });
    if (silentSince)
      list.push({ level: "warn", text: `${silentSince} bots without an AI update for over 10 simulated seconds.` });
  }
  if (gaps)
    list.push({
      level: "info",
      text: `${gaps} snapshot gap${gaps === 1 ? "" : "s"} this session, coalesced by the writer.`,
    });
  return list;
}

// ------------------------------------------------------------------------------------------- world state

// A planning world publishes every objective a bot has held, newest last. The one it is working on is the
// active objective; with none active the newest proposal or deferral says what it is about to do instead, and
// a cohort that has finished everything falls back to its last completed objective rather than showing nothing.
const OBJECTIVE_ORDER = ["active", "proposed", "deferred", "cancelled", "completed"];

export function currentObjective(bot) {
  const objectives = bot?.planning?.objectives;
  if (!objectives || !objectives.length) return null;
  const rank = (objective) => {
    const place = OBJECTIVE_ORDER.indexOf(objective.state);
    return place < 0 ? OBJECTIVE_ORDER.length : place;
  };
  return [...objectives].sort((a, b) => rank(a) - rank(b) || (b.id ?? 0) - (a.id ?? 0))[0];
}

// One row per bot: what it is working towards and how that is going. Bots without planning are left out, so a
// world build that publishes none produces an empty table rather than a column of blanks.
export function objectiveTable(bots) {
  return bots
    .map((bot) => ({ bot, objective: currentObjective(bot) }))
    .filter((row) => row.objective)
    .sort((a, b) => {
      const stuck = (row) => row.objective.activeWithoutProgressMs || 0;
      const blocked = (row) => (row.objective.obstruction && row.objective.obstruction !== "none" ? 1 : 0);
      return blocked(b) - blocked(a) || stuck(b) - stuck(a) || a.bot.name.localeCompare(b.bot.name);
    });
}

export function objectiveTally(bots) {
  const states = new Map();
  const approaches = new Map();
  let total = 0;
  for (const bot of bots)
    for (const objective of bot.planning?.objectives || []) {
      total += 1;
      states.set(objective.state, (states.get(objective.state) || 0) + 1);
      approaches.set(objective.approach, (approaches.get(objective.approach) || 0) + 1);
    }
  const rank = (map) => [...map.entries()].sort((a, b) => b[1] - a[1]);
  return { total, states: rank(states), approaches: rank(approaches) };
}

// Leads bots pass to each other, and the questions still waiting for one. Identical advice from the same
// source is one lead held by several bots, which is what makes a rumour visible as a rumour.
export function worldTalk(bots) {
  const leads = new Map();
  const asks = [];
  for (const bot of bots) {
    for (const report of bot.planning?.reports || []) {
      const key = `${report.source}\u0000${report.text}`;
      const lead = leads.get(key) || {
        source: report.source,
        text: report.text,
        holders: [],
        confidence: report.confidence,
        useful: 0,
        unsuccessful: 0,
        receivedMs: 0,
      };
      lead.holders.push(bot.name);
      lead.useful += report.usefulVisits || 0;
      lead.unsuccessful += report.unsuccessfulVisits || 0;
      lead.receivedMs = Math.max(lead.receivedMs, report.receivedMs || 0);
      leads.set(key, lead);
    }
    const information = currentObjective(bot)?.information;
    if (information && information.status && information.status !== "none" && information.question)
      asks.push({
        id: bot.id,
        name: bot.name,
        status: information.status,
        question: information.question,
        attempts: information.attempts || 0,
      });
  }
  return { leads: [...leads.values()].sort((a, b) => b.receivedMs - a.receivedMs), asks };
}

// Totals for the whole run beside the rate they accumulated at. A stock (memories held, money carried) has no
// rate: dividing a standing figure by elapsed time would read as a flow it is not.
export function runStatistics(snapshot) {
  const bots = snapshot.bots || [];
  const hours = Math.max(snapshot.simMs || 0, 1) / 3600000;
  const sum = (key) => bots.reduce((total, bot) => total + (Number(bot[key]) || 0), 0);
  const has = (key) => bots.some((bot) => bot[key] != null);
  const totals = snapshot.runTotals || {};
  const worker = snapshot.interpreter || {};
  const flow = (label, value, hint) => ({ label, value, perHour: value == null ? null : value / hours, hint });
  const stock = (label, value, hint, format) => ({ label, value, perHour: null, hint, format });
  const rows = [
    flow("XP earned", totals.xp ?? (has("earnedXp") ? sum("earnedXp") : null)),
    flow("Quests completed", totals.quests ?? (has("questCompletions") ? sum("questCompletions") : null)),
    flow("Deaths", totals.deaths ?? (has("deaths") ? sum("deaths") : null)),
    flow("AI actions", has("actions") ? sum("actions") : null, "decisions carried out"),
    flow("AI updates", has("aiUpdates") ? sum("aiUpdates") : null, "AI ticks across the cohort"),
    flow("Journal records", snapshot.journal?.events?.records ?? null, "events written to disk"),
    flow("Interpreter requests", worker.usedRequests ?? null, "model calls charged"),
    stock("Memories held", has("memoryCount") ? sum("memoryCount") : null, "across the cohort"),
    stock("Money carried", has("money") ? sum("money") : null, "across the cohort", "money"),
  ];
  return rows.filter((row) => row.value != null);
}

// Everything worth looking at before concluding a run is healthy, most serious first. Each note names the bots
// it is about so the dashboard can offer them, and a run with nothing wrong produces an empty list.
export function attention(snapshot, { stalledMs = 10000, noProgressMs = 60000, stationary = [] } = {}) {
  const notes = [];
  const bots = snapshot.bots || [];
  const name = (bot) => ({ id: bot.id, name: bot.name });
  // A note about particular bots carries a phrase the dashboard prefixes with their count ("2 bots dead");
  // a note about the run as a whole carries a whole sentence and an empty list.
  const add = (level, text, involved) => {
    if (involved.length) notes.push({ level, text, bots: involved });
  };
  const where = (predicate) => bots.filter(predicate).map(name);
  const expected = snapshot.expectedBots,
    online = snapshot.onlineBots;
  add("danger", "dead", where((bot) => bot.health === 0 || bot.alive === false));
  add("warn", "below 35% health", where((bot) => healthPercent(bot) > 0 && healthPercent(bot) < 35));
  add(
    "warn",
    `AI silent for over ${Math.round(stalledMs / 1000)} s`,
    stalledBots(snapshot, stalledMs).map(({ bot }) => name(bot)),
  );
  add("warn", "have not moved over the retained trail", stationary);
  const blocked = new Map();
  for (const bot of bots) {
    const objective = currentObjective(bot);
    if (!objective) continue;
    if (objective.obstruction && objective.obstruction !== "none") {
      const list = blocked.get(objective.obstruction) || [];
      list.push(name(bot));
      blocked.set(objective.obstruction, list);
    }
  }
  for (const [obstruction, involved] of blocked) add("warn", `objective obstructed: ${obstruction}`, involved);
  add(
    "warn",
    `objective making no progress for over ${Math.round(noProgressMs / 1000)} s`,
    where((bot) => (currentObjective(bot)?.activeWithoutProgressMs || 0) > noProgressMs),
  );
  add("danger", "memory store failed its last save", where((bot) => bot.saveFailed));
  add("warn", "memory store not ready", where((bot) => bot.memoryState != null && bot.memoryState !== "ready"));
  add("warn", "dropping perceptions", where((bot) => bot.droppedPerceptions > 0));
  const worker = snapshot.interpreter || {};
  const flag = (level, text) => notes.push({ level, text, bots: [] });
  if (expected != null && online != null && online < expected) flag("warn", `${online} of ${expected} bots online`);
  if (worker.ledgerFault) flag("danger", "Interpreter ledger fault: no further model requests");
  if (worker.connected === false) flag("warn", "Interpreter worker disconnected");
  const budget = budgetState(worker);
  if (budget.exhausted && budget.mode === "trial")
    flag("warn", "Interpreter request budget spent; memories form by template fallback");
  if (worker.oldestWaitingMs > 10000)
    flag("warn", `Interpreter jobs waiting up to ${(worker.oldestWaitingMs / 1000).toFixed(0)} s`);
  const alles = snapshot.alles || {};
  if (alles.lifecycleFault) flag("danger", "Bot lifecycle fault reported by the module");
  if (alles.dropped) flag("warn", `${alles.dropped} telemetry records dropped by the module`);
  if (alles.unsafePackets) flag("warn", `${alles.unsafePackets} unsafe packets refused by the module`);
  for (const [journal, figures] of Object.entries(snapshot.journal || {})) {
    if (!figures || typeof figures !== "object") continue;
    if (figures.failed) flag("danger", `Journal ${journal} failed to write`);
    if (figures.dropped) flag("warn", `Journal ${journal} dropped ${figures.dropped} records`);
  }
  if (snapshot.journal?.liveDropped)
    flag("warn", `${snapshot.journal.liveDropped} live records dropped before the journal`);
  const severity = { danger: 0, warn: 1 };
  return notes.sort((a, b) => severity[a.level] - severity[b.level]);
}

// How the interpreter's request budget should be read. The world publishes three modes and only two of them
// cap anything: "rolling" refills every minute, "trial" stops at a fixed count, and "unlimited" ignores the
// count entirely — `usedRequests` then runs past `maxRequests` while the model keeps answering, so treating a
// non-rolling mode as a trial reports a spent budget and a template fallback that are not happening.
export function budgetState(worker = {}) {
  const mode = worker.budgetMode === "rolling" ? "rolling" : worker.budgetMode === "unlimited" ? "unlimited" : "trial";
  const used = Number(worker.usedRequests) || 0;
  // The cap lives in a different field per mode: a rolling budget refills to requestsPerMinute every minute
  // while maxRequests may not be published at all, and a trial budget stops at maxRequests.
  const max = mode === "rolling" ? Number(worker.requestsPerMinute) || 0 : Number(worker.maxRequests) || 0;
  const remaining = worker.remainingRequests ?? Math.max(0, max - used);
  const capped = mode !== "unlimited" && max > 0;
  return {
    mode,
    max,
    used,
    remaining,
    capped,
    exhausted: capped && (mode === "rolling" ? remaining === 0 : used >= max),
  };
}

export function visibleBots(snapshot, map, zone, instance = "all") {
  return snapshot.bots.filter(
    (bot) =>
      String(bot.map) === map &&
      (zone === "all" || zone === "0" || bot.zone == null || String(bot.zone) === zone) &&
      (instance === "all" || String(bot.instance) === instance),
  );
}

export function observationAge(bot, now = Date.now()) {
  return bot.observedUnixMs === undefined ? null : Math.max(0, now - bot.observedUnixMs);
}

export function retainedHistory(frames, snapshot) {
  const bySequence = new Map();
  for (const frame of [...frames, snapshot]) {
    if (frame.run === snapshot.run && frame.seq <= snapshot.seq) bySequence.set(frame.seq, frame);
  }
  return [...bySequence.values()]
    .sort((a, b) => a.seq - b.seq)
    .slice(-4000)
    .map(summarize);
}

// Per-bot recent samples used for inspector sparklines and map trails.
export function recordTrails(trails, snapshot, limit = 240) {
  for (const bot of snapshot.bots) {
    const trail = trails.get(bot.id) || [];
    const last = trail.at(-1);
    if (last && last.simMs === snapshot.simMs) continue;
    trail.push({
      simMs: snapshot.simMs,
      map: bot.map,
      instance: bot.instance,
      x: bot.x,
      y: bot.y,
      health: healthPercent(bot),
      xp: bot.earnedXp ?? null,
      level: bot.level,
    });
    if (trail.length > limit) trail.splice(0, trail.length - limit);
    trails.set(bot.id, trail);
  }
  return trails;
}

export function worldToScreen(bot, view, width, height) {
  return { x: width / 2 - (bot.y - view.y) * view.scale, y: height / 2 - (bot.x - view.x) * view.scale };
}

export function fitView(bots) {
  if (!bots.length) return { x: 0, y: 0, scale: 0.03 };
  const xs = bots.map((bot) => bot.x),
    ys = bots.map((bot) => bot.y);
  return {
    x: (Math.min(...xs) + Math.max(...xs)) / 2,
    y: (Math.min(...ys) + Math.max(...ys)) / 2,
    spanX: Math.max(100, Math.max(...xs) - Math.min(...xs)),
    spanY: Math.max(100, Math.max(...ys) - Math.min(...ys)),
  };
}

export function chooseMap(areas, map, zone, bots, selected) {
  const candidates = areas.filter((area) => String(area.map) === map);
  const focus = bots.find((bot) => bot.id === selected) || bots[0];
  const wanted = zone === "all" ? focus?.zone : Number(zone);
  return candidates.find((area) => area.zone === wanted) || candidates.find((area) => area.zone === 0);
}

export function mapView(area) {
  return { x: (area.x1 + area.x2) / 2, y: (area.y1 + area.y2) / 2, spanX: area.x1 - area.x2, spanY: area.y1 - area.y2 };
}

export function mapTile(area, index, view, width, height) {
  const column = index % 4,
    row = Math.floor(index / 4);
  const tileWidth = Math.min(area.tileSize, area.width - column * area.tileSize);
  const tileHeight = Math.min(area.tileSize, area.height - row * area.tileSize);
  return {
    ...mapRect(area, column * area.tileSize, row * area.tileSize, tileWidth, tileHeight, view, width, height),
    cropX: tileWidth / area.tileSize,
    cropY: tileHeight / area.tileSize,
  };
}

export function mapRect(area, left, top, tileWidth, tileHeight, view, width, height) {
  const y = area.y1 - (left / area.width) * (area.y1 - area.y2);
  const x = area.x1 - (top / area.height) * (area.x1 - area.x2);
  return {
    ...worldToScreen({ x, y }, view, width, height),
    width: (tileWidth / area.width) * (area.y1 - area.y2) * view.scale,
    height: (tileHeight / area.height) * (area.x1 - area.x2) * view.scale,
  };
}

// Memory inspector: client-side filtering and ordering of one owner's committed memories.
export function filterMemories(memories, filter = "", sort = "salience") {
  const needle = filter.trim().toLowerCase();
  const fields = (memory) => [
    memory.text,
    memory.kind,
    memory.formation,
    memory.attribution,
    memory.subject?.name,
    memory.source?.name,
  ];
  const kept = needle
    ? memories.filter((memory) => fields(memory).some((value) => value && value.toLowerCase().includes(needle)))
    : memories.slice();
  const keys = {
    salience: (memory) => memory.salience ?? 0,
    confidence: (memory) => memory.confidence ?? 0,
    newest: (memory) => memory.formedUnixMs ?? 0,
    recalled: (memory) => memory.recalledUnixMs ?? 0,
  };
  const key = keys[sort] || keys.salience;
  return kept.sort((a, b) => key(b) - key(a) || (b.id ?? 0) - (a.id ?? 0));
}

export function relativeTime(unixMs, now = Date.now()) {
  if (unixMs == null || !Number.isFinite(unixMs) || unixMs <= 0) return "unknown";
  const seconds = Math.max(0, Math.floor((now - unixMs) / 1000));
  if (seconds < 60) return "just now";
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min ago`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} h ${Math.floor(seconds / 60) % 60} min ago`;
  return `${Math.floor(seconds / 86400)} d ${Math.floor(seconds / 3600) % 24} h ago`;
}

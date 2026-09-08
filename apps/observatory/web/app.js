import {
  resizePopulation,
  ACTIVITIES,
  TRACE_KINDS,
  attention,
  budgetState,
  currentObjective,
  objectiveTable,
  objectiveTally,
  runStatistics,
  worldTalk,
  duration,
  clock,
  formatClock,
  formatMoney,
  formatNumber,
  healthPercent,
  summarize,
  rate,
  series,
  zoneTable,
  leaderboard,
  stalledBots,
  healthStats,
  questStats,
  gearCount,
  alerts,
  visibleBots,
  worldToScreen,
  fitView,
  chooseMap,
  mapView,
  mapTile,
  mapRect,
  observationAge,
  retainedHistory,
  recordTrails,
  bucketHistory,
  longTermSeries,
  formatBytes,
  describeEvent,
  isAllesEvent,
  filterMemories,
  relativeTime,
} from "./model.js";
import { surface, sparkline, speedBar, stackedArea, lineChart, tokens, labelFont, compactNumber } from "./charts.js";

const $ = (id) => document.getElementById(id);
const palette = tokens();
const ACTIVITY_COLORS = {
  idle: "#5f7480",
  moving: palette.sky,
  combat: palette.ember,
  casting: palette.lilac,
  dead: palette.dead,
};
const SERIES_COLORS = [
  palette.brass,
  palette.sky,
  palette.moss,
  palette.lilac,
  palette.ember,
  palette.parchment,
  palette.ash,
];
const RATE_WINDOW_MS = 10 * 60 * 1000;
const SCALE_STEPS = [50, 100, 250, 500, 1000, 2500, 5000];
// Above this many bots only the hovered and selected names are drawn; below it every name that finds room is.
const MAP_LABEL_LIMIT = 12;
const MAP_LABEL_HEIGHT = 19;
// The simulation controls only exist on a run the world lets the dashboard drive. A read-only feed — an
// ordinary realm publishing through mod-alles, or a recorded run — keeps Hold and loses the rest.
const SIMULATION_CONTROLS = [
  "pause",
  "speed-control",
  "max-speed-settings",
  "max-speed-status",
  "population",
  "population-status",
  "population-draft-status",
  "population-error",
  "race-population",
  "observer-group",
  "observer-status",
];
const continentNames = {
  0: "Eastern Kingdoms",
  1: "Kalimdor (western continent)",
  530: "Outland & draenei isles",
  571: "Northrend",
};

let token = "",
  state = null,
  selected = "",
  selectedAt = 0,
  history = [],
  events = [],
  eventStats = null,
  longTerm = { bucketMs: 0, points: [] },
  retention = null,
  freshAt = 0,
  abort = null,
  workerLog = null,
  chartMetricChosen = false,
  manifestRun = "";
let view = { x: 0, y: 0, scale: 0.03 },
  needsFit = true,
  pendingControl = 0,
  controlLog = [],
  gaps = 0,
  ingestTimes = [],
  chartHover = null,
  feedFrozen = null,
  held = false,
  heldAt = null,
  heldSamples = 0,
  hovered = null,
  shownCount = null;
const trails = new Map();
let mapAreas = [],
  fitArtwork = true,
  artwork = null,
  loadingArtwork = null;
const mapImages = new Map();

// ---------------------------------------------------------------------------------------------- transport

async function api(path, options = {}) {
  const response = await fetch(path, { ...options, headers: { Authorization: `Bearer ${token}`, ...options.headers } });
  if (!response.ok) throw new Error((await response.json()).error || `HTTP ${response.status}`);
  return response;
}

function notice(text) {
  $("notice").textContent = text;
}

function linkState(value) {
  $("link-state").dataset.state = value;
  $("link-state").textContent = value === "off" ? "not connected" : value;
}

async function loadMaps() {
  mapAreas = (await (await api("/api/maps")).json()).areas;
  if (state) updateMaps();
  $("map-art-status").textContent = mapAreas.length ? "Local client artwork" : "No local artwork installed";
}

async function loadArtwork(area) {
  loadingArtwork = area.id;
  try {
    const tiles = await Promise.all(
      [...area.tiles, ...(area.overlays || []).map((layer) => layer.file)].map(async (file) =>
        createImageBitmap(await (await api(`/api/maps/${file}`)).blob()),
      ),
    );
    mapImages.set(area.id, tiles);
    // Bound decoded image memory as viewers browse zones. The current area stays available.
    while (mapImages.size > 4) {
      const oldest = mapImages.keys().next().value;
      mapImages.get(oldest).forEach((tile) => tile.close());
      mapImages.delete(oldest);
    }
    $("map-art-status").textContent = `${area.name} artwork from the local client`;
  } catch {
    mapImages.set(area.id, []);
    $("map-art-status").textContent = "Map artwork unavailable";
  } finally {
    loadingArtwork = null;
  }
}

async function loadManifest() {
  if (!state || manifestRun === state.run) return;
  manifestRun = state.run;
  $("manifest").replaceChildren();
  try {
    const manifest = await (await api("/api/export/manifest.json")).json();
    const rows = [
      ["Run", manifest.run],
      ["Schema", manifest.schema],
      ["Step", manifest.stepMs == null ? null : `${manifest.stepMs} ms`],
      ["Initial target", manifest.expectedBots],
      ["Label", manifest.label],
      ["Time basis", manifest.timeBasis],
      ["Model", manifest.model],
      ["Worker profile", manifest.profile ? `${manifest.profile.slice(0, 16)}…` : null],
      [
        "Journal",
        manifest.journal == null
          ? null
          : manifest.journal
            ? "events and snapshots journaled by the world"
            : "current snapshot only; this world build journals nothing",
      ],
      [
        "History retention",
        "Up to 4,000 recent samples in this session; recent recorded history loads on connect. " +
          "The bridge keeps five-minute long-term buckets and milestone snapshots for the whole run " +
          "(see the retention note in the Journal panel). Python observations are sampled per bot.",
      ],
    ];
    const settings = manifest.settings || {};
    const keep = (key) =>
      key.startsWith("Observatory.") ||
      key.startsWith("Rate.XP") ||
      key === "MapUpdateInterval" ||
      /^AiPlayerbot\.(MinRandomBots|MaxRandomBots|BotGuids|RandomBotAutologin)$/.test(key);
    const shown = Object.keys(settings).filter(keep).sort();
    for (const key of shown) rows.push([key, settings[key] === "" ? "(empty)" : settings[key]]);
    const hidden = Object.keys(settings).length - shown.length;
    if (hidden > 0) rows.push(["Other settings", `${hidden} more in the manifest export`]);
    $("manifest").replaceChildren(
      ...rows
        .filter(([, value]) => value != null)
        .flatMap(([key, value]) => {
          const dt = document.createElement("dt"),
            dd = document.createElement("dd");
          dt.textContent = key;
          dd.textContent = String(value);
          return [dt, dd];
        }),
    );
  } catch (error) {
    const dd = document.createElement("dd");
    dd.textContent = `Manifest unavailable: ${error.message}`;
    $("manifest").replaceChildren(dd);
  }
}

async function connect() {
  abort?.abort();
  abort = new AbortController();
  const signal = abort.signal;
  while (!signal.aborted) {
    try {
      linkState("connecting");
      // Older bridges still support live snapshots; history becomes available when the bridge is updated.
      let frames = [];
      try {
        frames = await (await api("/api/history", { signal })).json();
      } catch {
        if (signal.aborted) return;
      }
      const snapshot = await (await api("/api/snapshot", { signal })).json();
      if (signal.aborted) return;
      // Restore once per connection; the snapshot establishes a cutoff before live SSE resumes.
      const restored = retainedHistory(frames, snapshot);
      const sameRun = state?.run === snapshot.run;
      if (!sameRun) resetRun();
      history = (sameRun ? history.filter((point) => point.simMs < restored[0].simMs) : [])
        .concat(restored)
        .slice(-4000);
      for (const frame of frames) if (frame.run === snapshot.run && frame.bots) recordTrails(trails, frame);
      state = null;
      events = [];
      ingest(snapshot, true);
      linkState("live");
      await refreshLongTerm(signal);
      const response = await api("/api/stream", { signal });
      const reader = response.body.pipeThrough(new TextDecoderStream()).getReader();
      let buffer = "";
      while (!signal.aborted) {
        const { value, done } = await reader.read();
        if (done) break;
        buffer += value;
        let end;
        while ((end = buffer.indexOf("\n\n")) >= 0) {
          const message = buffer.slice(0, end);
          buffer = buffer.slice(end + 2);
          if (message.startsWith("data: ")) ingest(JSON.parse(message.slice(6)));
        }
      }
    } catch (error) {
      if (!signal.aborted) {
        notice(`Connection interrupted: ${error.message}. Reconnecting.`);
        linkState("reconnecting");
      }
    }
    if (!signal.aborted) await new Promise((resolve) => setTimeout(resolve, 1500));
  }
}

// The long-term tier lives in the bridge: five-minute buckets for the whole run, refreshed once a minute.
async function refreshLongTerm(signal) {
  if (!token || isPython()) return;
  try {
    const [snapshots, counts] = await Promise.all([
      api("/api/history?scope=long-term", { signal }).then((response) => response.json()),
      api("/api/event-stats?scope=long-term", { signal }).then((response) => response.json()),
    ]);
    longTerm = {
      bucketMs: snapshots.bucketMs,
      points: longTermSeries(snapshots.points, counts.points, snapshots.bucketMs),
    };
  } catch {
    if (signal?.aborted) return;
    longTerm = { bucketMs: 0, points: [] }; // older bridges have no long-term tier
  }
  try {
    retention = await (await api("/api/retention", { signal })).json();
  } catch {
    retention = null;
  }
  renderRetention();
  drawChart();
}

function renderRetention() {
  if (!retention) {
    $("retention").textContent = "Retention: this bridge keeps no long-term tier; update it for whole-run history.";
    return;
  }
  const journal = (name) => {
    const entry = retention.journals[name];
    const parts = [`${name} ${formatBytes(entry.currentBytes)} live`];
    if (entry.segments) parts.push(`${entry.segments} segments ${formatBytes(entry.segmentBytes)}`);
    if (entry.prunedSegments) parts.push(`${entry.prunedSegments} pruned ${formatBytes(entry.prunedBytes)}`);
    return parts.join(", ");
  };
  const long = retention.longTerm;
  $("retention").textContent =
    `Recent on disk: ${journal("events")}; ${journal("snapshots")}. ` +
    (retention.rotation
      ? `Segments beyond ${formatBytes(retention.retainBytes)} per journal are pruned once folded in. `
      : "The world is not rotating journals (Observatory.JournalSegmentBytes = 0), so nothing is pruned. ") +
    `Long term: ${formatNumber(long.snapshotBuckets)} snapshot and ${formatNumber(long.eventBuckets)} event ` +
    `${Math.round(retention.bucketMs / 60000)}-minute buckets, ${formatNumber(long.milestones)} milestones, ` +
    `progression ${formatBytes(long.progressionBytes)}.`;
}

function resetRun() {
  history = [];
  events = [];
  longTerm = { bucketMs: 0, points: [] };
  retention = null;
  selected = "";
  needsFit = true;
  pendingControl = 0;
  controlLog = [];
  gaps = 0;
  trails.clear();
  feedFrozen = null;
  eventStats = null;
  workerLog = null;
  held = false;
  heldSamples = 0;
  renderHold();
}

// ------------------------------------------------------------------------------------------------ ingest

function ingest(snapshot, restoring = false) {
  if (state && state.run !== snapshot.run) resetRun();
  if (state?.run === snapshot.run && state.seq >= snapshot.seq) return;
  if (!restoring && state?.run === snapshot.run && snapshot.seq > state.seq + 1) gaps += snapshot.seq - state.seq - 1;
  const previousTarget = state?.expectedBots;
  const previousLimit = state?.speedControl?.backlogLimitMs;
  const previousQueueLimit = state?.llmQueueLimit;
  state = snapshot;
  if (previousLimit !== state.speedControl?.backlogLimitMs && state.speedControl)
    $("backlog-limit").value = state.speedControl.backlogLimitMs;
  if (previousQueueLimit !== state.llmQueueLimit && state.llmQueueLimit !== undefined)
    $("llm-queue-limit").value = state.llmQueueLimit;
  freshAt = performance.now();
  ingestTimes.push(freshAt);
  ingestTimes = ingestTimes.filter((time) => freshAt - time <= 10000);
  if (!restoring && (!history.length || history.at(-1).simMs !== state.simMs)) history.push(summarize(state));
  if (history.length > 4000) history.splice(0, history.length - 4000);
  recordTrails(trails, state);
  for (const entry of controlLog) {
    if (entry.ackMs == null && state.controlSeq >= entry.sequence) {
      entry.ackMs = performance.now() - entry.sentAt;
      entry.error = state.controlSeq === entry.sequence ? state.controlError || "" : "";
    }
  }
  // A held view keeps ingesting — history, trails and the journal stay complete — and simply stops redrawing,
  // so a moment can be read without it scrolling away. Releasing shows everything that arrived meanwhile.
  if (held) {
    heldSamples += 1;
    renderHold();
    return;
  }
  renderAll(previousTarget);
}

function renderAll(previousTarget) {
  updateMaps();
  renderHeader();
  renderInstruments(previousTarget);
  renderMetrics();
  renderAlerts();
  renderLegend();
  renderRoster();
  renderDetails();
  renderWorldState();
  renderAnalytics();
  drawChart();
  renderControlLog();
  renderInterpreter();
  loadManifest();
}

function renderHold() {
  const button = $("hold");
  button.setAttribute("aria-pressed", String(held));
  button.textContent = held ? "Release" : "Hold";
  document.body.classList.toggle("held", held);
  if (!held) {
    $("hold-status").textContent = "";
    return;
  }
  const samples = `${formatNumber(heldSamples)} sample${heldSamples === 1 ? "" : "s"} behind`;
  const behind = heldSamples ? samples : "up to date";
  $("hold-status").textContent = `View held at ${heldAt} · ${behind}`;
}

$("hold").onclick = () => {
  held = !held;
  heldSamples = 0;
  heldAt = state ? (isPython() ? duration(state.simMs) : formatClock(state.simMs)) : "";
  renderHold();
  if (!held && state) renderAll(state.expectedBots);
};

function isPython() {
  return state?.source === "python-api";
}

// An ordinary realm publishing through mod-alles: read-only, real time, with an interpreter worker.
function isAlles() {
  return state?.source === "alles-live" || state?.source === "alles-simulation";
}

function isReadOnly() {
  return isPython() || Boolean(state?.readOnly);
}

// ------------------------------------------------------------------------------------------------ header

function renderHeader() {
  const python = isPython();
  $("run-label").textContent = python
    ? `${state.label}, Python observation feed, run ${state.run}`
    : `Run ${state.run}, ${state.telemetryStale ? "recorded" : "in-memory"} telemetry`;
  if (state.telemetryStale) {
    notice(state.source === "alles-live"
      ? "World telemetry is stale. Showing the last recorded sample until the server reports again."
      : "Recorded Observatory run. No live world telemetry is connected to this dashboard.");
    return;
  }
  if (isAlles()) {
    const worker = state.interpreter || {};
    const budget = budgetState(worker);
    const conversation = state.conversation || {};
    $("run-label").textContent = `Live realm, run ${state.run}`;
    notice(
      `Live world telemetry · ${worker.model || "no model"} · ` +
        `${worker.connected ? "worker connected" : "worker disconnected"} · ` +
        (budget.mode === "rolling"
          ? `${worker.remainingRequests ?? "?"} of ${worker.requestsPerMinute} requests left this minute`
          : budget.capped
            ? `${budget.used} of ${budget.max} pilot requests used`
            : `${formatNumber(budget.used)} requests charged, no cap`) +
        ` · ${formatNumber(worker.modelMemories)} model memories, ${formatNumber(worker.fallbackMemories)} by fallback` +
        (conversation.enabled
          ? ` · ${formatNumber(conversation.replies)} replies, ${formatNumber(conversation.pendingReplies)} pending, ` +
            `${formatNumber(conversation.actions)} actions`
          : "") +
        (worker.ledgerFault
          ? " · provider ledger fault"
          : budget.exhausted && budget.mode === "trial"
            ? " · budget used; template fallback active"
            : ""),
    );
    return;
  }
  const backlog = `${((state.backlogMs ?? 0) / 1000).toFixed(1)} s`;
  notice(
    python
      ? `${state.label}: ${state.completed ? "feed closed, showing recorded history." : "Python observations."}`
      : state.completed
        ? "Simulated duration complete. Export this run to compare it."
        : state.fault
          ? `Run frozen: ${state.fault}. Export the run and inspect the server.`
          : state.paused
            ? "Paused. Observation and controls stay available."
            : state.observers
              ? `GM POV connected (${state.observers}, ${observerModeLabel(state.observerMode)}), ` +
                "1× locked until all observers disconnect."
              : state.overloaded
                ? `Running behind: ${backlog} of simulated steps queued, none dropped.`
                : `Run ${state.run} live at ${state.requestedSpeed}×.`,
  );
}

function renderAlerts() {
  const stale =
    !state.completed &&
    (performance.now() - freshAt > 3000 ||
      (state.publishedUnixMs !== undefined && Date.now() - state.publishedUnixMs > 3000));
  const silent = isPython() ? 0 : stalledBots(state).length;
  $("alerts").replaceChildren(
    ...alerts(state, { stale, gaps, silentSince: silent || null }).map((alert) => {
      const li = document.createElement("li");
      li.dataset.level = alert.level;
      li.textContent = alert.text;
      return li;
    }),
  );
}

// ------------------------------------------------------------------------------------------- instruments

function figureSpark(id, key, color, max) {
  requestAnimationFrame(() => {
    const { ctx, width, height } = surface($(id));
    sparkline(ctx, width, height, series(history, key, 60), color, { max });
  });
}

function renderInstruments(previousTarget) {
  const python = isPython();
  const readOnly = isReadOnly();
  const time = clock(state.simMs);
  const perSecond = `${(ingestTimes.length / 10).toFixed(1)} snapshots/s`;
  $("clock-label").textContent = state.source === "alles-live" ? "Realm uptime" : python ? "Elapsed publisher time" : "Simulated time";
  $("clock-time").textContent = time.time;
  $("clock-days").textContent = python ? "" : `day ${time.days + 1}`;
  $("clock-real").textContent = python ? perSecond : `${duration(state.realMs)} real, ${perSecond}`;

  const ratio = python || !state.requestedSpeed ? 0 : state.achievedSpeed / state.requestedSpeed;
  const speedColor = ratio >= 0.95 ? palette.moss : ratio >= 0.7 ? palette.brass : palette.ember;
  requestAnimationFrame(() => {
    const { ctx, width, height } = surface($("speed-bar"));
    speedBar(ctx, width, height, ratio, {
      color: speedColor,
      requested: python ? "" : `${state.requestedSpeed}×`,
      paused: Boolean(state.paused),
      empty: python,
    });
  });
  $("speed-achieved").textContent = python ? "–" : `${state.achievedSpeed.toFixed(2)}×`;
  const maxSpeed = state.speedControl?.mode === "max";
  $("speed-requested").textContent = python
    ? "no simulation control"
    : `${maxSpeed ? "Max · " : ""}requested ${state.requestedSpeed}×`;
  $("speed-ratio").textContent = python ? "" : state.paused ? ", paused" : `, ${Math.round(ratio * 100)}% achieved`;

  const alles = isAlles();
  document.querySelector(".bench").classList.toggle("alles-simulation", state.source === "alles-simulation");
  // An ordinary realm runs at 1× with no backlog or tick budget; its bench shows the interpreter instead.
  $("speed-figure").hidden = alles && readOnly;
  $("backlog-figure").hidden = $("tick-figure").hidden = python || (alles && readOnly);
  $("requests-figure").hidden = $("memories-figure").hidden = !alles;
  if (!python && !(alles && readOnly)) {
    $("backlog-value").textContent = `${(state.backlogMs / 1000).toFixed(2)} s`;
    $("backlog-value").dataset.state = state.overloaded ? "danger" : "";
    $("tick-value").textContent = `${(state.maxTickUs / 1000).toFixed(1)} ms`;
    figureSpark("backlog-spark", "backlogMs", state.overloaded ? palette.ember : palette.brass);
    figureSpark("tick-spark", "tickMs", palette.ash);
  }
  if (alles) renderInterpreterFigures();

  // The deck is never emptied: Hold works on any feed, and a feed without simulation control says so rather
  // than leaving a blank space where the buttons used to be.
  const controllable = !readOnly;
  for (const id of SIMULATION_CONTROLS) $(id).hidden = !controllable;
  $("no-controls").hidden = controllable;
  if (!controllable)
    $("no-controls").textContent = isAlles()
      ? "This realm runs at real time: an ordinary world has no fixed-step clock to pause or accelerate. " +
        "Hold freezes the view while the bridge keeps recording."
      : "This feed is read-only, so pause, speed and population cannot be set from here. " +
        "Hold freezes the view while the bridge keeps recording.";
  const locked = Boolean(readOnly || state.fault || state.completed || state.baseline || state.observers);
  $("pause").disabled = locked;
  $("pause").textContent = state.paused ? "Resume" : "Pause";
  $("compact-clock").textContent = time.time;
  $("compact-speed").textContent = state.paused ? "Paused" :
    `${state.achievedSpeed.toFixed(1)}×${maxSpeed ? " · Max" : ""}${state.queueHeld ? " · waiting for LLM" : ""}`;
  $("compact-status").textContent = `${state.onlineBots ?? 0} bots` +
    (state.llmQueueLimit === undefined ? "" : ` · LLM ${state.llmQueued ?? 0}/${state.llmQueueLimit}`);
  $("compact-pause").disabled = locked;
  $("compact-pause").hidden = readOnly;
  $("compact-pause").textContent = state.paused ? "Resume" : "Pause";
  for (const button of $("speed-control").querySelectorAll("button")) {
    const isMax = button.dataset.speed === "max";
    button.disabled = locked || (isMax && !state.speedControl);
    button.setAttribute(
      "aria-pressed",
      String(isMax ? maxSpeed : !maxSpeed && Number(button.dataset.speed) === state.requestedSpeed),
    );
  }
  $("llm-queue-limit").disabled = locked || state.llmQueueLimit === undefined;
  $("backlog-limit").disabled = $("set-max-speed").disabled = locked || !state.speedControl;
  $("max-speed-status").textContent = maxSpeed
    ? `${state.speedControl.status}. Backlog target: ${state.speedControl.backlogLimitMs} ms; ` +
      "brief spikes may occur while speed adjusts."
    : state.speedControl
      ? state.speedStep === 0.1
        ? "Max adjusts from 1× to 10× in 0.1× steps using the backlog trend."
        : "Max selects 1×, 2×, 5× or 10×. Update the worldserver to enable 0.1× steps."
      : "Bridge update required for Max speed.";
  if (state.llmQueueLimit !== undefined)
    $("max-speed-status").textContent += ` LLM jobs: ${state.llmQueued ?? 0}/${state.llmQueueLimit}. ` +
      (state.queueHeld ? "Simulation waiting for the queue to drain." : "Max holds gameplay at the queue limit.");
  const populationLocked = Boolean(
    readOnly || state.fault || state.completed || state.baseline || state.maxBots === undefined,
  );
  $("bot-count").disabled = $("set-bots").disabled = $("bot-range").disabled = populationLocked;
  $("bot-count").max = $("bot-range").max = state.maxBots ?? 100;
  if (!state.racePopulation && previousTarget !== state.expectedBots)
    $("bot-count").value = $("bot-range").value = state.expectedBots;
  renderRacePopulation(populationLocked);
  const observerLocked = Boolean(
    readOnly || state.fault || state.completed || state.baseline || state.observerMode === undefined,
  );
  for (const button of $("observer-mode").querySelectorAll("button")) {
    button.disabled = observerLocked;
    button.setAttribute("aria-pressed", String(Number(button.dataset.mode) === state.observerMode));
  }
  $("observer-status").textContent = readOnly
    ? ""
    : state.observerMode === undefined
      ? "Observer mode needs a newer server"
      : !state.observersAllowed
        ? "GM observers are refused by this run's configuration."
        : state.observerMode === 2
          ? "Full GM: commands and movement are journaled; this run is not a clean comparison."
          : state.observerMode === 1
            ? "Roam: observers may move; commands stay limited to /pov. Grid load may differ."
            : "Locked: observers only use /pov. Applies at login and live to connected GMs.";
  $("population-status").textContent = readOnly
    ? ""
    : state.maxBots === undefined
      ? "Bot count needs a newer server"
      : `${state.onlineBots} of ${state.expectedBots} bots online, ` +
        (state.populationPending ? (state.paused ? "waiting for Resume" : "adjusting") : "matched") +
        `. Pool ${state.maxBots}.`;
  $("control-status").textContent = readOnly
    ? "This feed is read-only."
    : pendingControl && state.controlSeq < pendingControl
      ? `Request ${pendingControl} sent, waiting for the world`
      : state.controlError || `World applied request ${state.controlSeq}`;

  // Lamps stay hidden while every state is the default; one non-default state shows the whole row for comparison.
  const lamps = python
    ? [
        ["ok", state.completed ? "Feed closed" : "Feed publishing", !state.completed],
        [performance.now() - freshAt > 3000 ? "warn" : "ok", "Snapshots fresh", performance.now() - freshAt <= 3000],
      ]
    : alles
      ? allesLamps()
      : [
        [
          state.ready ? "ok" : "warn",
          state.ready ? `Cohort ready since ${duration(state.readyAtMs)}` : "Cohort forming",
          Boolean(state.ready),
        ],
        [
          state.populationPending ? "warn" : "ok",
          state.populationPending ? "Population adjusting" : "Population matched",
          !state.populationPending,
        ],
        [
          state.overloaded ? "danger" : "ok",
          state.overloaded ? "Load overloaded" : "Load keeping up",
          !state.overloaded,
        ],
        [state.fault ? "danger" : "ok", state.fault ? `Journal ${state.fault}` : "Journal healthy", !state.fault],
        [
          state.observers ? "warn" : "ok",
          state.observers
            ? `${state.observers} GM observer${state.observers === 1 ? "" : "s"} connected, ` +
              observerModeLabel(state.observerMode)
            : state.observersAllowed
              ? `GM observers allowed, ${observerModeLabel(state.observerMode)}`
              : "GM observers refused",
          !state.observers && !state.observerMode,
        ],
        [
          state.baseline ? "warn" : state.completed ? "off" : "ok",
          state.baseline ? "Real-time baseline" : state.completed ? "Completed" : "Virtual time",
          !state.baseline && !state.completed,
        ],
      ];
  const allDefault = lamps.every(([, , isDefault]) => isDefault);
  $("lamps").replaceChildren(
    ...(allDefault ? [] : lamps).map(([on, text]) => {
      const span = document.createElement("span");
      span.className = "lamp";
      span.dataset.on = on;
      span.textContent = text;
      return span;
    }),
  );
}

// ----------------------------------------------------------------------------------------------- metrics

function renderMetrics() {
  const python = isPython();
  const xpRate = rate(history, "xp", RATE_WINDOW_MS),
    questRate = rate(history, "quests", RATE_WINDOW_MS),
    deathRate = rate(history, "deaths", RATE_WINDOW_MS),
    killRate = rate(history, "kills", RATE_WINDOW_MS);
  const perHour = (value, basis) =>
    value ? `${formatNumber(value.perHour, value.perHour < 10 ? 1 : 0)} per ${basis} hour` : "collecting rate";
  const windowMs = Math.min(RATE_WINDOW_MS, (xpRate || questRate || deathRate || { deltaMs: 0 }).deltaMs);
  const window = `last ${Math.max(1, Math.round(windowMs / 60000))} min`;
  const health = healthStats(state.bots);
  const quests = questStats(state.bots);
  const levels = state.bots.map((bot) => bot.level);
  const last = history.at(-1);
  const worker = state.interpreter || {};
  const unmeasured = state?.source === "alles-simulation"
    ? "not reported by this simulation feed"
    : "not measured by this world build";
  const tiles = isAlles()
    ? [
        [
          "Bots",
          state.activeBots == null ? `${state.onlineBots}` : `${state.activeBots} of ${state.onlineBots}`,
          state.activeBots == null ? "online; AI activity is not reported by this world build" : "active of online",
        ],
        [
          "XP earned",
          last?.xp == null ? "unknown" : formatNumber(state.runTotals?.xp ?? last.xp),
          last?.xp == null ? unmeasured : `${perHour(xpRate, "realm")}, ${window}`,
          "xp",
          palette.brass,
        ],
        [
          "Quests completed",
          last?.quests == null ? "unknown" : formatNumber(state.runTotals?.quests ?? last.quests),
          last?.quests == null ? `${quests.active} in logs; ${unmeasured}` : `${perHour(questRate, "realm")}, ${quests.active} in logs`,
          "quests",
          palette.moss,
        ],
        [
          "Deaths",
          last?.deaths == null ? "unknown" : formatNumber(state.runTotals?.deaths ?? last.deaths),
          last?.deaths == null ? unmeasured : `${perHour(deathRate, "realm")}, ${window}`,
          "deaths",
          palette.ember,
        ],
        [
          "Memories held",
          formatNumber(last?.memories),
          `${formatNumber(last?.pendingPerceptions)} perceptions pending`,
          "memories",
          palette.lilac,
        ],
        [
          "Model memories",
          formatNumber(worker.modelMemories),
          `${formatNumber(worker.fallbackMemories)} by fallback` +
            (worker.invalidResults ? `, ${formatNumber(worker.invalidResults)} invalid` : ""),
          "modelMemories",
          palette.sky,
        ],
        [
          "Mean level",
          formatNumber(last?.meanLevel, 1),
          levels.length ? `lowest ${Math.min(...levels)}, highest ${Math.max(...levels)}` : "no bots",
        ],
        [
          "Mean health",
          health.mean == null ? "unknown" : `${health.mean.toFixed(0)}%`,
          `${health.low} below 35%, ${health.dead} dead`,
        ],
      ]
    : python
    ? [
        ["Observed bots", state.bots.length, `${state.bots.filter((bot) => observationAge(bot) > 3000).length} stale`],
        ["Alive", state.bots.filter((bot) => bot.alive).length, "last observation"],
        ["In combat", state.bots.filter((bot) => bot.combat).length, "last observation"],
        [
          "Kills",
          state.runTotals.kills,
          killRate ? `${perHour(killRate, "elapsed")}` : "since baselines",
          "kills",
          palette.brass,
        ],
        ["Deaths", state.runTotals.deaths, "since baselines", "deaths", palette.ember],
        ["Levels gained", state.runTotals.levelGains, "since baselines", "levelGains", palette.lilac],
      ]
    : [
        ["Bots", `${state.activeBots} of ${state.onlineBots}`, `active of online, target ${state.expectedBots}`],
        [
          "XP earned",
          formatNumber(state.runTotals?.xp ?? last?.xp),
          `${perHour(xpRate, "simulated")}, ${window}`,
          "xp",
          palette.brass,
        ],
        [
          "Quests completed",
          formatNumber(state.runTotals?.quests ?? last?.quests),
          `${perHour(questRate, "simulated")}, ${quests.active} in logs`,
          "quests",
          palette.moss,
        ],
        [
          "Deaths",
          formatNumber(state.runTotals?.deaths ?? last?.deaths),
          `${perHour(deathRate, "simulated")}, ${window}`,
          "deaths",
          palette.ember,
        ],
        [
          "Mean level",
          formatNumber(last?.meanLevel, 1),
          levels.length ? `lowest ${Math.min(...levels)}, highest ${Math.max(...levels)}` : "no bots",
        ],
        [
          "Mean health",
          health.mean == null ? "unknown" : `${health.mean.toFixed(0)}%`,
          `${health.low} below 35%, ${health.dead} dead`,
        ],
      ];
  $("metrics").replaceChildren(
    ...tiles.map(([label, value, sub, key, color]) => {
      const div = document.createElement("div");
      div.className = "metric";
      const span = document.createElement("span");
      span.textContent = label;
      const b = document.createElement("b");
      b.textContent = String(value ?? "unknown");
      const small = document.createElement("small");
      small.textContent = sub;
      div.append(span, b, small);
      if (key) {
        const canvas = document.createElement("canvas");
        canvas.setAttribute("aria-hidden", "true");
        div.append(canvas);
        requestAnimationFrame(() => {
          const { ctx, width, height } = surface(canvas);
          sparkline(ctx, width, height, series(history, key), color);
        });
      }
      return div;
    }),
  );
}

// ---------------------------------------------------------------------------------------------- controls

function observerModeLabel(mode) {
  return mode === 2 ? "full GM" : mode === 1 ? "roam" : "locked";
}

async function control(paused, speed, bots, observerMode, raceCounts) {
  if (!state || isReadOnly()) return;
  if (speed === "max" && !$("max-speed-settings").reportValidity()) return;
  const speedLabel = speed === "max" ? `Max (backlog target ${$("backlog-limit").value} ms)` : `${speed}×`;
  const text =
    observerMode !== undefined
      ? `GM observers ${observerModeLabel(observerMode)}`
      : bots === undefined
        ? `${paused ? "pause" : "resume"} at ${speedLabel}`
        : `${bots} bots at ${speedLabel}${paused ? ", paused" : ""}`;
  try {
    const response = await api("/api/control", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        run: state.run,
        speed,
        paused,
        ...(bots === undefined ? {} : { bots }),
        ...(raceCounts === undefined ? {} : { raceCounts }),
        ...(speed === "max" && state.llmQueueLimit !== undefined
          ? { llmQueueLimit: Number($("llm-queue-limit").value) } : {}),
        ...(observerMode === undefined ? {} : { observerMode }),
        ...(speed === "max" ? { backlogLimitMs: Number($("backlog-limit").value) } : {}),
      }),
    });
    pendingControl = (await response.json()).sequence;
    controlLog.push({ sequence: pendingControl, text, sentAt: performance.now(), ackMs: null, error: null });
    controlLog = controlLog.slice(-8);
    $("control-status").textContent = `Request ${pendingControl} sent, waiting for the world`;
  } catch (error) {
    controlLog.push({ sequence: null, text, sentAt: performance.now(), ackMs: 0, error: error.message });
    controlLog = controlLog.slice(-8);
    notice(error.message);
    if (raceCounts !== undefined) $("population-error").textContent = error.message;
    renderControlLog();
    return false;
  }
  renderControlLog();
  return true;
}

function renderControlLog() {
  $("control-log").replaceChildren(
    ...controlLog
      .slice()
      .reverse()
      .map((entry) => {
        const li = document.createElement("li");
        const b = document.createElement("b");
        b.textContent = entry.sequence ? `#${entry.sequence} ${entry.text}` : entry.text;
        const status = document.createElement("span");
        status.className = entry.error ? "err" : entry.ackMs == null ? "" : "ok";
        status.textContent = entry.error
          ? ` rejected: ${entry.error}`
          : entry.ackMs == null
            ? " sent, waiting for the world"
            : ` applied after ${(entry.ackMs / 1000).toFixed(1)} s`;
        li.append(b, status);
        return li;
      }),
  );
  if (!controlLog.length) {
    const li = document.createElement("li");
    li.textContent = "No control requests yet. Pause, speed, bot count and observer mode changes appear here.";
    $("control-log").replaceChildren(li);
  }
}

$("connect").addEventListener("submit", (event) => {
  event.preventDefault();
  token = $("token").value;
  // The token is entered once; after that the header belongs to the run identity and the link lamp.
  $("connect").classList.add("collapsed");
  $("change-token").hidden = false;
  loadMaps().catch(() => {
    $("map-art-status").textContent = "Map artwork unavailable";
  });
  connect();
});
$("change-token").onclick = () => {
  $("connect").classList.remove("collapsed");
  $("change-token").hidden = true;
  $("token").focus();
  $("token").select();
};
$("population").onsubmit = async (event) => {
  event.preventDefault();
  if (!$("population").reportValidity()) return;
  if (!state.racePopulation) return control(state.paused, selectedSpeed(), Number($("bot-count").value));
  await applyPopulation();
};
function resizePopulationDraft(value) {
  $("population-mode").value = "auto";
  try {
    if (value === "") return;
    populationDraft = resizePopulation(populationResizeBase ?? populationDraft, Number(value),
      Object.fromEntries(state.racePopulation.map((row) => [row.race, row.capacity])));
    populationDirty = true;
    $("population-error").textContent = "";
    writePopulationDraft();
  } catch (error) {
    $("population-error").textContent = error.message;
  }
}
$("bot-range").oninput = () => {
  $("bot-count").value = $("bot-range").value;
  if (state?.racePopulation) resizePopulationDraft($("bot-range").value);
};
$("bot-count").oninput = () => {
  $("bot-range").value = $("bot-count").value;
  if (state?.racePopulation) resizePopulationDraft($("bot-count").value);
};
$("population-mode").onchange = () => {
  if ($("population-mode").value === "races") $("race-population").open = true;
};
function selectedSpeed() {
  return state.speedControl?.mode === "max" ? "max" : state.requestedSpeed;
}
$("pause").onclick = $("compact-pause").onclick = () => control(!state.paused, selectedSpeed());
$("expand-controls").onclick = () => window.scrollTo({ top: 0, behavior: "smooth" });
$("max-speed-settings").onsubmit = (event) => {
  event.preventDefault();
  control(state.paused, "max");
};
for (const button of $("speed-control").querySelectorAll("button"))
  button.onclick = () => control(state.paused, button.dataset.speed === "max" ? "max" : Number(button.dataset.speed));
// Observer mode is independent of the speed lock: it may change while GMs are connected.
for (const button of $("observer-mode").querySelectorAll("button"))
  button.onclick = () => control(state.paused, selectedSpeed(), undefined, Number(button.dataset.mode));
$("export").onclick = async () => {
  try {
    const name = $("export-file").value;
    const response = await api(`/api/export/${name}`),
      blob = await response.blob();
    const url = URL.createObjectURL(blob),
      link = document.createElement("a");
    link.href = url;
    link.download = name;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  } catch (error) {
    notice(error.message);
  }
};

// --------------------------------------------------------------------------------------------- map panel

function options(id, values, all = false) {
  const element = $(id),
    old = element.value;
  const desired = (all ? ["all"] : []).concat([...new Set(values)].sort((a, b) => Number(a) - Number(b)));
  if ([...element.options].map((option) => option.value).join() === desired.join()) return;
  element.replaceChildren(
    ...desired.map((value) => {
      const label =
        id === "map"
          ? continentNames[value] || `Map ${value}`
          : value === "all"
            ? "Bot zone (automatic)"
            : value === "0"
              ? "Whole continent"
              : zoneName($("map").value, value);
      return new Option(label, value);
    }),
  );
  if (desired.includes(old)) element.value = old;
}

function placeName(map, zone) {
  return zone == null ? continentNames[map] || `Map ${map}` : zoneName(map, zone);
}

function zoneName(map, zone) {
  const area = mapAreas.find((area) => String(area.map) === String(map) && String(area.zone) === String(zone));
  return area ? area.name.replace(/([a-z])([A-Z])/g, "$1 $2") : `Zone ${zone}`;
}

function updateMaps() {
  options("map", [...state.bots.map((bot) => String(bot.map)), ...mapAreas.map((area) => String(area.map))]);
  updateZones();
  updateInstances();
}

function updateZones() {
  if (state)
    options(
      "zone",
      [
        ...state.bots
          .filter((bot) => String(bot.map) === $("map").value && bot.zone != null)
          .map((bot) => String(bot.zone)),
        ...mapAreas.filter((area) => String(area.map) === $("map").value).map((area) => String(area.zone)),
      ],
      true,
    );
}

function updateInstances() {
  const element = $("instance"),
    old = element.value;
  const values = [
    ...new Set(state.bots.filter((bot) => String(bot.map) === $("map").value).map((bot) => String(bot.instance))),
  ].sort((a, b) => Number(a) - Number(b));
  element.replaceChildren(new Option("All instances", "all"), ...values.map((id) => new Option(`Instance ${id}`, id)));
  element.value = values.includes(old) ? old : "all";
}

$("map").onchange = () => {
  updateZones();
  updateInstances();
  needsFit = true;
  fitArtwork = true;
};
$("instance").onchange = () => {
  needsFit = true;
};
$("zone").onchange = $("fit-map").onclick = () => {
  needsFit = true;
  fitArtwork = true;
};
$("fit").onclick = () => {
  needsFit = true;
  fitArtwork = false;
};
$("map-art").onchange = () => {
  if ($("map-art").checked) {
    needsFit = true;
    fitArtwork = true;
  }
};
$("map-colour").onchange = () => {
  renderLegend();
  renderRoster();
};

function healthColor(percent) {
  if (percent <= 0) return ACTIVITY_COLORS.dead;
  if (percent < 35) return palette.ember;
  if (percent < 70) return palette.brass;
  return palette.moss;
}

function levelColor(level, min, max) {
  const t = max > min ? (level - min) / (max - min) : 0.5;
  const mix = (a, b) => Math.round(a + (b - a) * t);
  return `rgb(${mix(108, 212)}, ${mix(176, 162)}, ${mix(220, 72)})`;
}

function levelRange(bots) {
  const levels = bots.map((bot) => bot.level);
  return levels.length ? [Math.min(...levels), Math.max(...levels)] : [0, 1];
}

function markerColor(bot, bots) {
  const mode = $("map-colour").value;
  if (mode === "health") return healthColor(healthPercent(bot));
  if (mode === "level") return levelColor(bot.level, ...levelRange(bots));
  return ACTIVITY_COLORS[bot.activity] || ACTIVITY_COLORS.idle;
}

function renderLegend() {
  const mode = $("map-colour").value;
  const bots = state?.bots || [];
  const mix = state ? summarize(state).activity : {};
  const [low, high] = levelRange(bots);
  const items =
    mode === "health"
      ? [
          ["healthy", palette.moss],
          ["hurt", palette.brass],
          ["under 35%", palette.ember],
          ["dead", ACTIVITY_COLORS.dead],
        ]
      : mode === "level"
        ? [
            [`level ${low}`, levelColor(low, low, high)],
            [`level ${high}`, levelColor(high, low, high)],
          ]
        : ACTIVITIES.map((activity) => [`${activity}${state ? ` ${mix[activity]}` : ""}`, ACTIVITY_COLORS[activity]]);
  $("map-legend").replaceChildren(
    ...items.map(([label, color]) => {
      const li = document.createElement("li");
      li.style.setProperty("--swatch", color);
      li.textContent = label;
      return li;
    }),
  );
}

const canvas = $("map-canvas");
let drag = null;

function hitTest(event) {
  if (!state) return null;
  const rect = canvas.getBoundingClientRect();
  const bots = visibleBots(state, $("map").value, $("zone").value, $("instance").value);
  return bots
    .map((bot) => ({ bot, point: worldToScreen(bot, view, rect.width, rect.height) }))
    .find(({ point }) => Math.hypot(point.x - event.clientX + rect.left, point.y - event.clientY + rect.top) < 12);
}

canvas.onpointerdown = (event) => {
  drag = { px: event.clientX, py: event.clientY, x: view.x, y: view.y, moved: false };
  canvas.setPointerCapture(event.pointerId);
};
canvas.onpointermove = (event) => {
  if (drag) {
    const dx = event.clientX - drag.px,
      dy = event.clientY - drag.py;
    if (Math.hypot(dx, dy) > 4) drag.moved = true;
    view.x = drag.x + dy / view.scale;
    view.y = drag.y + dx / view.scale;
    return;
  }
  const hit = hitTest(event);
  hovered = hit?.bot.id || null;
  const tip = $("map-tip");
  if (!hit) {
    tip.hidden = true;
    return;
  }
  const rect = canvas.getBoundingClientRect();
  const bot = hit.bot;
  const bots = visibleBots(state, $("map").value, $("zone").value, $("instance").value);
  const title = document.createElement("b");
  title.style.setProperty("--swatch", markerColor(bot, bots));
  title.textContent = `${bot.name}, level ${bot.level}`;
  const detail = document.createElement("span");
  detail.textContent =
    `${bot.activity}, ${Math.round(healthPercent(bot))}% health` +
    (bot.earnedXp != null ? `, ${formatNumber(bot.earnedXp)} XP earned` : "");
  tip.replaceChildren(title, detail);
  tip.hidden = false;
  tip.style.left = `${Math.min(rect.width - 260, event.clientX - rect.left + 14)}px`;
  tip.style.top = `${event.clientY - rect.top + 14}px`;
};
canvas.onpointerleave = () => {
  $("map-tip").hidden = true;
  hovered = null;
};
canvas.onpointerup = (event) => {
  if (drag && !drag.moved) {
    const hit = hitTest(event);
    if (hit) selectBot(hit.bot.id, false);
  }
  drag = null;
};
canvas.onpointercancel = () => {
  drag = null;
};
canvas.onwheel = (event) => {
  event.preventDefault();
  view.scale = Math.max(0.002, Math.min(10, view.scale * Math.exp(-event.deltaY * 0.001)));
};

function drawScaleBar(ctx, height) {
  const yards = [...SCALE_STEPS].reverse().find((step) => step * view.scale <= 160) || SCALE_STEPS[0];
  const px = yards * view.scale;
  const x = 12,
    y = height - 44;
  ctx.strokeStyle = palette.parchment;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(x, y + 0.5);
  ctx.lineTo(x + px, y + 0.5);
  ctx.moveTo(x + 0.5, y - 4);
  ctx.lineTo(x + 0.5, y + 4);
  ctx.moveTo(x + px - 0.5, y - 4);
  ctx.lineTo(x + px - 0.5, y + 4);
  ctx.stroke();
  ctx.fillStyle = palette.parchment;
  ctx.fillText(`${yards.toLocaleString()} yd`, x, y - 7);
}

function drawMap() {
  // Returning before the surface is cleared leaves the last frame on the canvas, which is what a hold means.
  if (held) {
    requestAnimationFrame(drawMap);
    return;
  }
  const { ctx, width, height } = surface(canvas);
  ctx.fillStyle = palette.ground;
  ctx.fillRect(0, 0, width, height);
  if (state) {
    const bots = visibleBots(state, $("map").value, $("zone").value, $("instance").value);
    const area = $("map-art").checked ? chooseMap(mapAreas, $("map").value, $("zone").value, bots, selected) : null;
    if (area?.id !== artwork) {
      artwork = area?.id;
      if (area && mapImages.get(area.id)?.length)
        $("map-art-status").textContent = `${area.name} artwork from the local client`;
      if (fitArtwork) needsFit = true;
    }
    if (area && !mapImages.has(area.id) && loadingArtwork === null) loadArtwork(area);
    if (needsFit && (bots.length || area)) {
      const fit = area && (fitArtwork || !bots.length) ? mapView(area) : fitView(bots);
      view = { ...fit, scale: Math.min((width - 60) / fit.spanY, (height - 60) / fit.spanX) };
      needsFit = false;
    }
    const tiles = area && mapImages.get(area.id);
    if (tiles?.length) {
      tiles.forEach((tile, index) => {
        const layer = area.overlays?.[index - 12];
        const rect = layer
          ? { ...layer, ...mapRect(area, layer.left, layer.top, layer.width, layer.height, view, width, height) }
          : mapTile(area, index, view, width, height);
        ctx.drawImage(
          tile,
          0,
          0,
          tile.width * rect.cropX,
          tile.height * rect.cropY,
          rect.x,
          rect.y,
          rect.width,
          rect.height,
        );
      });
    }
    ctx.strokeStyle = palette.rule;
    ctx.lineWidth = 1;
    ctx.globalAlpha = tiles?.length ? 0.1 : 0.4;
    for (let x = 0; x < width; x += 50) {
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, height);
      ctx.stroke();
    }
    for (let y = 0; y < height; y += 50) {
      ctx.beginPath();
      ctx.moveTo(0, y + 0.5);
      ctx.lineTo(width, y + 0.5);
      ctx.stroke();
    }
    ctx.globalAlpha = 1;
    const focus = bots.find((bot) => bot.id === selected);
    if (focus && $("map-trails").checked) {
      const trail = (trails.get(focus.id) || []).filter(
        (sample) => sample.map === focus.map && sample.instance === focus.instance,
      );
      ctx.strokeStyle = palette.brass;
      ctx.lineWidth = 2;
      ctx.lineJoin = "round";
      ctx.beginPath();
      trail.forEach((sample, index) => {
        const point = worldToScreen(sample, view, width, height);
        if (index) ctx.lineTo(point.x, point.y);
        else ctx.moveTo(point.x, point.y);
      });
      ctx.globalAlpha = 0.7;
      ctx.stroke();
      ctx.globalAlpha = 1;
    }
    const mode = $("map-colour").value;
    const ringFade = Math.min(1, (performance.now() - selectedAt) / 150);
    labelFont(ctx);
    const labels = [];
    for (const bot of bots) {
      const point = worldToScreen(bot, view, width, height);
      const isSelected = bot.id === selected;
      const active = isSelected || bot.id === hovered;
      const radius = active ? 8 : 6;
      const dead = bot.activity === "dead" || bot.health === 0 || bot.alive === false;
      ctx.save();
      ctx.globalAlpha = observationAge(bot) > 3000 ? 0.4 : 1;
      ctx.beginPath();
      ctx.arc(point.x, point.y, radius, 0, 2 * Math.PI);
      if (dead) {
        ctx.strokeStyle = palette.ash;
        ctx.lineWidth = 1.5;
        ctx.stroke();
      } else {
        ctx.fillStyle = markerColor(bot, bots);
        ctx.fill();
        ctx.strokeStyle = palette.ground;
        ctx.lineWidth = 1.25;
        ctx.stroke();
        if (mode !== "activity" && bot.activity === "combat") {
          ctx.beginPath();
          ctx.arc(point.x, point.y, radius + 2, 0, 2 * Math.PI);
          ctx.strokeStyle = palette.ember;
          ctx.lineWidth = 1.5;
          ctx.stroke();
        }
      }
      if (isSelected) {
        ctx.beginPath();
        ctx.arc(point.x, point.y, radius, 0, 2 * Math.PI);
        ctx.strokeStyle = palette.brass;
        ctx.lineWidth = 2;
        ctx.stroke();
        ctx.beginPath();
        ctx.arc(point.x, point.y, 14, 0, 2 * Math.PI);
        ctx.globalAlpha = 0.4 * ringFade;
        ctx.lineWidth = 1.5;
        ctx.stroke();
        ctx.globalAlpha = observationAge(bot) > 3000 ? 0.4 : 1;
      }
      if (active || bots.length <= MAP_LABEL_LIMIT) labels.push({ bot, point, radius, active, alpha: ctx.globalAlpha });
      ctx.restore();
    }
    drawMapLabels(ctx, labels, palette);
    ctx.font = '11px "Noto Sans", "Segoe UI", system-ui, sans-serif';
    drawScaleBar(ctx, height);
    if (shownCount !== bots.length) {
      shownCount = bots.length;
      $("map-count").textContent = `${bots.length} bot${bots.length === 1 ? "" : "s"} in view`;
    }
  }
  requestAnimationFrame(drawMap);
}

// Names are drawn after every marker, so a label never hides a bot. A name that would collide with one already
// placed tries the other side of its marker, then above and below, and is dropped when every position is taken;
// the selected and hovered bots are placed first so the one being read always keeps its name.
function drawMapLabels(ctx, labels, palette) {
  labelFont(ctx);
  const placed = [];
  const collides = (box) =>
    placed.some(
      (other) =>
        box.x < other.x + other.w && other.x < box.x + box.w && box.y < other.y + other.h && other.y < box.y + box.h,
    );
  for (const label of labels.sort((a, b) => Number(b.active) - Number(a.active))) {
    const w = ctx.measureText(label.bot.name).width + 12;
    const gap = label.radius + 4;
    const boxes = [
      { x: label.point.x + gap, y: label.point.y - 10 },
      { x: label.point.x - gap - w, y: label.point.y - 10 },
      { x: label.point.x - w / 2, y: label.point.y - gap - MAP_LABEL_HEIGHT },
      { x: label.point.x - w / 2, y: label.point.y + gap },
    ].map((corner) => ({ ...corner, w, h: MAP_LABEL_HEIGHT }));
    // A hovered or selected bot keeps its name even where nothing is free; every other one gives way.
    const box = boxes.find((candidate) => !collides(candidate)) || (label.active ? boxes[0] : null);
    if (!box) continue;
    placed.push(box);
    ctx.save();
    ctx.globalAlpha = label.alpha;
    ctx.fillStyle = "rgba(15, 23, 27, 0.85)";
    ctx.fillRect(box.x, box.y, box.w, box.h);
    ctx.fillStyle = label.active ? palette.brass : palette.parchment;
    ctx.fillText(label.bot.name, box.x + 6, box.y + 14);
    ctx.restore();
  }
}

// ------------------------------------------------------------------------------------------------ roster

function selectBot(id, recenter = true) {
  selected = id;
  selectedAt = performance.now();
  const bot = state?.bots.find((bot) => bot.id === selected);
  if (bot && recenter) {
    $("map").value = String(bot.map);
    updateZones();
    updateInstances();
    $("zone").value = "all";
    $("instance").value = String(bot.instance);
    view = { x: bot.x, y: bot.y, scale: Math.max(view.scale, 0.1) };
    needsFit = fitArtwork = false;
  }
  renderRoster();
  renderDetails();
  renderBoard();
}

function rosterOrder() {
  const sort = $("roster-sort").value;
  return (a, b) => {
    if (sort === "name") return a.name.localeCompare(b.name);
    if (sort === "health") return healthPercent(a) - healthPercent(b);
    if (sort === "activity") return ACTIVITIES.indexOf(a.activity) - ACTIVITIES.indexOf(b.activity);
    return (b[sort] ?? 0) - (a[sort] ?? 0) || a.name.localeCompare(b.name);
  };
}

function rosterBots() {
  const search = $("bot-search").value.toLowerCase();
  return (state?.bots || [])
    .filter((bot) => `${bot.name} ${bot.id}`.toLowerCase().includes(search))
    .sort(rosterOrder());
}

function renderRoster() {
  const all = state?.bots || [];
  const bots = rosterBots();
  $("roster-count").textContent = !all.length
    ? "No bots yet"
    : $("bot-search").value
      ? `${bots.length} of ${all.length} bots match`
      : `${all.length} of ${all.length} bots`;
  $("roster-column").classList.toggle("compact", Boolean(selected));
  const list = $("bot-list");
  list.replaceChildren(
    ...bots.map((bot) => {
      const row = document.createElement("div");
      row.className = "roster-row";
      row.setAttribute("role", "option");
      row.setAttribute("aria-selected", String(bot.id === selected));
      row.dataset.id = bot.id;
      row.id = `bot-${bot.id.replace(/[^\w-]/g, "_")}`;
      if (observationAge(bot) > 3000) row.classList.add("stale");
      const swatch = document.createElement("i");
      swatch.style.setProperty("--swatch", markerColor(bot, all));
      const cells = [bot.controlGroup ? `${bot.name} (control)` : bot.name, bot.level, bot.activity, `${Math.round(healthPercent(bot))}%`].map((text) => {
        const span = document.createElement("span");
        span.textContent = String(text);
        return span;
      });
      row.append(swatch, ...cells);
      row.onclick = () => selectBot(bot.id);
      return row;
    }),
  );
  const current = list.querySelector('[aria-selected="true"]');
  if (current) {
    list.setAttribute("aria-activedescendant", current.id);
    if (list.scrollHeight > list.clientHeight) {
      const top = current.offsetTop - list.offsetTop;
      if (top < list.scrollTop || top > list.scrollTop + list.clientHeight - 24) list.scrollTop = top - 24;
    }
  } else list.removeAttribute("aria-activedescendant");
}
$("bot-search").oninput = $("roster-sort").onchange = renderRoster;
$("bot-list").onkeydown = (event) => {
  if (!["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
  event.preventDefault();
  const bots = rosterBots();
  if (!bots.length) return;
  const index = bots.findIndex((bot) => bot.id === selected);
  const next =
    event.key === "Home"
      ? 0
      : event.key === "End"
        ? bots.length - 1
        : Math.max(0, Math.min(bots.length - 1, index + (event.key === "ArrowDown" ? 1 : -1)));
  selectBot(bots[next].id);
};

function row(label, value) {
  const div = document.createElement("div");
  div.className = "detail-row";
  const a = document.createElement("span"),
    b = document.createElement("span");
  a.textContent = label;
  b.textContent = value;
  div.append(a, b);
  return div;
}

function bar(label, fraction, text, color) {
  const div = document.createElement("div");
  div.className = "bar";
  const name = document.createElement("span");
  name.textContent = label;
  const track = document.createElement("i");
  const fill = document.createElement("b");
  fill.style.width = `${Math.max(0, Math.min(100, fraction * 100))}%`;
  fill.style.setProperty("--fill", color);
  track.append(fill);
  const value = document.createElement("span");
  value.textContent = text;
  div.append(name, track, value);
  return div;
}

function shortGuid(id) {
  const match = /Type: (\w+).*?(?:Entry: (\d+))?\s*Low: (\d+)/.exec(id || "");
  return match ? `${match[1]}${match[2] ? ` ${match[2]}` : ""} #${match[3]}` : id;
}

// Why this bot is doing what it is doing: the objective it holds, how far through it is, and the planner's own
// sentence about the decision. A world build without planning simply leaves the block hidden.
function renderBotObjective(bot) {
  const objective = currentObjective(bot);
  const block = $("bot-objective");
  block.hidden = !objective;
  if (!objective) return;
  const head = document.createElement("b");
  head.textContent = objective.outcome || `Objective ${objective.id}`;
  const line = document.createElement("span");
  line.className = "objective-state";
  line.dataset.blocked = String(Boolean(objective.obstruction && objective.obstruction !== "none"));
  const stuck = objective.activeWithoutProgressMs || 0;
  line.textContent =
    `${objective.state} · ${objective.step} · ${(objective.approach || "").replaceAll("_", " ")}` +
    (objective.obstruction && objective.obstruction !== "none" ? ` · blocked on ${objective.obstruction}` : "") +
    (objective.attempts > 1 ? ` · try ${objective.attempts}` : "") +
    (stuck ? ` · no progress for ${duration(stuck)}` : "");
  const reason = document.createElement("span");
  reason.className = "objective-reason";
  reason.textContent = objective.reason || "";
  const held = (bot.planning?.objectives || []).length;
  const rest = document.createElement("span");
  rest.className = "objective-reason";
  rest.textContent = `${held} objective${held === 1 ? "" : "s"} held; engine ${bot.planning?.engine || "unknown"}.`;
  block.replaceChildren(head, line, reason, rest);
}

function renderDetails() {
  const bot = state?.bots.find((bot) => bot.id === selected);
  for (const id of ["bot-bars", "bot-spark", "bot-details", "events-title", "events"]) $(id).hidden = !bot;
  $("bot-objective").hidden = true;
  $("bot-empty").hidden = Boolean(bot);
  if (!bot) {
    $("bot-name").textContent = selected ? "Bot is no longer in this feed" : "No bot selected";
    $("bot-empty").textContent = selected
      ? "Its recorded events stay in the exports."
      : "Pick one from the list or click a marker on the map.";
    return;
  }
  const python = isPython();
  const alles = isAlles();
  const unmeasured = state?.source === "alles-simulation"
    ? "not reported by this simulation feed"
    : "not measured by this world build";
  $("bot-name").textContent = `${bot.name}, level ${bot.level}${bot.controlGroup ? " · Troll control group" : ""}`;
  renderBotObjective(bot);
  const health = healthPercent(bot);
  $("bot-bars").replaceChildren(
    bar("Health", health / 100, `${formatNumber(bot.health)} / ${formatNumber(bot.maxHealth)}`, healthColor(health)),
    python
      ? bar("Power", (bot.power ?? 0) / Math.max(1, bot.maxPower ?? 1), `${bot.power} / ${bot.maxPower}`, palette.sky)
      : bar(
          "Level XP",
          (bot.xp ?? 0) / Math.max(1, bot.nextLevelXp ?? 1),
          `${formatNumber(bot.xp)} / ${formatNumber(bot.nextLevelXp)}`,
          palette.brass,
        ),
  );
  const trail = trails.get(bot.id) || [];
  requestAnimationFrame(() => {
    const { ctx, width, height } = surface($("bot-spark"));
    sparkline(
      ctx,
      width,
      height - 14,
      trail.map((sample) => sample.health),
      healthColor(health),
      { max: 100 },
    );
    if (!python)
      sparkline(
        ctx,
        width,
        height - 14,
        trail.map((sample) => sample.xp),
        palette.brass,
        { baseline: false },
      );
    ctx.fillStyle = palette.ash;
    ctx.fillText(`${python ? "health" : "health and earned XP"}, last ${trail.length} samples`, 0, height - 2);
  });
  const silentMs = bot.lastAiMs == null ? null : state.simMs - bot.lastAiMs;
  $("bot-details").replaceChildren(
    ...[
      ["Identity", shortGuid(bot.id)],
      [
        "Map, instance, zone",
        `${continentNames[bot.map] || `Map ${bot.map}`}, ${bot.instance}, ${placeName(bot.map, bot.zone)}`,
      ],
      ["Position (x, y, z)", [bot.x, bot.y, bot.z].map((value) => value.toFixed(1)).join(", ")],
      ["Activity", bot.activity],
      ...(python
        ? [
            ["Power type", bot.powerType],
            ["World tick", bot.worldTick],
            ["Registration, episode", `${bot.registration}, ${bot.episode}`],
            ["API elapsed world time", duration(bot.apiElapsedMs)],
            [
              "Observation age",
              `${(observationAge(bot) / 1000).toFixed(1)} s${observationAge(bot) > 3000 ? ", stale" : ""}`,
            ],
            ["Registration kills, deaths, levels", `${bot.kills}, ${bot.deaths}, ${bot.levelGains}`],
          ]
        : [
            ["Earned XP this run", bot.earnedXp == null && alles ? unmeasured : formatNumber(bot.earnedXp)],
            ["Quests completed", bot.questCompletions == null && alles ? unmeasured : formatNumber(bot.questCompletions)],
            ["Deaths", bot.deaths == null && alles ? unmeasured : formatNumber(bot.deaths)],
            ["Money", formatMoney(bot.money)],
            ["Equipped items", `${gearCount(bot)} of ${(bot.gear || []).length} slots`],
            ["AI updates", bot.aiUpdates == null && alles ? unmeasured : formatNumber(bot.aiUpdates)],
            [
              "Last AI update",
              silentMs == null
                ? alles
                  ? unmeasured
                  : "unknown"
                : silentMs > 10000
                  ? `${(silentMs / 1000).toFixed(1)} s ago, silent for over 10 s`
                  : `${(silentMs / 1000).toFixed(1)} s ago`,
            ],
            ...(alles
              ? [
                  [
                    "AI actions",
                    bot.actions == null
                      ? unmeasured
                      : `${formatNumber(bot.actions)}` +
                        (bot.lastAction ? `, last “${bot.lastAction}”` : "") +
                        (bot.lastActionMs == null ? "" : ` ${((state.simMs - bot.lastActionMs) / 1000).toFixed(0)} s ago`),
                  ],
                  [
                    "Memories",
                    `${formatNumber(bot.memoryCount)} held, ${formatNumber(bot.pendingPerceptions)} perceptions pending`,
                  ],
                  [
                    "Memory store",
                    bot.memoryState == null
                      ? "unknown"
                      : `${bot.memoryState}, revision ${formatNumber(bot.memoryRevision)}, ` +
                        `committed ${formatNumber(bot.committedRevision)}` +
                        (bot.saving ? ", saving" : "") +
                        (bot.saveFailed ? ", last save failed" : "") +
                        (bot.droppedPerceptions ? `, ${formatNumber(bot.droppedPerceptions)} perceptions dropped` : ""),
                  ],
                  ["Bag slots", bot.bags?.map((bag) => bag.capacity).join(" / ") || "none"],
                ]
              : []),
          ]),
      ...(bot.quests || []).map((quest) => [
        `Quest ${quest.id}`,
        (quest.state & 1 ? "complete" : quest.state & 2 ? "failed" : "in progress") +
          `, objectives ${quest.objectives.join("/")}` +
          ((quest.items || []).some(Boolean) ? `, items ${quest.items.join("/")}` : ""),
      ]),
    ].map(([label, value]) => row(label, value)),
  );
  $("events").replaceChildren(
    ...events
      .filter((event) => event.run === state.run && event.bot === selected && !TRACE_KINDS.has(event.kind))
      .slice(-40)
      .reverse()
      .map((event) => {
        const li = document.createElement("li");
        const time = document.createElement("time");
        time.textContent = formatClock(event.simMs);
        const text = document.createElement("span");
        text.textContent = describeEvent(event);
        li.append(time, text);
        return li;
      }),
  );
  if (!$("events").children.length) {
    const li = document.createElement("li");
    li.textContent =
      alles && !state.journal
        ? "No events: this world build publishes no journal."
        : "No progression events for this bot yet.";
    $("events").replaceChildren(li);
  }
}


// ------------------------------------------------------------------------------------------ world state

// What the cohort is actually doing, what looks wrong, and how far the run has got. Everything here is read
// from the authoritative snapshot: no figure is estimated and nothing is measured by the browser.

function botChip(entry) {
  const chip = document.createElement("button");
  chip.type = "button";
  chip.className = "chip";
  chip.textContent = entry.name;
  chip.onclick = () => selectBot(entry.id);
  return chip;
}

function stationaryBots() {
  // A trail is the retained position history; a bot whose whole trail sits within a yard has not moved.
  const still = [];
  for (const bot of state.bots) {
    const trail = trails.get(bot.id);
    if (!trail || trail.length < 8) continue;
    const spread = (key) => {
      const values = trail.map((point) => point[key]);
      return Math.max(...values) - Math.min(...values);
    };
    if (spread("x") < 1 && spread("y") < 1) still.push({ id: bot.id, name: bot.name });
  }
  return still;
}

function renderWorldState() {
  const rows = objectiveTable(state.bots);
  const body = $("objective-table").tBodies[0];
  body.replaceChildren(
    ...rows.map(({ bot, objective }) => {
      const tr = document.createElement("tr");
      const name = document.createElement("td");
      name.className = "text";
      name.append(botChip({ id: bot.id, name: bot.name }));
      const obstruction = cell(objective.obstruction === "none" ? "—" : objective.obstruction, "text");
      if (objective.obstruction && objective.obstruction !== "none") obstruction.classList.add("blocked");
      const stuck = objective.activeWithoutProgressMs || 0;
      tr.append(
        name,
        cell(objective.outcome || `objective ${objective.id}`, "text"),
        cell((objective.approach || "").replaceAll("_", " "), "text"),
        cell(`${objective.step || "?"} · ${objective.state || "?"}`, "text"),
        obstruction,
        cell(formatNumber(objective.attempts ?? 0)),
        cell(stuck ? duration(stuck) : "—"),
      );
      // The reason is a sentence the planner wrote about this decision; it is too long for a column of its own.
      tr.title = objective.reason || "";
      if (bot.id === selected) tr.className = "selected";
      tr.onclick = () => selectBot(bot.id);
      return tr;
    }),
  );
  const tally = objectiveTally(state.bots);
  $("objective-tally").textContent = tally.total
    ? `${tally.total} objectives: ${tally.states.map(([name, count]) => `${count} ${name}`).join(", ")}`
    : "";
  const approaches = tally.approaches.map(([name, count]) => `${name.replaceAll("_", " ")} ${count}`).join(", ");
  $("objective-status").textContent = rows.length
    ? `${rows.length} of ${state.bots.length} bots are planning. ` +
      `Approaches so far: ${approaches}. ` +
      "Hover a row for the planner's own reason."
    : "This world build publishes no bot planning, so there is no objective to show.";

  const notes = attention(state, { stationary: stationaryBots() });
  $("attention").replaceChildren(
    ...notes.map((note) => {
      const li = document.createElement("li");
      li.dataset.level = note.level;
      const text = document.createElement("span");
      text.textContent = note.bots.length
        ? `${note.bots.length} bot${note.bots.length === 1 ? "" : "s"} ${note.text}`
        : note.text;
      li.append(text);
      if (note.bots.length) {
        const who = document.createElement("span");
        who.className = "chips";
        who.append(...note.bots.slice(0, 8).map(botChip));
        if (note.bots.length > 8) who.append(` +${note.bots.length - 8}`);
        li.append(who);
      }
      return li;
    }),
  );
  if (!notes.length) {
    const li = document.createElement("li");
    li.dataset.level = "ok";
    li.textContent = "Nothing needs attention: every bot is online, moving, saving and within budget.";
    $("attention").replaceChildren(li);
  }

  const statistics = runStatistics(state);
  $("run-stats").tBodies[0].replaceChildren(
    ...statistics.map((row) => {
      const tr = document.createElement("tr");
      const label = cell(row.label, "text");
      if (row.hint) label.title = row.hint;
      tr.append(
        label,
        cell(row.format === "money" ? formatMoney(row.value) : formatNumber(row.value)),
        cell(row.perHour == null ? "—" : compactNumber(row.perHour)),
      );
      return tr;
    }),
  );
  const elapsed = isPython() ? duration(state.simMs) : formatClock(state.simMs);
  $("run-stats-note").textContent =
    `Totals for the whole run, ${elapsed} of ${isAlles() ? "realm" : "simulated"} time. ` +
    "A standing figure has no rate.";
  $("world-note").textContent = `${state.bots.length} bots, ${notes.length} note${notes.length === 1 ? "" : "s"}`;

  const talk = worldTalk(state.bots);
  $("world-talk").hidden = !talk.leads.length && !talk.asks.length;
  $("lead-count").textContent = talk.leads.length ? `${talk.leads.length} distinct` : "";
  $("lead-table").tBodies[0].replaceChildren(
    ...talk.leads.slice(0, 20).map((lead) => {
      const tr = document.createElement("tr");
      tr.append(
        cell(lead.text, "text"),
        cell(lead.source, "text"),
        cell(lead.holders.join(", "), "text"),
        cell(lead.confidence == null ? "—" : lead.confidence.toFixed(2)),
        cell(`${lead.useful} / ${lead.useful + lead.unsuccessful}`),
      );
      tr.title = `${lead.useful} useful of ${lead.useful + lead.unsuccessful} visits acted on this lead`;
      return tr;
    }),
  );
  $("ask-list").replaceChildren(
    ...talk.asks.map((ask) => {
      const li = document.createElement("li");
      const who = document.createElement("b");
      who.textContent = ask.name;
      const meta = document.createElement("span");
      meta.className = "meta";
      meta.textContent = `${ask.status.replaceAll("_", " ")}, ${ask.attempts} attempt${ask.attempts === 1 ? "" : "s"}`;
      li.append(who, ` ${ask.question}`, meta);
      return li;
    }),
  );
  if (!talk.asks.length) {
    const li = document.createElement("li");
    li.textContent = "No bot is waiting on an answer.";
    $("ask-list").replaceChildren(li);
  }
}

// ------------------------------------------------------------------------------------------------ cohort

function renderAnalytics() {
  const bots = state.bots;
  const mix = summarize(state).activity;
  const python = isPython();
  const timeFormat = python ? duration : formatClock;
  $("cohort-title").textContent = python
    ? "Cohort over elapsed time"
    : isAlles()
      ? "Cohort over realm time"
      : "Cohort over simulated time";
  requestAnimationFrame(() => {
    const points = bucketHistory(history, 120);
    const activity = surface($("activity-chart"));
    // Stacked bottom to top so the quiet idle band sits on top in ash.
    const order = [
      ["dead", ACTIVITY_COLORS.dead, 0.9],
      ["combat", ACTIVITY_COLORS.combat, 0.7],
      ["casting", ACTIVITY_COLORS.casting, 0.7],
      ["moving", ACTIVITY_COLORS.moving, 0.7],
      ["idle", palette.ash, 0.35],
    ];
    stackedArea(
      activity.ctx,
      activity.width,
      activity.height,
      order.map(([name, color, alpha]) => ({
        color,
        alpha,
        points: points.map((point) => ({ x: point.simMs, y: point.activity?.[name] ?? 0 })),
      })),
      { formatX: timeFormat },
    );
    const levels = surface($("level-chart"));
    if (points.length)
      lineChart(
        levels.ctx,
        levels.width,
        levels.height,
        [
          {
            name: "Mean",
            color: palette.brass,
            width: 2,
            points: points.map((point) => ({ x: point.simMs, y: point.meanLevel })),
          },
        ],
        {
          band: {
            low: points.map((point) => ({ x: point.simMs, y: point.minLevel })),
            high: points.map((point) => ({ x: point.simMs, y: point.maxLevel })),
          },
          formatX: timeFormat,
          formatY: (value) => formatNumber(value, 0),
          yMin: Math.max(0, Math.floor(Math.min(...points.map((point) => point.minLevel ?? 1))) - 1),
          yMax: Math.ceil(Math.max(...points.map((point) => point.maxLevel ?? 1))) + 1,
          xTicks: 2,
        },
      );
  });
  $("activity-list").replaceChildren(
    ...ACTIVITIES.map((activity) => {
      const li = document.createElement("li");
      li.style.setProperty("--swatch", ACTIVITY_COLORS[activity]);
      li.textContent = `${activity} ${mix[activity]}`;
      return li;
    }),
  );
  const health = healthStats(bots);
  const quests = questStats(bots);
  summaryLines($("health-summary"), [
    ["Mean health", health.mean == null ? "unknown" : `${health.mean.toFixed(0)}%`],
    ["Lowest", health.min == null ? "unknown" : `${health.min.toFixed(0)}%`],
    ["Under 35%", health.low],
    [
      "Silent AI",
      python || (isAlles() && !bots.some((bot) => bot.lastAiMs != null)) ? "not reported" : stalledBots(state).length,
    ],
  ]);
  const levelsNow = bots.map((bot) => bot.level);
  const first = history[0],
    last = history.at(-1);
  summaryLines($("level-summary"), [
    [
      "Now: lowest, mean, highest",
      levelsNow.length
        ? `${Math.min(...levelsNow)}, ${formatNumber(last?.meanLevel, 1)}, ${Math.max(...levelsNow)}`
        : "none",
    ],
    [
      "Mean level change",
      first && last && first.meanLevel != null && last.meanLevel != null && last.simMs > first.simMs
        ? `${last.meanLevel >= first.meanLevel ? "+" : ""}${formatNumber(last.meanLevel - first.meanLevel, 2)}` +
          ` over ${duration(last.simMs - first.simMs)}`
        : "collecting",
    ],
    [
      "Quests in logs",
      python
        ? "not reported"
        : `${quests.active}, ${quests.complete} ready to turn in, ${quests.progressing} progressing`,
    ],
  ]);
  const total = Math.max(1, bots.length);
  $("zone-table").tBodies[0].replaceChildren(
    ...zoneTable(bots)
      .slice(0, 8)
      .map((entry) => {
        const tr = document.createElement("tr");
        const name = document.createElement("td");
        name.className = "share";
        name.style.setProperty("--share", `${(100 * entry.count) / total}%`);
        name.textContent = placeName(entry.map, entry.zone);
        const count = document.createElement("td");
        count.textContent = entry.count;
        const combat = document.createElement("td");
        combat.textContent = entry.combat;
        tr.append(name, count, combat);
        return tr;
      }),
  );
  renderBoard();
}

function summaryLines(element, rows) {
  element.replaceChildren(
    ...rows.map(([label, value]) => {
      const div = document.createElement("div");
      const b = document.createElement("b");
      b.textContent = String(value);
      div.append(`${label} `, b);
      return div;
    }),
  );
}

function renderBoard() {
  if (!state) return;
  for (const option of $("board-metric").options) {
    option.hidden = option.disabled = option.hasAttribute("data-python")
      ? !isPython()
      : option.hasAttribute("data-alles")
        ? !isAlles()
        : ["money", "stalled"].includes(option.value) && isPython();
  }
  if ($("board-metric").selectedOptions[0]?.disabled)
    $("board-metric").value = isPython() ? "level" : isAlles() ? "memoryCount" : "earnedXp";
  // Until a journaling world build reports earned XP, the live realm's board starts with what it measures.
  if (isAlles() && $("board-metric").value === "earnedXp" && !state.bots.some((bot) => bot.earnedXp != null))
    $("board-metric").value = "memoryCount";
  const metric = $("board-metric").value;
  const entries =
    metric === "stalled"
      ? stalledBots(state)
          .slice(0, 6)
          .map(({ bot, silentMs }) => [bot, `${(silentMs / 1000).toFixed(0)} s silent`])
      : leaderboard(state.bots, $("board-metric").value, 6).map((bot) => [
          bot,
          metric === "money" ? formatMoney(bot.money) : formatNumber(bot[metric]),
        ]);
  $("board").replaceChildren(
    ...entries.map(([bot, text]) => {
      const li = document.createElement("li");
      li.tabIndex = 0;
      li.setAttribute("role", "button");
      if (bot.id === selected) li.className = "selected";
      const name = document.createElement("span");
      name.textContent = bot.name;
      const level = document.createElement("span");
      level.textContent = `L${bot.level}`;
      const value = document.createElement("span");
      value.textContent = text;
      li.append(name, level, value);
      li.onclick = () => selectBot(bot.id);
      li.onkeydown = (event) => {
        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault();
          selectBot(bot.id);
        }
      };
      return li;
    }),
  );
  if (!entries.length) {
    const li = document.createElement("li");
    li.className = "empty";
    li.textContent = metric === "stalled" ? "Every bot's AI updated in the last 10 simulated seconds." : "No bots yet";
    $("board").replaceChildren(li);
  }
}
$("board-metric").onchange = renderBoard;

// ---------------------------------------------------------------------------------------------- timeline

const CHART_SERIES = {
  speed: [
    { key: "achievedSpeed", name: "Achieved ×", width: 2 },
    { key: "requestedSpeed", name: "Requested ×", dashed: true },
  ],
  population: [
    { key: "active", name: "Active", width: 2 },
    { key: "online", name: "Online" },
    { key: "inWorld", name: "In world" },
    { key: "expected", name: "Target", dashed: true },
  ],
  activity: ACTIVITIES.map((activity) => ({
    key: ["activity", activity],
    name: activity,
    color: activity === "idle" ? palette.ash : ACTIVITY_COLORS[activity],
  })),
  records: [
    { key: ["records", "progression"], name: "Progression / min", width: 2 },
    { key: ["records", "trace"], name: "Trace / min", dashed: true },
  ],
  formation: [
    { key: "modelMemories", name: "By model", width: 2 },
    { key: "fallbackMemories", name: "By fallback", dashed: true },
    { key: "invalidResults", name: "Invalid results", dashed: true },
  ],
  conversation: [
    { key: "conversationReplies", name: "Replies", width: 2 },
    { key: "conversationActions", name: "Actions" },
    { key: "conversationPending", name: "Queued and pending", dashed: true },
  ],
};
const CHART_MAX = { meanHealth: 100, health: 100 };

function pointValue(point, key) {
  return Array.isArray(key) ? (point[key[0]]?.[key[1]] ?? null) : point[key];
}

function chartSeries(metric, points) {
  if (metric === "levels" || metric === "zones") {
    const keys = [...new Set(points.flatMap((point) => Object.keys(point[metric])))].sort((a, b) =>
      metric === "levels" ? Number(a.slice(6)) - Number(b.slice(6)) : a.localeCompare(b),
    );
    return keys.map((key, index) => ({
      name: metric === "zones" ? zoneName(...key.split("/")) : key,
      color: levelColor(index, 0, Math.max(1, keys.length - 1)),
      points: points.map((point) => ({ x: point.simMs, y: point[metric][key] || 0 })),
    }));
  }
  const lines = CHART_SERIES[metric] || [
    { key: metric, name: $("chart-metric").selectedOptions[0].textContent, width: 2 },
  ];
  return lines.map((line, index) => ({
    ...line,
    color: line.color || SERIES_COLORS[index % SERIES_COLORS.length],
    points: points.map((point) => ({ x: point.simMs, y: pointValue(point, line.key) })),
  }));
}

function drawChart() {
  const python = isPython();
  const alles = isAlles();
  $("chart-title").textContent = python ? "Timeline over elapsed real time" : alles ? "Timeline over realm time" : "Timeline";
  for (const option of $("chart-metric").options) {
    option.disabled = option.hidden = python
      ? !["levels", "deaths"].includes(option.value) && !option.hasAttribute("data-python")
      : option.hasAttribute("data-python") ||
        (alles ? option.hasAttribute("data-simulation") : option.hasAttribute("data-alles"));
  }
  if ($("chart-metric").selectedOptions[0]?.disabled)
    $("chart-metric").value = python ? "observed" : alles ? "memories" : "xp";
  // The live realm's first chart is something it measures; a deliberate choice is left alone.
  if (alles && !chartMetricChosen && $("chart-metric").value === "xp" && history.at(-1)?.xp == null)
    $("chart-metric").value = "memories";
  const { ctx, width, height } = surface($("chart-canvas"));
  const metric = $("chart-metric").value;
  // Whole-run views draw the bridge's long-term buckets; every other window uses this session's samples.
  const wholeRun = !python && ($("chart-window").value === "run" || metric === "records");
  const window = Number($("chart-window").value) || 0;
  const runPoints = wholeRun ? longTerm.points : [];
  const points = runPoints.length
    ? runPoints
    : window && history.length
      ? history.filter((point) => point.simMs >= history.at(-1).simMs - window)
      : history;
  $("history-status").textContent = runPoints.length
    ? `${runPoints.length.toLocaleString()} long-term points over ${duration(
        runPoints.at(-1).simMs - runPoints[0].bucket,
      )}, ${Math.round(longTerm.bucketMs / 60000)}-minute buckets or wider`
    : points.length
      ? `${points.length.toLocaleString()} samples in view${wholeRun ? " (no long-term history yet)" : ""}`
      : "No history yet";
  if (points.length < 1) {
    $("legend").replaceChildren();
    $("chart-readout").replaceChildren();
    return;
  }
  const lines = chartSeries(metric, points);
  const layout = lineChart(ctx, width, height, lines, {
    formatX: python ? duration : formatClock,
    formatY: compactNumber,
    hover: chartHover,
    yMax: CHART_MAX[metric],
  });
  const readings = layout.readings || {
    x: points.at(-1).simMs,
    values: lines
      .map((line) => ({ name: line.name, color: line.color, y: line.points.at(-1).y }))
      .filter((entry) => entry.y != null),
  };
  const time = document.createElement("b");
  time.textContent = `${layout.readings ? "" : "Latest "}${python ? duration(readings.x) : formatClock(readings.x)}`;
  const hint = document.createElement("span");
  hint.textContent = layout.readings ? "at the pointer" : "Hover to read a point";
  $("chart-readout").replaceChildren(time, hint);
  $("legend").replaceChildren(
    ...lines.map((line) => {
      const item = document.createElement("div");
      item.className = "series";
      item.style.setProperty("--swatch", line.color);
      const swatch = document.createElement("i");
      const name = document.createElement("span");
      name.textContent = line.name;
      const value = document.createElement("span");
      const reading = readings.values.find((entry) => entry.name === line.name);
      value.textContent = reading ? formatNumber(reading.y, Number.isInteger(reading.y) ? 0 : 2) : "";
      item.append(swatch, name, value);
      return item;
    }),
  );
}
$("chart-window").onchange = drawChart;
$("chart-metric").onchange = () => {
  chartMetricChosen = true;
  drawChart();
};
$("chart-canvas").onpointermove = (event) => {
  chartHover = event.clientX - $("chart-canvas").getBoundingClientRect().left;
  drawChart();
};
$("chart-canvas").onpointerleave = () => {
  chartHover = null;
  drawChart();
};

// -------------------------------------------------------------------------------------------- event feed

function renderFeed() {
  if (!state) return;
  const kinds = [...new Set(events.filter((event) => !TRACE_KINDS.has(event.kind)).map((event) => event.kind))].sort();
  const select = $("feed-kind");
  const current = select.value;
  if (
    [...select.options]
      .slice(1)
      .map((option) => option.value)
      .join() !== kinds.join()
  ) {
    select.replaceChildren(
      new Option("All progression events", ""),
      ...kinds.map((kind) => new Option(kind.replaceAll("_", " "), kind)),
    );
    select.value = kinds.includes(current) ? current : "";
  }
  const following = $("feed-follow").getAttribute("aria-pressed") === "true";
  if (!following) {
    if (feedFrozen == null) feedFrozen = events;
  } else feedFrozen = null;
  const source = feedFrozen || events;
  const names = new Map(state.bots.map((bot) => [bot.id, bot.name]));
  const listed = source
    .filter(
      (event) =>
        event.run === state.run && !TRACE_KINDS.has(event.kind) && (!select.value || event.kind === select.value),
    )
    .slice(-120)
    .reverse();
  const timeFormat = isPython() ? duration : formatClock;
  $("feed").replaceChildren(
    ...listed.map((event) => {
      const li = document.createElement("li");
      const time = document.createElement("time");
      time.textContent = timeFormat(event.simMs);
      let who;
      if (event.bot && names.has(event.bot)) {
        who = document.createElement("button");
        who.className = "link";
        who.type = "button";
        who.onclick = () => selectBot(event.bot);
      } else who = document.createElement("b");
      who.textContent = event.bot ? names.get(event.bot) || shortGuid(event.bot) : "run";
      const text = document.createElement("span");
      text.textContent = describeEvent(event);
      text.title = text.textContent;
      li.append(time, who, text);
      return li;
    }),
  );
  const traced = source.filter((event) => TRACE_KINDS.has(event.kind)).length;
  $("feed-status").textContent = feedFrozen
    ? `Paused at ${listed.length} events. Press Following to resume.`
    : `${listed.length} progression events in the last ${formatNumber(source.length)} journal records.` +
      (traced ? ` ${formatNumber(traced)} trace records hidden.` : "");
}
$("feed-kind").onchange = renderFeed;
$("feed-follow").onclick = () => {
  const button = $("feed-follow");
  const next = button.getAttribute("aria-pressed") !== "true";
  button.setAttribute("aria-pressed", String(next));
  button.textContent = next ? "Following" : "Paused";
  renderFeed();
};

function renderEventMix() {
  const body = $("event-mix").tBodies[0];
  if (!eventStats) {
    body.replaceChildren();
    return;
  }
  const rows = Object.entries(eventStats.kinds).sort((a, b) => b[1] - a[1]);
  const max = Math.max(1, ...rows.map(([, count]) => count));
  const groups = [
    ["", rows.filter(([kind]) => !TRACE_KINDS.has(kind))],
    ["Trace records", rows.filter(([kind]) => TRACE_KINDS.has(kind))],
  ];
  body.replaceChildren(
    ...groups.flatMap(([title, group]) => {
      if (!group.length) return [];
      const heading = [];
      if (title) {
        const tr = document.createElement("tr");
        tr.className = "group";
        const td = document.createElement("td");
        td.colSpan = 3;
        td.textContent = title;
        tr.append(td);
        heading.push(tr);
      }
      return heading.concat(
        group.map(([kind, count]) => {
          const tr = document.createElement("tr");
          const name = document.createElement("td");
          name.className = "share";
          name.style.setProperty("--share", `${(100 * count) / max}%`);
          name.textContent = kind.replaceAll("_", " ");
          const total = document.createElement("td");
          total.textContent = formatNumber(count);
          const recent = document.createElement("td");
          recent.textContent = formatNumber(eventStats.recent[kind] || 0);
          tr.append(name, total, recent);
          return tr;
        }),
      );
    }),
  );
  if (!rows.length) {
    const tr = document.createElement("tr");
    const td = document.createElement("td");
    td.colSpan = 3;
    td.textContent = "No journal records yet.";
    tr.append(td);
    body.replaceChildren(tr);
  }
}

// ------------------------------------------------------------------------------------------- interpreter

function allesLamps() {
  const worker = state.interpreter || {};
  const journal = state.journal?.events;
  const budget = budgetState(worker);
  const exhausted = budget.exhausted;
  return [
    [
      state.telemetryStale ? "danger" : "ok",
      state.telemetryStale ? "World telemetry stale" : "World publishing",
      !state.telemetryStale,
    ],
    [
      worker.connected ? "ok" : "warn",
      worker.connected ? "Worker connected" : "Worker disconnected",
      Boolean(worker.connected),
    ],
    [
      worker.ledgerFault ? "danger" : exhausted ? "warn" : "ok",
      worker.ledgerFault
        ? "Ledger fault"
        : exhausted
          ? "Request budget used"
          : budget.capped
            ? "Requests available"
            : "Requests uncapped",
      !worker.ledgerFault && !exhausted,
    ],
    [
      state.alles?.lifecycleFault ? "danger" : "ok",
      state.alles?.lifecycleFault ? "Lifecycle ingress overflow" : "Ingress healthy",
      !state.alles?.lifecycleFault,
    ],
    [
      !journal ? "off" : journal.failed ? "danger" : journal.dropped ? "warn" : "ok",
      !journal
        ? "No journal from this world build"
        : journal.failed
          ? "Journal failed"
          : journal.dropped
            ? `Journal dropped ${formatNumber(journal.dropped)} records`
            : "Journal healthy",
      Boolean(journal) && !journal.failed && !journal.dropped,
    ],
  ];
}

function renderInterpreterFigures() {
  const worker = state.interpreter || {};
  const budget = budgetState(worker);
  const { mode, used, max, remaining, exhausted } = budget;
  const rolling = mode === "rolling";
  // An unlimited world enforces no cap, so its count is a running total rather than a fraction of a budget.
  $("requests-value").textContent = !budget.capped
    ? formatNumber(used)
    : rolling
      ? `${remaining} / ${max}`
      : `${used} / ${max}`;
  $("requests-sub").textContent = worker.ledgerFault
    ? "provider ledger fault"
    : !budget.capped
      ? "charged since boot, no cap"
      : rolling
        ? `left this minute, ${formatNumber(used)} charged since boot`
        : exhausted
          ? "trial budget used; template fallback"
          : "trial requests used";
  requestAnimationFrame(() => {
    const { ctx, width, height } = surface($("requests-bar"));
    speedBar(ctx, width, height, budget.capped ? (rolling ? remaining / max : used / max) : 0, {
      color: worker.ledgerFault || exhausted ? palette.ember : palette.moss,
      requested: "",
      empty: !budget.capped,
    });
  });
  const last = history.at(-1);
  $("memories-value").textContent = formatNumber(last?.memories);
  $("memories-sub").textContent =
    `${formatNumber(worker.modelMemories)} by model, ${formatNumber(worker.fallbackMemories)} by fallback` +
    (worker.invalidResults ? `, ${formatNumber(worker.invalidResults)} invalid` : "");
  figureSpark("memories-spark", "memories", palette.lilac);
}

function words(key) {
  return key.replace(/([a-z])([A-Z])/g, "$1 $2").toLowerCase();
}

function cell(text, className) {
  const td = document.createElement("td");
  td.textContent = text;
  if (className) td.className = className;
  return td;
}

function renderInterpreter() {
  const alles = isAlles();
  $("interpreter-region").hidden = !alles;
  $("memory-region").hidden = !alles;
  if (!alles) return;
  const worker = state.interpreter || {};
  const stats = worker.stats || {};
  const rolling = worker.budgetMode === "rolling";
  $("interpreter-note").textContent = `${worker.mode || "provider"} mode, ${worker.model || "no model"}`;
  const facts = [
    ["Worker", worker.connected ? "connected" : "disconnected"],
    ["Model", worker.model || "unknown"],
    ["Profile", worker.profile ? `${worker.profile.slice(0, 16)}…` : "unknown"],
    [
      "Budget",
      rolling
        ? `${worker.remainingRequests ?? "?"} of ${worker.requestsPerMinute} requests left this minute, ` +
          `${formatNumber(worker.usedRequests)} charged since boot`
        : budgetState(worker).capped
          ? `${formatNumber(worker.usedRequests)} of ${formatNumber(worker.maxRequests)} trial requests used`
          : `unlimited: ${formatNumber(worker.usedRequests)} requests charged since boot, no cap enforced`,
    ],
    ["Ledger", worker.ledgerFault ? "fault: no further model requests" : "healthy"],
    [
      "Memories",
      `${formatNumber(worker.modelMemories)} by model, ${formatNumber(worker.fallbackMemories)} by fallback, ` +
        `${formatNumber(worker.invalidResults)} invalid results`,
    ],
  ];
  if (worker.conversationQueued != null)
    facts.push([
      "Conversations",
      `${formatNumber(worker.conversationQueued)} queued, ${formatNumber(worker.conversationCompleted)} completed, ` +
        `${formatNumber(worker.conversationFailed)} failed`,
    ]);
  // Any other scalar the world publishes is listed as is, so a newer world build needs no dashboard change.
  const known = new Set([
    "mode", "connected", "model", "profile", "usedRequests", "maxRequests", "ledgerFault", "modelMemories",
    "invalidResults", "fallbackMemories", "stats", "budgetMode", "requestsPerMinute", "remainingRequests",
    "conversationQueued", "conversationCompleted", "conversationFailed",
  ]);
  const idleFacts = [];
  for (const [key, value] of Object.entries(worker)) {
    if (known.has(key) || (typeof value === "object" && value !== null)) continue;
    // A build reports twenty-odd queue and token counters, most of them zero on a healthy worker. The ones
    // that moved stay in the list; the rest are folded away so the panel shows work, not a column of noughts.
    (value === 0 ? idleFacts : facts).push([words(key), String(value)]);
  }
  const definitions = (rows) =>
    rows.flatMap(([key, value]) => {
      const dt = document.createElement("dt"),
        dd = document.createElement("dd");
      dt.textContent = key;
      dd.textContent = value;
      return [dt, dd];
    });
  $("worker-status").replaceChildren(...definitions(facts));
  $("worker-idle").hidden = !idleFacts.length;
  if (idleFacts.length) {
    $("worker-idle").querySelector("summary").textContent =
      `${idleFacts.length} further counter${idleFacts.length === 1 ? "" : "s"} at zero`;
    $("worker-idle-facts").replaceChildren(...definitions(idleFacts));
  }
  // Human-to-bot conversations: the world's live counters plus the worker log's per-turn outcomes.
  const talk = state.conversation;
  const turns = workerLog?.available ? workerLog.summary.conversations : null;
  const actionMix = turns
    ? Object.entries(turns.actions)
        .sort((a, b) => b[1] - a[1])
        .map(([action, count]) => `${action} ${count}`)
        .join(", ")
    : "";
  $("conversation-status").replaceChildren(
    ...definitions(
      !talk
        ? [["Conversations", "not reported by this world build"]]
        : [
            ["Enabled", talk.enabled ? "yes" : "no"],
            ["Backlog", `${formatNumber(talk.queuedTurns)} turns queued, ${formatNumber(talk.pendingReplies)} replies pending`],
            ["Replies", `${formatNumber(talk.replies)} delivered, ${formatNumber(talk.omitted)} omitted`],
            ["Actions", `${formatNumber(talk.actions)} performed, ${formatNumber(talk.following)} following`],
            ...(worker.conversationQueued != null
              ? [
                  [
                    "Worker queue",
                    `${formatNumber(worker.conversationQueued)} queued, ${formatNumber(worker.conversationCompleted)} completed, ` +
                      `${formatNumber(worker.conversationFailed)} failed`,
                  ],
                ]
              : []),
            ...(turns && turns.turns
              ? [
                  [
                    "Log tail",
                    `${turns.turns} turns: ${turns.accepted} accepted, ${turns.otherStatus} other, ${turns.failed} failed; ` +
                      `${turns.replies} replied` +
                      (turns.meanLatencyMs == null ? "" : `; ${turns.meanLatencyMs} ms mean, ${turns.maxLatencyMs} ms max`),
                  ],
                  ["Actions in tail", actionMix || "none"],
                  ["Tokens in tail", `${formatNumber(turns.promptTokens)} prompt, ${formatNumber(turns.completionTokens)} completion`],
                ]
              : []),
          ],
    ),
  );
  const counters = Object.entries(stats);
  const moved = counters.filter(([, value]) => Number(value) !== 0);
  const atZero = counters.filter(([, value]) => Number(value) === 0);
  const statsBody = $("coordinator-stats").tBodies[0];
  statsBody.replaceChildren(
    ...(moved.length ? moved : counters).map(([key, value]) => {
      const tr = document.createElement("tr");
      tr.append(cell(words(key)), cell(formatNumber(value)));
      return tr;
    }),
  );
  // Naming the untouched counters keeps them accounted for without spending a table row on each nought.
  $("coordinator-idle").hidden = !moved.length || !atZero.length;
  $("coordinator-idle").textContent = `At zero: ${atZero.map(([key]) => words(key)).join(", ")}.`;
  if (!counters.length) {
    const tr = document.createElement("tr");
    const td = cell("Coordinator counters are not reported by this world build.");
    td.colSpan = 2;
    tr.append(td);
    statsBody.replaceChildren(tr);
  }
  $("owner-table").tBodies[0].replaceChildren(
    ...state.bots.map((bot) => {
      const tr = document.createElement("tr");
      const name = document.createElement("td");
      const link = document.createElement("button");
      link.className = "link";
      link.type = "button";
      link.textContent = bot.name;
      link.onclick = () => selectBot(bot.id);
      name.append(link);
      const saved =
        bot.committedRevision == null
          ? "unknown"
          : `${formatNumber(bot.committedRevision)} of ${formatNumber(bot.memoryRevision)}` +
            (bot.saving ? ", saving" : "") +
            (bot.saveFailed ? ", failed" : "");
      tr.append(
        name,
        cell(bot.memoryState ?? "unknown"),
        cell(formatNumber(bot.memoryCount)),
        cell(formatNumber(bot.pendingPerceptions)),
        cell(saved),
      );
      return tr;
    }),
  );
  renderWorkerLog();
  renderAllesFeed();
  refreshRegionNav();
}

function renderWorkerLog() {
  const list = $("worker-log");
  if (!workerLog || !workerLog.available) {
    summaryLines($("worker-summary"), [
      [
        "Worker log",
        !workerLog
          ? "not configured on the bridge (start it with --worker-log)"
          : `unavailable at ${workerLog.path || "the configured path"}`,
      ],
    ]);
    list.replaceChildren();
    return;
  }
  const s = workerLog.summary;
  summaryLines($("worker-summary"), [
    ["Jobs in tail", `${s.jobs}: ${s.applied} applied, ${s.otherStatus} other, ${s.failed} failed`],
    ["Latency", s.meanLatencyMs == null ? "no completed jobs" : `${s.meanLatencyMs} ms mean, ${s.maxLatencyMs} ms max`],
    ["Tokens", `${formatNumber(s.promptTokens)} prompt, ${formatNumber(s.completionTokens)} completion`],
    ["Last connected", s.lastConnected || "not in this tail"],
    ...(s.lastError ? [["Last error", `${s.lastError.time || ""} ${s.lastError.text}`.trim()]] : []),
    ...(s.budgetExhausted
      ? [["Budget", `reported exhausted at ${s.budgetExhaustedAt || "an earlier time"}; gameplay continues with fallback`]]
      : []),
  ]);
  list.replaceChildren(
    ...workerLog.lines
      .slice(-40)
      .reverse()
      .map((line) => {
        const li = document.createElement("li");
        li.textContent = line;
        if (/failed|refused|fault|error/i.test(line)) li.className = "err";
        return li;
      }),
  );
}

function renderAllesFeed() {
  if (!state) return;
  const select = $("alles-kind");
  const listed = events.filter((event) => event.run === state.run && isAllesEvent(event));
  const kinds = [...new Set(listed.map((event) => event.kind))].sort();
  const current = select.value;
  if ([...select.options].slice(1).map((option) => option.value).join() !== kinds.join()) {
    select.replaceChildren(
      new Option("All alles events", ""),
      ...kinds.map((kind) => new Option(kind.replace("alles_", "").replaceAll("_", " "), kind)),
    );
    select.value = kinds.includes(current) ? current : "";
  }
  const names = new Map(state.bots.map((bot) => [bot.id, bot.name]));
  const shown = listed
    .filter((event) => !select.value || event.kind === select.value)
    .slice(-80)
    .reverse();
  $("alles-feed").replaceChildren(
    ...shown.map((event) => {
      const li = document.createElement("li");
      const time = document.createElement("time");
      time.textContent = formatClock(event.simMs);
      let who;
      if (event.bot && names.has(event.bot)) {
        who = document.createElement("button");
        who.className = "link";
        who.type = "button";
        who.onclick = () => selectBot(event.bot);
      } else who = document.createElement("b");
      who.textContent = event.bot ? names.get(event.bot) || shortGuid(event.bot) : "world";
      const text = document.createElement("span");
      text.textContent = describeEvent(event);
      text.title = text.textContent;
      li.append(time, who, text);
      return li;
    }),
  );
  $("alles-feed-status").textContent = listed.length
    ? `${shown.length} shown of ${listed.length} alles events in the bridge's recent window.`
    : state.journal
      ? "No perceptions, memories or speech journaled yet."
      : "No journal: this world build publishes only the current snapshot. Deploy the telemetry update to see " +
        "actions, perceptions, memories and speech here.";
}
$("alles-kind").onchange = renderAllesFeed;

// ------------------------------------------------------------------------------------------- memory

// The committed memory stores (mod-alles tables in the characters database) and out-of-game questions to a
// character, both served by the bridge. Nothing here reaches the world: no perception, memory or speech.
let memoryOwners = null,
  memoryDetail = null,
  memoryLoading = false,
  talkBusy = false;
const talkLogs = new Map(); // owner -> [{role, text, meta, error}]

function memoryOwner() {
  return $("memory-owner").value;
}

async function loadMemoryOwners() {
  if (!token || !isAlles()) return;
  try {
    const response = await api("/api/memory");
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || response.statusText);
    memoryOwners = payload;
  } catch (error) {
    memoryOwners = { owners: [], error: error.message };
  }
  renderMemoryOwners();
}

async function loadMemory(owner = memoryOwner()) {
  if (!token || !owner || memoryLoading) return;
  memoryLoading = true;
  try {
    const response = await api(`/api/memory?owner=${encodeURIComponent(owner)}`);
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || response.statusText);
    memoryDetail = payload;
  } catch (error) {
    memoryDetail = { owner, error: error.message, memories: [], perceptions: [] };
  } finally {
    memoryLoading = false;
  }
  // The selection may have moved while the read was in flight; read the current one instead.
  if (memoryOwner() && memoryDetail.owner !== memoryOwner()) return loadMemory();
  renderMemory();
  renderTalk();
}

function renderMemoryOwners() {
  const select = $("memory-owner");
  const previous = select.value;
  const owners = memoryOwners?.owners || [];
  select.replaceChildren(
    ...(owners.length ? [] : [new Option(memoryOwners?.error ? "Unavailable" : "No committed owners", "")]),
    ...owners.map((owner) => {
      const held = owner.live
        ? `online, ${formatNumber(owner.live.memoryCount)} held`
        : owner.online
          ? "online, not sampled"
          : "offline";
      return new Option(
        `${owner.name} (${owner.owner}) – ${held}, ${formatNumber(owner.committedMemories)} committed`,
        owner.owner,
      );
    }),
  );
  if (owners.some((owner) => owner.owner === previous)) select.value = previous;
  $("memory-note").textContent = memoryOwners?.error
    ? memoryOwners.error
    : memoryOwners?.database
      ? `${owners.length} owners committed in ${memoryOwners.database.database}`
      : memoryOwners?.reasons?.database || "";
  if (select.value !== previous || (select.value && !memoryDetail)) {
    memoryDetail = null;
    renderMemory();
    loadMemory();
  }
  renderTalk();
}

function spanningRow(text, span) {
  const tr = document.createElement("tr");
  const td = cell(text, "text");
  td.colSpan = span;
  tr.append(td);
  return tr;
}

function renderMemory() {
  const detail = memoryDetail;
  const owner = memoryOwner();
  const memoryBody = $("memory-table").tBodies[0];
  const perceptionBody = $("perception-table").tBodies[0];
  if (!detail || detail.owner !== owner) {
    memoryBody.replaceChildren();
    perceptionBody.replaceChildren();
    $("memory-count").textContent = $("perception-count").textContent = "";
    $("memory-status").textContent = owner ? "Reading the committed store…" : "Select a character.";
    return;
  }
  if (detail.error) {
    memoryBody.replaceChildren(spanningRow(detail.error, 8));
    perceptionBody.replaceChildren();
    $("memory-count").textContent = $("perception-count").textContent = "";
    $("memory-status").textContent = "The committed store could not be read.";
    return;
  }
  const now = Date.now();
  const shown = filterMemories(detail.memories, $("memory-filter").value, $("memory-sort").value);
  memoryBody.replaceChildren(
    ...shown.map((memory) => {
      const tr = document.createElement("tr");
      const salience = cell((memory.salience ?? 0).toFixed(2), "share");
      salience.style.setProperty("--share", `${Math.round(Math.min(1, Math.max(0, memory.salience ?? 0)) * 100)}%`);
      const via = memory.attribution && memory.attribution !== memory.source.name ? ` via ${memory.attribution}` : "";
      const depth = memory.reportedDepth == null ? "" : `, depth ${memory.reportedDepth}`;
      tr.append(
        cell(memory.text, "text"),
        cell(memory.kind, "text"),
        cell(memory.source.name ? `${memory.source.name}${via}${depth}` : memory.subject.name || "", "text"),
        cell((memory.confidence ?? 0).toFixed(2)),
        salience,
        cell(relativeTime(memory.formedUnixMs, now), "text"),
        cell(memory.recalledUnixMs ? relativeTime(memory.recalledUnixMs, now) : "never", "text"),
        cell(memory.formation, "text"),
      );
      tr.title = `memory ${memory.id}, revision ${memory.revision}`;
      return tr;
    }),
  );
  if (!shown.length)
    memoryBody.replaceChildren(
      spanningRow(detail.memories.length ? "No memory matches the filter." : "No committed memories.", 8),
    );
  $("memory-count").textContent = `${shown.length} of ${detail.memories.length}`;
  const sheet = detail.sheet || {};
  const live = detail.live;
  const store =
    live && live.memoryRevision != null
      ? `; the live store is at revision ${formatNumber(live.memoryRevision)} holding ` +
        `${formatNumber(live.memoryCount)} memories and ${formatNumber(live.pendingPerceptions)} pending perceptions` +
        (live.saving ? ", saving" : "") +
        (live.saveFailed ? ", last save failed" : "")
      : "; the character is not in the current world sample";
  const who = sheet.level != null ? `level ${sheet.level} ${sheet.race} ${sheet.class}` : "no character sheet";
  $("memory-status").textContent =
    `${detail.name}, ${who}, in ${detail.place}. Committed revision ${formatNumber(detail.committedRevision)} ` +
    `read ${relativeTime(detail.readUnixMs, now)}${store}.`;
  perceptionBody.replaceChildren(
    ...detail.perceptions.map((perception) => {
      const tr = document.createElement("tr");
      tr.append(
        cell(perception.text || perception.subject || "", "text"),
        cell(perception.kind + (perception.comprehended ? "" : ", not understood"), "text"),
        cell(perception.source, "text"),
        cell(perception.place, "text"),
        cell(relativeTime(perception.unixMs, now), "text"),
      );
      return tr;
    }),
  );
  if (!detail.perceptions.length)
    perceptionBody.replaceChildren(
      spanningRow("No perception is waiting for interpretation in the committed store.", 5),
    );
  $("perception-count").textContent = String(detail.perceptions.length);
}

function talkLog(owner) {
  if (!talkLogs.has(owner)) talkLogs.set(owner, []);
  return talkLogs.get(owner);
}

function renderTalk() {
  const owner = memoryOwner();
  const available = Boolean(memoryOwners?.talk) && Boolean(owner);
  $("talk-input").disabled = $("talk-send").disabled = !available || talkBusy;
  const log = $("talk-log");
  const name = memoryDetail?.owner === owner && memoryDetail.name ? memoryDetail.name : owner;
  log.replaceChildren(
    ...(owner ? talkLog(owner) : []).map((turn) => {
      const li = document.createElement("li");
      li.className = turn.error ? "err" : turn.role;
      const who = document.createElement("b");
      who.textContent = turn.role === "observer" ? "You: " : turn.error ? "Bridge: " : `${name}: `;
      li.append(who, turn.text);
      if (turn.meta) {
        const meta = document.createElement("span");
        meta.className = "meta";
        meta.textContent = turn.meta;
        li.append(meta);
      }
      return li;
    }),
  );
  if (talkBusy) {
    const li = document.createElement("li");
    li.className = "character";
    li.textContent = `${name} is thinking…`;
    log.append(li);
  }
  log.scrollTop = log.scrollHeight;
  $("talk-status").textContent = !memoryOwners
    ? "Waiting for the bridge."
    : !memoryOwners.talk
      ? memoryOwners.reasons?.talk || "Questions are unavailable."
      : `Out-of-game interview through ${memoryOwners.talk.model}: the question is not heard in the world, ` +
        "forms no memory and uses no interpreter budget. Answers draw on the committed memories shown here.";
}

async function askMemory(event) {
  event.preventDefault();
  const owner = memoryOwner();
  const input = $("talk-input");
  const message = input.value.trim();
  if (!owner || !message || talkBusy) return;
  const log = talkLog(owner);
  const history = log
    .filter((turn) => !turn.error)
    .slice(-8)
    .map((turn) => ({ role: turn.role, text: turn.text }));
  log.push({ role: "observer", text: message });
  input.value = "";
  talkBusy = true;
  renderTalk();
  try {
    const response = await api("/api/memory/talk", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ owner, message, history }),
    });
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || response.statusText);
    log.push({
      role: "character",
      text: payload.text,
      meta:
        `${(payload.latencyMs / 1000).toFixed(1)} s, ${payload.memoriesOffered.length} of ` +
        `${payload.memoriesCommitted} memories offered, ${formatNumber(payload.promptTokens)} prompt and ` +
        `${formatNumber(payload.completionTokens)} completion tokens`,
    });
  } catch (error) {
    log.push({ role: "character", text: error.message, error: true });
  } finally {
    talkBusy = false;
  }
  renderTalk();
  $("talk-input").focus();
}

$("memory-owner").onchange = () => {
  memoryDetail = null;
  renderMemory();
  loadMemory();
  renderTalk();
};
$("memory-refresh").onclick = () => {
  loadMemoryOwners();
  loadMemory();
};
$("memory-filter").oninput = renderMemory;
$("memory-sort").onchange = renderMemory;
$("talk-form").onsubmit = askMemory;
$("talk-clear").onclick = () => {
  talkLogs.delete(memoryOwner());
  renderTalk();
};

// ----------------------------------------------------------------------------------------------- polling

let pollCount = 0;
setInterval(async () => {
  if (state && !state.completed) {
    const stale =
      performance.now() - freshAt > 3000 ||
      (state.publishedUnixMs !== undefined && Date.now() - state.publishedUnixMs > 3000);
    if (stale) {
      notice("No snapshot for 3 s. Check the bridge.");
      linkState("stale");
    }
    if (!held) renderAlerts();
  }
  if (held) return;
  renderRoster();
  if (!token) return;
  pollCount += 1;
  try {
    events = await (await api("/api/events?scope=progression")).json();
    renderDetails();
    renderFeed();
    renderAllesFeed();
  } catch {
    /* freshness is shown above */
  }
  if (isAlles() && pollCount % 2 === 0) {
    try {
      workerLog = await (await api("/api/worker-log")).json();
    } catch {
      workerLog = null;
    }
    renderInterpreter();
  }
  // The committed stores change once per save, so they are re-read more slowly than the live sample.
  if (isAlles() && pollCount % 10 === 1) loadMemoryOwners();
  if (isAlles() && pollCount % 5 === 3) loadMemory();
  if (pollCount % 2 === 1) {
    try {
      eventStats = await (await api("/api/event-stats")).json();
      renderEventMix();
    } catch {
      eventStats = null;
    }
  }
  if (pollCount % 30 === 0) await refreshLongTerm(abort?.signal);
}, 2000);
window.addEventListener("resize", () => {
  drawChart();
  if (state) {
    renderInstruments(state.expectedBots);
    renderMetrics();
    renderAnalytics();
  }
});
// The page is one long scroll through nine regions. The strip in the bench jumps between them and marks the one
// being read; every region carries the bench's measured height as a scroll margin so a jump clears it.
const regionsInView = new Set();
const regionObserver = new IntersectionObserver(
  (entries) => {
    for (const entry of entries) {
      if (entry.isIntersecting) regionsInView.add(entry.target.id);
      else regionsInView.delete(entry.target.id);
    }
    const links = [...$("region-nav").children];
    const reading = links.find((link) => regionsInView.has(link.dataset.region));
    for (const link of links) link.setAttribute("aria-current", String(link === reading));
  },
  { rootMargin: "-25% 0px -60% 0px" },
);

function refreshRegionNav() {
  const regions = [...document.querySelectorAll("section.region")].filter((region) => !region.hidden);
  const nav = $("region-nav");
  const wanted = regions.map((region) => region.getAttribute("aria-label")).join("|");
  if (nav.dataset.regions === wanted) return;
  nav.dataset.regions = wanted;
  nav.replaceChildren(
    ...regions.map((region) => {
      const label = region.getAttribute("aria-label");
      if (!region.id) region.id = `region-${label.toLowerCase().replace(/[^a-z0-9]+/g, "-")}`;
      const link = document.createElement("a");
      link.href = `#${region.id}`;
      link.textContent = label;
      link.dataset.region = region.id;
      return link;
    }),
  );
  regionsInView.clear();
  regionObserver.disconnect();
  for (const region of regions) regionObserver.observe(region);
}

const benchElement = document.querySelector(".bench");
new ResizeObserver(() => {
  const height = Math.round(benchElement.getBoundingClientRect().height);
  document.documentElement.style.setProperty("--bench-height", `${Math.min(height, 72)}px`);
}).observe(benchElement);
// Preserve the page's layout while replacing the full bench with a small fixed strip.
// This avoids scroll jumps and leaves the other regions readable beneath the essentials.
function collapseBench() {
  const compact = window.scrollY > 120;
  if (compact === benchElement.classList.contains("compact")) return;
  const placeholder = $("bench-placeholder");
  if (compact) placeholder.style.height = `${benchElement.getBoundingClientRect().height}px`;
  placeholder.hidden = !compact;
  benchElement.classList.toggle("compact", compact);
  document.body.classList.toggle("scrolled", compact);
}
document.addEventListener("scroll", collapseBench, { passive: true });
collapseBench();

renderLegend();
renderControlLog();
renderHold();
refreshRegionNav();
drawMap();

const raceNames = { 1: "Human", 2: "Orc", 3: "Dwarf", 4: "Night Elf", 5: "Undead", 6: "Tauren",
  7: "Gnome", 8: "Troll (control)", 10: "Blood Elf", 11: "Draenei" };
let populationDraft = null;
let populationResizeBase = null;
let populationDraftRun = "";
let populationDirty = false;
let populationPendingDraft = null;
let raceCapacitySignature = "";
function populationTotal(counts) {
  return Object.values(counts).reduce((sum, count) => sum + count, 0);
}
function samePopulation(left, right) {
  return left && right && Object.keys(left).every((race) => left[race] === right[race]);
}
function writePopulationDraft() {
  const total = populationTotal(populationDraft);
  $("bot-count").value = $("bot-range").value = total;
  for (const input of $("race-counts").querySelectorAll("input"))
    input.value = populationDraft[input.dataset.race] ?? 0;
  $("population-draft-status").textContent = `${total} requested: ` +
    Object.entries(populationDraft).filter(([, count]) => count > 0)
      .map(([race, count]) => `${count} ${raceNames[race]}`).join(", ") +
    (populationPendingDraft ? " · applying" : populationDirty ? " · not applied" : "");
}
function renderRacePopulation(locked) {
  $("race-population").hidden = !state.racePopulation || isReadOnly();
  $("population-mode").disabled = locked || !state.racePopulation;
  if (!state.racePopulation) return;
  const actual = Object.fromEntries(state.racePopulation.map((row) => [row.race, row.target]));
  if (populationDraftRun !== state.run) {
    populationDraftRun = state.run;
    populationDraft = actual;
    populationResizeBase = null;
    populationDirty = false;
    populationPendingDraft = null;
    raceCapacitySignature = "";
  }
  if (samePopulation(actual, populationPendingDraft)) {
    if (samePopulation(populationDraft, populationPendingDraft)) populationDirty = false;
    populationPendingDraft = null;
  }
  if (!populationDirty && !populationPendingDraft) populationDraft = actual;
  const signature = JSON.stringify(state.racePopulation.map(({race, capacity}) => [race, capacity]));
  if (signature !== raceCapacitySignature) {
    raceCapacitySignature = signature;
    $("race-counts").replaceChildren(...state.racePopulation.map((row) => {
      const label = document.createElement("label");
      label.textContent = `${raceNames[row.race]} (max ${row.capacity}) `;
      const input = document.createElement("input");
      Object.assign(input, { type: "number", min: "0", max: String(row.capacity), step: "1",
        value: String(populationDraft[row.race]), required: true });
      input.dataset.race = row.race;
      input.oninput = () => {
        if (!input.validity.valid) return;
        populationDraft[row.race] = Number(input.value);
        populationDirty = true;
        $("population-mode").value = "races";
        $("population-error").textContent = "";
        writePopulationDraft();
      };
      label.append(input);
      return label;
    }));
  }
  for (const input of $("race-counts").querySelectorAll("input")) input.disabled = locked;
  $("set-races").disabled = locked;
  // Leave the user's partially typed input alone while fresh snapshots arrive.
  if (!document.activeElement?.matches("#race-counts input, #bot-count")) writePopulationDraft();
}
async function applyPopulation() {
  if (!$("race-population-form").reportValidity() || $("population-error").textContent) return;
  const counts = { ...populationDraft };
  populationPendingDraft = counts;
  populationDirty = true;
  writePopulationDraft();
  if (!(await control(state.paused, selectedSpeed(), populationTotal(counts), undefined, counts))) {
    populationPendingDraft = null;
    writePopulationDraft();
  }
}
$("race-population-form").onsubmit = async (event) => {
  event.preventDefault();
  await applyPopulation();
};

for (const id of ["bot-count", "bot-range"]) {
  $(id).onfocus = () => { populationResizeBase = { ...populationDraft }; };
  $(id).onblur = () => { populationResizeBase = null; };
}

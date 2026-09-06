import {
  ACTIVITIES,
  TRACE_KINDS,
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
  freshAt = 0,
  abort = null,
  manifestRun = "";
let view = { x: 0, y: 0, scale: 0.03 },
  needsFit = true,
  pendingControl = 0,
  controlLog = [],
  gaps = 0,
  ingestTimes = [],
  chartHover = null,
  feedFrozen = null,
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
      [
        "History retention",
        "Up to 4,000 samples in this session; recent recorded history loads on connect. " +
          "Full timestamped history is retained in exports. Python observations are sampled per bot.",
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

function resetRun() {
  history = [];
  events = [];
  selected = "";
  needsFit = true;
  pendingControl = 0;
  controlLog = [];
  gaps = 0;
  trails.clear();
  feedFrozen = null;
  eventStats = null;
}

// ------------------------------------------------------------------------------------------------ ingest

function ingest(snapshot, restoring = false) {
  if (state && state.run !== snapshot.run) resetRun();
  if (state?.run === snapshot.run && state.seq >= snapshot.seq) return;
  if (!restoring && state?.run === snapshot.run && snapshot.seq > state.seq + 1) gaps += snapshot.seq - state.seq - 1;
  const previousTarget = state?.expectedBots;
  const previousLimit = state?.speedControl?.backlogLimitMs;
  state = snapshot;
  if (previousLimit !== state.speedControl?.backlogLimitMs && state.speedControl)
    $("backlog-limit").value = state.speedControl.backlogLimitMs;
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
  updateMaps();
  renderHeader();
  renderInstruments(previousTarget);
  renderMetrics();
  renderAlerts();
  renderLegend();
  renderRoster();
  renderDetails();
  renderAnalytics();
  drawChart();
  renderControlLog();
  loadManifest();
}

function isPython() {
  return state?.source === "python-api";
}

function isReadOnly() {
  return isPython() || Boolean(state?.readOnly);
}

// ------------------------------------------------------------------------------------------------ header

function renderHeader() {
  const python = isPython();
  $("run-label").textContent = python
    ? `${state.label}, Python observation feed, run ${state.run}`
    : `Run ${state.run}, in-memory telemetry`;
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
  $("clock-label").textContent = python ? "Elapsed publisher time" : "Simulated time";
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

  $("backlog-figure").hidden = $("tick-figure").hidden = python;
  if (!python) {
    $("backlog-value").textContent = `${(state.backlogMs / 1000).toFixed(2)} s`;
    $("backlog-value").dataset.state = state.overloaded ? "danger" : "";
    $("tick-value").textContent = `${(state.maxTickUs / 1000).toFixed(1)} ms`;
    figureSpark("backlog-spark", "backlogMs", state.overloaded ? palette.ember : palette.brass);
    figureSpark("tick-spark", "tickMs", palette.ash);
  }

  $("deck").hidden = Boolean(readOnly);
  const locked = Boolean(readOnly || state.fault || state.completed || state.baseline || state.observers);
  $("pause").disabled = locked;
  $("pause").textContent = state.paused ? "Resume" : "Pause";
  for (const button of $("speed-control").querySelectorAll("button")) {
    const isMax = button.dataset.speed === "max";
    button.disabled = locked || (isMax && !state.speedControl);
    button.setAttribute(
      "aria-pressed",
      String(isMax ? maxSpeed : !maxSpeed && Number(button.dataset.speed) === state.requestedSpeed),
    );
  }
  $("backlog-limit").disabled = $("set-max-speed").disabled = locked || !state.speedControl;
  $("max-speed-status").textContent = maxSpeed
    ? `${state.speedControl.status}. Backlog target: ${state.speedControl.backlogLimitMs} ms; ` +
      "brief spikes may occur while speed adjusts."
    : state.speedControl
      ? state.speedStep === 0.1
        ? "Max adjusts from 1× to 10× in 0.1× steps using the backlog trend."
        : "Max selects 1×, 2×, 5× or 10×. Update the worldserver to enable 0.1× steps."
      : "Bridge update required for Max speed.";
  const populationLocked = Boolean(
    readOnly || state.fault || state.completed || state.baseline || state.maxBots === undefined,
  );
  $("bot-count").disabled = $("set-bots").disabled = $("bot-range").disabled = populationLocked;
  $("bot-count").max = $("bot-range").max = state.maxBots ?? 100;
  if (previousTarget !== state.expectedBots) $("bot-count").value = $("bot-range").value = state.expectedBots;
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
  const tiles = python
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

async function control(paused, speed, bots, observerMode) {
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
  }
  renderControlLog();
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
  loadMaps().catch(() => {
    $("map-art-status").textContent = "Map artwork unavailable";
  });
  connect();
});
$("population").onsubmit = (event) => {
  event.preventDefault();
  if ($("population").reportValidity()) control(state.paused, selectedSpeed(), Number($("bot-count").value));
};
$("bot-range").oninput = () => {
  $("bot-count").value = $("bot-range").value;
};
$("bot-count").oninput = () => {
  $("bot-range").value = $("bot-count").value;
};
function selectedSpeed() {
  return state.speedControl?.mode === "max" ? "max" : state.requestedSpeed;
}
$("pause").onclick = () => control(!state.paused, selectedSpeed());
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
      if (active || bots.length <= 5) {
        const labelWidth = ctx.measureText(bot.name).width;
        ctx.fillStyle = "rgba(15, 23, 27, 0.85)";
        ctx.fillRect(point.x + radius + 4, point.y - 10, labelWidth + 12, 19);
        ctx.fillStyle = palette.parchment;
        ctx.fillText(bot.name, point.x + radius + 10, point.y + 4);
      }
      ctx.restore();
    }
    ctx.font = '11px "Noto Sans", "Segoe UI", system-ui, sans-serif';
    drawScaleBar(ctx, height);
    if (shownCount !== bots.length) {
      shownCount = bots.length;
      $("map-count").textContent = `${bots.length} bot${bots.length === 1 ? "" : "s"} in view`;
    }
  }
  requestAnimationFrame(drawMap);
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
      const cells = [bot.name, bot.level, bot.activity, `${Math.round(healthPercent(bot))}%`].map((text) => {
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

function describeEvent(event) {
  const what = event.detail || (event.value ? formatNumber(event.value) : "");
  const spell = event.spell ? `, spell ${event.spell}` : "";
  const context = event.context && event.context !== event.detail ? ` [${event.context}]` : "";
  return `${event.kind.replaceAll("_", " ")}${what ? `: ${what}` : ""}${spell}${context}`;
}

function renderDetails() {
  const bot = state?.bots.find((bot) => bot.id === selected);
  for (const id of ["bot-bars", "bot-spark", "bot-details", "events-title", "events"]) $(id).hidden = !bot;
  $("bot-empty").hidden = Boolean(bot);
  if (!bot) {
    $("bot-name").textContent = selected ? "Bot is no longer in this feed" : "No bot selected";
    $("bot-empty").textContent = selected
      ? "Its recorded events stay in the exports."
      : "Pick one from the list or click a marker on the map.";
    return;
  }
  const python = isPython();
  $("bot-name").textContent = `${bot.name}, level ${bot.level}`;
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
            ["Earned XP this run", formatNumber(bot.earnedXp)],
            ["Quests completed", bot.questCompletions],
            ["Deaths", bot.deaths],
            ["Money", formatMoney(bot.money)],
            ["Equipped items", `${gearCount(bot)} of ${(bot.gear || []).length} slots`],
            ["AI updates", formatNumber(bot.aiUpdates)],
            [
              "Last AI update",
              silentMs == null
                ? "unknown"
                : silentMs > 10000
                  ? `${(silentMs / 1000).toFixed(1)} s ago, silent for over 10 s`
                  : `${(silentMs / 1000).toFixed(1)} s ago`,
            ],
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
    li.textContent = "No progression events for this bot yet.";
    $("events").replaceChildren(li);
  }
}

// ------------------------------------------------------------------------------------------------ cohort

function renderAnalytics() {
  const bots = state.bots;
  const mix = summarize(state).activity;
  const python = isPython();
  const timeFormat = python ? duration : formatClock;
  $("cohort-title").textContent = python ? "Cohort over elapsed time" : "Cohort over simulated time";
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
    ["Silent AI", python ? "not reported" : stalledBots(state).length],
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
  const metric = $("board-metric").value;
  for (const option of $("board-metric").options) {
    option.hidden = option.disabled = option.hasAttribute("data-python")
      ? !isPython()
      : ["money", "stalled"].includes(option.value) && isPython();
  }
  if ($("board-metric").selectedOptions[0]?.disabled) $("board-metric").value = isPython() ? "level" : "earnedXp";
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
  $("chart-title").textContent = python ? "Timeline over elapsed real time" : "Timeline";
  for (const option of $("chart-metric").options) {
    option.disabled = option.hidden = python
      ? !["levels", "deaths"].includes(option.value) && !option.hasAttribute("data-python")
      : option.hasAttribute("data-python");
  }
  if ($("chart-metric").selectedOptions[0]?.disabled) $("chart-metric").value = python ? "observed" : "xp";
  const { ctx, width, height } = surface($("chart-canvas"));
  const window = Number($("chart-window").value);
  const points =
    window && history.length ? history.filter((point) => point.simMs >= history.at(-1).simMs - window) : history;
  $("history-status").textContent = points.length
    ? `${points.length.toLocaleString()} samples in view`
    : "No history yet";
  if (points.length < 1) {
    $("legend").replaceChildren();
    $("chart-readout").replaceChildren();
    return;
  }
  const metric = $("chart-metric").value;
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
$("chart-metric").onchange = $("chart-window").onchange = drawChart;
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
    renderAlerts();
  }
  renderRoster();
  if (!token) return;
  pollCount += 1;
  try {
    events = await (await api("/api/events?scope=progression")).json();
    renderDetails();
    renderFeed();
  } catch {
    /* freshness is shown above */
  }
  if (pollCount % 2 === 1) {
    try {
      eventStats = await (await api("/api/event-stats")).json();
      renderEventMix();
    } catch {
      eventStats = null;
    }
  }
}, 2000);
window.addEventListener("resize", () => {
  drawChart();
  if (state) {
    renderInstruments(state.expectedBots);
    renderMetrics();
    renderAnalytics();
  }
});
renderLegend();
renderControlLog();
drawMap();

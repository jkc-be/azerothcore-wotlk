import {
  duration,
  summarize,
  visibleBots,
  worldToScreen,
  fitView,
  chooseMap,
  mapView,
  mapTile,
  mapRect,
} from "./model.js";
const $ = (id) => document.getElementById(id);
let token = "",
  state = null,
  selected = "",
  history = [],
  events = [],
  freshAt = 0,
  abort = null;
let view = { x: 0, y: 0, scale: 0.03 },
  needsFit = true,
  pendingControl = 0;
const colors = ["#79d8bc", "#70b7ff", "#f4c774", "#c89cf6", "#ee91a2", "#accb71"];
let mapAreas = [],
  fitArtwork = true,
  artwork = null,
  loadingArtwork = null;
const mapImages = new Map();
const continentNames = {
  0: "Eastern Kingdoms",
  1: "Kalimdor (western continent)",
  530: "Outland & draenei isles",
  571: "Northrend",
};

async function loadMaps() {
  mapAreas = (await (await api("/api/maps")).json()).areas;
  if (state) updateMaps();
  $("map-art-status").textContent = mapAreas.length
    ? "Local client artwork · calibrated to world coordinates."
    : "No local artwork installed; showing world coordinates.";
}

function updateMaps() {
  options("map", [...state.bots.map((bot) => String(bot.map)), ...mapAreas.map((area) => String(area.map))]);
  updateZones();
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
    $("map-art-status").textContent = `${area.name} · local client artwork, aligned to world coordinates.`;
  } catch {
    mapImages.set(area.id, []);
    $("map-art-status").textContent = "Map artwork unavailable; world coordinates remain live.";
  } finally {
    loadingArtwork = null;
  }
}

async function api(path, options = {}) {
  const response = await fetch(path, { ...options, headers: { Authorization: `Bearer ${token}`, ...options.headers } });
  if (!response.ok) throw new Error((await response.json()).error || `HTTP ${response.status}`);
  return response;
}
function notice(text) {
  $("notice").textContent = text;
}
function options(id, values, all = false) {
  const element = $(id),
    old = element.value;
  const desired = (all ? ["all"] : []).concat([...new Set(values)].sort((a, b) => Number(a) - Number(b)));
  if ([...element.options].map((option) => option.value).join() === desired.join()) return;
  element.replaceChildren(
    ...desired.map((value) => {
      const area = mapAreas.find((area) => String(area.map) === $("map").value && String(area.zone) === value);
      const label =
        id === "map"
          ? continentNames[value] || `Map ${value}`
          : value === "all"
            ? "Bot zone (automatic)"
            : value === "0"
              ? "Whole continent"
              : area
                ? area.name.replace(/([a-z])([A-Z])/g, "$1 $2")
                : `Zone ${value}`;
      return new Option(label, value);
    }),
  );
  if (desired.includes(old)) element.value = old;
}
function ingest(snapshot) {
  if (state && state.run !== snapshot.run) {
    history = [];
    events = [];
    selected = "";
    needsFit = true;
    pendingControl = 0;
  }
  if (state?.run === snapshot.run && state.seq >= snapshot.seq) return;
  const previousTarget = state?.expectedBots;
  state = snapshot;
  freshAt = performance.now();
  if (!history.length || history.at(-1).simMs !== state.simMs) history.push(summarize(state));
  if (history.length > 4000) history.splice(0, history.length - 4000);
  updateMaps();
  $("pause").disabled = $("speed").disabled =
    Boolean(state.fault || state.completed || state.baseline || state.observers);
  $("bot-count").disabled = $("set-bots").disabled =
    Boolean(state.fault || state.completed || state.baseline || state.maxBots === undefined);
  $("bot-count").max = state.maxBots ?? 100;
  if (previousTarget !== state.expectedBots) $("bot-count").value = state.expectedBots;
  $("population-status").textContent = state.maxBots === undefined
    ? "Server update required to change bot count"
    : state.populationPending
    ? `${state.onlineBots} / ${state.expectedBots} bots · ` +
      (state.paused ? "waiting for Resume" : "adjusting population…")
    : `${state.onlineBots} / ${state.expectedBots} bots · matched`;
  $("pause").textContent = state.paused ? "Resume" : "Pause";
  $("speed").value = state.requestedSpeed;
  $("control-status").textContent =
    pendingControl && state.controlSeq < pendingControl
      ? `Request ${pendingControl} awaiting world acknowledgement`
      : state.controlError || `Applied request ${state.controlSeq}`;
  const metrics = [
    ["Simulated", duration(state.simMs)],
    ["Real elapsed", duration(state.realMs)],
    ["Requested / achieved", `${state.requestedSpeed}× / ${state.achievedSpeed.toFixed(2)}×`],
    ["Active / online / in world", `${state.activeBots} / ${state.onlineBots} / ${state.bots.length}`],
    ["Target bots", `${state.expectedBots} (limit ${state.maxBots ?? state.expectedBots})`],
    ["Backlog", `${(state.backlogMs / 1000).toFixed(2)}s`],
  ];
  $("metrics").replaceChildren(
    ...metrics.map(([label, value]) => {
      const div = document.createElement("div");
      div.className = "metric";
      const span = document.createElement("span");
      span.textContent = label;
      const b = document.createElement("b");
      b.textContent = value;
      div.append(span, b);
      return div;
    }),
  );
  notice(
    state.completed
      ? "Configured simulated duration completed. Export this run for comparison."
      : state.fault
        ? `RUN FROZEN: ${state.fault}. Export the evidence and inspect the server.`
        : state.paused
          ? "Gameplay paused. Observation and controls remain available."
          : state.observers
            ? `GM POV connected (${state.observers}) · 1× locked until all observers disconnect.`
            : state.overloaded
              ? "Capacity shortfall: simulation debt is growing; gameplay steps are retained."
              : `Connected · run ${state.run} · authoritative in-memory telemetry`,
  );
  renderDetails();
  drawChart();
}
function updateZones() {
  if (state)
    options(
      "zone",
      [
        ...state.bots.filter((bot) => String(bot.map) === $("map").value).map((bot) => String(bot.zone)),
        ...mapAreas.filter((area) => String(area.map) === $("map").value).map((area) => String(area.zone)),
      ],
      true,
    );
}
async function connect() {
  abort?.abort();
  abort = new AbortController();
  const signal = abort.signal;
  while (!signal.aborted) {
    try {
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
      if (!signal.aborted) notice(`Connection interrupted: ${error.message}. Reconnecting…`);
    }
    if (!signal.aborted) await new Promise((resolve) => setTimeout(resolve, 1500));
  }
}
$("connect").addEventListener("submit", (event) => {
  event.preventDefault();
  token = $("token").value;
  loadMaps().catch(() => {
    $("map-art-status").textContent = "Map artwork unavailable.";
  });
  connect();
});
async function control(paused, speed, bots) {
  if (!state) return;
  try {
    const response = await api("/api/control", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ run: state.run, speed, paused, ...(bots === undefined ? {} : { bots }) }),
    });
    pendingControl = (await response.json()).sequence;
    $("control-status").textContent = `Request ${pendingControl} awaiting world acknowledgement`;
  } catch (error) {
    notice(error.message);
  }
}
$("population").onsubmit = (event) => {
  event.preventDefault();
  if ($("population").reportValidity()) control(state.paused, state.requestedSpeed, Number($("bot-count").value));
};
$("pause").onclick = () => control(!state.paused, state.requestedSpeed);
$("speed").onchange = () => control(state.paused, Number($("speed").value));
$("map").onchange = () => {
  updateZones();
  needsFit = true;
  fitArtwork = true;
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
$("chart-metric").onchange = drawChart;
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
function renderDetails() {
  const bot = state?.bots.find((bot) => bot.id === selected);
  if (!bot) {
    $("bot-name").textContent = selected ? "Bot is offline" : "Select a bot";
    $("bot-details").textContent = selected ? "This bot has left the simulated population." : "Choose a marker.";
    $("events").replaceChildren();
    return;
  }
  $("bot-name").textContent = `${bot.name} · level ${bot.level}`;
  $("bot-details").replaceChildren(
    ...[
      ["Identity", bot.id],
      ["Map / instance / zone", `${bot.map} / ${bot.instance} / ${bot.zone}`],
      ["XP", `${bot.xp} / ${bot.nextLevelXp}`],
      ["Earned XP", bot.earnedXp],
      ["Health", `${bot.health} / ${bot.maxHealth}`],
      ["Activity", bot.activity],
      ["Deaths / completed quests", `${bot.deaths} / ${bot.questCompletions}`],
      ...bot.quests.map((quest) => [
        `Quest ${quest.id}`,
        `state ${quest.state}: ${quest.objectives.join(" / ")}; items ${(quest.items || []).join(" / ")}`,
      ]),
    ].map(([label, value]) => row(label, value)),
  );
  $("events").replaceChildren(
    ...events
      .filter((event) => event.bot === selected)
      .slice(-40)
      .reverse()
      .map((event) => {
        const li = document.createElement("li");
        li.textContent =
          `${duration(event.simMs)} · ${event.kind}: ${event.detail || event.value}` +
          (event.context ? ` [${event.context}]` : "");
        return li;
      }),
  );
}
const canvas = $("map-canvas");
let drag = null;
canvas.onpointerdown = (event) => {
  drag = { px: event.clientX, py: event.clientY, x: view.x, y: view.y, moved: false };
  canvas.setPointerCapture(event.pointerId);
};
canvas.onpointermove = (event) => {
  if (!drag) return;
  const dx = event.clientX - drag.px,
    dy = event.clientY - drag.py;
  if (Math.hypot(dx, dy) > 4) drag.moved = true;
  view.x = drag.x + dy / view.scale;
  view.y = drag.y + dx / view.scale;
};
canvas.onpointerup = (event) => {
  if (drag && !drag.moved && state) {
    const rect = canvas.getBoundingClientRect();
    const bots = visibleBots(state, $("map").value, $("zone").value);
    const hit = bots
      .map((bot) => ({ bot, point: worldToScreen(bot, view, rect.width, rect.height) }))
      .find(({ point }) => Math.hypot(point.x - event.clientX + rect.left, point.y - event.clientY + rect.top) < 12);
    if (hit) {
      selected = hit.bot.id;
      renderDetails();
    }
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
function surface(element) {
  const { width, height } = element.getBoundingClientRect(),
    ratio = window.devicePixelRatio || 1;
  if (element.width !== Math.round(width * ratio) || element.height !== Math.round(height * ratio)) {
    element.width = Math.round(width * ratio);
    element.height = Math.round(height * ratio);
  }
  const ctx = element.getContext("2d");
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, height);
  return { ctx, width, height };
}
function drawMap() {
  const { ctx, width, height } = surface(canvas);
  ctx.fillStyle = "#111d25";
  ctx.fillRect(0, 0, width, height);
  if (state) {
    const bots = visibleBots(state, $("map").value, $("zone").value);
    const area = $("map-art").checked ? chooseMap(mapAreas, $("map").value, $("zone").value, bots, selected) : null;
    if (area?.id !== artwork) {
      artwork = area?.id;
      if (area && mapImages.get(area.id)?.length)
        $("map-art-status").textContent = `${area.name} · local client artwork, aligned to world coordinates.`;
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
    ctx.strokeStyle = "#263640";
    ctx.lineWidth = 1;
    ctx.globalAlpha = tiles?.length ? 0.18 : 1;
    for (let x = 0; x < width; x += 50) {
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, height);
      ctx.stroke();
    }
    for (let y = 0; y < height; y += 50) {
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(width, y);
      ctx.stroke();
    }
    ctx.globalAlpha = 1;
    for (const bot of bots) {
      const point = worldToScreen(bot, view, width, height);
      const radius = bot.id === selected ? 11 : 9;
      ctx.save();
      ctx.shadowColor = "#000";
      ctx.shadowBlur = 5;
      ctx.fillStyle = bot.health === 0 ? "#a52649" : "#1267cb";
      ctx.strokeStyle = bot.id === selected ? "#ffe08a" : "#fff";
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(point.x, point.y, radius, 0, 2 * Math.PI);
      ctx.fill();
      ctx.stroke();
      ctx.shadowBlur = 0;
      ctx.fillStyle = "#fff";
      ctx.beginPath();
      ctx.arc(point.x, point.y - 3, 2.3, 0, 2 * Math.PI);
      ctx.fill();
      ctx.beginPath();
      ctx.arc(point.x, point.y + 4, 4.5, Math.PI, 0);
      ctx.fill();
      if (bot.id === selected || bots.length <= 5) {
        ctx.font = "bold 12px sans-serif";
        const labelWidth = ctx.measureText(bot.name).width;
        ctx.fillStyle = "#102033";
        ctx.fillRect(point.x + 14, point.y - 12, labelWidth + 10, 19);
        ctx.fillStyle = "#fff";
        ctx.fillText(bot.name, point.x + 19, point.y + 2);
      }
      ctx.restore();
    }
    ctx.fillStyle = "#90a3b0";
    ctx.fillText(`50 px ≈ ${(50 / view.scale).toFixed(0)} yards`, 12, height - 12);
  }
  requestAnimationFrame(drawMap);
}
function drawChart() {
  const { ctx, width, height } = surface($("chart-canvas"));
  if (!history.length) return;
  const metric = $("chart-metric").value,
    distributions = metric === "levels" || metric === "zones";
  const keys = distributions ? [...new Set(history.flatMap((point) => Object.keys(point[metric])))].sort() : [metric];
  const max = Math.max(
    1,
    ...history.map((point) => (distributions ? Math.max(0, ...Object.values(point[metric])) : point[metric])),
  );
  const start = history[0].simMs,
    span = Math.max(1, history.at(-1).simMs - start);
  const legend = [];
  keys.forEach((key, index) => {
    ctx.strokeStyle = colors[index % colors.length];
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    history.forEach((point, i) => {
      const value = distributions ? point[metric][key] || 0 : point[metric];
      const x = 45 + ((point.simMs - start) / span) * (width - 60),
        y = height - 28 - (value / max) * (height - 45);
      if (i) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
    });
    ctx.stroke();
    const item = document.createElement("span");
    item.textContent = `${index + 1}. ${key}`;
    legend.push(item);
  });
  $("legend").replaceChildren(...legend);
  ctx.fillStyle = "#a1b0bb";
  ctx.fillText(max.toLocaleString(), 4, 16);
  ctx.fillText("0", 4, height - 28);
  ctx.fillText(duration(start), 45, height - 6);
  ctx.fillText(duration(start + span), width - 115, height - 6);
}
setInterval(async () => {
  if (state && performance.now() - freshAt > 3000) notice("STALE: no new world snapshot for over 3 seconds.");
  if (!token) return;
  try {
    events = await (await api("/api/events")).json();
    renderDetails();
  } catch {
    /* freshness is shown above */
  }
}, 2000);
window.addEventListener("resize", drawChart);
drawMap();

// Canvas drawing helpers. Every function draws into an already-sized 2D context and returns layout data.
// Colours come from the CSS tokens so the canvases match the page without duplicating hex values.
const TICK_FONT = '11px "Noto Sans", "Segoe UI", system-ui, sans-serif';
const LABEL_FONT = '12px "Noto Sans", "Segoe UI", system-ui, sans-serif';
let cached = null;

export function tokens() {
  if (cached) return cached;
  const style = getComputedStyle(document.documentElement);
  const read = (name, fallback) => style.getPropertyValue(name).trim() || fallback;
  cached = {
    ground: read("--ground", "#0f171b"),
    bench: read("--bench", "#18232a"),
    rule: read("--rule", "#2a3940"),
    parchment: read("--parchment", "#e9dfc7"),
    ash: read("--ash", "#93a3ab"),
    brass: read("--brass", "#d4a248"),
    moss: read("--moss", "#79c39c"),
    ember: read("--ember", "#e0655c"),
    sky: read("--sky", "#6cb0dc"),
    lilac: read("--lilac", "#b49cd8"),
    dead: read("--dead", "#5b2a31"),
  };
  return cached;
}

export function surface(element) {
  const { width, height } = element.getBoundingClientRect(),
    ratio = window.devicePixelRatio || 1;
  if (element.width !== Math.round(width * ratio) || element.height !== Math.round(height * ratio)) {
    element.width = Math.round(width * ratio);
    element.height = Math.round(height * ratio);
  }
  const ctx = element.getContext("2d");
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.font = TICK_FONT;
  return { ctx, width, height };
}

// Step size with a 1, 2, 2.5 or 5 mantissa, so axis labels land on round numbers.
export function niceStep(range, ticks = 4) {
  const raw = Math.max(1e-9, range) / ticks;
  const magnitude = 10 ** Math.floor(Math.log10(raw));
  const scaled = raw / magnitude;
  const mantissa = scaled <= 1 ? 1 : scaled <= 2 ? 2 : scaled <= 2.5 ? 2.5 : scaled <= 5 ? 5 : 10;
  return mantissa * magnitude;
}

// Axis bounds with 8% headroom so a rising line never sits on the top gridline.
export function axisBounds(low, high, ticks = 4) {
  const floor = Math.min(low, high);
  const span = Math.max(high - floor, Number.EPSILON);
  const step = niceStep(span * 1.08, ticks);
  const bottom = Math.floor(floor / step) * step;
  const top = Math.max(bottom + step, Math.ceil((floor + span * 1.08) / step) * step);
  return { bottom, top, step };
}

export function compactNumber(value) {
  if (value == null || !Number.isFinite(value)) return "";
  const abs = Math.abs(value);
  if (abs >= 1e6) return `${(value / 1e6).toFixed(abs >= 1e7 ? 0 : 1)}M`;
  if (abs >= 1e4) return `${(value / 1e3).toFixed(abs >= 1e5 ? 0 : 1)}k`;
  return value.toLocaleString(undefined, { maximumFractionDigits: Number.isInteger(value) ? 0 : 1 });
}

function hairline(ctx, x1, y1, x2, y2) {
  ctx.beginPath();
  ctx.moveTo(x1, Math.round(y1) + 0.5);
  ctx.lineTo(x2, Math.round(y2) + 0.5);
  ctx.stroke();
}

export function sparkline(ctx, width, height, values, color, { max, baseline = true } = {}) {
  const points = values.filter((value) => value != null && Number.isFinite(value));
  if (points.length < 2) return;
  const palette = tokens();
  const top = max ?? Math.max(...points),
    bottom = Math.min(0, ...points);
  const span = Math.max(1e-9, top - bottom);
  if (baseline) {
    ctx.strokeStyle = palette.ash;
    ctx.globalAlpha = 0.3;
    ctx.lineWidth = 1;
    hairline(ctx, 0, height - 1, width, height - 1);
    ctx.globalAlpha = 1;
  }
  ctx.beginPath();
  values.forEach((value, index) => {
    if (value == null) return;
    const x = (index / Math.max(1, values.length - 1)) * (width - 2) + 1;
    const y = height - 2 - ((value - bottom) / span) * (height - 4);
    if (index === 0 || values[index - 1] == null) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.5;
  ctx.lineJoin = "round";
  ctx.stroke();
}

// Linear achieved-versus-requested speed instrument. `ratio` is achieved / requested; a tick marks 1.0.
export function speedBar(ctx, width, height, ratio, { color, requested = "", paused = false, empty = false } = {}) {
  const palette = tokens();
  const trackY = 6,
    trackHeight = 6,
    fullAt = width * 0.8;
  ctx.fillStyle = palette.rule;
  ctx.fillRect(0, trackY, width, trackHeight);
  if (!empty) {
    const fill = Math.min(ratio, 1.2) * fullAt;
    ctx.fillStyle = paused ? palette.ash : color;
    ctx.fillRect(0, trackY, Math.max(0, fill), trackHeight);
    ctx.fillStyle = palette.parchment;
    ctx.fillRect(Math.round(fullAt) - 1, trackY - 3, 2, trackHeight + 6);
    if (requested) {
      ctx.fillStyle = palette.ash;
      ctx.textAlign = "center";
      ctx.fillText(requested, fullAt, height - 1);
      ctx.textAlign = "left";
    }
  }
}

// Stacked area of shares over x. Series are stacked bottom to top in the order given.
export function stackedArea(ctx, width, height, series, { formatX } = {}) {
  const palette = tokens();
  const left = 40,
    right = width - 8,
    top = 8,
    bottom = height - 22;
  const count = series[0]?.points.length || 0;
  if (count < 2) return;
  const start = series[0].points[0].x,
    span = Math.max(1, series[0].points.at(-1).x - start);
  const xToPx = (x) => left + ((x - start) / span) * (right - left);
  const totals = series[0].points.map((_, index) => series.reduce((sum, line) => sum + (line.points[index].y || 0), 0));
  const below = new Array(count).fill(0);
  for (const line of series) {
    const upper = line.points.map((point, index) => below[index] + (point.y || 0) / Math.max(1, totals[index]));
    ctx.beginPath();
    upper.forEach((value, index) => {
      const x = xToPx(line.points[index].x),
        y = bottom - value * (bottom - top);
      if (index) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
    });
    for (let index = count - 1; index >= 0; index -= 1)
      ctx.lineTo(xToPx(line.points[index].x), bottom - below[index] * (bottom - top));
    ctx.closePath();
    ctx.globalAlpha = line.alpha ?? 0.7;
    ctx.fillStyle = line.color;
    ctx.fill();
    ctx.globalAlpha = 1;
    ctx.strokeStyle = palette.ground;
    ctx.lineWidth = 1;
    ctx.beginPath();
    upper.forEach((value, index) => {
      const x = xToPx(line.points[index].x),
        y = bottom - value * (bottom - top);
      if (index) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
    });
    ctx.stroke();
    upper.forEach((value, index) => {
      below[index] = value;
    });
  }
  ctx.strokeStyle = palette.rule;
  ctx.fillStyle = palette.ash;
  ctx.textAlign = "right";
  for (const tick of [0, 0.5, 1]) {
    const y = bottom - tick * (bottom - top);
    ctx.globalAlpha = tick === 0 ? 1 : 0.6;
    hairline(ctx, left, y, right, y);
    ctx.globalAlpha = 1;
    ctx.fillText(`${tick * 100}%`, left - 6, y + 4);
  }
  if (formatX) {
    ctx.textAlign = "left";
    ctx.fillText(formatX(start), left, height - 6);
    ctx.textAlign = "right";
    ctx.fillText(formatX(start + span), right, height - 6);
  }
  ctx.textAlign = "left";
}

// Multi-series line chart with a shared x axis. Returns a layout for hover mapping.
// `band` is an optional { low, high } pair of point arrays filled beneath the lines.
export function lineChart(ctx, width, height, series, options = {}) {
  const palette = tokens();
  const { formatX, formatY = compactNumber, hover = null, yMax, yMin = 0, xTicks = 5, band = null } = options;
  const left = 56,
    right = width - 12,
    top = 12,
    bottom = height - 24;
  const values = series.flatMap((line) => line.points.map((point) => point.y)).filter((y) => y != null);
  if (band) values.push(...band.low.map((point) => point.y), ...band.high.map((point) => point.y));
  const xs = series.flatMap((line) => line.points.map((point) => point.x));
  const finite = values.filter((value) => Number.isFinite(value));
  const rawMax = yMax ?? (finite.length ? Math.max(0, ...finite) : 1);
  const rawMin = yMin ?? (finite.length ? Math.min(0, ...finite) : 0);
  const bounds = yMax != null ? { bottom: rawMin, top: yMax, step: (yMax - rawMin) / 4 } : axisBounds(rawMin, rawMax);
  const floor = bounds.bottom,
    max = bounds.top;
  const start = Math.min(...xs),
    end = Math.max(...xs),
    span = Math.max(1, end - start);
  const xToPx = (x) => left + ((x - start) / span) * (right - left);
  const yToPx = (y) => bottom - ((y - floor) / (max - floor)) * (bottom - top);
  const clamp = (y) => yToPx(Math.max(floor, Math.min(y, max)));
  ctx.lineWidth = 1;
  ctx.fillStyle = palette.ash;
  ctx.textAlign = "right";
  for (let tickValue = floor; tickValue <= max + bounds.step / 2; tickValue += bounds.step) {
    const y = yToPx(tickValue);
    ctx.strokeStyle = palette.rule;
    ctx.globalAlpha = tickValue === floor ? 1 : 0.6;
    hairline(ctx, left, y, right, y);
    ctx.globalAlpha = 1;
    ctx.fillText(formatY(tickValue), left - 8, y + 4);
  }
  ctx.strokeStyle = palette.rule;
  for (let tick = 0; tick <= xTicks; tick += 1) {
    const x = left + ((right - left) * tick) / xTicks;
    ctx.beginPath();
    ctx.moveTo(Math.round(x) + 0.5, bottom);
    ctx.lineTo(Math.round(x) + 0.5, bottom + 4);
    ctx.stroke();
    ctx.textAlign = tick === 0 ? "left" : tick === xTicks ? "right" : "center";
    if (formatX) ctx.fillText(formatX(start + (span * tick) / xTicks), x, height - 7);
  }
  ctx.textAlign = "left";
  if (band && band.low.length > 1) {
    ctx.beginPath();
    band.high.forEach((point, index) => {
      const x = xToPx(point.x),
        y = clamp(point.y ?? floor);
      if (index) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
    });
    for (let index = band.low.length - 1; index >= 0; index -= 1)
      ctx.lineTo(xToPx(band.low[index].x), clamp(band.low[index].y ?? floor));
    ctx.closePath();
    ctx.fillStyle = band.color || palette.parchment;
    ctx.globalAlpha = band.alpha ?? 0.12;
    ctx.fill();
    ctx.globalAlpha = 1;
  }
  for (const line of series) {
    ctx.strokeStyle = line.color;
    ctx.lineWidth = line.width ?? 1.25;
    ctx.setLineDash(line.dashed ? [4, 4] : []);
    ctx.lineJoin = "round";
    ctx.beginPath();
    let connected = false;
    for (const point of line.points) {
      if (point.y == null || !Number.isFinite(point.y)) {
        connected = false;
        continue;
      }
      const x = xToPx(point.x),
        y = clamp(point.y);
      if (connected) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
      connected = true;
    }
    ctx.stroke();
  }
  ctx.setLineDash([]);
  let readings = null;
  if (hover != null && hover >= left && hover <= right) {
    const at = start + ((hover - left) / (right - left)) * span;
    ctx.strokeStyle = palette.parchment;
    ctx.globalAlpha = 0.4;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(Math.round(hover) + 0.5, top);
    ctx.lineTo(Math.round(hover) + 0.5, bottom);
    ctx.stroke();
    ctx.globalAlpha = 1;
    readings = { x: at, values: [] };
    for (const line of series) {
      const point = nearest(line.points, at);
      if (!point || point.y == null) continue;
      readings.values.push({ name: line.name, color: line.color, y: point.y, x: point.x });
      ctx.beginPath();
      ctx.arc(xToPx(point.x), clamp(point.y), 3, 0, 2 * Math.PI);
      ctx.fillStyle = line.color;
      ctx.fill();
      ctx.strokeStyle = palette.ground;
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }
  }
  return { left, right, top, bottom, start, end, max, floor, readings };
}

function nearest(points, x) {
  let low = 0,
    high = points.length - 1;
  if (high < 0) return null;
  while (low < high) {
    const middle = (low + high) >> 1;
    if (points[middle].x < x) low = middle + 1;
    else high = middle;
  }
  const candidate = points[low],
    previous = points[low - 1];
  return previous && Math.abs(previous.x - x) < Math.abs(candidate.x - x) ? previous : candidate;
}

// Text helper for canvases that need a label font rather than the tick font.
export function labelFont(ctx) {
  ctx.font = LABEL_FONT;
}

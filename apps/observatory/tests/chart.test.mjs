import test from "node:test";
import assert from "node:assert/strict";

// charts.js reads its palette from the page's CSS tokens; without a document it falls back to the defaults,
// which is all these tests need. The stubs must be in place before the module is imported.
globalThis.document = { documentElement: {} };
globalThis.getComputedStyle = () => ({ getPropertyValue: () => "" });
const { lineChart, niceStep, axisBounds } = await import("../web/charts.js");

// A recording 2D context: enough of the interface for lineChart, and a record of what was drawn.
function recorder() {
  const calls = { labels: [], arcs: [], lines: 0 };
  return {
    calls,
    fillStyle: "",
    strokeStyle: "",
    lineWidth: 1,
    lineJoin: "",
    globalAlpha: 1,
    textAlign: "",
    font: "",
    beginPath() {},
    closePath() {},
    moveTo() {},
    lineTo() {
      calls.lines += 1;
    },
    stroke() {},
    fill() {},
    setLineDash() {},
    arc(x, y, radius) {
      calls.arcs.push({ x, y, radius });
    },
    fillText(text, x) {
      calls.labels.push({ text, x });
    },
  };
}

const clock = (ms) => new Date(ms).toISOString().slice(11, 19);

test("a run with one long-term bucket carries one axis label, not six identical ones", () => {
  const ctx = recorder();
  lineChart(ctx, 600, 200, [{ name: "XP", color: "#fff", points: [{ x: 231000, y: 296 }] }], { formatX: clock });
  const xLabels = ctx.calls.labels.filter((label) => label.text.includes(":"));
  assert.equal(xLabels.length, 1);
  assert.equal(xLabels[0].text, "00:03:51");
});

test("a lone point is drawn as a dot, since a single sample strokes no line", () => {
  const ctx = recorder();
  const layout = lineChart(ctx, 600, 200, [{ name: "XP", color: "#fff", points: [{ x: 231000, y: 296 }] }], {
    formatX: clock,
  });
  assert.equal(ctx.calls.arcs.length, 1);
  // With no x range to map, the point sits in the middle of the plot rather than pinned to the left edge.
  assert.equal(ctx.calls.arcs[0].x, (layout.left + layout.right) / 2);
});

test("a gap between two samples leaves each side visible", () => {
  const ctx = recorder();
  const points = [{ x: 0, y: 1 }, { x: 1000, y: null }, { x: 2000, y: 3 }];
  lineChart(ctx, 600, 200, [{ name: "XP", color: "#fff", points }], {});
  assert.equal(ctx.calls.arcs.length, 2);
});

test("a window too short to separate ticks does not repeat the same label", () => {
  const ctx = recorder();
  const points = [
    { x: 1000, y: 1 },
    { x: 1400, y: 2 },
  ];
  lineChart(ctx, 600, 200, [{ name: "XP", color: "#fff", points }], { formatX: clock });
  const texts = ctx.calls.labels.filter((label) => label.text.includes(":")).map((label) => label.text);
  assert.deepEqual(texts, [...new Set(texts)]);
});

test("a run with a real range keeps its full set of axis labels", () => {
  const ctx = recorder();
  const points = Array.from({ length: 6 }, (unused, index) => ({ x: index * 600000, y: index * 10 }));
  lineChart(ctx, 600, 200, [{ name: "XP", color: "#fff", points }], { formatX: clock });
  const texts = ctx.calls.labels.filter((label) => label.text.includes(":")).map((label) => label.text);
  assert.equal(texts.length, 6);
  assert.equal(new Set(texts).size, 6);
  assert.equal(ctx.calls.arcs.length, 0);
});

test("axis steps land on round numbers", () => {
  assert.equal(niceStep(100), 25);
  assert.equal(axisBounds(0, 96).top >= 96, true);
});

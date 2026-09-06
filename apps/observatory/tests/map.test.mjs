import test from "node:test";
import assert from "node:assert/strict";
import { visibleBots, chooseMap, mapView, mapTile, mapRect } from "../web/model.js";

test("map corners and cropped edge tiles stay aligned with world coordinates", () => {
  const area = { x1: 668, x2: 0, y1: 1002, y2: 0, width: 1002, height: 668, tileSize: 256 };
  const view = { ...mapView(area), scale: 1 };
  const first = mapTile(area, 0, view, 1002, 668);
  const last = mapTile(area, 11, view, 1002, 668);
  assert.equal(first.x, 0);
  assert.equal(first.y, 0);
  assert.equal(last.x + last.width, 1002);
  assert.equal(last.y + last.height, 668);
  assert.equal(last.cropX, 234 / 256);
  assert.equal(last.cropY, 156 / 256);
  const detail = mapRect(area, 515, 279, 256, 256, view, 1002, 668);
  for (const [key, value] of Object.entries({ x: 515, y: 279, width: 256, height: 256 }))
    assert.ok(Math.abs(detail[key] - value) < 1e-9);
});

test("continent artwork remains selectable without resident bots", () => {
  const east = { map: 0, zone: 0, name: "Azeroth" };
  const west = { map: 1, zone: 0, name: "Kalimdor" };
  assert.equal(chooseMap([east, west], "1", "all", [], ""), west);
  assert.equal(chooseMap([east, west], "0", "0", [], ""), east);
});

test("draenei islands use their zone artwork on the shared Outland map", () => {
  const outland = { map: 530, zone: 0 };
  const island = { map: 530, zone: 3524 };
  const bot = { id: "8", map: 530, zone: 3524 };
  assert.equal(chooseMap([outland, island], "530", "all", [bot], "8"), island);
  assert.equal(chooseMap([outland, island], "530", "0", [bot], "8"), outland);
  assert.equal(visibleBots({ bots: [bot] }, "530", "0").length, 1);
});

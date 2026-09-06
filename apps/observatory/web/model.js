export function duration(ms) {
  const seconds = Math.floor(ms / 1000);
  return `${Math.floor(seconds / 3600)}h ${Math.floor(seconds / 60) % 60}m ${seconds % 60}s`;
}

export function summarize(snapshot) {
  const result = { simMs: snapshot.simMs, xp: 0, quests: 0, deaths: 0, levels: {}, zones: {} };
  for (const bot of snapshot.bots) {
    result.xp += bot.earnedXp ?? 0;
    result.quests += bot.questCompletions ?? 0;
    result.deaths += bot.deaths;
    const level = `Level ${bot.level}`;
    const zone = `${bot.map}/${bot.zone}`;
    result.levels[level] = (result.levels[level] || 0) + 1;
    result.zones[zone] = (result.zones[zone] || 0) + 1;
  }
  if (snapshot.runTotals) Object.assign(result, snapshot.runTotals);
  if (snapshot.source === "python-api") {
    result.xp = result.quests = null;
    result.observed = snapshot.bots.length;
    result.combat = snapshot.bots.filter((bot) => bot.combat).length;
    result.alive = snapshot.bots.filter((bot) => bot.alive).length;
    result.health = snapshot.bots.length
      ? snapshot.bots.reduce((sum, bot) => sum + (100 * bot.health) / Math.max(1, bot.maxHealth), 0) /
        snapshot.bots.length
      : null;
  }
  return result;
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

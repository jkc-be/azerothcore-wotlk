export function duration(ms) {
  const seconds = Math.floor(ms / 1000);
  return `${Math.floor(seconds / 3600)}h ${Math.floor(seconds / 60) % 60}m ${seconds % 60}s`;
}

export function summarize(snapshot) {
  const result = { simMs: snapshot.simMs, xp: 0, quests: 0, deaths: 0, levels: {}, zones: {} };
  for (const bot of snapshot.bots) {
    result.xp += bot.earnedXp;
    result.quests += bot.questCompletions;
    result.deaths += bot.deaths;
    const level = `Level ${bot.level}`;
    const zone = `${bot.map}/${bot.zone}`;
    result.levels[level] = (result.levels[level] || 0) + 1;
    result.zones[zone] = (result.zones[zone] || 0) + 1;
  }
  return result;
}

export function visibleBots(snapshot, map, zone) {
  return snapshot.bots.filter(bot => String(bot.map) === map && (zone === 'all' || String(bot.zone) === zone));
}

export function worldToScreen(bot, view, width, height) {
  return { x: width / 2 - (bot.y - view.y) * view.scale, y: height / 2 - (bot.x - view.x) * view.scale };
}

export function fitView(bots) {
  if (!bots.length) return { x: 0, y: 0, scale: 0.03 };
  const xs = bots.map(bot => bot.x), ys = bots.map(bot => bot.y);
  return { x: (Math.min(...xs) + Math.max(...xs)) / 2, y: (Math.min(...ys) + Math.max(...ys)) / 2,
    spanX: Math.max(100, Math.max(...xs) - Math.min(...xs)),
    spanY: Math.max(100, Math.max(...ys) - Math.min(...ys)) };
}

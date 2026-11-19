#!/usr/bin/env node
// Sorts out the lap records driven with catch-up on before the runtime told (runtime/game.c,
// flag 32 in web/laps.mjs). The game turns catch-up on whenever Game Setup leaves one player, so
// every online race had it, and trailing players drove laps no one can alone. Each lap gets
// flag 32 when:
//   - its race's replay is on the server (build/replays): the race header says so (+0x10 race
//     mode: 0 one player, 1 split screen, 4 network; +0x18 catch-up), or else
//   - the page log of its session (build/clientlogs) shows the lap in an online game: after
//     "net: joined room" (a game started in a room only reports laps of online races).
// Laps it can't place stay as they are. Nothing is deleted: the laps move to the catch-up & mods
// boards.
//
//   node tools/catchup-laps.mjs [--data DIR]           what would change
//   node tools/catchup-laps.mjs [--data DIR] --write   change it (stop serve.mjs first: it
//                                                      reads the laps when it starts; the old
//                                                      files are kept as *.bak-catchup)
import { readdirSync, readFileSync, renameSync, writeFileSync, copyFileSync, existsSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
const di = args.indexOf('--data');
const data = di >= 0 ? resolve(args[di + 1]) : resolve(dirname(fileURLToPath(import.meta.url)), '..', 'build');
const write = args.includes('--write');
const CU = 32;
const lapTime = (t) => `${Math.floor(t / 3840)}:${String(Math.floor(t / 64) % 60).padStart(2, '0')}.${String(Math.floor((t % 64) * 100 / 64)).padStart(2, '0')}`;

// replays: race id -> catch-up on?
const replayDir = join(data, 'replays'), replayCache = new Map();
function fromReplay(race) {
  if (!race) return undefined;
  if (replayCache.has(race)) return replayCache.get(race);
  let v;
  const f = join(replayDir, race + '.rpy.gz');
  if (existsSync(f)) {
    try { const b = gunzipSync(readFileSync(f)); if (b.length >= 0x1c) v = b.readUInt32LE(0x18) !== 0 && b.readUInt32LE(0x10) >= 1; } catch {}
  }
  replayCache.set(race, v);
  return v;
}

// page logs: "track:lap/laps:time:car" -> [{ t, online }]
const logs = new Map();
const logDir = join(data, 'clientlogs');
let nLogs = 0;
for (const name of existsSync(logDir) ? readdirSync(logDir) : []) {
  const m = /^(\d{4})(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)-.*\.log$/.exec(name);
  if (!m) continue;
  const start = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
  let text; try { text = readFileSync(join(logDir, name), 'utf8'); } catch { continue; }
  if (!text.includes('game: lap ')) continue;
  nLogs++;
  let online = false;
  for (const line of text.split('\n')) {
    if (line.includes('net: joined room as node')) online = true;
    else if (line.includes('net: relay connection lost')) online = false;
    const l = /^\s*([\d.]+)\s.*game: lap (\d+)\/(\d+) (\d+:\d\d\.\d\d) \(track (\d+), flags (0x[0-9a-f]+|0), car (\S+)\)/.exec(line);
    if (!l) continue;
    const key = `${l[5]}:${l[2]}/${l[3]}:${l[4]}:${l[7].toLowerCase()}`;
    if (!logs.has(key)) logs.set(key, []);
    logs.get(key).push({ t: start + Number(l[1]) * 1000, online: online || (Number(l[6]) & CU) !== 0 });
  }
}
function fromLogs(r) {
  const c = logs.get(`${r.track}:${r.lap}/${r.laps}:${lapTime(r.ticks)}:${r.car}`);
  if (!c) return undefined;
  const near = c.filter((e) => Math.abs(e.t - r.t) < 180e3);
  if (!near.length) return undefined;
  return near.some((e) => e.online);
}

const lapDir = join(data, 'laps');
const stat = { laps: 0, already: 0, replay: 0, log: 0, offline: 0, unknown: 0 };
const moved = [];
for (const f of readdirSync(lapDir).filter((f) => /^laps-.*\.jsonl$/.test(f)).sort()) {
  const path = join(lapDir, f);
  const out = [];
  let changed = 0;
  for (const line of readFileSync(path, 'utf8').split('\n')) {
    if (!line) { out.push(line); continue; }
    let r; try { r = JSON.parse(line); } catch { out.push(line); continue; }
    stat.laps++;
    if (r.f & CU) { stat.already++; out.push(line); continue; }
    let cu = fromReplay(r.race), how = 'replay';
    if (cu === undefined) { cu = fromLogs(r); how = 'log'; }
    if (cu === undefined) stat.unknown++;
    else if (!cu) stat.offline++;
    else {
      stat[how]++; changed++;
      moved.push(r);
      r.f |= CU;
      out.push(JSON.stringify(r)); continue;
    }
    out.push(line);
  }
  if (write && changed) {
    copyFileSync(path, path + '.bak-catchup');
    writeFileSync(path + '.tmp', out.join('\n'));
    renameSync(path + '.tmp', path);
    console.log(`${f}: ${changed} laps flagged (old file: ${f}.bak-catchup)`);
  }
}

console.log(`${nLogs} page logs with laps, ${replayCache.size} replays looked at`);
console.log(`${stat.laps} laps: ${stat.replay + stat.log} with catch-up (${stat.replay} by their replay, ${stat.log} by the page log), ${stat.offline} without, ${stat.unknown} unknown, ${stat.already} flagged already`);
// the best of them per track and direction, as on the boards
const best = new Map();
for (const r of moved) {
  const k = `${r.track}:${r.f & 1 ? 'b' : 'f'}`, o = r.p || 'n:' + String(r.n).toLowerCase();
  if (!best.has(k)) best.set(k, new Map());
  const m = best.get(k);
  if (!m.has(o) || r.ticks < m.get(o).ticks) m.set(o, r);
}
for (const [k, m] of [...best].sort()) {
  const top = [...m.values()].sort((a, b) => a.ticks - b.ticks).slice(0, 5).map((r) => `${r.n} ${lapTime(r.ticks)} ${r.car}`);
  console.log(`  track ${k}: ${m.size} racers, fastest ${top.join(', ')}`);
}
if (!write) console.log('nothing written (add --write; stop serve.mjs first)');

// Replays for the lap records: the game's own replay of the race a best lap was driven in.
//
// At the end of every race (finished or quit) the game writes replay.rpy in its folder
// (f_0046E970): always 0x1e5b4 bytes, starting with a copy of the race config (0x5b0 bytes from
// 0x50ffc8: laps, track, direction, mirrored, the racers and their cars), then the recording.
// The page (main.js watchReplay) sees the file change after a race whose laps it sent, and
// uploads it here with the player key: POST /__replay?race=<id>. It is taken only when that
// race's laps are this player's and the file's settings are theirs, and kept gzipped in
// build/replays/<race>.rpy.gz. GET /__replay/<race>.rpy gives it back; /__laps links it on
// that race's laps (laps.mjs). The game plays it from FEDATA\PC\SAVE\<name>.rpy (Options >
// Load/Save > View Replay), which is where the start page puts it.
// Replays of races with no best lap on any board are deleted after an hour.
import { gunzipSync, gzipSync } from 'node:zlib';
import { mkdirSync, readdirSync, readFileSync, renameSync, statSync, unlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { clientIp, foreignOrigin } from './relay.mjs';
import { TOKEN } from './players.mjs';

export const REPLAY_SIZE = 0x1e5b4;
// race settings in the config copy at the start of the file (game.c's RACE_* - 0x50ffc8); the
// writer sets +0x1c (the race's mode) to 2, a replay
const AT = { laps: 0x04, track: 0x24, backward: 0x30, mirrored: 0x34 };
const FLAG = { backward: 1, mirrored: 2 };
const MODE = 0x1c, MODE_REPLAY = 2;
const PER_HOUR = 30;
const ID = /^[a-z0-9]{8,16}$/;

// the race a replay is of, from its config copy (null when it isn't one of the game's replays)
export function replayInfo(buf) {
  if (buf.length !== REPLAY_SIZE || buf.readUInt32LE(MODE) !== MODE_REPLAY) return null;
  const u = (k) => buf.readUInt32LE(AT[k]);
  let f = 0;
  for (const k of Object.keys(FLAG)) if (u(k)) f |= FLAG[k];
  return { track: u('track'), flags: f, laps: u('laps') };
}

export function createReplays(dir, { players, laps }) {
  mkdirSync(dir, { recursive: true });
  const have = new Set(readdirSync(dir).filter((f) => /^[a-z0-9]+\.rpy\.gz$/.test(f)).map((f) => f.split('.')[0]));
  laps.hasReplay = (id) => have.has(id);
  const file = (id) => join(dir, `${id}.rpy.gz`);

  const recent = new Map();             // ip -> [uploads, hour]
  function send(res, code, o) {
    res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(o));
  }
  function handlePost(req, res, url) {
    if (foreignOrigin(req)) { res.writeHead(403).end(); req.resume(); return; }
    const ip = clientIp(req), hour = Math.floor(Date.now() / 3600e3), c = recent.get(ip);
    if (c && c[1] === hour && c[0] >= PER_HOUR) { req.resume(); send(res, 429, { error: 'busy' }); return; }
    if (recent.size > 10000) recent.clear();
    recent.set(ip, [c && c[1] === hour ? c[0] + 1 : 1, hour]);
    const race = url.searchParams.get('race') || '', key = String(req.headers['x-player-key'] || '');
    const chunks = []; let size = 0;
    req.on('data', (d) => { size += d.length; if (size > REPLAY_SIZE) req.destroy(); else chunks.push(d); });
    req.on('end', () => {
      if (!ID.test(race) || !TOKEN.test(key)) { send(res, 400, { error: 'bad' }); return; }
      const pl = players.byToken(key);
      if (!pl) { send(res, 404, { error: 'unknown' }); return; }
      const buf = Buffer.concat(chunks), info = replayInfo(buf);
      if (!info) { send(res, 400, { error: 'not a replay' }); return; }
      const own = laps.lapsOfRace(race);
      if (!own.length || own.some((r) => !r.p || players.resolve(r.p) !== pl.p)) { send(res, 404, { error: 'no such race' }); return; }
      const r = own[0];
      if (info.track !== r.track || info.flags !== (r.f & 3) || info.laps !== r.laps) { send(res, 409, { error: 'another race', info }); return; }
      if (have.has(race)) { send(res, 200, { ok: true, had: true }); return; }
      try {
        writeFileSync(file(race) + '.tmp', gzipSync(buf, { level: 9 }));
        renameSync(file(race) + '.tmp', file(race));
      } catch (e) { console.log('[replays] write failed:', e.message); send(res, 500, { error: 'write' }); return; }
      have.add(race); laps.changed();
      console.log(`[replays] ${pl.n} (${pl.p}): race ${race}, track ${r.track}, ${own.length} lap(s)`);
      send(res, 200, { ok: true });
    });
  }
  function handleGet(req, res, url) {
    const id = (/^\/__replay\/([a-z0-9]+)\.rpy$/.exec(url.pathname) || [])[1];
    if (!id || !have.has(id)) { res.writeHead(404).end('not found'); return; }
    let body;
    try { body = gunzipSync(readFileSync(file(id))); } catch { res.writeHead(404).end('not found'); return; }
    res.writeHead(200, {
      'Content-Type': 'application/octet-stream', 'Content-Length': body.length, 'Cache-Control': 'public, max-age=86400',
      'Content-Disposition': `attachment; filename="nfs2se-${id}.rpy"`, 'Cross-Origin-Resource-Policy': 'same-origin',
    });
    res.end(req.method === 'HEAD' ? undefined : body);
  }
  // replays no board needs: gone an hour after they came (a race's laps all arrive before it)
  function prune() {
    const keep = laps.bestRaces();
    for (const id of [...have]) {
      if (keep.has(id)) continue;
      try { if (Date.now() - statSync(file(id)).mtimeMs < 3600e3) continue; unlinkSync(file(id)); } catch {}
      have.delete(id);
    }
    laps.changed();
  }
  setInterval(prune, 3600e3).unref();
  return { handlePost, handleGet, prune, has: (id) => have.has(id) };
}

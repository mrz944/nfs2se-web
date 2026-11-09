// Renders the share images from docs/og-card.html with headless Chromium over CDP:
//   web/icons/og.png         1200x630, the page's og:image
//   docs/social-preview.png  1280x640, GitHub social preview (Settings > Social preview)
// node tools/og.mjs [outdir]   (default: into the repo)   BROWSER=chromium to pick another browser
import { spawn } from 'node:child_process';
import { writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..'), out = process.argv[2];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const prof = mkdtempSync(join(tmpdir(), 'og-')), port = 9350;
const br = spawn(process.env.BROWSER || 'brave', ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${prof}`, '--no-first-run', '--hide-scrollbars', 'about:blank'], { stdio: 'ignore' });
try {
  let tabs;
  for (let i = 0; i < 50 && !tabs; i++) { try { tabs = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); } catch { await sleep(200); } }
  const ws = new WebSocket(tabs.find((t) => t.type === 'page').webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0; const pend = new Map();
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d.result ?? d.error); pend.delete(d.id); } };
  const send = (method, params = {}) => new Promise((r) => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  await send('Page.enable');
  for (const [w, h, file] of [[1200, 630, out ? join(out, 'og.png') : join(root, 'web/icons/og.png')],
                              [1280, 640, out ? join(out, 'social-preview.png') : join(root, 'docs/social-preview.png')]]) {
    await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: `file://${root}/docs/og-card.html?w=${w}&h=${h}` });
    for (let i = 0; i < 100; i++) {
      await sleep(100);
      if ((await send('Runtime.evaluate', { expression: 'document.title', returnByValue: true }))?.result?.value === 'ready') break;
    }
    await sleep(200);
    const r = await send('Page.captureScreenshot', { format: 'png', clip: { x: 0, y: 0, width: w, height: h, scale: 1 } });
    writeFileSync(file, Buffer.from(r.data, 'base64'));
    console.log(file);
  }
} finally { br.kill(); await sleep(300); rmSync(prof, { recursive: true, force: true }); }
process.exit(0);

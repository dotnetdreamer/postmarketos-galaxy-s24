// drive.mjs: an Edge kept open and driven step by step over HTTP on loopback, for testing
// s24-remote's page through the real tunnel or the Wi-Fi door (Claude, 2026-11-19). Input goes in as
// CDP Input events: trusted events, through the page's own handlers. Headless unless `visible` is
// given: then a real window on the screen, at the size asked for, that someone can watch.
//
//   node drive.mjs <edge exe> <work dir> <control port> [WxH] [visible]
//   /open?url=  /state  /code?c=  /click?x=&y=&b=&n=  /move?x=&y=  /drag?x1&y1&x2&y2&steps
//   /type?text=  /key?key=&code=&vk=&mod=  /wheel?x=&y=&dy=&dx=  /js?e=  /shot?name=
//   /size?w=&h=  /grant  /errors  /quit  /proceed (past a certificate warning)  /init?js=
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';

const [exe, work, ctlPortArg, sizeArg, modeArg] = process.argv.slice(2);
const visible = modeArg === 'visible';
const [W, H] = (sizeArg || '1600x900').split('x').map(Number);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((resolve) => {
  const s = net.createServer();
  s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); });
});
fs.mkdirSync(path.join(work, 'shots'), { recursive: true });

const cdpPort = await freePort();
const profile = path.join(work, `profile-${cdpPort}`);
const proc = spawn(exe, [...(visible ? [] : ['--headless=new']), `--remote-debugging-port=${cdpPort}`, `--user-data-dir=${profile}`,
  '--no-first-run', '--no-default-browser-check', '--disable-extensions', `--window-size=${W},${H}`, '--window-position=40,40',
  'about:blank'], { stdio: 'ignore' });
function killBrowser() {
  try { proc.kill(); } catch { /* gone */ }
  spawnSync('powershell', ['-NoProfile', '-Command',
    `Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*profile-${cdpPort}*' -and $_.Name -match 'msedge' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }`]);
}
process.on('exit', killBrowser);

let target = null;
for (let i = 0; i < 100 && !target; i++) {
  try { target = (await (await fetch(`http://127.0.0.1:${cdpPort}/json`)).json()).find((t) => t.type === 'page'); } catch { /* not yet */ }
  if (!target) await sleep(200);
}
if (!target) { console.log('NO BROWSER'); process.exit(1); }
const sock = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => (sock.onopen = r));
let id = 0;
const pending = new Map();
const errors = [];
const consoleLines = [];
sock.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  else if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  else if (m.method === 'Runtime.consoleAPICalled') consoleLines.push(m.params.type + ': ' + m.params.args.map((a) => a.value ?? a.description).join(' '));
};
const cdp = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); sock.send(JSON.stringify({ id: i, method, params })); });
const js = async (expression) => {
  const m = await cdp('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
  if (m.result && m.result.exceptionDetails) return { error: m.result.exceptionDetails.exception?.description || m.result.exceptionDetails.text };
  return m.result ? m.result.result.value : undefined;
};
await cdp('Page.enable');
await cdp('Runtime.enable');
if (!visible) {
  // a visible window keeps its own size and focus, as a person's would
  await cdp('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
  await cdp('Emulation.setFocusEmulationEnabled', { enabled: true });
}

const STATE = `(() => {
  const $ = (i) => document.getElementById(i);
  const vis = (i) => $(i) && !$(i).hidden;
  const c = $('screen');
  return {
    url: location.href,
    view: vis('signin') ? 'signin' : vis('nope') ? 'nope' : vis('desk') ? 'desk' : '?',
    signinMsg: $('signin-msg') && $('signin-msg').textContent,
    cover: $('cover') && !$('cover').classList.contains('gone') ? $('cover-msg').textContent : null,
    banner: vis('banner') ? $('banner').textContent : null,
    canvas: c ? c.width + 'x' + c.height + ' shown ' + Math.round(c.getBoundingClientRect().width) + 'x' + Math.round(c.getBoundingClientRect().height) : null,
    stats: vis('stats') ? $('stats').textContent : null,
    quality: [...document.querySelectorAll('.seg button')].filter((b) => b.getAttribute('aria-checked') === 'true').map((b) => b.dataset.q)[0],
    phoneScreen: $('b-phone') && $('b-phone').getAttribute('aria-pressed'),
    nope: vis('nope') ? $('nope-why').textContent : null,
  };
})()`;

// The canvas point (in the desktop's pixels) → this window's CSS pixels.
const SCREEN_TO_CSS = (x, y) => `(() => { const c = document.getElementById('screen'); const r = c.getBoundingClientRect();
  return { x: r.left + (${x} + 0.5) * r.width / c.width, y: r.top + (${y} + 0.5) * r.height / c.height }; })()`;

async function mouse(type, x, y, button = 'none', clickCount = 0, buttons = 0) {
  await cdp('Input.dispatchMouseEvent', { type, x, y, button, clickCount, buttons, pointerType: 'mouse' });
}

const BTN = { left: [0, 1], middle: [1, 4], right: [2, 2] };

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x');
  const p = Object.fromEntries(u.searchParams);
  const send = (v) => { res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify(v)); };
  try {
    switch (u.pathname) {
      case '/open':
        await cdp('Page.navigate', { url: p.url });
        await sleep(1500);
        return send(await js(STATE));
      case '/state':
        return send(await js(STATE));
      case '/code': {
        await js(`document.getElementById('code').focus()`);
        await cdp('Input.insertText', { text: p.c });
        await sleep(Number(p.wait || 2500));
        return send(await js(STATE));
      }
      case '/at': {
        // a desktop pixel → CSS point, so tests can aim at the phone's screen coordinates
        return send(await js(SCREEN_TO_CSS(Number(p.x), Number(p.y))));
      }
      case '/move': {
        const pt = p.screen ? await js(SCREEN_TO_CSS(Number(p.x), Number(p.y))) : { x: Number(p.x), y: Number(p.y) };
        await mouse('mouseMoved', pt.x, pt.y);
        return send({ ok: true, at: pt });
      }
      case '/click': {
        const pt = p.screen ? await js(SCREEN_TO_CSS(Number(p.x), Number(p.y))) : { x: Number(p.x), y: Number(p.y) };
        const [, bits] = BTN[p.b || 'left'];
        const n = Number(p.n || 1);
        await mouse('mouseMoved', pt.x, pt.y);
        for (let i = 1; i <= n; i++) {
          await mouse('mousePressed', pt.x, pt.y, p.b || 'left', i, bits);
          await sleep(40);
          await mouse('mouseReleased', pt.x, pt.y, p.b || 'left', i, 0);
          await sleep(60);
        }
        return send({ ok: true, at: pt });
      }
      case '/drag': {
        const a = p.screen ? await js(SCREEN_TO_CSS(Number(p.x1), Number(p.y1))) : { x: Number(p.x1), y: Number(p.y1) };
        const b = p.screen ? await js(SCREEN_TO_CSS(Number(p.x2), Number(p.y2))) : { x: Number(p.x2), y: Number(p.y2) };
        const steps = Number(p.steps || 20);
        await mouse('mouseMoved', a.x, a.y);
        await mouse('mousePressed', a.x, a.y, 'left', 1, 1);
        for (let i = 1; i <= steps; i++) {
          await sleep(Number(p.ms || 30));
          await mouse('mouseMoved', a.x + ((b.x - a.x) * i) / steps, a.y + ((b.y - a.y) * i) / steps, 'left', 0, 1);
        }
        await mouse('mouseReleased', b.x, b.y, 'left', 1, 0);
        return send({ ok: true, from: a, to: b });
      }
      case '/wheel': {
        const pt = p.screen ? await js(SCREEN_TO_CSS(Number(p.x), Number(p.y))) : { x: Number(p.x), y: Number(p.y) };
        await cdp('Input.dispatchMouseEvent', { type: 'mouseWheel', x: pt.x, y: pt.y, deltaX: Number(p.dx || 0), deltaY: Number(p.dy || 0) });
        return send({ ok: true });
      }
      case '/type': {
        for (const ch of p.text) {
          const upper = ch >= 'A' && ch <= 'Z';
          const code = /[a-z]/i.test(ch) ? 'Key' + ch.toUpperCase() : /[0-9]/.test(ch) ? 'Digit' + ch : ch === ' ' ? 'Space' : '';
          const vk = /[a-z0-9]/i.test(ch) ? ch.toUpperCase().charCodeAt(0) : ch === ' ' ? 32 : 0;
          const mods = upper ? 8 : 0;
          if (upper) await cdp('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: 'Shift', code: 'ShiftLeft', windowsVirtualKeyCode: 16, modifiers: 8 });
          await cdp('Input.dispatchKeyEvent', { type: 'keyDown', key: ch, code, text: ch, unmodifiedText: ch.toLowerCase(), windowsVirtualKeyCode: vk, modifiers: mods });
          await cdp('Input.dispatchKeyEvent', { type: 'keyUp', key: ch, code, windowsVirtualKeyCode: vk, modifiers: mods });
          if (upper) await cdp('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Shift', code: 'ShiftLeft', windowsVirtualKeyCode: 16, modifiers: 0 });
          await sleep(Number(p.ms || 25));
        }
        return send({ ok: true });
      }
      case '/key': {
        const base = { key: p.key, code: p.code || p.key, windowsVirtualKeyCode: Number(p.vk || 0), modifiers: Number(p.mod || 0) };
        await cdp('Input.dispatchKeyEvent', { ...base, type: 'rawKeyDown' });
        await sleep(30);
        await cdp('Input.dispatchKeyEvent', { ...base, type: 'keyUp' });
        return send({ ok: true });
      }
      case '/proceed': {
        // Chrome's "Your connection isn't private" page (the Wi-Fi door's own certificate): Advanced,
        // then Continue, as a person would click them
        const before = await js('location.href');
        const clicked = await js(`(() => { const d = document.getElementById('details-button'); if (d) d.click();
          const p = document.getElementById('proceed-link'); if (p) { p.click(); return true; } return false; })()`);
        await sleep(Number(p.wait || 2500));
        return send({ before, clicked, after: await js('location.href'), state: await js(STATE) });
      }
      case '/init': {
        // a script run in every page before its own (Page.addScriptToEvaluateOnNewDocument)
        const r = await cdp('Page.addScriptToEvaluateOnNewDocument', { source: p.js });
        return send({ result: r.result || r.error });
      }
      case '/grant': {
        // clipboard permissions for the page's origin (a real browser asks the person once)
        const origin = new URL(await js('location.href')).origin;
        const r = await cdp('Browser.grantPermissions', { origin, permissions: ['clipboardReadWrite', 'clipboardSanitizedWrite'] });
        return send({ origin, result: r.result || r.error });
      }
      case '/size':
        await cdp('Emulation.setDeviceMetricsOverride', { width: Number(p.w), height: Number(p.h), deviceScaleFactor: 1, mobile: false });
        return send({ ok: true });
      case '/js':
        return send(await js(p.e));
      case '/shot': {
        const m = await cdp('Page.captureScreenshot', { format: 'png' });
        const f = path.join(work, 'shots', (p.name || 'shot') + '.png');
        fs.writeFileSync(f, Buffer.from(m.result.data, 'base64'));
        return send({ file: f });
      }
      case '/errors':
        return send({ errors, console: consoleLines.slice(-40) });
      case '/quit':
        send({ ok: true });
        killBrowser();
        setTimeout(() => process.exit(0), 200);
        return;
      default:
        res.writeHead(404); res.end('?');
    }
  } catch (e) {
    res.writeHead(500); res.end(String(e && e.stack || e));
  }
});
server.listen(Number(ctlPortArg), '127.0.0.1', () => console.log('ready on', ctlPortArg, 'cdp', cdpPort));

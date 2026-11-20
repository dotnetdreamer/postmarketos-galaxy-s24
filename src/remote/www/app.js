// The phone's desktop in this browser.
//
// The phone sends H.264, already in the form every browser's WebCodecs decoder takes; this page
// decodes it on this computer's GPU and draws each frame the moment it leaves the decoder. It
// acknowledges every frame it showed, which is how the phone paces itself to this link. Pointer and
// keys go back the same WebSocket. The pointer is this computer's own, so it moves at the screen's
// rate whatever the video does.

import { keysymFor, charKeysym, isMac } from './keys.js';

const $ = (id) => document.getElementById(id);
const IN_POINTER = 0x10, IN_KEY = 0x11, IN_ACK = 0x12, MSG_VIDEO = 0x01;

const store = {
  get(k, d) { try { return localStorage.getItem(k) ?? d; } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch { /* private window: fine */ } },
};

const s = {
  ws: null,
  decoder: null,
  config: null,
  software: false,
  needKey: true,
  drawn: false,
  quality: ['low', 'medium', 'high'].includes(store.get('quality')) ? store.get('quality') : 'medium',
  phoneScreen: false,
  clip: store.get('clip') === 'on',
  lastClip: null,
  retries: 0,
  bye: null,
  closedByUs: false,
  buttons: 0,
  keys: new Map(),
  wheel: { x: 0, y: 0 },
  frames: 0,
  bytes: 0,
  rtt: 0,
  server: null,
  timers: [],
  lastKeyAsk: 0,
  lastSeq: 0,
  locked: false,
};

const canvas = $('screen');
const ctx = canvas.getContext('2d', { alpha: false, desynchronized: true });

// ── what is on screen ───────────────────────────────────────────────────

function show(id) {
  for (const el of ['signin', 'nope', 'desk']) $(el).hidden = el !== id;
}

function cover(text, button) {
  const c = $('cover');
  c.classList.remove('gone');
  c.classList.toggle('still', !!button);
  $('cover-msg').textContent = text;
  const b = $('cover-retry');
  b.hidden = !button;
  if (button) b.textContent = button;
}

function uncover() {
  $('cover').classList.add('gone');
}

const LOCKED = 'The phone is locked: type its PIN on the lock screen.';
let bannerTimer = 0;
function banner(text, ms) {
  const b = $('banner');
  clearTimeout(bannerTimer);
  b.hidden = !text;
  b.textContent = text || '';
  // A passing note gives way to the lasting one again.
  if (text && ms) bannerTimer = setTimeout(() => banner(s.locked ? LOCKED : '', 0), ms);
}

// ── signing in ──────────────────────────────────────────────────────────

async function session() {
  const r = await fetch('/api/session', { credentials: 'same-origin', cache: 'no-store' });
  if (r.status !== 200 && r.status !== 401) throw new Error('HTTP ' + r.status);
  return r.json();
}

// The code from the phone, or the owner's password (set in Access): this browser remembers which.
const MODE_KEY = 's24r.signin';
const LEAD_CODE = $('signin-lead').innerHTML;

function signinMode() {
  try {
    return localStorage.getItem(MODE_KEY) === 'password' ? 'password' : 'code';
  } catch {
    return 'code';
  }
}

function setSigninMode(mode, remember) {
  const pw = mode === 'password';
  $('code').hidden = pw;
  $('pw').hidden = !pw;
  if (pw) $('signin-lead').textContent = 'Type the password you chose for Remote Desktop.';
  else $('signin-lead').innerHTML = LEAD_CODE;
  $('signin-mode').textContent = pw ? 'Use the code from the phone instead' : 'Use your password instead';
  if (remember) {
    try { localStorage.setItem(MODE_KEY, mode); } catch { /* private window */ }
  }
  (pw ? $('pw') : $('code')).focus();
}

function showSignIn(info) {
  show('signin');
  const msg = $('signin-msg');
  msg.textContent = info && info.locked
    ? `Too many wrong codes: sign-in is closed for ${Math.ceil(info.locked / 60)} minutes, or until Remote Desktop is turned off and on at the phone.`
    : '';
  $('code').value = '';
  $('pw').value = '';
  setSigninMode(signinMode(), false);
}

$('signin-mode').addEventListener('click', () => {
  $('signin-msg').textContent = '';
  setSigninMode($('pw').hidden ? 'password' : 'code', true);
});

async function signIn(code, how = 'code') {
  const go = $('signin-go');
  const msg = $('signin-msg');
  go.disabled = true;
  msg.textContent = '';
  try {
    const r = await fetch('/api/login', {
      method: 'POST',
      credentials: 'same-origin',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code }),
    });
    const j = await r.json().catch(() => ({}));
    if (r.ok && j.ok) {
      startDesk();
      return;
    }
    show('signin');
    if (r.status === 429) {
      msg.textContent = `Too many wrong tries: sign-in is closed for ${Math.ceil((j.locked || 3600) / 60)} minutes, or until Remote Desktop is turned off and on at the phone.`;
    } else if (how === 'password') {
      msg.textContent = "That password isn't right.";
    } else if (j.replaced) {
      msg.textContent = 'Too many wrong codes: the phone shows a new one now.';
    } else {
      msg.textContent = `That code isn't right. ${j.left ?? ''} ${j.left === 1 ? 'try' : 'tries'} left before it changes.`;
    }
    (how === 'password' ? $('pw') : $('code')).select();
  } catch {
    show('signin');
    msg.textContent = "Couldn't reach the phone. Is Remote Desktop still on?";
  } finally {
    go.disabled = false;
  }
}

$('signin-form').addEventListener('submit', (e) => {
  e.preventDefault();
  if (!$('pw').hidden) {
    const p = $('pw').value;
    if (p) signIn(p, 'password');
    return;
  }
  const code = $('code').value.replace(/\D/g, '');
  if (code.length >= 6) signIn(code);
});

$('code').addEventListener('input', (e) => {
  const d = e.target.value.replace(/\D/g, '').slice(0, 8);
  e.target.value = d.length > 4 ? d.slice(0, 4) + ' ' + d.slice(4) : d;
  if (d.length === 8) signIn(d);
});

// ── the WebSocket ───────────────────────────────────────────────────────

function send(v) {
  if (s.ws && s.ws.readyState === WebSocket.OPEN) s.ws.send(typeof v === 'string' ? v : JSON.stringify(v));
}

function sendBin(bytes) {
  if (s.ws && s.ws.readyState === WebSocket.OPEN) s.ws.send(bytes);
}

function startDesk() {
  show('desk');
  canvas.focus();
  connect();
}

function connect() {
  s.bye = null;
  s.closedByUs = false;
  s.drawn = false;
  cover('Connecting…');
  const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  s.ws = ws;
  ws.onopen = () => {
    s.retries = 0;
    send({ t: 'hello', v: 1, w: innerWidth, h: innerHeight, dpr: devicePixelRatio, quality: s.quality, phoneScreen: s.phoneScreen });
    if (s.clip) send({ t: 'clipshare', on: true });
    s.timers.push(setInterval(() => send({ t: 'ping', n: performance.now() }), 2000));
    s.timers.push(setInterval(renderStats, 1000));
  };
  ws.onmessage = (e) => {
    if (typeof e.data === 'string') {
      let m;
      try { m = JSON.parse(e.data); } catch { return; }
      onText(m);
    } else {
      onVideo(e.data);
    }
  };
  ws.onclose = () => onClosed(ws);
}

async function onClosed(ws) {
  if (s.ws !== ws) return;
  s.ws = null;
  for (const t of s.timers) clearInterval(t);
  s.timers = [];
  closeDecoder();
  releaseAll(false);
  if (s.bye === 'taken') return cover('Your desktop was opened in another browser.', 'Use it here');
  if (s.bye === 'off') return cover('Remote Desktop was turned off at the phone.', 'Connect again');
  if (s.closedByUs) return cover('Disconnected.', 'Connect again');
  try {
    const info = await session();
    if (!info.ok) return showSignIn(info);
  } catch {
    // The phone or the tunnel isn't answering: keep trying.
  }
  const wait = Math.min(8000, 1000 * 2 ** s.retries++);
  cover(s.retries > 3 ? "Can't reach the phone. Still trying…" : 'Connection lost. Trying again…');
  setTimeout(() => { if (!s.ws && !s.closedByUs) connect(); }, wait);
}

$('cover-retry').addEventListener('click', () => {
  s.retries = 0;
  connect();
});

function onText(m) {
  switch (m.t) {
    case 'welcome':
      setQuality(m.quality, false);
      setPhoneScreen(m.phoneScreen);
      break;
    case 'state':
      if (m.state === 'starting') cover(m.message || "Starting the phone's desktop…");
      else if (m.state === 'failed') cover(m.message || "The phone's desktop couldn't start.", 'Try again');
      else if (m.state === 'stopped') cover(m.message || "The phone's desktop stopped.");
      break;
    case 'video':
      configure(m);
      break;
    case 'stats':
      s.server = m;
      break;
    case 'pong':
      s.rtt = performance.now() - m.n;
      break;
    case 'locked':
      s.locked = !!m.on;
      banner(s.locked ? LOCKED : '', 0);
      break;
    case 'phonescreen':
      setPhoneScreen(m.on);
      if (m.why === 'power') {
        banner(m.on
          ? "Someone pressed the phone's power button: its screen is on and it is locked."
          : "The phone's power button turned its own screen off; yours stays.", 8000);
      }
      break;
    case 'quality':
      setQuality(m.q, false);
      break;
    case 'clip':
      if (s.clip && typeof m.text === 'string' && m.text !== s.lastClip) {
        s.lastClip = m.text;
        if (document.hasFocus() && navigator.clipboard) navigator.clipboard.writeText(m.text).catch(() => {});
      }
      break;
    case 'bye':
      s.bye = m.reason;
      break;
    case 'sync':
      // The phone heard nothing back for a while: everything up to here has arrived.
      ack(Math.max(m.seq >>> 0, s.lastSeq));
      s.needKey = true;
      break;
  }
}

// ── video ───────────────────────────────────────────────────────────────

function b64(text) {
  const bin = atob(text);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

function makeDecoder() {
  const d = new VideoDecoder({
    output: onFrame,
    error: (e) => {
      console.warn('decoder:', e);
      if (s.decoder !== d) return;
      s.decoder = null;
      // The frames that were inside it are gone: acknowledge them, or the phone waits for them.
      ack(s.lastSeq);
      // A browser whose hardware decoder refuses the stream decodes it in software.
      if (!s.software) s.software = true;
      if (s.config) configure(s.config);
    },
  });
  s.decoder = d;
  return d;
}

function configure(m) {
  s.config = m;
  const cfg = {
    codec: m.codec,
    description: b64(m.desc),
    optimizeForLatency: true,
    hardwareAcceleration: s.software ? 'prefer-software' : 'no-preference',
  };
  if (m.w && m.h) {
    cfg.codedWidth = m.w;
    cfg.codedHeight = m.h;
  }
  const d = s.decoder && s.decoder.state !== 'closed' ? s.decoder : makeDecoder();
  try {
    d.configure(cfg);
  } catch (e) {
    console.warn('configure:', e);
    cover("This browser can't decode the phone's video (" + m.codec + ').', 'Try again');
    return;
  }
  s.needKey = true;
  askKey();
}

function closeDecoder() {
  if (s.decoder && s.decoder.state !== 'closed') {
    try { s.decoder.close(); } catch { /* closed already */ }
  }
  s.decoder = null;
}

function ack(seq) {
  const b = new Uint8Array(5);
  b[0] = IN_ACK;
  new DataView(b.buffer).setUint32(1, seq);
  sendBin(b);
}

function askKey() {
  const now = performance.now();
  if (now - s.lastKeyAsk < 400) return;
  s.lastKeyAsk = now;
  send({ t: 'keyframe' });
}

function onVideo(buf) {
  const u8 = new Uint8Array(buf);
  if (u8.length < 10 || u8[0] !== MSG_VIDEO) return;
  const dv = new DataView(buf);
  const key = (u8[1] & 1) === 1;
  const seq = dv.getUint32(2);
  s.lastSeq = seq;
  s.bytes += u8.length;
  const d = s.decoder;
  if (!d || d.state !== 'configured') {
    ack(seq);
    return;
  }
  if (s.needKey && !key) {
    ack(seq);
    askKey();
    return;
  }
  if (d.decodeQueueSize > 8) {
    // This computer can't keep up: start again from the next key frame.
    ack(seq);
    s.needKey = true;
    askKey();
    return;
  }
  s.needKey = false;
  try {
    d.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: seq * 1000, data: u8.subarray(10) }));
  } catch (e) {
    console.warn('decode:', e);
    ack(seq);
    s.needKey = true;
    askKey();
  }
}

function onFrame(frame) {
  const seq = Math.round(frame.timestamp / 1000);
  const w = frame.displayWidth, h = frame.displayHeight;
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
    fit();
  }
  ctx.drawImage(frame, 0, 0);
  frame.close();
  ack(seq);
  s.frames++;
  if (!s.drawn) {
    s.drawn = true;
    uncover();
  }
}

function fit() {
  const W = canvas.width, H = canvas.height;
  if (!W || !H) return;
  let k = Math.min(innerWidth / W, innerHeight / H);
  // The phone sizes its screen to this window, rounded down: shown pixel for pixel, never blurred
  // by a stretch of a few pixels.
  if (k >= 1 && k < 1.03) k = 1;
  canvas.style.width = W * k + 'px';
  canvas.style.height = H * k + 'px';
}

let resizeTimer = 0;
addEventListener('resize', () => {
  fit();
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => send({ t: 'resize', w: innerWidth, h: innerHeight }), 400);
});

document.addEventListener('visibilitychange', () => {
  if (!s.ws) return;
  if (document.hidden) {
    send({ t: 'pause' });
    releaseAll(true);
  } else {
    s.needKey = true;
    send({ t: 'resume' });
    syncClipboard();
  }
});

// ── pointer ─────────────────────────────────────────────────────────────

let last = { x: 0, y: 0 };

function point(e) {
  const r = canvas.getBoundingClientRect();
  const x = Math.round(((e.clientX - r.left) * canvas.width) / r.width);
  const y = Math.round(((e.clientY - r.top) * canvas.height) / r.height);
  last = { x: Math.max(0, Math.min(canvas.width - 1, x)), y: Math.max(0, Math.min(canvas.height - 1, y)) };
  return last;
}

function sendPointer(mask, p = last) {
  const b = new Uint8Array(6);
  b[0] = IN_POINTER;
  b[1] = mask;
  b[2] = p.x >> 8; b[3] = p.x & 255;
  b[4] = p.y >> 8; b[5] = p.y & 255;
  sendBin(b);
}

const BUTTON_BIT = [1, 2, 4, 128];

canvas.addEventListener('pointerdown', (e) => {
  canvas.focus({ preventScroll: true });
  if (e.pointerType === 'touch') return touchDown(e);
  e.preventDefault();
  canvas.setPointerCapture(e.pointerId);
  const bit = BUTTON_BIT[e.button] || 0;
  s.buttons |= bit;
  sendPointer(s.buttons, point(e));
});

canvas.addEventListener('pointermove', (e) => {
  if (e.pointerType === 'touch') return touchMove(e);
  sendPointer(s.buttons, point(e));
  revealBar(e);
});

canvas.addEventListener('pointerup', (e) => {
  if (e.pointerType === 'touch') return touchUp(e);
  const bit = BUTTON_BIT[e.button] || 0;
  s.buttons &= ~bit;
  sendPointer(s.buttons, point(e));
});

canvas.addEventListener('pointercancel', (e) => {
  if (e.pointerType === 'touch') return touchCancel(e);
  s.buttons = 0;
  sendPointer(0);
});

canvas.addEventListener('contextmenu', (e) => e.preventDefault());

// A wheel notch here (100 px) is two clicks there, as Open Android DeX's window sends them.
canvas.addEventListener('wheel', (e) => {
  e.preventDefault();
  const k = e.deltaMode === 1 ? 33 : e.deltaMode === 2 ? 400 : 1;
  s.wheel.y += e.deltaY * k;
  s.wheel.x += e.deltaX * k;
  point(e);
  const click = (bit) => { sendPointer(s.buttons | bit); sendPointer(s.buttons); };
  while (s.wheel.y <= -50) { click(8); s.wheel.y += 50; }
  while (s.wheel.y >= 50) { click(16); s.wheel.y -= 50; }
  while (s.wheel.x <= -50) { click(32); s.wheel.x += 50; }
  while (s.wheel.x >= 50) { click(64); s.wheel.x -= 50; }
}, { passive: false });

// Touch on a tablet or phone: a tap is a click, a hold is a right click, one finger dragging
// drags, two fingers scroll.
const touch = { ids: new Map(), start: null, dragging: false, holdTimer: 0, scroll: null };

function touchDown(e) {
  e.preventDefault();
  canvas.setPointerCapture(e.pointerId);
  touch.ids.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (touch.ids.size === 1) {
    touch.start = { ...point(e), cx: e.clientX, cy: e.clientY };
    touch.dragging = false;
    sendPointer(0);
    touch.holdTimer = setTimeout(() => {
      if (!touch.dragging && touch.ids.size === 1) {
        sendPointer(4, touch.start);
        sendPointer(0, touch.start);
        touch.start = null;
      }
    }, 600);
  } else if (touch.ids.size === 2) {
    clearTimeout(touch.holdTimer);
    if (touch.dragging) sendPointer(0);
    touch.dragging = false;
    touch.start = null;
    touch.scroll = { y: avgY(), x: avgX() };
  }
}

const avgY = () => [...touch.ids.values()].reduce((a, p) => a + p.y, 0) / touch.ids.size;
const avgX = () => [...touch.ids.values()].reduce((a, p) => a + p.x, 0) / touch.ids.size;

function touchMove(e) {
  if (!touch.ids.has(e.pointerId)) return;
  touch.ids.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (touch.ids.size >= 2 && touch.scroll) {
    const dy = touch.scroll.y - avgY(), dx = touch.scroll.x - avgX();
    if (Math.abs(dy) >= 25) { sendPointer(dy < 0 ? 8 : 16); sendPointer(0); touch.scroll.y = avgY(); }
    if (Math.abs(dx) >= 25) { sendPointer(dx < 0 ? 32 : 64); sendPointer(0); touch.scroll.x = avgX(); }
    return;
  }
  if (!touch.start) return;
  const moved = Math.hypot(e.clientX - touch.start.cx, e.clientY - touch.start.cy);
  if (!touch.dragging && moved > 10) {
    clearTimeout(touch.holdTimer);
    touch.dragging = true;
    sendPointer(1, touch.start);
  }
  if (touch.dragging) sendPointer(1, point(e));
}

function touchUp(e) {
  if (!touch.ids.has(e.pointerId)) return;
  touch.ids.delete(e.pointerId);
  clearTimeout(touch.holdTimer);
  if (touch.ids.size === 0) {
    if (touch.dragging) sendPointer(0, point(e));
    else if (touch.start) { sendPointer(1, touch.start); sendPointer(0, touch.start); }
    touch.start = null;
    touch.dragging = false;
    touch.scroll = null;
  }
}

function touchCancel(e) {
  touch.ids.delete(e.pointerId);
  clearTimeout(touch.holdTimer);
  if (touch.dragging) sendPointer(0);
  touch.start = null;
  touch.dragging = false;
}

// ── keys ────────────────────────────────────────────────────────────────

function sendKey(down, keysym) {
  const b = new Uint8Array(6);
  b[0] = IN_KEY;
  b[1] = down ? 1 : 0;
  new DataView(b.buffer).setUint32(2, keysym);
  sendBin(b);
}

function tap(keysym) {
  sendKey(true, keysym);
  sendKey(false, keysym);
}

function releaseAll(pointerToo) {
  for (const ks of s.keys.values()) sendKey(false, ks);
  s.keys.clear();
  if (pointerToo && s.buttons) {
    s.buttons = 0;
    sendPointer(0);
  }
}

const inBar = (el) => el && el.closest && el.closest("#bar, #access");
const deskKeys = () => !$("desk").hidden && s.ws && !inBar(document.activeElement);

addEventListener('keydown', (e) => {
  if (!deskKeys() || e.isComposing || e.target === $('typing') && e.key === 'Unidentified') return;
  const ks = keysymFor(e);
  if (!ks) return;
  e.preventDefault();
  s.keys.set(e.code || e.key, ks);
  sendKey(true, ks);
}, true);

addEventListener('keyup', (e) => {
  const id = e.code || e.key;
  const ks = s.keys.get(id);
  if (ks === undefined) return;
  e.preventDefault();
  s.keys.delete(id);
  sendKey(false, ks);
  // A Mac sends no key-up for keys let go of while Cmd was down.
  if (isMac && (e.key === 'Meta' || e.code === 'MetaLeft' || e.code === 'MetaRight')) {
    for (const [k, v] of s.keys) if (!/^(Shift|Control|Alt)/.test(k)) { sendKey(false, v); s.keys.delete(k); }
  }
}, true);

addEventListener('blur', () => releaseAll(true));

// The on-screen keyboard (touch screens): a hidden field takes what it types.
const typing = $('typing');
const SENTINEL = '​​';
typing.value = SENTINEL;
let composing = false;
typing.addEventListener('compositionstart', () => { composing = true; });
typing.addEventListener('compositionend', () => { composing = false; flushTyping(); });
typing.addEventListener('input', () => { if (!composing) flushTyping(); });
function flushTyping() {
  const v = typing.value;
  if (v.length < SENTINEL.length) {
    for (let i = v.length; i < SENTINEL.length; i++) tap(0xff08);
  } else {
    for (const ch of v.slice(SENTINEL.length)) tap(charKeysym(ch));
  }
  typing.value = SENTINEL;
}

// ── the bar ─────────────────────────────────────────────────────────────

const bar = $('bar');
let barTimer = 0;

function openBar() {
  bar.classList.add('show');
  clearTimeout(barTimer);
  barTimer = setTimeout(closeBar, 4000);
}

function closeBar() {
  if (bar.matches(':hover') || bar.contains(document.activeElement)) {
    barTimer = setTimeout(closeBar, 2000);
    return;
  }
  bar.classList.remove('show');
}

function revealBar(e) {
  if (e.clientY <= 3 && Math.abs(e.clientX - innerWidth / 2) < 200) openBar();
}

$('handle').addEventListener('click', () => (bar.classList.contains('show') ? closeBar() : openBar()));
$('handle').addEventListener('pointerenter', openBar);
bar.addEventListener('pointerleave', () => { clearTimeout(barTimer); barTimer = setTimeout(closeBar, 1200); });
bar.addEventListener('click', () => setTimeout(() => canvas.focus({ preventScroll: true }), 0));

function setQuality(q, tell) {
  if (!['low', 'medium', 'high'].includes(q)) return;
  s.quality = q;
  store.set('quality', q);
  for (const b of bar.querySelectorAll('.seg button')) b.setAttribute('aria-checked', String(b.dataset.q === q));
  if (tell) send({ t: 'quality', q });
}

for (const b of bar.querySelectorAll('.seg button')) b.addEventListener('click', () => setQuality(b.dataset.q, true));

function setPhoneScreen(on) {
  s.phoneScreen = !!on;
  const b = $('b-phone');
  b.setAttribute('aria-pressed', String(s.phoneScreen));
  b.title = s.phoneScreen
    ? "The phone's own screen is on (click to turn it off while you're connected)"
    : "The phone's own screen is off while you're connected (click to turn it on)";
}

$('b-phone').addEventListener('click', () => send({ t: 'phonescreen', on: !s.phoneScreen }));

$('b-full').addEventListener('click', async () => {
  try {
    if (document.fullscreenElement) {
      await document.exitFullscreen();
    } else {
      await document.documentElement.requestFullscreen({ navigationUI: 'hide' });
      // Esc, Alt+Tab and the like go to the phone while full screen (Chrome and Edge).
      if (navigator.keyboard && navigator.keyboard.lock) await navigator.keyboard.lock().catch(() => {});
    }
  } catch { /* the browser said no */ }
});

document.addEventListener('fullscreenchange', () => {
  $('b-full').textContent = document.fullscreenElement ? 'Exit full screen' : 'Full screen';
  if (!document.fullscreenElement && navigator.keyboard && navigator.keyboard.unlock) navigator.keyboard.unlock();
});

$('b-keys').addEventListener('click', () => {
  typing.focus({ preventScroll: true });
});

async function syncClipboard() {
  if (!s.clip || !navigator.clipboard || !navigator.clipboard.readText || document.hidden) return;
  try {
    const text = await navigator.clipboard.readText();
    if (typeof text === 'string' && text !== s.lastClip && text.length <= 1 << 20) {
      s.lastClip = text;
      send({ t: 'clip', text });
    }
  } catch { /* not allowed, or nothing there */ }
}

function setClip(on) {
  s.clip = on;
  store.set('clip', on ? 'on' : 'off');
  $('b-clip').setAttribute('aria-pressed', String(on));
  send({ t: 'clipshare', on });
  if (on) syncClipboard();
}

$('b-clip').addEventListener('click', () => setClip(!s.clip));
addEventListener('focus', syncClipboard);

$('b-stats').addEventListener('click', () => {
  const st = $('stats');
  st.hidden = !st.hidden;
  $('b-stats').setAttribute('aria-pressed', String(!st.hidden));
  renderStats();
});

$('b-bye').addEventListener('click', () => {
  s.closedByUs = true;
  if (s.ws) s.ws.close(1000, 'bye');
});

// ── access: the phone's addresses, and the owner's password ─────────────

const access = $('access');

async function openAccess() {
  closeBar();
  releaseAll(true); // nothing held down on the phone while typing here
  $('pass1').value = '';
  $('pass2').value = '';
  passNote('');
  access.showModal();
  $('pass1').focus();
  try {
    const j = await session();
    $('addr-net').textContent = j.url || 'none yet';
    $('addr-lan').textContent = j.lan || 'none: the phone is on no Wi-Fi';
    $('pass-remove').hidden = !j.password;
    $('pass-state').textContent = j.password
      ? 'You have one: any browser signs in with it. Change it here, or remove it.'
      : 'Sign in with it from any browser, without the code from the phone.';
  } catch {
    passNote("Couldn't reach the phone.");
  }
}

async function postPassword(body) {
  const r = await fetch('/api/password', {
    method: 'POST',
    credentials: 'same-origin',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  });
  const j = await r.json().catch(() => ({}));
  return { ok: r.ok && !!j.ok, error: j.error, password: !!j.password };
}

$('b-access').addEventListener('click', openAccess);
$('access-close').addEventListener('click', () => access.close());
access.addEventListener('close', () => canvas.focus({ preventScroll: true }));

$('access-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const a = $('pass1').value;
  if (a.length < 8) {
    passNote('At least 8 characters.');
    return;
  }
  if (a !== $('pass2').value) {
    passNote("The two don't match.");
    return;
  }
  $('pass-save').disabled = true;
  try {
    const r = await postPassword({ password: a });
    if (r.ok) {
      $('pass1').value = '';
      $('pass2').value = '';
      $('pass-remove').hidden = false;
      $('pass-state').textContent = 'You have one: any browser signs in with it. Change it here, or remove it.';
      passNote('Saved. The phone shows a note that it changed.', true);
    } else {
      passNote(r.error || "It wasn't saved.");
    }
  } catch {
    passNote("Couldn't reach the phone.");
  } finally {
    $('pass-save').disabled = false;
  }
});

// A line under the password: red when something is wrong, quiet when it went well.
function passNote(text, ok = false) {
  const msg = $('pass-msg');
  msg.textContent = text;
  msg.classList.toggle('ok', ok);
}

$('pass-remove').addEventListener('click', async () => {
  try {
    const r = await postPassword({ remove: true });
    if (r.ok) {
      $('pass-remove').hidden = true;
      $('pass-state').textContent = 'Sign in with it from any browser, without the code from the phone.';
      passNote('Removed: only the code from the phone signs in now.', true);
    } else {
      passNote(r.error || "It wasn't removed.");
    }
  } catch {
    passNote("Couldn't reach the phone.");
  }
});

function renderStats() {
  const fps = s.frames;
  const kbps = Math.round((s.bytes * 8) / 1000);
  s.frames = 0;
  s.bytes = 0;
  const el = $('stats');
  if (el.hidden) return;
  const m = s.server || {};
  el.textContent =
    `${m.w || canvas.width}×${m.h || canvas.height}  ${m.quality || s.quality}  ${m.encoder || ''}\n` +
    `${fps} fps shown, ${(kbps / 1000).toFixed(1)} Mbit/s\n` +
    `delay ${Math.round(s.rtt)} ms, frames ${m.rtt ?? '?'} ms\n` +
    `encoder target ${m.bitrate ?? '?'} kbit/s, ${m.inflight ?? '?'} in flight` +
    (s.software ? '\nsoftware decoding' : '');
}

// ── start ───────────────────────────────────────────────────────────────

addEventListener('pagehide', () => { if (s.ws) s.ws.close(); });

async function main() {
  setQuality(s.quality, false);
  $('b-clip').setAttribute('aria-pressed', String(s.clip));
  if (!window.isSecureContext || typeof VideoDecoder === 'undefined') {
    show('nope');
    return;
  }
  const support = await VideoDecoder.isConfigSupported({ codec: 'avc1.42e01f' }).catch(() => ({ supported: false }));
  if (!support.supported) {
    $('nope-why').textContent = "This browser has no H.264 video decoder for web pages. A recent Chrome, Edge or Safari has one.";
    show('nope');
    return;
  }
  // A link with the code in it (Copy link on the phone): sign in with it, and take it off the address.
  const m = /[#&]c=(\d{8})/.exec(location.hash);
  if (m) history.replaceState(null, '', location.pathname);
  let info = null;
  try {
    info = await session();
  } catch {
    show('signin');
    $('signin-msg').textContent = "Couldn't reach the phone. Is Remote Desktop still on?";
    return;
  }
  if (info.ok) startDesk();
  else if (m) signIn(m[1]);
  else showSignIn(info);
}

main();

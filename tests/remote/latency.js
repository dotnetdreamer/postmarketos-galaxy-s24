// latency.js: run in the page (drive.mjs /js). s24Latency(n, x, y, mode): n times, a space key sent the way
// the page sends a real one (its own keydown and keyup handlers, so over whichever route it uses),
// then the time until the picture at (x, y) on the phone's screen has turned (flip.py). mode 'raf'
// (the default) looks once an animation frame, when the page is drawn: the same rounding for both
// routes. 'tight' looks about every millisecond, but sees the canvas before it is shown and the
// video only when it is, so it favours the WebSocket. Returns the times in ms (null: no change in
// 3 s) and the route each used.
window.s24Latency = async (n, x, y, mode = 'raf') => {
  const probe = document.createElement('canvas');
  probe.width = probe.height = 1;
  const pc = probe.getContext('2d', { willReadFrequently: true });
  const video = document.getElementById('rtc-video');
  const canvas = document.getElementById('screen');
  const onRtc = () => video.classList.contains('live');
  const src = () => (onRtc() ? video : canvas);
  const light = () => {
    pc.drawImage(src(), x, y, 1, 1, 0, 0, 1, 1);
    return pc.getImageData(0, 0, 1, 1).data[0] > 128;
  };
  const frame = () => new Promise((r) => requestAnimationFrame(r));
  // Looked at about every millisecond, not once an animation frame: an unfocused window may draw
  // only 30 times a second, which would round every time to 33 ms steps.
  const tick = () => new Promise((r) => { const c = new MessageChannel(); c.port1.onmessage = () => r(); c.port2.postMessage(0); });
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const key = (type) => window.dispatchEvent(new KeyboardEvent(type, { key: ' ', code: 'Space', keyCode: 32, bubbles: true }));
  const out = [];
  for (let i = 0; i < n; i++) {
    await frame();
    const before = light();
    const route = onRtc() ? 'webrtc' : 'websocket';
    const t0 = performance.now();
    key('keydown');
    key('keyup');
    let ms = null;
    while (performance.now() - t0 < 3000) {
      await (mode === 'tight' ? tick() : frame());
      if (light() !== before) {
        ms = Math.round(performance.now() - t0);
        break;
      }
    }
    out.push({ ms, route });
    await sleep(250 + Math.random() * 250);
  }
  return out;
};
'ok';

// cdpeval.mjs <port> <url part> <expression>: evaluate in the first page whose URL has <url part>, over a
// DevTools endpoint, for a phone's Chrome: adb -s SERIAL forward tcp:PORT localabstract:chrome_devtools_remote
// (Claude, 2026-11-20). Prints the value as JSON.
const [port, part, expr] = process.argv.slice(2);
const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const page = list.find((t) => t.type === 'page' && t.url.includes(part));
if (!page) { console.log(JSON.stringify({ error: 'no page', pages: list.map((t) => t.url) })); process.exit(0); }
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
ws.onmessage = (m) => {
  const d = JSON.parse(m.data);
  if (d.id === 1) {
    console.log(JSON.stringify(d.result?.result?.value ?? d.result?.exceptionDetails?.text ?? d.error ?? null));
    ws.close();
    process.exit(0);
  }
};
ws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate', params: { expression: expr, awaitPromise: true, returnByValue: true } }));
setTimeout(() => { console.log(JSON.stringify({ error: 'timeout' })); process.exit(0); }, 20000);

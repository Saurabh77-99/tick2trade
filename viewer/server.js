// Book replay server.
//
//   node server.js ../data/AAPL.jsonl [--port 8080]
//
// Loads the snapshots written by `itch snapshot` and streams them to the
// browser over a WebSocket at the playback speed the user picks.
//
// The browser never reads the file and never decides what to show next: it
// renders whatever the server sends. That's the same split the live
// dashboard will use in Phase 4, where the engine publishes snapshots and a
// separate process relays them to the screen.
//
// Throttling: the server sends at most one frame per TICK_MS (20 fps). At
// 1800x speed a tick covers 90 one-second snapshots; we send only the
// latest book plus the trades and chart points from all of them. Nobody can
// read 1800 ladder updates per second, and sending them all would only
// flood the socket.

import { createReadStream, readFileSync, existsSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, dirname, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';

const TICK_MS = 50;
const MAX_TRADES = 60;
const MAX_HISTORY_POINTS = 2000;

const args = process.argv.slice(2);
const file = args.find((a) => !a.startsWith('--'));
const portIdx = args.indexOf('--port');
const port = portIdx >= 0 ? Number(args[portIdx + 1]) : 8080;
if (!file || !existsSync(file)) {
  console.error('usage: node server.js <snapshots.jsonl> [--port 8080]');
  console.error('make one with: ./build/itch snapshot data/synthetic.itch --symbol AAPL --out data/AAPL.jsonl');
  process.exit(1);
}

// ---- load snapshots --------------------------------------------------------

async function load(path) {
  let meta = null;
  const snaps = [];
  const rl = createInterface({ input: createReadStream(path), crlfDelay: Infinity });
  for await (const line of rl) {
    if (!line) continue;
    const obj = JSON.parse(line);
    if (obj.type === 'meta') meta = obj;
    else if (obj.type === 'snap') snaps.push(obj);
  }
  if (!meta || snaps.length === 0) throw new Error(`${path}: no snapshots found`);
  return { meta, snaps };
}

const { meta, snaps } = await load(file);

// Chart point per snapshot: [t, mid, bid, ask] (prices as integers). When a
// side of the book is empty (e.g. right at the open) there is no mid; carry
// the previous one forward instead of plotting 0.
let lastMid = 0;
const points = snaps.map((s) => {
  const bid = s.bids.length ? s.bids[0][0] : 0;
  const ask = s.asks.length ? s.asks[0][0] : 0;
  const mid = bid && ask ? (bid + ask) / 2 : lastMid || s.last || bid || ask;
  if (mid) lastMid = mid;
  return [s.t, mid, bid, ask];
});
// Before the first real price there's nothing to plot: back-fill with the
// first known mid so the chart's y-axis isn't stretched down to 0.
const firstMid = points.find((p) => p[1])?.[1] ?? 0;
for (const p of points) {
  if (p[1]) break;
  p[1] = firstMid;
}

function downsample(from, to, max) {
  const n = to - from;
  if (n <= 0) return [];
  const step = Math.max(1, Math.ceil(n / max));
  const out = [];
  for (let i = from; i < to; i += step) out.push(points[i]);
  if (out[out.length - 1] !== points[to - 1]) out.push(points[to - 1]);
  return out;
}

// The most recent trades at or before snapshot `index`.
function recentTrades(index) {
  const out = [];
  for (let i = index; i >= 0 && out.length < MAX_TRADES; i--) {
    const tr = snaps[i].trades;
    for (let k = tr.length - 1; k >= 0 && out.length < MAX_TRADES; k--) out.push(tr[k]);
  }
  return out.reverse();
}

console.log(`loaded ${snaps.length} snapshots of ${meta.symbol} (${meta.interval_ms} ms apart)`);

// ---- static files ----------------------------------------------------------

const publicDir = join(dirname(fileURLToPath(import.meta.url)), 'public');
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };

const http = createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const rel = url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
  const path = resolve(publicDir, rel);
  if (!path.startsWith(publicDir) || !existsSync(path)) {
    res.writeHead(404).end('not found');
    return;
  }
  res.writeHead(200, { 'Content-Type': types[extname(path)] ?? 'application/octet-stream' });
  res.end(readFileSync(path));
});

// ---- playback sessions -----------------------------------------------------

const wss = new WebSocketServer({ server: http, path: '/ws' });

wss.on('connection', (ws) => {
  const s = { index: 0, clock: snaps[0].t, playing: false, speed: 60 };
  const send = (msg) => ws.readyState === ws.OPEN && ws.send(JSON.stringify(msg));

  const reset = () =>
    send({
      type: 'reset',
      index: s.index,
      count: snaps.length,
      playing: s.playing,
      speed: s.speed,
      snap: snaps[s.index],
      trades: recentTrades(s.index),
      history: downsample(0, s.index + 1, MAX_HISTORY_POINTS),
    });

  send({ type: 'meta', meta, count: snaps.length, t0: snaps[0].t, t1: snaps[snaps.length - 1].t });
  reset();

  const timer = setInterval(() => {
    if (!s.playing) return;
    s.clock += TICK_MS * 1e6 * s.speed;  // market-time nanoseconds
    let i = s.index;
    while (i + 1 < snaps.length && snaps[i + 1].t <= s.clock) i++;
    if (i === s.index) return;

    const trades = [];
    for (let k = s.index + 1; k <= i; k++) trades.push(...snaps[k].trades);
    const from = s.index + 1;
    s.index = i;
    if (i === snaps.length - 1) s.playing = false;

    send({
      type: 'tick',
      index: i,
      playing: s.playing,
      snap: snaps[i],
      trades: trades.slice(-MAX_TRADES),
      points: downsample(from, i + 1, 50),
    });
  }, TICK_MS);

  ws.on('message', (raw) => {
    let m;
    try {
      m = JSON.parse(raw);
    } catch {
      return;
    }
    switch (m.cmd) {
      case 'play':
        if (s.index >= snaps.length - 1) s.index = 0;
        s.clock = snaps[s.index].t;
        s.playing = true;
        reset();
        break;
      case 'pause':
        s.playing = false;
        send({ type: 'state', playing: false, speed: s.speed });
        break;
      case 'speed':
        if (Number.isFinite(m.value) && m.value > 0 && m.value <= 100000) s.speed = m.value;
        send({ type: 'state', playing: s.playing, speed: s.speed });
        break;
      case 'seek':
      case 'step': {
        const target = m.cmd === 'seek' ? m.index : s.index + m.delta;
        if (!Number.isInteger(target)) return;
        s.index = Math.max(0, Math.min(snaps.length - 1, target));
        s.clock = snaps[s.index].t;
        if (m.cmd === 'step') s.playing = false;
        reset();
        break;
      }
    }
  });

  ws.on('close', () => clearInterval(timer));
});

http.listen(port, () => console.log(`viewer: http://localhost:${port}`));

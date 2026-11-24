// Book replay client: renders whatever the server streams.
//
// Performance notes (the same rules apply to a live trading dashboard):
//  * DOM nodes for the ladder and the tape are created once and only their
//    text and styles are updated. Rebuilding the DOM 20x/sec causes layout
//    thrash and garbage collection pauses.
//  * The chart is redrawn at most once per animation frame, only when data
//    changed.

const DEPTH = 10;
const TAPE_ROWS = 40;
const SPEEDS = [1, 10, 60, 300, 1800];

const $ = (id) => document.getElementById(id);
const state = {
  meta: null,
  count: 0,
  t0: 0,
  t1: 0,
  index: 0,
  playing: false,
  speed: 60,
  snap: null,
  history: [],  // [t, mid, bid, ask]
  trades: [],   // newest last
  seeking: false,
};

// ---- formatting ------------------------------------------------------------

const fmtPx = (p) => (p ? (p / 10000).toFixed(p % 100 === 0 ? 2 : 4) : '—');
const fmtQty = (q) => q.toLocaleString('en-US');
const fmtBig = (n) =>
  n >= 1e9 ? (n / 1e9).toFixed(2) + 'B' : n >= 1e6 ? (n / 1e6).toFixed(2) + 'M' : n >= 1e4 ? (n / 1e3).toFixed(1) + 'K' : String(n);
function fmtClock(ns, ms = true) {
  const s = Math.floor(ns / 1e9);
  const pad = (v, n = 2) => String(v).padStart(n, '0');
  const base = `${pad(Math.floor(s / 3600))}:${pad(Math.floor(s / 60) % 60)}:${pad(s % 60)}`;
  return ms ? `${base}.${pad(Math.floor(ns / 1e6) % 1000, 3)}` : base;
}

// ---- ladder ----------------------------------------------------------------

function buildSide(el) {
  const rows = [];
  for (let i = 0; i < DEPTH; i++) {
    const row = document.createElement('div');
    row.className = 'row empty';
    row.innerHTML = '<div class="bar"></div><span class="orders"></span><span class="size"></span><span class="price"></span>';
    el.appendChild(row);
    rows.push({ row, bar: row.children[0], orders: row.children[1], size: row.children[2], price: row.children[3], key: null });
  }
  return rows;
}
const askRows = buildSide($('asks'));  // rendered top-down: worst ... best
const bidRows = buildSide($('bids'));  // best ... worst

function fillSide(rows, levels, reversed, maxQty, prev) {
  for (let i = 0; i < DEPTH; i++) {
    const r = rows[reversed ? DEPTH - 1 - i : i];
    const lv = levels[i];
    if (!lv) {
      r.row.className = 'row empty';
      r.bar.style.width = '0';
      r.key = null;
      continue;
    }
    const [price, qty, count] = lv;
    let cls = 'row' + (i === 0 ? ' best' : '');
    const before = prev.get(price);
    if (before !== undefined && before !== qty && r.key === price) cls += qty > before ? ' up' : ' down';
    if (r.row.className !== cls) {
      r.row.className = 'row';
      void r.row.offsetWidth;  // restart the flash animation
      r.row.className = cls;
    }
    r.key = price;
    r.price.textContent = fmtPx(price);
    r.size.textContent = fmtQty(qty);
    r.orders.textContent = count;
    r.bar.style.width = `calc((100% - 138px) * ${Math.min(1, qty / maxQty).toFixed(3)})`;
  }
}

let prevBids = new Map();
let prevAsks = new Map();

function renderBook(snap) {
  const maxQty = Math.max(1, ...snap.bids.map((l) => l[1]), ...snap.asks.map((l) => l[1]));
  fillSide(askRows, snap.asks, true, maxQty, prevAsks);
  fillSide(bidRows, snap.bids, false, maxQty, prevBids);
  prevBids = new Map(snap.bids.map((l) => [l[0], l[1]]));
  prevAsks = new Map(snap.asks.map((l) => [l[0], l[1]]));

  const bid = snap.bids[0];
  const ask = snap.asks[0];
  $('s-bid').textContent = bid ? fmtPx(bid[0]) : '—';
  $('s-bidsz').textContent = bid ? `${fmtQty(bid[1])} sh` : '—';
  $('s-ask').textContent = ask ? fmtPx(ask[0]) : '—';
  $('s-asksz').textContent = ask ? `${fmtQty(ask[1])} sh` : '—';
  if (bid && ask) {
    const spread = ask[0] - bid[0];
    const mid = (ask[0] + bid[0]) / 2;
    $('s-spread').textContent = `${(spread / 100).toFixed(spread % 100 ? 2 : 0)}¢`;
    $('s-bps').textContent = `${((spread / mid) * 1e4).toFixed(2)} bps`;
    $('spread-val').textContent = `${fmtPx(spread)}  ·  mid ${fmtPx(Math.round(mid))}`;
  } else {
    $('s-spread').textContent = '—';
    $('s-bps').textContent = 'one side empty';
    $('spread-val').textContent = '—';
  }

  // Order book imbalance over the top 5 levels: +1 all bids, -1 all asks.
  const sum = (levels) => levels.slice(0, 5).reduce((a, l) => a + l[1], 0);
  const b5 = sum(snap.bids);
  const a5 = sum(snap.asks);
  const imb = b5 + a5 ? (b5 - a5) / (b5 + a5) : 0;
  $('s-imb').textContent = (imb >= 0 ? '+' : '') + imb.toFixed(2);
  $('s-imb').className = 'v ' + (imb > 0.05 ? 'bid' : imb < -0.05 ? 'ask' : '');
  const bar = $('s-imb-bar');
  bar.className = imb < 0 ? 'neg' : '';
  bar.style.width = `${Math.abs(imb) * 50}%`;
  bar.style.left = imb < 0 ? `${50 - Math.abs(imb) * 50}%` : '50%';

  $('s-vol').textContent = fmtBig(snap.vol);
  $('s-trades').textContent = `${fmtQty(snap.ntrades)} trades`;
  const perSec = state.meta ? snap.msgs / (state.meta.interval_ms / 1000) : snap.msgs;
  $('s-rate').textContent = fmtQty(Math.round(perSec));
  $('last-px').textContent = snap.last ? `last ${fmtPx(snap.last)}` : '—';
  $('clock').textContent = fmtClock(snap.t);
}

// ---- trade tape ------------------------------------------------------------

const tapeEl = $('tape');
const tapeRows = [];
for (let i = 0; i < TAPE_ROWS; i++) {
  const row = document.createElement('div');
  row.className = 'trade';
  row.innerHTML = '<span class="time"></span><span class="side"></span><span class="px"></span><span class="qty"></span>';
  tapeEl.appendChild(row);
  tapeRows.push(row);
}

function renderTape(freshCount) {
  const recent = state.trades.slice(-TAPE_ROWS).reverse();  // newest first
  for (let i = 0; i < TAPE_ROWS; i++) {
    const row = tapeRows[i];
    const t = recent[i];
    if (!t) {
      row.className = 'trade';
      for (const c of row.children) c.textContent = '';
      continue;
    }
    const [ts, price, qty, side] = t;
    row.className = `trade ${side}`;
    if (i < freshCount) {
      void row.offsetWidth;
      row.className = `trade ${side} fresh`;
    }
    row.children[0].textContent = fmtClock(ts);
    row.children[1].textContent = side === 'B' ? 'BUY' : side === 'S' ? 'SELL' : 'CROSS';
    row.children[2].textContent = fmtPx(price);
    row.children[3].textContent = fmtQty(qty);
  }
}

// ---- chart -----------------------------------------------------------------

const canvas = $('chart');
const ctx = canvas.getContext('2d');
let chartDirty = true;

function drawChart() {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const css = getComputedStyle(document.documentElement);
  const color = (v) => css.getPropertyValue(v).trim();
  const pad = { l: 8, r: 68, t: 10, b: 24 };
  const pw = w - pad.l - pad.r;
  const ph = h - pad.t - pad.b;
  const pts = state.history;
  if (!state.t1 || pw <= 0 || ph <= 0) return;

  let lo = Infinity;
  let hi = -Infinity;
  for (const p of pts) {
    if (p[1] < lo) lo = p[1];
    if (p[1] > hi) hi = p[1];
  }
  if (!isFinite(lo)) return;
  const span = Math.max(hi - lo, 100);
  lo -= span * 0.08;
  hi += span * 0.08;
  const x = (t) => pad.l + ((t - state.t0) / (state.t1 - state.t0)) * pw;
  const y = (v) => pad.t + (1 - (v - lo) / (hi - lo)) * ph;

  // Grid: 4 price lines, one line per hour. Axis labels that would collide
  // with the live price tag are skipped.
  const tagY = pts.length ? y(pts[pts.length - 1][1]) : -100;
  ctx.font = '11px ui-monospace, Menlo, monospace';
  ctx.lineWidth = 1;
  ctx.strokeStyle = color('--line');
  ctx.fillStyle = color('--faint');
  ctx.textBaseline = 'middle';
  for (let i = 0; i <= 4; i++) {
    const v = lo + ((hi - lo) * i) / 4;
    const yy = Math.round(y(v)) + 0.5;
    ctx.beginPath();
    ctx.moveTo(pad.l, yy);
    ctx.lineTo(pad.l + pw, yy);
    ctx.stroke();
    if (Math.abs(yy - tagY) > 16) ctx.fillText((v / 10000).toFixed(2), pad.l + pw + 8, yy);
  }
  ctx.textBaseline = 'top';
  ctx.textAlign = 'center';
  const hour = 3600e9;
  for (let t = Math.ceil(state.t0 / hour) * hour; t <= state.t1; t += hour) {
    const xx = Math.round(x(t)) + 0.5;
    ctx.beginPath();
    ctx.moveTo(xx, pad.t);
    ctx.lineTo(xx, pad.t + ph);
    ctx.stroke();
    ctx.fillText(fmtClock(t, false).slice(0, 5), xx, pad.t + ph + 6);
  }
  ctx.textAlign = 'left';

  if (pts.length < 2) return;

  // Area under the line, then the line itself.
  const accent = color('--accent');
  ctx.beginPath();
  ctx.moveTo(x(pts[0][0]), y(pts[0][1]));
  for (const p of pts) ctx.lineTo(x(p[0]), y(p[1]));
  const last = pts[pts.length - 1];
  ctx.lineTo(x(last[0]), pad.t + ph);
  ctx.lineTo(x(pts[0][0]), pad.t + ph);
  ctx.closePath();
  const grad = ctx.createLinearGradient(0, pad.t, 0, pad.t + ph);
  grad.addColorStop(0, 'rgba(106,167,255,0.18)');
  grad.addColorStop(1, 'rgba(106,167,255,0)');
  ctx.fillStyle = grad;
  ctx.fill();

  ctx.beginPath();
  ctx.moveTo(x(pts[0][0]), y(pts[0][1]));
  for (const p of pts) ctx.lineTo(x(p[0]), y(p[1]));
  ctx.strokeStyle = accent;
  ctx.lineWidth = 1.5;
  ctx.stroke();

  // Current price marker and label.
  const lx = x(last[0]);
  const ly = y(last[1]);
  ctx.fillStyle = accent;
  ctx.beginPath();
  ctx.arc(lx, ly, 3.5, 0, Math.PI * 2);
  ctx.fill();
  const label = (last[1] / 10000).toFixed(3);
  ctx.textBaseline = 'middle';
  const tw = ctx.measureText(label).width + 10;
  ctx.fillRect(pad.l + pw + 2, ly - 9, tw, 18);
  ctx.fillStyle = '#06101f';
  ctx.fillText(label, pad.l + pw + 7, ly);
}

function frame() {
  if (chartDirty) {
    chartDirty = false;
    drawChart();
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
new ResizeObserver(() => (chartDirty = true)).observe(canvas.parentElement);

// ---- controls ----------------------------------------------------------------

let ws = null;
const send = (msg) => ws && ws.readyState === WebSocket.OPEN && ws.send(JSON.stringify(msg));

function renderControls() {
  const play = $('play');
  play.textContent = state.playing ? 'Pause' : 'Play';
  play.classList.toggle('playing', state.playing);
  for (const b of $('speeds').children) b.classList.toggle('active', Number(b.dataset.speed) === state.speed);
  if (!state.seeking) $('seek').value = state.index;
}

$('play').onclick = () => send({ cmd: state.playing ? 'pause' : 'play' });
$('back').onclick = () => send({ cmd: 'step', delta: -1 });
$('fwd').onclick = () => send({ cmd: 'step', delta: 1 });
for (const b of $('speeds').children) b.onclick = () => send({ cmd: 'speed', value: Number(b.dataset.speed) });

const seek = $('seek');
seek.addEventListener('input', () => {
  state.seeking = true;
  const snapT = state.t0 + ((state.t1 - state.t0) * seek.value) / Math.max(1, state.count - 1);
  $('clock').textContent = fmtClock(snapT);
});
seek.addEventListener('change', () => {
  state.seeking = false;
  send({ cmd: 'seek', index: Number(seek.value) });
});

window.addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT') return;
  if (e.code === 'Space') {
    e.preventDefault();
    send({ cmd: state.playing ? 'pause' : 'play' });
  } else if (e.code === 'ArrowLeft') send({ cmd: 'step', delta: e.shiftKey ? -60 : -1 });
  else if (e.code === 'ArrowRight') send({ cmd: 'step', delta: e.shiftKey ? 60 : 1 });
  else if (/^Digit[1-5]$/.test(e.code)) send({ cmd: 'speed', value: SPEEDS[Number(e.code.slice(5)) - 1] });
});

// ---- connection ----------------------------------------------------------------

function connect() {
  const conn = $('conn');
  ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
  ws.onopen = () => {
    conn.textContent = 'connected';
    conn.className = 'conn ok';
  };
  ws.onclose = () => {
    conn.textContent = 'reconnecting…';
    conn.className = 'conn down';
    setTimeout(connect, 1000);
  };
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.type === 'meta') {
      state.meta = m.meta;
      state.count = m.count;
      state.t0 = m.t0;
      state.t1 = m.t1;
      $('symbol').textContent = m.meta.symbol;
      $('depth-hint').textContent = `top ${m.meta.depth} levels`;
      document.title = `${m.meta.symbol} · Book Replay`;
      $('t-start').textContent = fmtClock(m.t0, false).slice(0, 5);
      $('t-end').textContent = fmtClock(m.t1, false).slice(0, 5);
      seek.max = m.count - 1;
    } else if (m.type === 'reset') {
      Object.assign(state, { index: m.index, playing: m.playing, speed: m.speed, snap: m.snap });
      state.history = m.history;
      state.trades = m.trades;
      prevBids = new Map();
      prevAsks = new Map();
      renderBook(m.snap);
      renderTape(0);
      chartDirty = true;
    } else if (m.type === 'tick') {
      state.index = m.index;
      state.playing = m.playing;
      state.snap = m.snap;
      for (const p of m.points) state.history.push(p);
      if (state.history.length > 6000) {
        const n = state.history.length;
        state.history = state.history.filter((_, i) => i % 2 === 0 || i === n - 1);
      }
      state.trades.push(...m.trades);
      if (state.trades.length > 200) state.trades = state.trades.slice(-TAPE_ROWS);
      renderBook(m.snap);
      renderTape(Math.min(m.trades.length, TAPE_ROWS));
      chartDirty = true;
    } else if (m.type === 'state') {
      state.playing = m.playing;
      state.speed = m.speed;
    }
    renderControls();
  };
}
connect();

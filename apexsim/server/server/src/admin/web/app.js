'use strict';
/* ApexSim server dashboard: one page, no framework. Every view polls the
   JSON API behind /api; a 401 sends the page back to the login. */

const $ = (sel, el = document) => el.querySelector(sel);
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const AMBER = 'oklch(0.72 0.17 90)', GREEN = 'oklch(0.75 0.14 150)', RED = 'oklch(0.75 0.15 25)', PURPLE = 'oklch(0.75 0.17 300)';

// ---------------------------------------------------------------- formatting
const pad = n => String(n).padStart(2, '0');
function fmtLap(ms) {
  if (ms == null || !isFinite(ms) || ms <= 0) return '—';
  const m = Math.floor(ms / 60000), s = (ms - m * 60000) / 1000;
  return m + ':' + (s < 10 ? '0' : '') + s.toFixed(3);
}
function fmtSector(ms) { return ms == null || ms <= 0 ? '—' : (ms / 1000).toFixed(3); }
function fmtDur(s) {
  s = Math.max(0, Math.floor(s || 0));
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60), x = s % 60;
  return (d ? d + 'd ' : '') + pad(h) + ':' + pad(m) + ':' + pad(x);
}
function fmtGap(g) {
  if (!g) return '—';
  if (g.laps != null) return '+' + g.laps + ' LAP' + (g.laps > 1 ? 'S' : '');
  return '+' + g.s.toFixed(3);
}
function fmtClock(minutes) { return pad(Math.floor(minutes / 60) % 24) + ':' + pad(Math.round(minutes % 60)); }
function fmtDate(unix) {
  const d = new Date(unix * 1000);
  return d.toLocaleDateString(undefined, { month: 'short', day: '2-digit' }) + ' · ' + pad(d.getHours()) + ':' + pad(d.getMinutes());
}
function spark(arr, lo, hi, w, h) {
  return arr.map((v, i) => `${(i / Math.max(arr.length - 1, 1) * w).toFixed(1)},${(h - (Math.min(hi, Math.max(lo, v)) - lo) / (hi - lo) * h).toFixed(1)}`).join(' ');
}
const last = a => a.length ? a[a.length - 1] : 0;

// ---------------------------------------------------------------- plumbing
let overview = null;
let view = null;
let timers = [];
let toastTimer = null;

class AuthError extends Error {}

async function api(path, opts = {}) {
  const init = { credentials: 'same-origin', ...opts };
  if (opts.body !== undefined && typeof opts.body !== 'string') {
    init.body = JSON.stringify(opts.body);
    init.headers = { 'Content-Type': 'application/json' };
  } else if (opts.body !== undefined) {
    init.headers = { 'Content-Type': 'application/json' };
  }
  const res = await fetch(path, init);
  if (res.status === 401 && path !== '/api/login') { showLogin(); throw new AuthError(); }
  let data = null;
  try { data = await res.json(); } catch (_) { /* not JSON */ }
  if (!res.ok) throw new Error((data && data.error) || res.statusText);
  return data;
}

function every(ms, fn) {
  let busy = false;
  const run = async () => {
    if (busy || document.hidden) return;
    busy = true;
    try { await fn(); } catch (e) { if (!(e instanceof AuthError)) console.warn(e); } finally { busy = false; }
  };
  run();
  timers.push(setInterval(run, ms));
}
function stopTimers() { timers.forEach(clearInterval); timers = []; }

function toast(msg, isError) {
  let el = $('#toast');
  if (!el) { el = document.createElement('div'); el.id = 'toast'; document.body.appendChild(el); }
  el.className = 'toast' + (isError ? ' err' : '');
  el.textContent = msg;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.remove(), 3500);
}

function modal({ eyebrow, title, body, confirm, danger, reason }) {
  return new Promise(resolve => {
    const bg = document.createElement('div');
    bg.className = 'modal-bg';
    bg.innerHTML = `<div class="modal"><div class="body">
      <div class="eyebrow" style="color:${danger ? RED : AMBER}">${esc(eyebrow)}</div>
      <h2>${esc(title)}</h2><p>${esc(body)}</p>
      ${reason ? '<input class="input" id="m-reason" maxlength="200" placeholder="Reason (shown to the player)" style="height:40px;font-family:var(--sans);font-size:18px">' : ''}
      </div><div class="acts"><button class="btn" data-m="no">CANCEL</button>
      <button class="btn ${danger ? 'danger' : 'primary'}" data-m="yes">${esc(confirm)}</button></div></div>`;
    const done = ok => { const r = $('#m-reason', bg); bg.remove(); resolve({ ok, reason: r ? r.value.trim() : '' }); };
    bg.addEventListener('click', e => {
      if (e.target === bg) done(false);
      const m = e.target.closest('[data-m]');
      if (m) done(m.dataset.m === 'yes');
    });
    document.body.appendChild(bg);
    const r = $('#m-reason', bg); if (r) r.focus();
  });
}

// ---------------------------------------------------------------- login
function showLogin() {
  stopTimers();
  view = null;
  $('#app').innerHTML = `<div class="login"><form id="login">
    <div style="display:flex;align-items:center;gap:10px"><span style="font-size:22px;font-weight:700;letter-spacing:.06em">APEX</span>
    <span style="width:2px;height:16px;background:var(--amber);transform:skewX(-18deg)"></span><span class="eyebrow">SERVER</span></div>
    <h1>Sign in</h1>
    <input class="input" id="token" type="password" autocomplete="current-password" placeholder="Access token" autofocus>
    <div class="err" id="login-err"></div>
    <button class="btn primary" style="height:44px;font-size:19px">SIGN IN</button>
    <div class="eyebrow" style="line-height:1.6">THE TOKEN IS admin.token IN server.toml.<br>IF NONE IS SET THE SERVER PRINTED A ONE-RUN TOKEN IN ITS LOG AT STARTUP.</div>
  </form></div>`;
  $('#login').addEventListener('submit', async e => {
    e.preventDefault();
    try {
      await api('/api/login', { method: 'POST', body: { token: $('#token').value } });
      boot();
    } catch (err) { $('#login-err').textContent = err.message; }
  });
}

// ---------------------------------------------------------------- shell
const VIEWS = [
  { id: 'live', code: '01', title: 'Live session', sub: 'TRACK MAP AND TELEMETRY' },
  { id: 'timing', code: '02', title: 'Timing', sub: 'SECTORS AND GAPS' },
  { id: 'sessions', code: '03', title: 'Sessions', sub: 'HOST, START, STOP, BROADCAST' },
  { id: 'schedule', code: '04', title: 'Schedule', sub: 'SESSIONS AT SET TIMES' },
  { id: 'players', code: '05', title: 'Players', sub: 'CONNECTIONS AND BANS' },
  { id: 'history', code: '06', title: 'History', sub: 'RECORDED SESSIONS' },
  { id: 'logs', code: '07', title: 'Logs', sub: 'SERVER OUTPUT' },
  { id: 'perf', code: '08', title: 'Performance', sub: 'TICK LOOP AND PROCESS' },
  { id: 'config', code: '09', title: 'Config', sub: 'SERVER.TOML' },
  { id: 'content', code: '10', title: 'Content', sub: 'TRACKS AND CARS' },
];

function mountShell() {
  $('#app').innerHTML = `<div class="shell">
    <div class="side">
      <div class="brand"><span class="name">APEX</span><span class="slash"></span><span class="sub">SERVER</span></div>
      <div class="nav" id="nav">${VIEWS.map(v => `<a data-view="${v.id}" title="${esc(v.title)}"><span class="code">${v.code}</span><span class="label">${esc(v.title)}</span><span class="badge" id="badge-${v.id}"></span></a>`).join('')}</div>
      <div class="side-foot">
        <div class="meta"><div id="foot-bind"></div><div id="foot-ver"></div></div>
        <button class="btn quiet" id="logout">SIGN OUT</button>
      </div>
    </div>
    <div class="main">
      <div class="top"><div><h1 id="vtitle"></h1><span class="sub" id="vsub"></span></div>
        <div class="stats"><span id="st-status"></span><span id="st-up"></span><span id="st-players"></span><span id="st-hz"></span></div></div>
      <div id="banner"></div>
      <div class="content" id="content"></div>
    </div></div>`;
  $('#nav').addEventListener('click', e => {
    const a = e.target.closest('[data-view]');
    if (a) location.hash = '#' + a.dataset.view;
  });
  $('#logout').addEventListener('click', async () => { try { await api('/api/logout', { method: 'POST', body: {} }); } catch (_) { /* leaving anyway */ } showLogin(); });
}

function paintOverview() {
  const o = overview;
  if (!o) return;
  const colour = o.status === 'online' ? GREEN : AMBER;
  $('#st-status').innerHTML = `<span style="color:${colour}"><span class="dot"></span>${o.status.toUpperCase()}</span>`;
  $('#st-up').textContent = 'UP ' + fmtDur(o.uptime_s);
  $('#st-players').textContent = o.players + (o.players === 1 ? ' PLAYER' : ' PLAYERS') + ' · ' + o.sessions + (o.sessions === 1 ? ' SESSION' : ' SESSIONS');
  $('#st-hz').textContent = (o.tick_hz != null ? o.tick_hz.toFixed(0) : '—') + ' / ' + o.tick_target_hz + ' HZ';
  $('#foot-bind').textContent = o.tcp_bind + (o.tls ? ' · TLS' : ' · PLAIN');
  $('#foot-ver').textContent = 'apexsim-server ' + o.version;
  $('#badge-players').textContent = o.players || '';
  $('#banner').innerHTML = o.status === 'online' ? '' : `<div class="banner">Server ${esc(o.status)}</div>`;
}

function route() {
  const id = (location.hash || '#live').slice(1);
  const v = VIEWS.find(x => x.id === id) || VIEWS[0];
  stopTimers();
  $('#vtitle').textContent = v.title;
  $('#vsub').textContent = v.sub;
  document.querySelectorAll('#nav a').forEach(a => a.classList.toggle('on', a.dataset.view === v.id));
  const el = $('#content');
  el.innerHTML = '';
  el.scrollTop = 0;
  view = v.id;
  PAGES[v.id](el);
}

async function boot() {
  try {
    overview = await api('/api/overview');
  } catch (e) { if (e instanceof AuthError) return; $('#app').textContent = e.message; return; }
  mountShell();
  paintOverview();
  window.onhashchange = route;
  route();
  setInterval(async () => {
    if (document.hidden || !$('#st-status')) return;
    try { overview = await api('/api/overview'); paintOverview(); } catch (_) { /* login handled in api() */ }
  }, 2000);
}

// ---------------------------------------------------------------- live + timing (shared session feed)
const feed = { sessions: [], id: null, detail: null, car: null, trace: [], traceCar: null, rpmMax: {} };

async function pollSessions() {
  const r = await api('/api/sessions');
  feed.sessions = r.sessions;
  if (!feed.sessions.find(s => s.id === feed.id)) feed.id = feed.sessions.length ? feed.sessions[0].id : null;
  const badge = $('#badge-live');
  if (badge) badge.textContent = feed.sessions.length || '';
}

async function pollDetail() {
  if (!feed.id) { feed.detail = null; return false; }
  try {
    feed.detail = await api('/api/sessions/' + feed.id);
  } catch (e) {
    if (e instanceof AuthError) throw e;
    feed.detail = null;
    return false;
  }
  const cars = feed.detail.cars;
  if (!cars.find(c => c.id === feed.car)) {
    const first = cars.find(c => !c.ai) || cars[0];
    feed.car = first ? first.id : null;
  }
  const sel = cars.find(c => c.id === feed.car);
  if (sel) {
    if (feed.traceCar !== feed.car) { feed.trace = []; feed.traceCar = feed.car; }
    feed.trace.push(sel.speed_kph);
    if (feed.trace.length > 60) feed.trace.shift();
    feed.rpmMax[sel.id] = Math.max(feed.rpmMax[sel.id] || 8000, Math.ceil(sel.rpm / 1000) * 1000);
  }
  return true;
}

/* The session picker in `slot`. Redrawn only when the list or the pick
   changes, and never while it has focus: the views repaint every 0.5-1 s,
   and a select rebuilt under the mouse closes before anything is chosen. */
function paintSessionPick(slot) {
  if (!slot) return;
  const pick = $('#sess-pick', slot);
  if (pick && document.activeElement === pick) return;
  const label = s => `${s.track} · ${s.mode} · ${s.humans}+${s.ai}`;
  const key = feed.id + '|' + feed.sessions.map(s => s.id + ':' + label(s)).join('|');
  if (slot.dataset.key === key) return;
  slot.dataset.key = key;
  slot.style.display = feed.sessions.length < 2 ? 'none' : '';
  slot.innerHTML = feed.sessions.length < 2 ? '' : `<select class="input" id="sess-pick" style="height:36px">${feed.sessions.map(s =>
    `<option value="${s.id}" ${s.id === feed.id ? 'selected' : ''}>${esc(label(s))}</option>`).join('')}</select>`;
}

function bindSessionPick(root) {
  root.addEventListener('change', e => {
    if (e.target.id === 'sess-pick') { feed.id = e.target.value; feed.car = null; feed.trace = []; feed.detail = null; e.target.blur(); }
  });
}

const PAGES = {};

PAGES.live = el => {
  el.innerHTML = `<div class="stack" id="live-root"><div class="empty panel">Loading…</div></div>`;
  const root = $('#live-root');
  let mapKey = null, proj = null;
  bindSessionPick(root);
  root.addEventListener('click', e => {
    const c = e.target.closest('[data-car]');
    if (c) { feed.car = c.dataset.car; feed.trace = []; feed.traceCar = feed.car; paint(); }
  });

  function project(outline) {
    const xs = outline.map(p => p[0]), ys = outline.map(p => p[1]);
    const x0 = Math.min(...xs), x1 = Math.max(...xs), y0 = Math.min(...ys), y1 = Math.max(...ys);
    const pad = 60, w = 1000 - 2 * pad, h = 600 - 2 * pad;
    const s = Math.min(w / Math.max(x1 - x0, 1), h / Math.max(y1 - y0, 1));
    const ox = (1000 - (x1 - x0) * s) / 2, oy = (600 - (y1 - y0) * s) / 2;
    return (x, y) => [ox + (x - x0) * s, 600 - (oy + (y - y0) * s)];
  }

  function paint() {
    const d = feed.detail;
    if (!d) {
      root.innerHTML = `<div class="panel empty">${feed.sessions.length ? 'Loading the session…' : 'No active session. Sessions appear here as soon as someone creates one.'}</div>`;
      mapKey = null;
      return;
    }
    if (!$('#live-kv')) {
      root.innerHTML = `<div class="kv"><div class="kv-cells" id="live-kv"></div>
        <div id="live-pick" style="flex:1 1 180px;justify-content:center;align-items:flex-end"></div></div><div class="live">
        <div class="panel map-panel"><div class="panel-head"><span class="eyebrow">TRACK MAP · CLICK A CAR FOR TELEMETRY</span>
          <span class="legend"><span><i style="background:${AMBER};border-radius:50%"></i>SELECTED</span><span><i style="background:#e8eaea;border-radius:50%"></i>DRIVER</span><span><i style="border:1px solid #8f9599;border-radius:50%"></i>AI</span></span></div>
          <div class="map-wrap"><div class="map" id="map"><svg viewBox="0 0 1000 600" id="map-svg"></svg><div id="map-over"></div></div></div></div>
        <div class="panel tel" id="tel"></div></div>
        <div class="panel scroll-x"><div style="min-width:680px" id="stand"></div></div>`;
      mapKey = null;
    }
    paintKv(d);
    paintMap(d);
    paintTel(d);
    paintStand(d);
  }

  function paintKv(d) {
    const length = d.laps ? `<span style="font-size:17px;color:var(--muted)"> / ${d.laps}</span>`
      : d.time_left_s != null ? `<span style="font-size:17px;color:var(--muted)"> · ${fmtDur(d.time_left_s)} left</span>` : '';
    $('#live-kv').innerHTML = `
      <div style="flex:1 1 200px"><span class="eyebrow">TRACK</span><span class="v" style="font-weight:700;letter-spacing:.03em">${esc(d.track.toUpperCase())}</span></div>
      <div><span class="eyebrow">SESSION</span><span class="v">${esc(d.mode)}</span></div>
      <div><span class="eyebrow">LEADER LAP</span><span class="v">${d.leader_lap}${length}</span></div>
      <div><span class="eyebrow">ELAPSED</span><span class="v mono" style="font-size:21px;padding-top:3px">${fmtDur(d.elapsed_s)}</span></div>
      <div><span class="eyebrow">CONDITIONS</span><span class="v">${Math.round(d.air_c)} °C · ${esc(d.weather)} · ${fmtClock(d.time_of_day_minutes)}</span></div>`;
    paintSessionPick($('#live-pick'));
  }

  function paintMap(d) {
    const svg = $('#map-svg');
    const key = d.id + ':' + d.outline.length;
    if (mapKey !== key) {
      mapKey = key;
      proj = d.outline.length > 2 ? project(d.outline) : null;
      if (proj) {
        const pts = d.outline.map(p => proj(p[0], p[1]).map(v => v.toFixed(1)).join(',')).join(' L ');
        const path = 'M ' + pts + ' Z';
        const [ax, ay] = proj(d.outline[0][0], d.outline[0][1]);
        const [bx, by] = proj(d.outline[1][0], d.outline[1][1]);
        const len = Math.hypot(bx - ax, by - ay) || 1, nx = -(by - ay) / len * 16, ny = (bx - ax) / len * 16;
        svg.innerHTML = `<path d="${path}" fill="none" stroke="#1c2125" stroke-width="34" stroke-linejoin="round"/>
          <path d="${path}" fill="none" stroke="#2d3439" stroke-width="26" stroke-linejoin="round"/>
          <path d="${path}" fill="none" stroke="rgba(255,255,255,0.12)" stroke-width="1" stroke-dasharray="6 8"/>
          <line x1="${(ax - nx).toFixed(1)}" y1="${(ay - ny).toFixed(1)}" x2="${(ax + nx).toFixed(1)}" y2="${(ay + ny).toFixed(1)}" stroke="${AMBER}" stroke-width="4"/>
          <text x="${(ax - nx * 2.2).toFixed(1)}" y="${(ay - ny * 2.2).toFixed(1)}" fill="#7d858a" font-family="IBM Plex Mono, monospace" font-size="14" letter-spacing="2">S/F</text>
          <g id="cars"></g>`;
      } else svg.innerHTML = '<g id="cars"></g>';
    }
    const over = d.state === 'lobby' ? 'LOBBY · WAITING TO START' : '';
    $('#map-over').innerHTML = over ? `<div class="overlay">${over}</div>` : '';
    if (!proj) return;
    // Selected on top, AI underneath.
    const order = [...d.cars].sort((a, b) => (a.id === feed.car) - (b.id === feed.car) || b.ai - a.ai);
    $('#cars').innerHTML = order.filter(c => !c.in_garage).map(c => {
      const [x, y] = proj(c.x, c.y), sel = c.id === feed.car;
      const fill = sel ? AMBER : c.ai ? '#111416' : '#e8eaea', stroke = sel ? AMBER : c.ai ? '#8f9599' : '#e8eaea';
      return `<g class="car" data-car="${c.id}" transform="translate(${x.toFixed(1)} ${y.toFixed(1)})">
        <circle r="${sel ? 15 : 12}" fill="${fill}" stroke="${stroke}" stroke-width="1.5"/>
        <text text-anchor="middle" dy="4.5" font-family="IBM Plex Mono, monospace" font-size="${c.pos > 9 ? 11 : 13}" font-weight="500" fill="${sel || !c.ai ? '#12140f' : '#c9ced1'}">${c.pos}</text>
        ${sel ? `<text x="24" dy="4.5" font-family="IBM Plex Mono, monospace" font-size="13" fill="#e8eaea" stroke="#0a0b0c" stroke-width="3" paint-order="stroke">${esc(c.name)}</text>` : ''}</g>`;
    }).join('');
  }

  function paintTel(d) {
    const c = d.cars.find(x => x.id === feed.car);
    const el = $('#tel');
    if (!c) { el.innerHTML = '<div class="empty">No cars in this session.</div>'; return; }
    const rpmMax = feed.rpmMax[c.id] || 8000, rpmPct = Math.min(100, c.rpm / rpmMax * 100);
    const delta = c.last_ms && c.best_ms ? c.last_ms - c.best_ms : null;
    const gear = c.gear < 0 ? 'R' : c.gear === 0 ? 'N' : c.gear;
    const top = Math.max(320, Math.ceil(Math.max(...feed.trace, 0) / 20) * 20);
    el.innerHTML = `
      <div class="tel-head"><div class="tel-pos"><span class="l">POS</span><span class="n">${c.pos}</span></div>
        <div style="flex:1;min-width:0;padding:10px 16px;display:flex;flex-direction:column;justify-content:center;gap:2px">
          <div style="font-size:24px;font-weight:700;letter-spacing:.03em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis"><span style="color:var(--muted);font-weight:500">#${c.num}</span> ${esc(c.name)}</div>
          <div class="eyebrow" style="font-size:11px;letter-spacing:.06em">${esc(c.car)} · ${c.ai ? 'AI DRIVER' : 'HUMAN'}</div></div></div>
      <div class="tel-body">
        <div style="display:flex;align-items:stretch;gap:12px"><div style="flex:1;display:flex;align-items:baseline;gap:8px">
          <span class="speed">${Math.round(c.speed_kph)}</span><span class="eyebrow" style="letter-spacing:.1em;font-size:12px">KM/H</span></div>
          <div class="gear"><span class="l">GEAR</span><span class="n">${gear}</span></div></div>
        <div style="display:flex;flex-direction:column;gap:8px">
          ${bar('RPM', rpmPct, rpmPct > 92 ? RED : AMBER, Math.round(c.rpm))}
          ${bar('THR', c.throttle * 100, 'oklch(0.72 0.14 150)', Math.round(c.throttle * 100) + '%')}
          ${bar('BRK', c.brake * 100, 'oklch(0.65 0.2 25)', Math.round(c.brake * 100) + '%')}</div>
        <div class="cells">
          <div><span class="l">LAP</span><span style="font-size:22px;font-weight:600">${c.lap}</span></div>
          <div><span class="l">SECTOR</span><span style="font-size:22px;font-weight:600">S${c.sector + 1}</span></div>
          <div><span class="l">LAST VS BEST</span><span class="v" style="font-size:16px;color:${delta == null ? 'inherit' : delta <= 0 ? GREEN : RED}">${delta == null ? '—' : (delta > 0 ? '+' : '−') + (Math.abs(delta) / 1000).toFixed(3)}</span></div>
          <div><span class="l">LAST</span><span class="v">${fmtLap(c.last_ms)}</span></div>
          <div><span class="l">BEST</span><span class="v ${c.best_ms && c.best_ms === bestOverall(d) ? 'sb' : ''}">${fmtLap(c.best_ms)}</span></div>
          <div><span class="l">GAP</span><span class="v">${c.pos === 1 ? 'LEADER' : fmtGap(c.gap)}</span></div></div>
        <div style="display:flex;flex-direction:column;gap:8px">
          <div style="display:flex;justify-content:space-between" class="eyebrow"><span style="font-size:10px;letter-spacing:.14em">SPEED · LAST 15 S</span><span style="font-size:10px">${top}</span></div>
          <svg class="trace" viewBox="0 0 300 60" preserveAspectRatio="none"><line x1="0" y1="30" x2="300" y2="30" stroke="#1d2226"/>
            <polyline points="${spark(feed.trace, 0, top, 300, 60)}" fill="none" stroke="${AMBER}" stroke-width="1.5" vector-effect="non-scaling-stroke"/></svg></div>
      </div>`;
  }

  function bar(label, pct, colour, text) {
    return `<div class="bar"><span style="letter-spacing:.12em">${label}</span><div class="track"><div class="fill" style="width:${pct.toFixed(1)}%;background:${colour}"></div></div><span style="text-align:right;color:var(--text-2)">${text}</span></div>`;
  }

  function paintStand(d) {
    const cols = 'grid-template-columns:52px 52px minmax(140px,1.6fr) 60px repeat(4,minmax(86px,1fr))';
    const best = bestOverall(d);
    $('#stand').innerHTML = `<div class="row head" style="${cols}"><span>POS</span><span>NO</span><span>DRIVER</span><span>LAP</span><span>GAP</span><span>INTERVAL</span><span>LAST</span><span>BEST</span></div>` +
      d.cars.map(c => `<div class="row click ${c.id === feed.car ? 'sel' : ''}" data-car="${c.id}" style="${cols}">
        <span class="${c.id === feed.car ? 'warn' : 'muted'}">${c.finished ? '🏁' : c.pos}</span><span class="muted">${c.num}</span>
        <span class="name">${esc(c.name)}${c.ai ? ' <span class="sm-mono">AI</span>' : ''}${c.in_garage ? ' <span class="sm-mono">GARAGE</span>' : ''}</span>
        <span>${c.lap}</span><span>${c.pos === 1 ? '—' : fmtGap(c.gap)}</span><span>${c.pos === 1 ? '—' : fmtGap(c.interval)}</span>
        <span class="${c.lap_invalid && c.last_ms ? 'bad' : ''}">${fmtLap(c.last_ms)}</span><span class="${c.best_ms && c.best_ms === best ? 'sb' : ''}">${fmtLap(c.best_ms)}</span></div>`).join('') || '<div class="empty">No cars.</div>';
  }

  every(3000, pollSessions);
  every(500, async () => { await pollDetail(); paint(); });
};

function bestOverall(d) {
  const b = d.cars.map(c => c.best_ms).filter(x => x > 0);
  return b.length ? Math.min(...b) : null;
}

PAGES.timing = el => {
  el.innerHTML = `<div class="stack"><div class="legend"><span><i style="background:${PURPLE}"></i>SESSION BEST</span><span><i style="background:${GREEN}"></i>PERSONAL BEST</span><span>SECTORS SHOW THE LAST COMPLETED LAP</span><span id="sess-slot"></span></div>
    <div class="panel scroll-x"><div style="min-width:900px" id="timing"></div></div></div>`;
  bindSessionPick(el);
  const cols = 'grid-template-columns:44px 44px minmax(150px,1.5fr) 50px 84px 84px repeat(3,70px) 92px 92px';
  const sectorColour = f => f === 'sb' ? PURPLE : f === 'pb' ? GREEN : 'inherit';
  function paint() {
    const d = feed.detail;
    paintSessionPick($('#sess-slot'));
    if (!d) { $('#timing').innerHTML = `<div class="empty">${feed.sessions.length ? 'Loading…' : 'No active session.'}</div>`; return; }
    const best = bestOverall(d);
    $('#timing').innerHTML = `<div class="row head" style="${cols}"><span>POS</span><span>NO</span><span>DRIVER</span><span>LAPS</span><span>GAP</span><span>INT</span><span>S1</span><span>S2</span><span>S3</span><span>LAST</span><span>BEST</span></div>` +
      d.cars.map(c => `<div class="row click" data-car="${c.id}" style="${cols}">
        <span class="muted">${c.pos}</span><span class="muted">${c.num}</span>
        <span style="display:flex;flex-direction:column;gap:1px"><span class="name">${esc(c.name)}</span><span class="sm-mono">${esc(c.car)}${c.ai ? ' · AI' : ''}</span></span>
        <span>${c.lap}</span><span>${c.pos === 1 ? '—' : fmtGap(c.gap)}</span><span>${c.pos === 1 ? '—' : fmtGap(c.interval)}</span>
        ${c.sectors.map(s => `<span style="color:${sectorColour(s.flag)}">${fmtSector(s.ms)}</span>`).join('')}
        <span class="${c.lap_invalid && c.last_ms ? 'bad' : ''}">${fmtLap(c.last_ms)}</span><span class="${c.best_ms && c.best_ms === best ? 'sb' : ''}">${fmtLap(c.best_ms)}</span></div>`).join('');
  }
  el.addEventListener('click', e => {
    const r = e.target.closest('[data-car]');
    if (r) { feed.car = r.dataset.car; feed.trace = []; location.hash = '#live'; }
  });
  every(3000, pollSessions);
  every(1000, async () => { await pollDetail(); paint(); });
};

// ---------------------------------------------------------------- session form (sessions + schedule)
/* The settings of a hosted session, as /api/sessions and /api/schedule take
   them (HostedSpec on the server). `content` is /api/content. */
function specForm(content, spec = {}) {
  const on = list => list.filter(x => x.enabled);
  const opt = (v, label, sel) => `<option value="${esc(v)}" ${sel ? 'selected' : ''}>${esc(label)}</option>`;
  const c = spec.conditions || {};
  const tod = c.time_of_day_minutes ?? 840;
  const format = spec.weekend ? 'weekend' : spec.race_seconds ? 'timed' : 'laps';
  const wk = spec.weekend || { practice_seconds: 900, qualifying_seconds: [600] };
  const skill = spec.ai_skill ?? '';
  const pit = spec.pit_rule || {};
  const rr = spec.race_rules || {};
  const yesNo = on => opt(0, 'No', !on) + opt(1, 'Yes', !!on);
  return `<div class="form">
    <label class="field"><span class="eyebrow">NAME IN THE LOBBY</span><input class="input" data-f="name" maxlength="40" placeholder="Server" value="${esc(spec.name || '')}"></label>
    <label class="field"><span class="eyebrow">TRACK</span><select class="input" data-f="track_id">${on(content.tracks).map(t => opt(t.id, t.name, t.id === spec.track_id)).join('')}</select></label>
    <label class="field"><span class="eyebrow">CLASS OF</span><select class="input" data-f="car_id">${on(content.cars).map(x => opt(x.id, x.name + (x.class ? ' · ' + x.class : ''), x.id === spec.car_id)).join('')}</select></label>
    <label class="field"><span class="eyebrow">PLAYER SLOTS</span><input class="input" type="number" min="1" max="30" data-f="player_slots" value="${spec.player_slots ?? 12}"></label>
    <label class="field"><span class="eyebrow">AI CARS</span><input class="input" type="number" min="0" max="30" data-f="ai_count" value="${spec.ai_count ?? 8}"></label>
    <label class="field"><span class="eyebrow">AI LEVEL</span><select class="input" data-f="ai_skill">${opt('', 'Mixed field', skill === '')}${[70, 80, 90, 100, 110].map(v => opt(v, String(v), skill === v)).join('')}</select></label>
    <label class="field"><span class="eyebrow">FORMAT</span><select class="input" data-f="format">${opt('laps', 'Race over laps', format === 'laps')}${opt('timed', 'Timed race', format === 'timed')}${opt('weekend', 'Weekend: practice, qualifying, race', format === 'weekend')}</select></label>
    <label class="field" data-show="laps weekend"><span class="eyebrow">LAPS</span><input class="input" type="number" min="1" max="255" data-f="lap_limit" value="${spec.lap_limit ?? 10}"></label>
    <label class="field" data-show="timed"><span class="eyebrow">RACE MINUTES</span><input class="input" type="number" min="1" max="1440" data-f="race_min" value="${spec.race_seconds ? Math.round(spec.race_seconds / 60) : 20}"></label>
    <label class="field" data-show="weekend"><span class="eyebrow">PRACTICE MINUTES (0: NONE)</span><input class="input" type="number" min="0" max="120" data-f="practice_min" value="${Math.round(wk.practice_seconds / 60)}"></label>
    <label class="field" data-show="weekend"><span class="eyebrow">QUALIFYING ROUNDS, MINUTES</span><input class="input" data-f="rounds" placeholder="12, 10, 8" value="${wk.qualifying_seconds.map(s => Math.round(s / 60)).join(', ')}"></label>
    <label class="field"><span class="eyebrow">WEATHER</span><select class="input" data-f="weather">${(content.weather || []).map(w => opt(w.id, w.name[0].toUpperCase() + w.name.slice(1), w.id === (c.weather ?? 0))).join('')}</select></label>
    <label class="field"><span class="eyebrow">TIME OF DAY</span><input class="input" type="time" data-f="tod" value="${fmtClock(tod)}"></label>
    <label class="field"><span class="eyebrow">DAMAGE</span><select class="input" data-f="damage">${opt(2, 'Full', (spec.damage ?? 2) === 2)}${opt(1, 'Reduced', spec.damage === 1)}${opt(0, 'Off', spec.damage === 0)}</select></label>
    <label class="field"><span class="eyebrow">MANDATORY STOPS</span><select class="input" data-f="mandatory_stops">${[0, 1, 2, 3].map(v => opt(v, v ? String(v) : 'None', (pit.mandatory_stops ?? 0) === v)).join('')}</select></label>
    <label class="field"><span class="eyebrow">SECOND DRY COMPOUND</span><select class="input" data-f="compound_change">${yesNo(pit.compound_change)}</select></label>
    <label class="field"><span class="eyebrow">START</span><select class="input" data-f="start">${opt(0, 'Standing', (rr.start ?? 0) === 0)}${opt(1, 'Rolling', rr.start === 1)}</select></label>
    <label class="field"><span class="eyebrow">FORMATION LAP (STANDING)</span><select class="input" data-f="formation_lap">${yesNo(rr.formation_lap)}</select></label>
    <label class="field"><span class="eyebrow">FALSE STARTS PENALISED</span><select class="input" data-f="jump_start">${yesNo(rr.jump_start)}</select></label>
    <label class="field"><span class="eyebrow">PASSES UNDER YELLOW PENALISED</span><select class="input" data-f="yellow_penalty">${yesNo(rr.yellow_penalty)}</select></label>
    <label class="field"><span class="eyebrow">RED-LIGHT PIT EXIT PENALISED</span><select class="input" data-f="pit_exit_penalty">${yesNo(rr.pit_exit_penalty)}</select></label>
    <label class="field"><span class="eyebrow">SAFETY CAR</span><select class="input" data-f="safety_car">${opt(0, 'Off', (rr.safety_car ?? 0) === 0)}${opt(1, 'Virtual', rr.safety_car === 1)}${opt(2, 'On the road', rr.safety_car === 2)}</select></label>
    <label class="field"><span class="eyebrow">REVERSE GRID</span><select class="input" data-f="reverse_grid">${[[0, 'Off'], [6, 'Top 6'], [8, 'Top 8'], [10, 'Top 10'], [255, 'Whole field']].map(([v, l]) => opt(v, l, (rr.reverse_grid ?? 0) === v)).join('')}</select></label>
  </div>`;
}

function bindSpecForm(root) {
  const sync = () => {
    const f = $('[data-f="format"]', root).value;
    root.querySelectorAll('[data-show]').forEach(x => { x.style.display = x.dataset.show.split(' ').includes(f) ? '' : 'none'; });
  };
  root.addEventListener('change', e => { if (e.target.dataset.f === 'format') sync(); });
  sync();
}

function readSpec(root) {
  const v = k => $(`[data-f="${k}"]`, root).value;
  const n = k => Math.max(0, parseInt(v(k), 10) || 0);
  const format = v('format');
  const [h, m] = (v('tod') || '14:00').split(':').map(x => parseInt(x, 10) || 0);
  const spec = {
    name: v('name').trim(), track_id: v('track_id'), car_id: v('car_id'),
    player_slots: n('player_slots'), ai_count: n('ai_count'), ai_skill: v('ai_skill') === '' ? null : n('ai_skill'),
    lap_limit: Math.max(1, n('lap_limit')), race_seconds: null, weekend: null,
    conditions: { weather: n('weather'), time_of_day_minutes: h * 60 + m }, damage: n('damage'),
    pit_rule: { mandatory_stops: n('mandatory_stops'), compound_change: n('compound_change') === 1 },
    race_rules: {
      start: n('start'), formation_lap: n('formation_lap') === 1, jump_start: n('jump_start') === 1,
      yellow_penalty: n('yellow_penalty') === 1, pit_exit_penalty: n('pit_exit_penalty') === 1,
      safety_car: n('safety_car'), reverse_grid: n('reverse_grid'),
    },
  };
  if (format === 'timed') spec.race_seconds = Math.max(1, n('race_min')) * 60;
  if (format === 'weekend') spec.weekend = {
    practice_seconds: n('practice_min') * 60,
    qualifying_seconds: v('rounds').split(/[ ,]+/).map(x => parseInt(x, 10)).filter(x => x > 0).slice(0, 3).map(x => x * 60),
  };
  if (!spec.track_id || !spec.car_id) throw new Error('No track or car is switched on.');
  return spec;
}

// ---------------------------------------------------------------- sessions
PAGES.sessions = el => {
  el.innerHTML = `<div class="stack">
    <div class="panel scroll-x"><div style="min-width:860px" id="slist"><div class="empty">Loading…</div></div></div>
    <div class="panel"><div class="panel-head"><span class="eyebrow">BROADCAST · SHOWN TO PLAYERS IN THE GAME FOR 8 S</span></div>
      <div style="display:flex;flex-wrap:wrap;gap:10px;padding:16px"><input class="input" id="bc-msg" maxlength="300" placeholder="Message" style="flex:3 1 280px;font-family:var(--sans);font-size:18px;height:40px">
        <select class="input" id="bc-to" style="flex:1 1 200px"></select><button class="btn primary" id="bc-send" style="height:40px">SEND</button></div></div>
    <div class="panel" id="host"><div class="panel-head"><span class="eyebrow">HOST A SESSION · THE SERVER RUNS IT, PLAYERS JOIN FROM THE LOBBY</span></div><div class="empty">Loading…</div></div></div>`;
  const cols = 'grid-template-columns:minmax(150px,1.3fr) minmax(110px,1fr) 100px 80px 90px minmax(280px,auto)';
  let sessions = [];
  function paint() {
    $('#slist').innerHTML = `<div class="row head" style="${cols}"><span>TRACK</span><span>HOST</span><span>SESSION</span><span>CARS</span><span>RUNNING</span><span></span></div>` +
      (sessions.map(s => `<div class="row" style="${cols}">
        <span class="name">${esc(s.track)}</span>
        <span style="font-size:11px;letter-spacing:.1em;color:${s.hosted ? AMBER : 'var(--text-2)'}">${s.hosted ? 'SERVER' : 'PLAYER'}</span>
        <span style="font-family:var(--sans);font-size:17px">${esc(s.mode)}${s.state === s.mode.toLowerCase() ? '' : ' · ' + esc(s.state)}</span>
        <span>${s.humans}+${s.ai}</span><span>${fmtDur(s.elapsed_s).replace(/^00:/, '')}</span>
        <span style="display:flex;justify-content:flex-end;gap:6px">
          ${s.state === 'lobby' ? `<button class="btn sm primary" data-act="start" data-id="${s.id}">START</button>` : ''}
          ${s.awaits_advance ? `<button class="btn sm primary" data-act="advance" data-id="${s.id}">NEXT PHASE</button>` : ''}
          ${s.restartable ? `<button class="btn sm" data-act="restart" data-id="${s.id}" data-name="${esc(s.track)}">RESTART</button>` : ''}
          <button class="btn sm danger" data-act="stop" data-id="${s.id}" data-name="${esc(s.track)}">STOP</button></span></div>`).join('')
      || '<div class="empty">No sessions. Host one below, or schedule one.</div>');
    const to = $('#bc-to'), keep = to.value;
    to.innerHTML = `<option value="">Everyone connected</option>` + sessions.map(s => `<option value="${s.id}">${esc(s.track)} · ${esc(s.mode)}</option>`).join('');
    if ([...to.options].some(o => o.value === keep)) to.value = keep;
  }
  async function load() { sessions = (await api('/api/sessions')).sessions; paint(); }
  el.addEventListener('click', async e => {
    const b = e.target.closest('button');
    if (!b) return;
    try {
      if (b.dataset.act) {
        const { act, id, name } = b.dataset;
        let body = {};
        if (act === 'stop' || act === 'restart') {
          const r = await modal({
            eyebrow: act === 'stop' ? 'STOP SESSION' : 'RESTART SESSION', title: (act === 'stop' ? 'Stop ' : 'Restart ') + name + '?',
            body: act === 'stop' ? 'It ends now. Everyone in it goes back to the menu and is shown the reason.'
              : 'It ends now and opens again in its lobby with the same settings. Players go back to the menu and join it again.',
            confirm: act === 'stop' ? 'STOP' : 'RESTART', danger: true, reason: act === 'stop' });
          if (!r.ok) return;
          body = { reason: r.reason };
        }
        const res = await api(`/api/sessions/${id}/${act}`, { method: 'POST', body });
        toast({ start: 'Started: ' + res.phase, advance: 'Moved on to ' + res.phase, stop: 'Session stopped', restart: 'Session reopened in its lobby' }[act]);
        load();
      } else if (b.id === 'bc-send') {
        const message = $('#bc-msg').value.trim();
        if (!message) return;
        const res = await api('/api/broadcast', { method: 'POST', body: { message, session: $('#bc-to').value || undefined } });
        $('#bc-msg').value = '';
        toast('Sent to ' + res.sent + (res.sent === 1 ? ' player' : ' players'));
      } else if (b.dataset.open) {
        const res = await api('/api/sessions' + (b.dataset.open === 'start' ? '?start=1' : ''), { method: 'POST', body: readSpec($('#host')) });
        toast(res.started ? 'Session open and started' : 'Session open: players can join it now');
        load();
      }
    } catch (err) { if (!(err instanceof AuthError)) toast(err.message, true); }
  });
  $('#bc-msg').addEventListener('keydown', e => { if (e.key === 'Enter') $('#bc-send').click(); });
  api('/api/content').then(c => {
    const host = $('#host');
    host.innerHTML = host.firstElementChild.outerHTML + specForm(c) +
      `<div class="acts-row"><button class="btn" data-open="lobby">OPEN</button><button class="btn primary" data-open="start">OPEN AND START</button></div>`;
    bindSpecForm(host);
  }).catch(err => { if (!(err instanceof AuthError)) toast(err.message, true); });
  every(2000, load);
};

// ---------------------------------------------------------------- schedule
PAGES.schedule = el => {
  el.innerHTML = `<div class="stack">
    <div class="panel scroll-x"><div style="min-width:900px" id="sched"><div class="empty">Loading…</div></div></div>
    <div class="panel" id="sched-form"><div class="empty">Loading…</div></div></div>`;
  const cols = 'grid-template-columns:150px minmax(150px,1.3fr) minmax(130px,1fr) 80px 100px minmax(150px,1fr) 150px';
  let entries = [], content = null, editing = null;
  const local = unix => { const d = new Date(unix * 1000); return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + 'T' + pad(d.getHours()) + ':' + pad(d.getMinutes()); };
  const statusColour = s => s === 'open' || s === 'running' ? GREEN : s === 'done' ? 'var(--muted)' : AMBER;
  function paintList() {
    $('#sched').innerHTML = `<div class="row head" style="${cols}"><span>STARTS</span><span>NAME · TRACK</span><span>CLASS OF</span><span>REPEAT</span><span>STATUS</span><span>NOTE</span><span></span></div>` +
      (entries.map(e => `<div class="row" style="${cols}">
        <span>${fmtDate(e.start_at)}</span>
        <span style="display:flex;flex-direction:column;gap:1px"><span class="name">${esc(e.spec.name || 'Server')}</span><span class="sm-mono">${esc(e.track || '?')}</span></span>
        <span style="font-family:var(--sans);font-size:17px">${esc(e.car || '?')}</span>
        <span>${e.repeat.toUpperCase()}</span>
        <span style="font-size:11px;letter-spacing:.1em;color:${statusColour(e.status)}">${e.status.toUpperCase()}</span>
        <span class="muted" title="${esc(e.last_error || '')}" style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${esc(e.last_error || (e.status === 'scheduled' ? 'opens ' + fmtDate(e.opens_at) : ''))}</span>
        <span style="display:flex;justify-content:flex-end;gap:6px"><button class="btn sm" data-edit="${e.id}">EDIT</button><button class="btn sm danger" data-del="${e.id}">DELETE</button></span></div>`).join('')
      || '<div class="empty">Nothing scheduled. A scheduled session opens in the lobby ahead of its start and starts on its own.</div>');
  }
  function paintForm() {
    const e = editing || {};
    const start = e.start_at || Math.ceil(Date.now() / 1000 / 3600 + 1) * 3600;
    $('#sched-form').innerHTML = `<div class="panel-head"><span class="eyebrow">${editing ? 'EDIT ENTRY' : 'NEW ENTRY'}</span>${editing ? '<button class="btn sm quiet" id="sf-new">NEW INSTEAD</button>' : ''}</div>
      <div class="form">
        <label class="field"><span class="eyebrow">STARTS (LOCAL TIME)</span><input class="input" type="datetime-local" id="sf-start" value="${local(start)}"></label>
        <label class="field"><span class="eyebrow">OPENS, MINUTES BEFORE</span><input class="input" type="number" min="0" max="1440" id="sf-open" value="${e.open_minutes ?? 10}"></label>
        <label class="field"><span class="eyebrow">REPEAT</span><select class="input" id="sf-repeat">${['once', 'daily', 'weekly'].map(r => `<option value="${r}" ${r === (e.repeat || 'once') ? 'selected' : ''}>${r[0].toUpperCase() + r.slice(1)}</option>`).join('')}</select></label>
        <label class="field"><span class="eyebrow">WEEKEND PHASES</span><select class="input" id="sf-auto"><option value="1">Move on by themselves</option><option value="0" ${e.auto_advance === false ? 'selected' : ''}>Wait for NEXT PHASE</option></select></label>
      </div>${specForm(content, e.spec)}
      <div class="acts-row"><button class="btn primary" id="sf-save">${editing ? 'SAVE' : 'SCHEDULE'}</button></div>`;
    bindSpecForm($('#sched-form'));
  }
  async function load() { entries = (await api('/api/schedule')).entries; paintList(); }
  el.addEventListener('click', async ev => {
    const b = ev.target.closest('button');
    if (!b) return;
    try {
      if (b.dataset.edit) { editing = entries.find(x => x.id === b.dataset.edit) || null; paintForm(); $('#sched-form').scrollIntoView({ behavior: 'smooth' }); }
      else if (b.id === 'sf-new') { editing = null; paintForm(); }
      else if (b.dataset.del) {
        const r = await modal({ eyebrow: 'DELETE ENTRY', title: 'Delete this entry?', body: 'A session it already opened stays; stop it on the Sessions view.', confirm: 'DELETE', danger: true });
        if (!r.ok) return;
        await api('/api/schedule/' + b.dataset.del, { method: 'DELETE' });
        if (editing && editing.id === b.dataset.del) { editing = null; paintForm(); }
        load();
      } else if (b.id === 'sf-save') {
        const start = new Date($('#sf-start').value).getTime() / 1000;
        if (!isFinite(start)) throw new Error('Pick a start time.');
        const body = {
          spec: readSpec($('#sched-form')), start_at: Math.round(start), open_minutes: Math.max(0, parseInt($('#sf-open').value, 10) || 0),
          repeat: $('#sf-repeat').value, auto_advance: $('#sf-auto').value === '1', enabled: true,
        };
        if (editing) body.id = editing.id;
        await api('/api/schedule', { method: 'POST', body });
        toast(editing ? 'Saved' : 'Scheduled');
        editing = null; paintForm(); load();
      }
    } catch (err) { if (!(err instanceof AuthError)) toast(err.message, true); }
  });
  api('/api/content').then(c => { content = c; paintForm(); }).catch(err => { if (!(err instanceof AuthError)) toast(err.message, true); });
  every(3000, load);
};

// ---------------------------------------------------------------- players
PAGES.players = el => {
  el.innerHTML = `<div class="stack">
    <div class="panel scroll-x"><div style="min-width:940px" id="plist"></div></div>
    <div class="stack" style="gap:10px"><div class="eyebrow" id="ban-title">BAN LIST</div><div class="panel scroll-x"><div style="min-width:700px" id="blist"></div></div></div></div>`;
  const pcols = 'grid-template-columns:minmax(170px,1.4fr) 92px minmax(130px,1fr) minmax(120px,1fr) 90px 70px 130px 230px';
  let data = null;
  function paint() {
    if (!data) return;
    $('#plist').innerHTML = `<div class="row head" style="${pcols}"><span>PLAYER</span><span>ROLE</span><span>CAR</span><span>SESSION</span><span>ONLINE</span><span>PING</span><span>ADDRESS</span><span></span></div>` +
      (data.players.map(p => `<div class="row" style="${pcols}">
        <span class="name">${esc(p.name)}</span>
        <span style="font-size:11px;letter-spacing:.1em;color:${p.role === 'HOST' ? AMBER : p.role === 'AI' ? 'var(--muted)' : 'var(--text-2)'}">${p.role}</span>
        <span style="font-family:var(--sans);font-size:17px">${esc(p.car || '—')}</span>
        <span style="font-family:var(--sans);font-size:17px">${esc(p.track || '—')}</span>
        <span class="${p.last_seen_ms > 2500 ? 'warn' : ''}" title="${p.human ? 'last heartbeat ' + p.last_seen_ms + ' ms ago' : ''}">${p.human ? fmtDur(p.online_s).replace(/^00:/, '') : '—'}</span>
        <span class="${p.ping_ms > 150 ? 'warn' : ''}" title="${p.human && p.ping_ms == null ? 'the game has not reported one' : ''}">${p.ping_ms != null ? p.ping_ms + ' ms' : '—'}</span>
        <span class="muted">${esc(p.address || '—')}</span>
        <span style="display:flex;justify-content:flex-end;gap:6px">${p.human ? `<button class="btn sm quiet" data-mute="${p.id}" data-muted="${p.muted ? 1 : 0}" data-name="${esc(p.name)}">${p.muted ? 'UNMUTE' : 'MUTE'}</button><button class="btn sm" data-kick="${p.id}" data-name="${esc(p.name)}">KICK</button><button class="btn sm danger" data-ban="${p.id}" data-name="${esc(p.name)}">BAN</button>`
          : `<button class="btn sm quiet" data-ai="${p.id}" data-session="${p.session}" data-name="${esc(p.name)}">REMOVE</button>`}</span></div>`).join('') || '<div class="empty">Nobody is connected.</div>');
    $('#ban-title').textContent = 'BAN LIST · ' + data.bans.length;
    $('#blist').innerHTML = data.bans.map(b => `<div class="row" style="grid-template-columns:minmax(150px,1fr) minmax(160px,1.4fr) 130px 110px 90px">
        <span class="name">${esc(b.name)}</span><span style="font-family:var(--sans);font-size:17px">${esc(b.reason)}</span>
        <span>${esc(b.ip || '—')}</span><span>${fmtDate(b.banned_at)}</span>
        <button class="btn sm" style="justify-self:end" data-unban="1" data-name="${esc(b.name)}" data-ip="${esc(b.ip)}">UNBAN</button></div>`).join('') || '<div class="empty">No banned players.</div>';
  }
  async function load() { data = await api('/api/players'); paint(); }
  el.addEventListener('click', async e => {
    const t = e.target.closest('button');
    if (!t) return;
    try {
      if (t.dataset.kick || t.dataset.ban) {
        const ban = !!t.dataset.ban, id = t.dataset.kick || t.dataset.ban;
        const r = await modal({ eyebrow: ban ? 'BAN PLAYER' : 'KICK PLAYER', title: (ban ? 'Ban ' : 'Kick ') + t.dataset.name + '?',
          body: ban ? 'They are disconnected now and refused at login by name and address until you unban them.' : 'They are disconnected and may rejoin straight away.',
          confirm: ban ? 'BAN' : 'KICK', danger: true, reason: true });
        if (!r.ok) return;
        await api(`/api/players/${id}/${ban ? 'ban' : 'kick'}`, { method: 'POST', body: { reason: r.reason } });
        toast((ban ? 'Banned ' : 'Kicked ') + t.dataset.name);
        load();
      } else if (t.dataset.mute) {
        const unmute = t.dataset.muted === '1';
        await api(`/api/players/${t.dataset.mute}/${unmute ? 'unmute' : 'mute'}`, { method: 'POST', body: {} });
        toast((unmute ? 'Unmuted ' : 'Muted ') + t.dataset.name + "'s chat");
        load();
      } else if (t.dataset.ai) {
        const r = await modal({ eyebrow: 'REMOVE AI CAR', title: 'Remove ' + t.dataset.name + '?', body: 'The car leaves the session now, from the grid or from the track.', confirm: 'REMOVE', danger: true });
        if (!r.ok) return;
        await api(`/api/sessions/${t.dataset.session}/ai/${t.dataset.ai}/remove`, { method: 'POST', body: {} });
        toast('Removed ' + t.dataset.name);
        load();
      } else if (t.dataset.unban) {
        await api('/api/bans/remove', { method: 'POST', body: { name: t.dataset.name, ip: t.dataset.ip } });
        toast('Unbanned ' + t.dataset.name);
        load();
      }
    } catch (err) { if (!(err instanceof AuthError)) toast(err.message, true); }
  });
  every(2000, load);
};

// ---------------------------------------------------------------- history
PAGES.history = el => {
  el.innerHTML = `<div class="panel scroll-x"><div style="min-width:860px" id="hist"><div class="empty">Loading…</div></div></div>`;
  const cols = 'grid-template-columns:130px minmax(150px,1.2fr) 90px 90px 60px 60px minmax(130px,1fr) minmax(150px,1fr) 24px';
  let rows = [], open = null;
  function paint() {
    $('#hist').innerHTML = `<div class="row head" style="${cols}"><span>DATE</span><span>TRACK</span><span>KIND</span><span>DURATION</span><span>CARS</span><span>LAPS</span><span>WINNER</span><span>FASTEST LAP</span><span></span></div>` +
      (rows.map(h => `<div style="border-bottom:1px solid var(--line-soft);${open === h.id ? 'background:#0f1214' : ''}">
        <div class="row click" data-h="${h.id}" style="${cols};border-bottom:0;color:var(--text-3)">
          <span>${fmtDate(h.recorded_at)}</span><span class="name">${esc(h.track)}</span><span style="font-family:var(--sans);font-size:17px">${esc(h.kind)}</span>
          <span>${fmtDur(h.duration_s).replace(/^00:/, '')}</span><span>${h.cars}</span><span>${h.laps || '—'}</span><span style="font-family:var(--sans);font-size:18px;color:var(--text)">${esc(h.winner || '—')}</span>
          <span>${h.fastest ? `<span class="sb">${fmtLap(h.fastest.ms)}</span> <span style="font-family:var(--sans);font-size:16px">${esc(h.fastest.name)}</span>` : '—'}</span>
          <span class="muted">${open === h.id ? '▾' : '▸'}</span></div>
        ${open === h.id ? `<div style="padding:4px 16px 16px 158px"><div>${h.results.map(x => `<div class="row" style="grid-template-columns:36px minmax(140px,220px) minmax(120px,1fr) 60px 100px;padding:5px 0">
            <span class="muted">${x.pos ?? '—'}</span><span class="name" style="font-size:17px">${esc(x.name)}${x.ai ? ' <span class="sm-mono">AI</span>' : ''}</span><span style="font-family:var(--sans);font-size:17px">${esc(x.car || '')}</span>
            <span>${x.laps ?? '—'}</span><span class="${h.fastest && x.best_ms === h.fastest.ms ? 'sb' : ''}">${fmtLap(x.best_ms)}</span></div>`).join('')}</div>
          <div style="margin-top:12px"><button class="btn sm" data-csv="${h.id}">EXPORT CSV</button></div></div>` : ''}</div>`).join('') || '<div class="empty">No recorded sessions yet. Races are recorded to the replays folder when they finish.</div>');
  }
  el.addEventListener('click', e => {
    const csv = e.target.closest('[data-csv]');
    if (csv) {
      const h = rows.find(r => r.id === csv.dataset.csv);
      const q = v => '"' + String(v ?? '').replace(/"/g, '""') + '"';
      const text = ['position,driver,ai,car,laps,best_lap_ms', ...h.results.map(x => [x.pos ?? '', q(x.name), x.ai, q(x.car), x.laps ?? '', x.best_ms ?? ''].join(','))].join('\n');
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([text], { type: 'text/csv' }));
      a.download = `${h.track}-${h.id.slice(0, 8)}.csv`;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
      return;
    }
    const r = e.target.closest('[data-h]');
    if (r) { open = open === r.dataset.h ? null : r.dataset.h; paint(); }
  });
  api('/api/history').then(r => { rows = r.sessions; paint(); }).catch(err => { if (!(err instanceof AuthError)) toast(err.message, true); });
};

// ---------------------------------------------------------------- logs
PAGES.logs = el => {
  let level = 'ALL', query = '', follow = true, seq = 0, lines = [];
  const LEVELS = ['ALL', 'INFO', 'WARN', 'ERROR'];
  const colour = { ERROR: RED, WARN: AMBER, INFO: 'var(--text-3)', DEBUG: 'var(--muted)', TRACE: 'var(--dim)' };
  el.innerHTML = `<div class="stack" style="gap:12px"><div style="display:flex;flex-wrap:wrap;align-items:center;gap:10px">
      <div class="seg" id="lv">${LEVELS.map(l => `<button data-l="${l}" class="${l === level ? 'on' : ''}">${l}</button>`).join('')}</div>
      <input class="input" id="lq" placeholder="Filter text" style="flex:1 1 200px">
      <button class="btn" id="follow"></button><a class="btn" id="dl" href="#">DOWNLOAD</a></div>
    <div class="logbox" id="logbox"></div><div class="eyebrow" id="lognote"></div></div>`;
  const box = $('#logbox');
  function paintLine(l) {
    const t = new Date(l.ts_ms);
    return `<div class="logline"><span class="t">${pad(t.getHours())}:${pad(t.getMinutes())}:${pad(t.getSeconds())}</span><span style="color:${colour[l.level]}">${l.level.slice(0, 4)}</span><span class="s">${esc(l.source)}</span><span class="m" style="color:${l.level === 'ERROR' ? RED : l.level === 'WARN' ? AMBER : 'var(--text-2)'}">${esc(l.message)}</span></div>`;
  }
  function followBtn() {
    const b = $('#follow');
    b.innerHTML = `<span style="display:inline-block;width:7px;height:7px;border-radius:50%;background:currentColor;margin-right:8px"></span>${follow ? 'FOLLOWING' : 'PAUSED'}`;
    b.style.color = follow ? GREEN : ''; b.style.borderColor = follow ? GREEN : '';
  }
  function dlLink() {
    $('#dl').href = '/api/logs/download?level=' + level + '&q=' + encodeURIComponent(query);
  }
  async function reload() {
    seq = 0; lines = [];
    box.innerHTML = '';
    await poll();
  }
  async function poll() {
    const r = await api('/api/logs?after=' + seq + '&level=' + level + '&q=' + encodeURIComponent(query));
    $('#lognote').textContent = lines.length || r.lines.length ? '' : 'NO LOG LINES YET FOR THIS FILTER.';
    if (!r.lines.length) return;
    seq = r.last_seq;
    lines.push(...r.lines);
    if (lines.length > 5000) lines.splice(0, lines.length - 5000);
    box.insertAdjacentHTML('beforeend', r.lines.map(paintLine).join(''));
    while (box.childElementCount > 5000) box.firstChild.remove();
    if (follow) box.scrollTop = box.scrollHeight;
  }
  $('#lv').addEventListener('click', e => {
    const b = e.target.closest('[data-l]'); if (!b) return;
    level = b.dataset.l;
    document.querySelectorAll('#lv button').forEach(x => x.classList.toggle('on', x.dataset.l === level));
    dlLink(); reload();
  });
  let qTimer = null;
  $('#lq').addEventListener('input', e => { query = e.target.value; dlLink(); clearTimeout(qTimer); qTimer = setTimeout(reload, 250); });
  $('#follow').addEventListener('click', () => { follow = !follow; followBtn(); if (follow) box.scrollTop = box.scrollHeight; });
  // Scrolling up pauses following, scrolling to the end resumes it.
  box.addEventListener('scroll', () => {
    const atEnd = box.scrollHeight - box.scrollTop - box.clientHeight < 24;
    if (atEnd !== follow) { follow = atEnd; followBtn(); }
  });
  followBtn(); dlLink();
  every(1000, poll);
};

// ---------------------------------------------------------------- performance
PAGES.perf = el => {
  el.innerHTML = `<div class="stack"><div class="cards" id="pcards"></div>
    <div class="panel"><div class="panel-head" style="flex-wrap:wrap"><span class="eyebrow">TICK TIME · LAST 2 MIN</span>
      <span class="eyebrow" style="display:flex;gap:16px"><span style="color:${RED}" id="budget"></span><span id="overruns"></span></span></div>
      <div style="padding:16px"><svg class="big-chart" viewBox="0 0 600 160" preserveAspectRatio="none" id="bigchart"></svg></div></div></div>`;
  async function load() {
    const p = await api('/api/perf');
    const S = p.samples, col = k => S.map(s => s[k] ?? 0);
    const hz = col('hz'), ms = col('tick_ms'), cpu = col('cpu_pct'), mem = col('mem_mb'), out = col('out_per_s'), inn = col('in_per_s');
    const noCpu = S.length && S[S.length - 1].cpu_pct == null, noMem = S.length && S[S.length - 1].mem_mb == null;
    const memTop = Math.max(256, Math.ceil(Math.max(...mem, 0) * 1.25 / 256) * 256);
    const outTop = Math.max(60, Math.ceil(Math.max(...out, 0) * 1.25 / 60) * 60), inTop = Math.max(60, Math.ceil(Math.max(...inn, 0) * 1.25 / 60) * 60);
    const cards = [
      { label: 'TICK RATE', note: 'TARGET ' + p.target_hz, value: last(hz).toFixed(1), unit: 'HZ', color: last(hz) < p.target_hz * 0.97 && S.length ? AMBER : '', arr: hz, lo: p.target_hz * 0.9, hi: p.target_hz * 1.03, line: GREEN },
      { label: 'TICK TIME', note: 'BUDGET ' + p.budget_ms.toFixed(2), value: last(ms).toFixed(2), unit: 'MS', color: last(ms) > p.budget_ms ? RED : '', arr: ms, lo: 0, hi: p.budget_ms * 1.5, line: AMBER },
      { label: 'CPU', note: p.cores + ' CORES', value: noCpu ? '—' : Math.round(last(cpu)), unit: '%', arr: cpu, lo: 0, hi: 100, line: AMBER },
      { label: 'MEMORY', note: 'RESIDENT', value: noMem ? '—' : Math.round(last(mem)), unit: 'MB', arr: mem, lo: 0, hi: memTop, line: '#8f9599' },
      { label: 'MSG OUT', note: 'FRAMES', value: Math.round(last(out)), unit: '/S', arr: out, lo: 0, hi: outTop, line: GREEN },
      { label: 'MSG IN', note: 'INPUTS', value: Math.round(last(inn)), unit: '/S', arr: inn, lo: 0, hi: inTop, line: GREEN },
    ];
    $('#pcards').innerHTML = cards.map(c => `<div class="card"><div class="top2"><span>${c.label}</span><span>${c.note}</span></div>
      <div><span class="val" style="${c.color ? 'color:' + c.color : ''}">${c.value}</span><span class="unit">${c.unit}</span></div>
      <svg viewBox="0 0 200 40" preserveAspectRatio="none"><polyline points="${spark(c.arr, c.lo, c.hi, 200, 40)}" fill="none" stroke="${c.line}" stroke-width="1.5" vector-effect="non-scaling-stroke"/></svg></div>`).join('');
    // Scale to the budget, not the worst tick: one start-up spike would flatten the rest.
    const top = p.budget_ms * 3;
    const y = v => 160 - Math.min(v, top) / top * 160;
    $('#bigchart').innerHTML = `<line x1="0" y1="53" x2="600" y2="53" stroke="#1d2226"/><line x1="0" y1="107" x2="600" y2="107" stroke="#1d2226"/>
      <line x1="0" y1="${y(p.budget_ms).toFixed(1)}" x2="600" y2="${y(p.budget_ms).toFixed(1)}" stroke="oklch(0.65 0.2 25)" stroke-dasharray="6 6" vector-effect="non-scaling-stroke"/>
      <polyline points="${spark(S.map(s => s.tick_max_ms), 0, top, 600, 160)}" fill="none" stroke="#5d4a1c" stroke-width="1" vector-effect="non-scaling-stroke"/>
      <polyline points="${spark(ms, 0, top, 600, 160)}" fill="none" stroke="${AMBER}" stroke-width="1.5" vector-effect="non-scaling-stroke"/>`;
    $('#budget').textContent = `— BUDGET ${p.budget_ms.toFixed(2)} MS @ ${p.target_hz} HZ · DIM LINE = WORST TICK`;
    $('#overruns').textContent = 'OVERRUNS ' + p.overruns_total;
  }
  every(1000, load);
};

// ---------------------------------------------------------------- config
PAGES.config = el => {
  el.innerHTML = `<div style="display:flex;flex-wrap:wrap;gap:16px;align-items:flex-start">
    <div class="panel cfg"><div class="panel-head" style="flex-wrap:wrap">
      <span class="mono" style="font-size:12px;color:var(--text-3);display:flex;gap:12px"><span id="cfg-name">server.toml</span><span id="cfg-dirty"></span></span>
      <span style="display:flex;gap:6px"><button class="btn quiet" id="restart-server" hidden>RESTART SERVER</button><button class="btn quiet" id="revert">REVERT</button><button class="btn primary" id="save">SAVE</button></span></div>
      <textarea id="cfg" spellcheck="false"></textarea></div>
    <div class="panel side-note"><div class="eyebrow">WHEN CHANGES APPLY</div>
      <div class="it"><span>[server] max_sessions, session_timeout_seconds, reconnect_grace_seconds</span><span class="tag">NOW</span></div>
      <div class="it"><span>[auth], [logging] level, [admin] log_buffer_lines</span><span class="tag">NOW</span></div>
      <div class="it"><span>[debug]</span><span class="tag">NEXT SESSION</span></div>
      <div class="it"><span>everything else</span><span class="tag">RESTART</span></div>
      <div style="font-size:16px;line-height:1.4;color:var(--muted)">Saving checks the file the way startup does and refuses a file the server would not start with. The settings marked NOW apply as you save (an <span class="mono" style="font-size:13px">APEXSIM_</span> variable or <span class="mono" style="font-size:13px">--log-level</span> still wins); RESTART SERVER for the rest: it drops every player, stops this process and starts it again from the saved file (<span class="mono" style="font-size:13px">[admin] restart</span> says how).</div>
      <div style="font-size:16px;line-height:1.4;color:var(--muted);border-top:1px solid var(--line);padding-top:12px">The previous file is kept as <span class="mono" style="font-size:13px">server.toml.bak</span> on every save.</div></div></div>`;
  let saved = '';
  const ta = $('#cfg');
  function dirty() {
    const d = ta.value !== saved;
    $('#cfg-dirty').textContent = d ? '● UNSAVED' : '● SAVED';
    $('#cfg-dirty').style.color = d ? AMBER : GREEN;
  }
  ta.addEventListener('input', dirty);
  ta.addEventListener('keydown', e => {
    if (e.key === 'Tab') { e.preventDefault(); document.execCommand('insertText', false, '  '); }
    if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); $('#save').click(); }
  });
  $('#revert').addEventListener('click', () => { ta.value = saved; dirty(); });
  $('#save').addEventListener('click', async () => {
    try {
      const r = await api('/api/config', { method: 'PUT', body: { text: ta.value } });
      saved = ta.value; dirty();
      const now = r.applied.length ? 'Applied now: ' + r.applied.join(', ') + '. ' : '';
      if (r.restart_required) $('#restart-server').classList.add('primary');
      toast(r.restart_required ? now + 'Restart the server for: ' + r.restart_keys.join(', ') + '.' : r.changed.length ? 'Saved. ' + (now || 'Applied.') : 'Saved. Nothing changed.');
    } catch (err) { if (!(err instanceof AuthError)) toast(err.message, true); }
  });
  // RESTART SERVER: the process stops and comes back (or its supervisor
  // brings it back); the page waits for the API to answer again and reloads.
  $('#restart-server').addEventListener('click', async () => {
    if (ta.value !== saved && !confirm('The file has unsaved changes; restart without them?')) return;
    if (!confirm('Restart the server now? Every player is disconnected until it is back.')) return;
    try {
      const r = await api('/api/server/restart', { method: 'POST', body: {} });
      toast(r.mode === 'exit' ? 'Stopping; the supervisor starts it again.' : 'Restarting...');
      $('#restart-server').disabled = true; $('#save').disabled = true;
      let down = false;
      const poll = setInterval(async () => {
        try {
          const res = await fetch('/api/overview', { credentials: 'same-origin' });
          if (down && (res.ok || res.status === 401)) { clearInterval(poll); location.reload(); }
        } catch (_) { down = true; }
      }, 1000);
    } catch (err) { if (!(err instanceof AuthError)) toast(err.message, true); }
  });
  api('/api/config').then(r => { $('#cfg-name').textContent = r.path; ta.value = saved = r.text; dirty(); $('#restart-server').hidden = r.restart_mode === 'off'; })
    .catch(err => { if (!(err instanceof AuthError)) { ta.value = ''; ta.disabled = true; $('#save').disabled = true; $('#revert').disabled = true; $('#cfg-dirty').textContent = err.message; } });
};

// ---------------------------------------------------------------- content
PAGES.content = el => {
  let data = { tracks: [], cars: [] }, tab = 'tracks';
  el.innerHTML = `<div class="stack"><div style="display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:12px">
      <div class="seg big" id="ctabs"></div><span class="eyebrow">WHAT THE SERVER OFFERS · CLIENTS NEED THE SAME FILES</span></div>
    <div class="panel scroll-x"><div style="min-width:820px" id="clist"></div></div>
    <label class="dropzone" id="drop"><input type="file" id="files" multiple hidden>
      <span id="drop-text"></span></label></div>`;
  function paint() {
    $('#ctabs').innerHTML = [['tracks', 'Tracks'], ['cars', 'Cars']].map(([k, l]) => `<button data-t="${k}" class="${tab === k ? 'on' : ''}">${l}<span class="n">${data[k].length}</span></button>`).join('');
    const tracks = tab === 'tracks';
    const cols = 'grid-template-columns:minmax(180px,1.5fr) 100px 110px minmax(160px,1.5fr) 90px 80px';
    $('#clist').innerHTML = `<div class="row head" style="${cols}"><span>NAME</span><span>${tracks ? 'LENGTH' : 'CLASS'}</span><span>SOURCE</span><span>FILE</span><span>CRC</span><span>OFFERED</span></div>` +
      data[tab].map(it => `<div class="row" style="${cols}${it.enabled ? '' : ';opacity:.55'}"><span class="name">${esc(it.name)}</span>
        <span>${tracks ? (it.length_m / 1000).toFixed(2) + ' km' : esc(it.class || '—')}</span>
        <span style="font-size:11px;letter-spacing:.1em;color:${it.source === 'CUSTOM' ? AMBER : 'var(--muted)'}">${it.source}</span>
        <span class="muted" style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${esc(it.file)}">${esc(it.file)}</span><span class="muted">${it.crc}</span>
        <button class="btn sm ${it.enabled ? '' : 'quiet'}" data-toggle="${it.id}" data-on="${it.enabled ? 0 : 1}" style="justify-self:end">${it.enabled ? 'ON' : 'OFF'}</button></div>`).join('') || '<div class="empty">Nothing loaded.</div>';
    $('#drop-text').innerHTML = tracks
      ? 'DROP A TRACK HERE, OR CLICK: its <span class="mono">&lt;Track&gt;.yaml</span> and the sidecars beside it (<span class="mono">&lt;Track&gt;.road.msgpack</span>…). It goes to <span class="mono">tracks/custom/&lt;Track&gt;/</span>.'
      : 'DROP A <span class="mono">car.toml</span> HERE, OR CLICK. It goes to <span class="mono">cars/custom/&lt;car name&gt;/</span>.';
  }
  const load = () => api('/api/content').then(r => { data = r; paint(); });
  async function upload(files) {
    // Sidecars first: the YAML is what puts the track in the catalog.
    const list = [...files].sort((a, b) => /\.ya?ml$/i.test(a.name) - /\.ya?ml$/i.test(b.name));
    const kind = tab === 'tracks' ? 'track' : 'car';
    for (const f of list) {
      $('#drop-text').textContent = 'UPLOADING ' + f.name + '…';
      try {
        const res = await fetch(`/api/content/upload?kind=${kind}&name=${encodeURIComponent(f.name)}`, { method: 'POST', credentials: 'same-origin', body: f });
        const r = await res.json().catch(() => ({}));
        if (!res.ok) throw new Error(f.name + ': ' + (r.error || res.statusText));
        toast(r.name ? 'Loaded car ' + r.name : r.track ? 'Loaded track ' + r.track.name : 'Uploaded ' + f.name);
      } catch (err) { toast(err.message, true); break; }
    }
    load().catch(() => {});
  }
  el.addEventListener('click', async e => {
    const b = e.target.closest('[data-t]'); if (b) { tab = b.dataset.t; paint(); return; }
    const t = e.target.closest('[data-toggle]');
    if (t) {
      try { await api('/api/content/toggle', { method: 'POST', body: { id: t.dataset.toggle, enabled: t.dataset.on === '1' } }); await load(); }
      catch (err) { if (!(err instanceof AuthError)) toast(err.message, true); }
    }
  });
  const drop = $('#drop');
  drop.addEventListener('dragover', e => { e.preventDefault(); drop.style.borderColor = AMBER; });
  drop.addEventListener('dragleave', () => { drop.style.borderColor = ''; });
  drop.addEventListener('drop', e => { e.preventDefault(); drop.style.borderColor = ''; upload(e.dataTransfer.files); });
  $('#files').addEventListener('change', e => { upload(e.target.files); e.target.value = ''; });
  load().catch(err => { if (!(err instanceof AuthError)) toast(err.message, true); });
};

boot();

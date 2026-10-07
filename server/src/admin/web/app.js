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
  { id: 'players', code: '03', title: 'Players', sub: 'CONNECTIONS AND BANS' },
  { id: 'history', code: '04', title: 'History', sub: 'RECORDED SESSIONS' },
  { id: 'logs', code: '05', title: 'Logs', sub: 'SERVER OUTPUT' },
  { id: 'perf', code: '06', title: 'Performance', sub: 'TICK LOOP AND PROCESS' },
  { id: 'config', code: '07', title: 'Config', sub: 'SERVER.TOML' },
  { id: 'content', code: '08', title: 'Content', sub: 'TRACKS AND CARS' },
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

function sessionSelect() {
  if (feed.sessions.length < 2) return '';
  return `<select class="input" id="sess-pick" style="height:36px">${feed.sessions.map(s =>
    `<option value="${s.id}" ${s.id === feed.id ? 'selected' : ''}>${esc(s.track)} · ${esc(s.mode)} · ${s.humans}+${s.ai}</option>`).join('')}</select>`;
}

function bindSessionPick(root) {
  root.addEventListener('change', e => {
    if (e.target.id === 'sess-pick') { feed.id = e.target.value; feed.car = null; feed.trace = []; }
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
      root.innerHTML = `<div class="kv" id="live-kv"></div><div class="live">
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
      <div><span class="eyebrow">CONDITIONS</span><span class="v">${Math.round(d.air_c)} °C · ${esc(d.weather)} · ${fmtClock(d.time_of_day_minutes)}</span></div>
      ${feed.sessions.length > 1 ? `<div style="flex:1 1 180px;justify-content:center;align-items:flex-end">${sessionSelect()}</div>` : ''}`;
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
    $('#sess-slot').innerHTML = sessionSelect();
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

// ---------------------------------------------------------------- players
PAGES.players = el => {
  el.innerHTML = `<div class="stack">
    <div class="panel scroll-x"><div style="min-width:860px" id="plist"></div></div>
    <div class="stack" style="gap:10px"><div class="eyebrow" id="ban-title">BAN LIST</div><div class="panel scroll-x"><div style="min-width:700px" id="blist"></div></div></div></div>`;
  const pcols = 'grid-template-columns:minmax(170px,1.4fr) 92px minmax(130px,1fr) minmax(120px,1fr) 90px 130px 150px';
  let data = null;
  function paint() {
    if (!data) return;
    $('#plist').innerHTML = `<div class="row head" style="${pcols}"><span>PLAYER</span><span>ROLE</span><span>CAR</span><span>SESSION</span><span>ONLINE</span><span>ADDRESS</span><span></span></div>` +
      (data.players.map(p => `<div class="row" style="${pcols}">
        <span class="name">${esc(p.name)}</span>
        <span style="font-size:11px;letter-spacing:.1em;color:${p.role === 'HOST' ? AMBER : p.role === 'AI' ? 'var(--muted)' : 'var(--text-2)'}">${p.role}</span>
        <span style="font-family:var(--sans);font-size:17px">${esc(p.car || '—')}</span>
        <span style="font-family:var(--sans);font-size:17px">${esc(p.track || '—')}</span>
        <span class="${p.last_seen_ms > 2500 ? 'warn' : ''}" title="${p.human ? 'last heartbeat ' + p.last_seen_ms + ' ms ago' : ''}">${p.human ? fmtDur(p.online_s).replace(/^00:/, '') : '—'}</span>
        <span class="muted">${esc(p.address || '—')}</span>
        <span style="display:flex;justify-content:flex-end;gap:6px">${p.human ? `<button class="btn sm" data-kick="${p.id}" data-name="${esc(p.name)}">KICK</button><button class="btn sm danger" data-ban="${p.id}" data-name="${esc(p.name)}">BAN</button>` : ''}</span></div>`).join('') || '<div class="empty">Nobody is connected.</div>');
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
  const cols = 'grid-template-columns:130px minmax(150px,1.2fr) 90px 90px 70px minmax(130px,1fr) 24px';
  let rows = [], open = null;
  function paint() {
    $('#hist').innerHTML = `<div class="row head" style="${cols}"><span>DATE</span><span>TRACK</span><span>KIND</span><span>DURATION</span><span>CARS</span><span>WINNER</span><span></span></div>` +
      (rows.map(h => `<div style="border-bottom:1px solid var(--line-soft);${open === h.id ? 'background:#0f1214' : ''}">
        <div class="row click" data-h="${h.id}" style="${cols};border-bottom:0;color:var(--text-3)">
          <span>${fmtDate(h.recorded_at)}</span><span class="name">${esc(h.track)}</span><span style="font-family:var(--sans);font-size:17px">${esc(h.kind)}</span>
          <span>${fmtDur(h.duration_s).replace(/^00:/, '')}</span><span>${h.cars}</span><span style="font-family:var(--sans);font-size:18px;color:var(--text)">${esc(h.winner || '—')}</span>
          <span class="muted">${open === h.id ? '▾' : '▸'}</span></div>
        ${open === h.id ? `<div style="padding:4px 16px 16px 158px"><div>${h.results.map(x => `<div class="row" style="grid-template-columns:36px minmax(140px,220px) minmax(120px,1fr);padding:5px 0">
            <span class="muted">${x.pos ?? '—'}</span><span class="name" style="font-size:17px">${esc(x.name)}${x.ai ? ' <span class="sm-mono">AI</span>' : ''}</span><span style="font-family:var(--sans);font-size:17px">${esc(x.car || '')}</span></div>`).join('')}</div>
          <div style="margin-top:12px"><button class="btn sm" data-csv="${h.id}">EXPORT CSV</button></div></div>` : ''}</div>`).join('') || '<div class="empty">No recorded sessions yet. Races are recorded to the replays folder when they finish.</div>');
  }
  el.addEventListener('click', e => {
    const csv = e.target.closest('[data-csv]');
    if (csv) {
      const h = rows.find(r => r.id === csv.dataset.csv);
      const q = v => '"' + String(v ?? '').replace(/"/g, '""') + '"';
      const text = ['position,driver,ai,car', ...h.results.map(x => [x.pos ?? '', q(x.name), x.ai, q(x.car)].join(','))].join('\n');
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
      <span style="display:flex;gap:6px"><button class="btn quiet" id="revert">REVERT</button><button class="btn primary" id="save">SAVE</button></span></div>
      <textarea id="cfg" spellcheck="false"></textarea></div>
    <div class="panel side-note"><div class="eyebrow">WHEN CHANGES APPLY</div>
      <div class="it"><span>every setting in the file</span><span class="tag">RESTART</span></div>
      <div style="font-size:16px;line-height:1.4;color:var(--muted)">The server reads <span class="mono" style="font-size:13px">server.toml</span> once, at startup. Saving checks the file the way startup does and refuses a file the server would not start with. Restart the server yourself to apply it.</div>
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
      toast(r.restart_required ? 'Saved (' + r.changed.join(', ') + '). Restart the server to apply.' : 'Saved. Nothing changed.');
    } catch (err) { if (!(err instanceof AuthError)) toast(err.message, true); }
  });
  api('/api/config').then(r => { $('#cfg-name').textContent = r.path; ta.value = saved = r.text; dirty(); })
    .catch(err => { if (!(err instanceof AuthError)) { ta.value = ''; ta.disabled = true; $('#save').disabled = true; $('#revert').disabled = true; $('#cfg-dirty').textContent = err.message; } });
};

// ---------------------------------------------------------------- content
PAGES.content = el => {
  let data = { tracks: [], cars: [] }, tab = 'tracks';
  el.innerHTML = `<div class="stack"><div style="display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:12px">
      <div class="seg big" id="ctabs"></div><span class="eyebrow">WHAT THE SERVER OFFERS · CLIENTS NEED THE SAME FILES</span></div>
    <div class="panel scroll-x"><div style="min-width:720px" id="clist"></div></div></div>`;
  function paint() {
    $('#ctabs').innerHTML = [['tracks', 'Tracks'], ['cars', 'Cars']].map(([k, l]) => `<button data-t="${k}" class="${tab === k ? 'on' : ''}">${l}<span class="n">${data[k].length}</span></button>`).join('');
    const tracks = tab === 'tracks';
    const cols = tracks ? 'grid-template-columns:minmax(180px,1.5fr) 100px 110px minmax(160px,1.5fr) 90px' : 'grid-template-columns:minmax(180px,1.5fr) 100px 110px minmax(160px,1.5fr) 90px';
    $('#clist').innerHTML = `<div class="row head" style="${cols}"><span>NAME</span><span>${tracks ? 'LENGTH' : 'CLASS'}</span><span>SOURCE</span><span>FILE</span><span>CRC</span></div>` +
      data[tab].map(it => `<div class="row" style="${cols}"><span class="name">${esc(it.name)}</span>
        <span>${tracks ? (it.length_m / 1000).toFixed(2) + ' km' : esc(it.class || '—')}</span>
        <span style="font-size:11px;letter-spacing:.1em;color:${it.source === 'CUSTOM' ? AMBER : 'var(--muted)'}">${it.source}</span>
        <span class="muted" style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${esc(it.file)}">${esc(it.file)}</span><span class="muted">${it.crc}</span></div>`).join('') || '<div class="empty">Nothing loaded.</div>';
  }
  el.addEventListener('click', e => { const b = e.target.closest('[data-t]'); if (b) { tab = b.dataset.t; paint(); } });
  api('/api/content').then(r => { data = r; paint(); }).catch(err => { if (!(err instanceof AuthError)) toast(err.message, true); });
};

boot();

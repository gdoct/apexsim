'use strict';
/* ApexSim verifier: one page, no framework (docs/verifier.md). The queue, an
   item's evidence beside the agent's verdict, and Save. Reviews are held in
   the page (and in localStorage, so a reload keeps them) until Save writes
   them all into the items file at once. */

const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => [...el.querySelectorAll(sel)];
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const prose = s => esc(s).replace(/`([^`]+)`/g, '<code>$1</code>');

const DECISIONS = [['confirmed', 'Confirmed'], ['not-right', 'Not right'], ['cant-tell', "Can't tell"], ['later', 'Later']];
const DECIDED = DECISIONS.map(d => d[0]);
const GROUPS = [
  ['ready', 'Ready to review', 'an agent verdict on the latest evidence: one keystroke each'],
  ['evidence', 'Evidence, no verdict', 'shots to look at with no fresh agent verdict'],
  ['waiting', 'Waiting for a run', 'nothing shot yet'],
  ['hardware', 'Needs hardware or hands', 'a wheel, a pad, speakers, a second machine'],
  ['reviewed', 'Reviewed', 'saved; the agent processes these'],
  ['later', 'Later', 'until the evidence changes'],
];
const SCORE_NAMES = { 1: 'broken', 2: 'wrong', 3: 'passable', 4: 'good', 5: 'ship it' };

// ---------------------------------------------------------------- storage
function load(key, fallback) {
  try { const v = localStorage.getItem('apexsim.verifier.' + key); return v ? JSON.parse(v) : fallback; } catch (_) { return fallback; }
}
function store(key, value) {
  try { localStorage.setItem('apexsim.verifier.' + key, JSON.stringify(value)); } catch (_) { /* private window: held in memory only */ }
}

let data = null;                        // /api/items
let held = load('held', {});            // id -> {status, score, comment, flag, shots, at}
const prefs = Object.assign({ areas: [], kinds: [], statuses: [], needs: [], run: '', from: '', to: '',
  flagged: false, evidence: false, sort: 'payoff', collapsed: ['reviewed', 'later'] }, load('prefs', {}));
const runs = {};                        // run id -> /api/runs/<run>
let nav = [];                           // the queue's order when an item was opened
let cursor = 0;                         // queue row
let ui = { shot: 0, askScore: false, hover: null, logs: {}, more: {} };
let toastTimer = null;

function saveHeld() { store('held', held); }
function savePrefs() { store('prefs', prefs); }

async function api(path, opts = {}) {
  const init = { ...opts };
  if (opts.body !== undefined) { init.body = JSON.stringify(opts.body); init.headers = { 'Content-Type': 'application/json' }; }
  const res = await fetch(path, init);
  let body = null;
  try { body = await res.json(); } catch (_) { /* not JSON */ }
  if (!res.ok) { const e = new Error((body && body.error) || res.statusText); e.status = res.status; throw e; }
  return body;
}

function toast(text, err = false) {
  let el = $('.toast');
  if (!el) { el = document.createElement('div'); document.body.appendChild(el); }
  el.className = 'toast' + (err ? ' err' : '');
  el.textContent = text;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.remove(), err ? 6000 : 2500);
}

// ---------------------------------------------------------------- the model
const byId = id => data.items.find(i => i.id === id);
const latestEvidence = item => (item.evidence || []).reduce((a, e) => (!a || String(e.date) > String(a.date) ? e : a), null);

function verdictFresh(item) {
  if (!item.verdict) return false;
  const ev = latestEvidence(item);
  return !ev || String(item.verdict.date) >= String(ev.date);
}

function group(item) {
  if (item.status === 'later') return 'later';
  if (['confirmed', 'not-right', 'cant-tell'].includes(item.status)) return 'reviewed';
  if (item.status === 'judged' && verdictFresh(item)) return 'ready';
  if ((item.evidence || []).length) return 'evidence';
  if ((item.needs || []).length || item.status === 'blocked') return 'hardware';
  return 'waiting';
}

function reviewProblem(r) {
  if (!r || !DECIDED.includes(r.status)) return 'decide first (1-4)';
  const comment = (r.comment || '').trim();
  if ((r.status === 'not-right' || r.status === 'cant-tell') && !comment) return `${r.status} needs a comment`;
  if (r.status === 'confirmed' && r.score > 0 && r.score < 3) return 'confirmed under 3 is a not-right';
  return null;
}

const isFlagged = item => (held[item.id] ? held[item.id].flag : item.review && item.review.flag) || false;
const heldValid = () => Object.entries(held).filter(([id, r]) => byId(id) && !reviewProblem(r));

function matches(item) {
  const f = prefs;
  if (f.areas.length && !f.areas.includes(item.area)) return false;
  if (f.kinds.length && !f.kinds.includes(item.kind)) return false;
  if (f.statuses.length && !f.statuses.includes(item.status)) return false;
  if (f.needs.length && !(item.needs || []).some(n => f.needs.includes(n))) return false;
  if (f.run && item.check_by !== f.run && !(item.evidence || []).some(e => e.run === f.run)) return false;
  if (f.from && String(item.created || '') < f.from) return false;
  if (f.to && String(item.created || '') > f.to) return false;
  if (f.flagged && !isFlagged(item)) return false;
  if (f.evidence && !(item.evidence || []).length) return false;
  return true;
}

const byAreaDate = (a, b) => data.areas.indexOf(a.area) - data.areas.indexOf(b.area) || String(a.created || '').localeCompare(String(b.created || ''));
const SORTS = {
  date: (a, b) => String(b.created || '').localeCompare(String(a.created || '')),
  area: byAreaDate,
  run: (a, b) => String(a.check_by).localeCompare(String(b.check_by)) || byAreaDate(a, b),
  score: (a, b) => ((b.review && b.review.score) || 0) - ((a.review && a.review.score) || 0) || byAreaDate(a, b),
};

function queue() {
  const items = data.items.filter(matches);
  if (prefs.sort !== 'payoff') return [{ key: 'all', title: 'All matching', items: items.sort(SORTS[prefs.sort]) }];
  return GROUPS.map(([key, title, hint]) => ({ key, title, hint, items: items.filter(i => group(i) === key).sort(byAreaDate) }));
}

function visibleOrder() {
  return queue().filter(g => !prefs.collapsed.includes(g.key) || prefs.sort !== 'payoff').flatMap(g => g.items.map(i => i.id));
}

// ---------------------------------------------------------------- routing
function route() {
  const h = location.hash.replace(/^#\/?/, '');
  if (h.startsWith('item/')) return { view: 'item', id: decodeURIComponent(h.slice(5)) };
  if (h === 'save') return { view: 'save' };
  return { view: 'queue' };
}
function go(hash) { if (location.hash !== hash) location.hash = hash; else render(); }
window.addEventListener('hashchange', () => { ui = { shot: 0, askScore: false, hover: null, logs: {}, more: {} }; render(); });

// ---------------------------------------------------------------- shell
function topBar() {
  const items = data.items;
  const count = s => items.filter(i => i.status === s).length;
  const unprocessed = items.filter(i => i.review && !i.review.processed).length;
  const nHeld = Object.keys(held).length, nValid = heldValid().length;
  return `<div class="top">
    <div class="brand" data-go="#/"><span class="name">APEXSIM</span><span class="slash"></span><span class="sub">VERIFIER</span></div>
    <div class="stats">
      <span><span class="dot muted"></span><b>${count('open') + count('blocked')}</b> open</span>
      <span class="info"><span class="dot"></span><b>${count('judged')}</b> judged</span>
      <span class="good"><span class="dot"></span><b>${count('confirmed')}</b> confirmed</span>
      <span class="bad"><span class="dot"></span><b>${count('not-right')}</b> not right</span>
      <span class="warn"><span class="dot"></span><b>${count('cant-tell')}</b> can't tell</span>
      <span class="muted">${data.closed} closed</span>
      <span class="muted" title="saved reviews the agent has not processed">${unprocessed} for the agent</span>
    </div>
    <button class="btn ${nHeld ? 'primary' : 'quiet'}" data-go="#/save">SAVE ${nValid}${nHeld > nValid ? ` <span class="k">+${nHeld - nValid} unfinished</span>` : ''} <span class="k">CTRL+S</span></button>
    <button class="btn quiet sm" data-help>?</button>
  </div>`;
}

function render() {
  if (!data) return;
  const r = route();
  const shown = $('.item');
  if (r.view === 'item' && shown && shown.dataset.id === r.id && byId(r.id)) {
    // A decision on the item on screen: the aside and the bar, not the evidence (its scroll stays).
    const top = $('.top'), aside = $('.aside');
    const scroll = aside.scrollTop;
    top.outerHTML = topBar();
    aside.outerHTML = asideHtml(byId(r.id));
    $('.aside').scrollTop = scroll;
    return;
  }
  const body = r.view === 'item' ? itemView(r.id) : r.view === 'save' ? saveView() : queueView();
  const missing = Object.keys(held).filter(id => !byId(id));
  $('#app').innerHTML = `<div class="main">${topBar()}${missing.length
    ? `<div class="banner">Held reviews for items no longer in the file: ${missing.map(esc).join(', ')} (discard them on the Save page)</div>` : ''}${body}</div>`;
  if (r.view === 'item') afterItem(r.id);
  if (r.view === 'queue') { const row = $('.row.cur'); if (row) row.scrollIntoView({ block: 'nearest' }); }
}

// ---------------------------------------------------------------- queue
function chips(key, values, counts) {
  return `<div class="chips">${values.map(v => `<button class="chip ${prefs[key].includes(v) ? 'on' : ''}" data-chip="${key}" data-v="${esc(v)}">${esc(v)}${counts ? `<span class="n">${counts[v] || 0}</span>` : ''}</button>`).join('')}</div>`;
}

function queueView() {
  const items = data.items;
  const tally = (key) => items.reduce((m, i) => { for (const v of [].concat(i[key] || [])) m[v] = (m[v] || 0) + 1; return m; }, {});
  const needs = Object.keys(tally('needs')).sort();
  const checkBy = [...new Set(items.flatMap(i => [i.check_by, ...(i.evidence || []).map(e => e.run)]))].sort();
  const areaTiles = data.areas.map(a => {
    const it = items.filter(i => i.area === a);
    if (!it.length) return '';
    const n = s => it.filter(i => s.includes(i.status)).length;
    return `<div class="${prefs.areas.includes(a) ? 'on' : ''}" data-chip="areas" data-v="${a}"><span class="a">${a}</span>
      <span class="c"><span>${n(['open', 'blocked'])} open</span><span class="info">${n(['judged'])} judged</span><span class="good">${n(['confirmed'])} ok</span><span class="bad">${n(['not-right'])} not</span></span></div>`;
  }).join('');
  const groups = queue();
  const order = visibleOrder();
  cursor = Math.min(cursor, Math.max(order.length - 1, 0));
  const curId = order[cursor];
  const rows = groups.map(g => {
    if (!g.items.length && g.key !== 'all') return '';
    const closed = prefs.sort === 'payoff' && prefs.collapsed.includes(g.key);
    return `<div class="group"><div class="group-head" data-collapse="${g.key}"><span class="tw">${closed ? '▸' : '▾'}</span>
      <h2>${esc(g.title)}</h2><span class="n">${g.items.length}</span><span class="hint">${esc(g.hint || '')}</span></div>
      ${closed ? '' : `<div class="panel">${g.items.map(i => row(i, i.id === curId)).join('') || '<div class="empty">Nothing matches.</div>'}</div>`}</div>`;
  }).join('');
  return `<div class="content">
    <div class="areas">${areaTiles}</div>
    <div class="panel filters">
      <span class="l">KIND</span><div class="line">${chips('kinds', data.kinds, tally('kind'))}</div>
      <span class="l">STATUS</span><div class="line">${chips('statuses', data.statuses, tally('status'))}</div>
      <span class="l">NEEDS</span><div class="line">${chips('needs', needs, tally('needs'))}</div>
      <span class="l">RUN</span><div class="line">
        <select class="input" data-pref="run"><option value="">any run or block</option>${checkBy.map(r => `<option ${r === prefs.run ? 'selected' : ''}>${esc(r)}</option>`).join('')}</select>
        <span class="l">CREATED</span><input class="input" type="date" data-pref="from" value="${esc(prefs.from)}"><span class="muted">to</span><input class="input" type="date" data-pref="to" value="${esc(prefs.to)}">
        <button class="chip ${prefs.flagged ? 'on' : ''}" data-toggle="flagged">flagged</button>
        <button class="chip ${prefs.evidence ? 'on' : ''}" data-toggle="evidence">has evidence</button>
        <span class="l">SORT</span><select class="input" data-pref="sort">${[['payoff', 'what pays off first'], ['date', 'newest'], ['area', 'area'], ['run', 'run'], ['score', 'score']]
          .map(([v, l]) => `<option value="${v}" ${v === prefs.sort ? 'selected' : ''}>${l}</option>`).join('')}</select>
        <button class="btn sm quiet" data-clear>CLEAR FILTERS</button>
      </div>
    </div>
    ${rows}
  </div>`;
}

function row(i, cur) {
  const h = held[i.id];
  const v = i.verdict;
  const ev = (i.evidence || []).length;
  const state = h ? `<span class="heldtag">${esc(h.status)}${h.score ? ' ' + h.score : ''} · held</span>`
    : `<span class="st ${i.status}">${esc(i.status)}${i.review && i.review.score ? ' ' + i.review.score : ''}</span>`;
  return `<div class="row ${cur ? 'cur' : ''} ${h ? 'held' : ''} ${isFlagged(i) ? 'flagged' : ''}" data-open="${esc(i.id)}">
    <span class="mark"></span>
    <div class="cell"><span class="t">${esc(i.title)}</span><span class="id">${esc(i.id)}</span></div>
    <span class="cell hide-n">${esc(i.area)}</span>
    <span class="cell hide-n" title="${esc(i.check_by)}">${esc(i.check_by)}</span>
    <span class="cell">${v ? `<span class="says ${v.says}">${esc(v.says)}</span>` : ''}</span>
    <span class="cell hide-n">${ev ? ev + ' run' + (ev > 1 ? 's' : '') : ''}</span>
    <span class="cell">${state}</span>
  </div>`;
}

// ---------------------------------------------------------------- item
function itemView(id) {
  const i = byId(id);
  if (!i) return `<div class="content"><div class="empty">No item ${esc(id)}. <a href="#/">Back to the queue</a></div></div>`;
  if (!nav.includes(id)) nav = visibleOrder().includes(id) ? visibleOrder() : [id];
  return `<div class="item" data-id="${esc(id)}">
    <div class="evidence" id="evidence">${(i.evidence || []).length ? '<div class="empty">Loading the evidence...</div>' : `<div class="empty">No evidence yet: ${group(i) === 'hardware' ? 'this one needs ' + esc((i.needs || ['a human']).join(', ')) : 'waiting for ' + esc(i.check_by)}.</div>`}</div>
    ${asideHtml(i)}
  </div>`;
}

function asideHtml(i) {
  const id = i.id;
  const k = nav.indexOf(id);
  const h = held[id];
  const r = h || {};
  const problem = h ? reviewProblem(h) : null;
  const saved = i.review ? `<div class="mono muted" style="font-size:12px">SAVED: <span class="st ${i.status}">${esc(i.status)}</span>${i.review.score ? ' · ' + i.review.score : ''}${i.review.comment ? ' · ' + esc(i.review.comment) : ''}${i.review.processed ? ' · processed' : ''}</div>` : '';
  const doc = i.doc ? `<a href="/doc?p=${encodeURIComponent(i.doc)}" target="_blank">${esc(i.doc)}</a>` : '—';
  const verdict = i.verdict ? `<div class="verdict ${i.verdict.says}"><div class="h"><span class="says ${i.verdict.says}">${esc(i.verdict.says)}</span><span>${esc(i.verdict.date)}</span>${i.verdict.shots ? `<span>shots ${i.verdict.shots.join(', ')}</span>` : ''}${verdictFresh(i) ? '' : '<span class="warn">older than the evidence</span>'}</div>${prose(i.verdict.text)}</div>`
    : '<div class="verdict"><span class="muted">No agent verdict yet.</span></div>';
  const notes = (i.notes || []).length ? `<details class="notes"><summary>NOTES (${i.notes.length})</summary>${i.notes.map(n => `<div class="note">${n.date ? `<span class="d">${esc(n.date)}</span>` : ''}${prose(n.text)}</div>`).join('')}</details>` : '';
  return `<div class="aside">
      <div class="eyebrow">${k + 1} / ${nav.length} · ${esc(i.area)} · ${esc(i.kind)}</div>
      <h1>${esc(i.title)}</h1>
      <div class="what">${prose(i.what)}</div>
      <div class="meta">
        <span class="l">ID</span><span>${esc(i.id)}</span>
        <span class="l">STATUS</span><span class="st ${i.status}">${esc(i.status)}</span>
        <span class="l">CHECK BY</span><span>${esc(i.check_by)}</span>
        <span class="l">DOC</span><span>${doc}</span>
        ${i.plan ? `<span class="l">PLAN</span><span>${esc(i.plan)}</span>` : ''}
        ${i.needs ? `<span class="l">NEEDS</span><span>${esc(i.needs.join(', '))}</span>` : ''}
        <span class="l">LANDED</span><span>${esc(i.created || '—')} ${esc(i.commit || '')}</span>
      </div>
      ${verdict}
      <div class="review ${h ? 'held' : ''}">
        <div class="decide">${DECISIONS.map(([s, l], n) => `<button class="btn ${s} ${r.status === s ? 'on' : ''}" data-decide="${s}">${l} <span class="k">${n + 1}</span></button>`).join('')}</div>
        <div class="scores ${ui.askScore ? 'ask' : ''}"><span class="eyebrow" style="width:52px">SCORE</span>${[0, 1, 2, 3, 4, 5].map(n => `<button class="btn sm ${(r.score || 0) === n && h ? 'on' : ''}" data-score="${n}" title="${SCORE_NAMES[n] || 'none'}">${n || '–'}</button>`).join('')}</div>
        <textarea id="comment" placeholder="Comment (c). Not right and can't tell need one: it is the follow-up's first line, or the next run's brief.">${esc(r.comment || '')}</textarea>
        <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
          <button class="btn sm ${r.flag ? 'on' : ''}" data-flag>${r.flag ? 'FLAGGED' : 'FLAG'} <span class="k">F</span></button>
          ${h ? '<button class="btn sm quiet" data-undo>UNDO <span class="k">U</span></button>' : ''}
          <span class="mono muted" style="font-size:11px">${(r.shots || []).length ? 'about ' + r.shots.map(s => `${esc(s.run)} #${s.shot}`).join(', ') : ''}</span>
        </div>
        <div class="problem">${problem ? esc(problem) : ''}</div>
        ${saved}
      </div>
      ${notes}
      <div class="keys"><span class="kbd">1</span>-<span class="kbd">4</span> decide, then <span class="kbd">0</span>-<span class="kbd">5</span> score · <span class="kbd">c</span> comment · <span class="kbd">f</span> flag · <span class="kbd">s</span> this shot is what the review is about<br>
        <span class="kbd">←</span><span class="kbd">→</span> shots · <span class="kbd">z</span> 1:1 · <span class="kbd">j</span><span class="kbd">k</span> items · <span class="kbd">Esc</span> queue · <span class="kbd">?</span> all keys</div>
    </div>`;
}

async function runInfo(run) {
  if (!runs[run]) runs[run] = await api('/api/runs/' + encodeURIComponent(run)).catch(() => ({ run, shots: [], logs: [], exists: false }));
  return runs[run];
}

async function afterItem(id) {
  const i = byId(id);
  const box = $('#evidence');
  if (!i || !(i.evidence || []).length) return;
  const evs = [...i.evidence].sort((a, b) => String(b.date).localeCompare(String(a.date)));
  const infos = await Promise.all(evs.map(e => runInfo(e.run)));
  if (route().id !== id) return;
  const latest = evs[0];
  const verdictShots = new Set((i.verdict && i.verdict.shots) || []);
  const refs = new Set(((held[id] || {}).shots || []).map(s => s.run + '#' + s.shot));
  box.innerHTML = evs.map((e, k) => {
    const info = infos[k];
    const chosen = (e.shots || []).length ? e.shots : info.shots.map(s => s.n);
    const rest = info.shots.filter(s => !chosen.includes(s.n));
    const key = e.run + '@' + e.date;
    const shown = info.shots.filter(s => chosen.includes(s.n)).concat(ui.more[key] ? rest : []);
    const logs = info.logs.map(l => `<button class="btn sm quiet ${ui.logs[e.run] === l ? 'on' : ''}" data-log="${esc(e.run)}" data-name="${l}">${l} log</button>`).join('');
    return `<div class="ev"><div class="ev-head"><h2>${esc(e.run)}</h2><span class="m">${esc(e.date)}${e.commit ? ' · ' + esc(e.commit) : ''} · ${esc(e.kind || 'shot')}${info.date ? ' · shot ' + esc(info.date) : ''}</span>
        ${e.note ? `<span class="m">${esc(e.note)}</span>` : ''}${info.about ? `<span class="about">${esc(info.about)}</span>` : ''}</div>
      ${info.exists ? '' : '<div class="empty">This run\'s folder is not on this machine (out/verify is not in git).</div>'}
      ${logs ? `<div class="logbar">${logs}</div><div id="log-${esc(e.run)}"></div>` : ''}
      <div class="shots">${shown.map(s => shotHtml(e.run, s, e === latest && verdictShots.has(s.n), refs.has(e.run + '#' + s.n))).join('')}</div>
      ${rest.length ? `<button class="btn sm quiet more" data-more="${esc(key)}">${ui.more[key] ? 'HIDE THE OTHER ' + rest.length : 'THE RUN\'S OTHER ' + rest.length + ' SHOTS'}</button>` : ''}
    </div>`;
  }).join('');
  for (const [run, name] of Object.entries(ui.logs)) showLog(run, name);
  markShot();
}

function shotHtml(run, s, isVerdict, isRef) {
  if (!s.file) return `<div class="shot" data-run="${esc(run)}" data-n="${s.n}"><div class="empty">Shot ${s.n} is not on disk.</div></div>`;
  return `<div class="shot ${isVerdict ? 'verdict-shot' : ''} ${isRef ? 'ref' : ''}" data-run="${esc(run)}" data-n="${s.n}">
    <img loading="lazy" src="/shot/${encodeURIComponent(run)}/${s.n}" alt="${esc(run)} shot ${s.n}">
    <div class="cap"><span class="n">#${s.n}</span>${s.at != null ? `<span>${s.at} s</span>` : ''}<span class="lab">${esc(s.label)}</span><span>${esc(run)}</span>${s.checks ? `<span class="chk">${esc(s.checks)}</span>` : ''}</div>
  </div>`;
}

function markShot(scroll) {
  const shots = $$('.shot');
  ui.shot = Math.max(0, Math.min(ui.shot, shots.length - 1));
  shots.forEach((el, k) => el.classList.toggle('cur', k === ui.shot));
  if (scroll && shots[ui.shot]) shots[ui.shot].scrollIntoView({ block: 'start', behavior: 'smooth' });
}

async function showLog(run, name) {
  const box = document.getElementById('log-' + run);
  if (!box) return;
  const res = await fetch(`/log/${encodeURIComponent(run)}/${name}`);
  box.innerHTML = `<div class="logbox">${esc(await res.text())}</div>`;
  const lb = $('.logbox', box);
  lb.scrollTop = lb.scrollHeight;
}

function zoom(shotEl, ev) {
  if (!shotEl || !$('img', shotEl)) return;
  const img = $('img', shotEl);
  const rect = img.getBoundingClientRect();
  const fx = ev ? (ev.clientX - rect.left) / rect.width : 0.5, fy = ev ? (ev.clientY - rect.top) / rect.height : 0.5;
  const z = document.createElement('div');
  z.className = 'zoom';
  z.innerHTML = `<img src="${img.src}"><div class="cap">#${shotEl.dataset.n} ${esc(shotEl.dataset.run)} · 1:1 · z or Esc closes</div>`;
  document.body.appendChild(z);
  const big = $('img', z);
  const place = () => { z.scrollLeft = big.naturalWidth * fx - innerWidth / 2; z.scrollTop = big.naturalHeight * fy - innerHeight / 2; };
  if (big.complete) place(); else big.onload = place;
  z.onclick = () => z.remove();
}

// ---------------------------------------------------------------- decisions
function hold(id, change) {
  const prev = held[id] || {};
  const next = { ...prev, ...change, at: new Date().toISOString().slice(0, 19) };
  if (next.status === 'confirmed' && next.score > 0 && next.score < 3) {
    toast('Confirmed under 3 is a not-right: press 2 for that.', true);
    return false;
  }
  held[id] = next;
  saveHeld();
  return true;
}

function decide(id, status) {
  if (!hold(id, { status, score: (held[id] && held[id].score) || 0 })) return;
  ui.askScore = true;
  render();
  if ((status === 'not-right' || status === 'cant-tell') && !(held[id].comment || '').trim()) $('#comment').focus();
}

function score(id, n) {
  if (!held[id]) { toast('Decide first (1-4).', true); return; }
  if (!hold(id, { score: n })) return;
  ui.askScore = false;
  if (!reviewProblem(held[id])) { render(); setTimeout(() => step(1), 180); } else render();
}

function step(d) {
  const r = route();
  if (r.view !== 'item') return;
  const k = nav.indexOf(r.id);
  const next = nav[k + d];
  if (next) go('#/item/' + encodeURIComponent(next));
  else if (d > 0) { toast('The end of the queue.'); go('#/'); }
}

function toggleShotRef(id) {
  const el = $$('.shot')[ui.shot];
  if (!el) return;
  if (!held[id]) { toast('Decide first (1-4), then mark the shots it is about.', true); return; }
  const ref = { run: el.dataset.run, shot: +el.dataset.n };
  const shots = (held[id].shots || []).filter(s => !(s.run === ref.run && s.shot === ref.shot));
  if (shots.length === (held[id].shots || []).length) shots.push(ref);
  hold(id, { shots });
  el.classList.toggle('ref');
  render();
}

// ---------------------------------------------------------------- save
function saveView() {
  const entries = Object.entries(held);
  if (!entries.length) return '<div class="content"><div class="empty">Nothing held. Decide items in the queue first.</div></div>';
  const valid = entries.filter(([id, r]) => byId(id) && !reviewProblem(r));
  const blocked = entries.filter(([id, r]) => !byId(id) || reviewProblem(r));
  const section = (title, list, why) => list.length ? `<div class="save-group"><h2>${title} <span class="mono muted" style="font-size:13px">${list.length}</span></h2><div class="panel">${list.map(([id, r]) => {
    const i = byId(id);
    return `<div class="save-row"><a href="#/item/${encodeURIComponent(id)}">${esc(i ? i.title : id)}<span class="id">${esc(id)}</span></a>
      <span class="mono">${r.score ? r.score + ' ' + SCORE_NAMES[r.score] : ''}${r.flag ? ' <span class="bad">FLAG</span>' : ''}</span>
      <span class="c">${why ? `<span class="bad mono" style="font-size:12px">${esc(i ? reviewProblem(r) : 'no such item')}</span> ` : ''}${esc(r.comment || '')}</span>
      <button class="btn sm quiet" data-discard="${esc(id)}">DISCARD</button></div>`;
  }).join('')}</div></div>` : '';
  return `<div class="content">
    <div style="display:flex;align-items:center;gap:16px;margin-bottom:18px">
      <h1 style="margin:0;font-size:30px">Save ${valid.length} review${valid.length === 1 ? '' : 's'}</h1>
      <button class="btn primary" data-save ${valid.length ? '' : 'disabled'}>WRITE ${esc(data.file)} <span class="k">ENTER</span></button>
      <span class="mono muted" style="font-size:12px">each item's status and review, nothing else; refused if the file changed on disk</span>
    </div>
    ${DECISIONS.map(([s, l]) => section(l, valid.filter(([, r]) => r.status === s))).join('')}
    ${section('Not ready to save', blocked, true)}
  </div>`;
}

async function save() {
  const valid = heldValid();
  if (!valid.length) return;
  const reviews = valid.map(([id, r]) => ({ id, status: r.status, review: { at: r.at, score: r.score || 0, comment: r.comment || '', flag: !!r.flag, shots: r.shots || [] } }));
  try {
    const res = await api('/api/reviews', { method: 'POST', body: { digest: data.digest, reviews } });
    for (const [id] of valid) delete held[id];
    saveHeld();
    await reload();
    toast(`Saved ${res.saved} review${res.saved === 1 ? '' : 's'}.`);
    go('#/');
  } catch (e) {
    if (e.status === 409) {
      await reload();
      toast('The items file changed on disk (an agent ran?). Reloaded it; your held reviews are kept: check them and save again.', true);
    } else toast('Not saved: ' + e.message, true);
  }
}

// ---------------------------------------------------------------- help
function help() {
  const keys = [['1 2 3 4', 'confirmed, not right, can\'t tell, later'], ['0-5', 'right after a decision: the score (1 broken ... 5 ship it, 0 none); moves on'],
    ['c', 'the comment (Enter or Esc leaves it)'], ['f', 'flag: come back to this'], ['s', 'mark the current shot as what the review is about'], ['u', 'undo the held review'],
    ['← →', 'the shot cursor'], ['z', 'the shot under the mouse (or the cursor) at 1:1'], ['j k ↑ ↓', 'next and previous item (in the queue: the row)'],
    ['Enter', 'open the row (queue); save (Save page)'], ['Esc', 'back to the queue'], ['Ctrl+S', 'the Save page']];
  const bg = document.createElement('div');
  bg.className = 'help-bg';
  bg.innerHTML = `<div class="help"><h2>Keys</h2><table>${keys.map(([k, v]) => `<tr><td><span class="kbd">${esc(k)}</span></td><td>${esc(v)}</td></tr>`).join('')}</table></div>`;
  bg.onclick = () => bg.remove();
  document.body.appendChild(bg);
}

// ---------------------------------------------------------------- events
document.addEventListener('click', e => {
  const t = e.target.closest('[data-go],[data-open],[data-chip],[data-toggle],[data-collapse],[data-clear],[data-decide],[data-score],[data-flag],[data-undo],[data-save],[data-discard],[data-log],[data-more],[data-help]');
  const shot = e.target.closest('.shot img');
  if (shot && !t) { const el = shot.closest('.shot'); ui.shot = $$('.shot').indexOf(el); markShot(); zoom(el, e); return; }
  if (!t) return;
  const id = route().id;
  const d = t.dataset;
  if (d.go !== undefined) go(d.go);
  else if (d.open) { nav = visibleOrder(); cursor = nav.indexOf(d.open); go('#/item/' + encodeURIComponent(d.open)); }
  else if (d.chip) { const list = prefs[d.chip]; const k = list.indexOf(d.v); if (k >= 0) list.splice(k, 1); else list.push(d.v); savePrefs(); render(); }
  else if (d.toggle) { prefs[d.toggle] = !prefs[d.toggle]; savePrefs(); render(); }
  else if (d.collapse) { const k = prefs.collapsed.indexOf(d.collapse); if (k >= 0) prefs.collapsed.splice(k, 1); else prefs.collapsed.push(d.collapse); savePrefs(); render(); }
  else if (d.clear !== undefined) { Object.assign(prefs, { areas: [], kinds: [], statuses: [], needs: [], run: '', from: '', to: '', flagged: false, evidence: false }); savePrefs(); render(); }
  else if (d.decide) decide(id, d.decide);
  else if (d.score) score(id, +d.score);
  else if (d.flag !== undefined) { if (held[id]) { hold(id, { flag: !held[id].flag }); render(); } else toast('Decide first (1-4).', true); }
  else if (d.undo !== undefined) { delete held[id]; saveHeld(); render(); }
  else if (d.save !== undefined) save();
  else if (d.discard) { delete held[d.discard]; saveHeld(); render(); }
  else if (d.log) { ui.logs[d.log] = ui.logs[d.log] === d.name ? undefined : d.name; if (ui.logs[d.log]) showLog(d.log, d.name); else document.getElementById('log-' + d.log).innerHTML = ''; afterItem(id); }
  else if (d.more) { ui.more[d.more] = !ui.more[d.more]; afterItem(id); }
  else if (d.help !== undefined) help();
});

document.addEventListener('change', e => {
  const p = e.target.dataset.pref;
  if (p) { prefs[p] = e.target.value; savePrefs(); render(); }
});

document.addEventListener('input', e => {
  if (e.target.id === 'comment') {
    const id = route().id;
    if (!held[id]) { held[id] = { status: '', score: 0 }; }
    held[id].comment = e.target.value;
    held[id].at = new Date().toISOString().slice(0, 19);
    saveHeld();
    const p = $('.problem');
    if (p) p.textContent = reviewProblem(held[id]) || '';
  }
});

document.addEventListener('mouseover', e => { const s = e.target.closest('.shot'); if (s) ui.hover = s; });
document.addEventListener('mousemove', e => { ui.mouse = e; });

document.addEventListener('keydown', e => {
  const r = route();
  const zoomed = $('.zoom'), helpBox = $('.help-bg');
  if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); go('#/save'); return; }
  if (e.target.matches('textarea, input, select')) {
    if (e.key === 'Escape' || (e.key === 'Enter' && !e.shiftKey && e.target.id === 'comment')) {
      e.preventDefault();
      e.target.blur();
      if (e.target.id === 'comment') render();
    }
    return;
  }
  if (helpBox) { helpBox.remove(); return; }
  if (zoomed) { if (e.key === 'Escape' || e.key === 'z') zoomed.remove(); return; }
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.key === '?') { help(); return; }
  if (r.view === 'queue') {
    const order = visibleOrder();
    if (e.key === 'j' || e.key === 'ArrowDown') { cursor = Math.min(cursor + 1, order.length - 1); render(); e.preventDefault(); }
    else if (e.key === 'k' || e.key === 'ArrowUp') { cursor = Math.max(cursor - 1, 0); render(); e.preventDefault(); }
    else if (e.key === 'Enter' && order[cursor]) { nav = order; go('#/item/' + encodeURIComponent(order[cursor])); }
    return;
  }
  if (r.view === 'save') {
    if (e.key === 'Enter') save();
    else if (e.key === 'Escape') go('#/');
    return;
  }
  const id = r.id;
  if (!byId(id)) { if (e.key === 'Escape') go('#/'); return; }
  if (/^[0-5]$/.test(e.key) && ui.askScore) { e.preventDefault(); score(id, +e.key); return; }
  if (/^[1-4]$/.test(e.key)) { e.preventDefault(); decide(id, DECIDED[+e.key - 1]); return; }
  if (e.key === '0' && held[id]) { score(id, 0); return; }
  switch (e.key) {
    case 'c': e.preventDefault(); $('#comment').focus(); break;
    case 'f': if (held[id]) { hold(id, { flag: !held[id].flag }); render(); } else toast('Decide first (1-4).', true); break;
    case 'u': delete held[id]; saveHeld(); ui.askScore = false; render(); break;
    case 's': toggleShotRef(id); break;
    case 'z': {
      const over = ui.hover && document.body.contains(ui.hover) && ui.mouse && ui.hover.matches(':hover') ? ui.hover : $$('.shot')[ui.shot];
      zoom(over, over === ui.hover ? ui.mouse : null); break;
    }
    case 'ArrowRight': e.preventDefault(); ui.shot++; markShot(true); break;
    case 'ArrowLeft': e.preventDefault(); ui.shot--; markShot(true); break;
    case 'j': case 'ArrowDown': e.preventDefault(); ui.askScore = false; step(1); break;
    case 'k': case 'ArrowUp': e.preventDefault(); ui.askScore = false; step(-1); break;
    case 'Escape': if (ui.askScore) { ui.askScore = false; render(); } else go('#/'); break;
    default: break;
  }
});

// ---------------------------------------------------------------- start
async function reload() {
  data = await api('/api/items');
  render();
}

reload().catch(e => { $('#app').innerHTML = `<div class="empty">The verifier's server did not answer: ${esc(e.message)}</div>`; });

'use strict';
/* ApexSim verifier: one page, no framework (docs/verifier.md). The queue, an
   item's evidence beside the agent's verdict, and Save. Reviews are held in
   the page (and in localStorage, so a reload keeps them) until Save writes
   them all into the items file at once. Sitting mode (#/sit) walks one
   sittings.yml block as a checklist of big answer buttons, for a tablet
   beside the wheel (serve --lan). */

const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => [...el.querySelectorAll(sel)];
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const prose = s => esc(s).replace(/`([^`]+)`/g, '<code>$1</code>');

const DECISIONS = [['confirmed', 'Confirmed'], ['not-right', 'Not right'], ['cant-tell', "Can't tell"], ['later', 'Later']];
const DECIDED = DECISIONS.map(d => d[0]);
const GROUPS = [
  ['ready', 'Ready to review', 'an agent verdict on the latest evidence: one keystroke each'],
  ['evidence', 'Evidence, no verdict', 'shots to look at with no fresh agent verdict'],
  ['waiting', 'Waiting for a run', 'nothing shot yet, or shot before the item\'s commit (greyed, the run to repeat named)'],
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
let sittings = null;                    // /api/sittings, loaded on the first #/sit
let sitCursor = 0;                      // the check under the cursor in a sitting block

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

const staleRun = item => (data.stale || {})[item.id];

function group(item) {
  if (item.status === 'later') return 'later';
  if (['confirmed', 'not-right', 'cant-tell'].includes(item.status)) return 'reviewed';
  if (staleRun(item)) return 'waiting';
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
  if (h === 'sit') return { view: 'sit' };
  if (h.startsWith('sit/')) return { view: 'sit', block: decodeURIComponent(h.slice(4)) };
  return { view: 'queue' };
}
function go(hash) { if (location.hash !== hash) location.hash = hash; else render(); }
window.addEventListener('hashchange', () => { ui = { shot: 0, askScore: false, hover: null, logs: {}, more: {} }; sitCursor = 0; render(); });

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
    <button class="btn quiet" data-go="#/sit">SITTING</button>
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
  if (r.view === 'sit' && !sittings) { loadSittings(); return; }
  const body = r.view === 'item' ? itemView(r.id) : r.view === 'save' ? saveView() : r.view === 'sit' ? sitView(r.block) : queueView();
  const missing = Object.keys(held).filter(id => !byId(id));
  $('#app').innerHTML = `<div class="main">${topBar()}${missing.length
    ? `<div class="banner">Held reviews for items no longer in the file: ${missing.map(esc).join(', ')} (discard them on the Save page)</div>` : ''}${body}</div>`;
  if (r.view === 'item') afterItem(r.id);
  if (r.view === 'queue') { const row = $('.row.cur'); if (row) row.scrollIntoView({ block: 'nearest' }); }
  if (r.view === 'sit') { const c = $('.check.cur'); if (c) c.scrollIntoView({ block: 'nearest' }); }
}

// ---------------------------------------------------------------- sitting mode
async function loadSittings() {
  try { sittings = (await api('/api/sittings')).blocks; } catch (e) { sittings = []; toast('No sittings: ' + e.message, true); }
  render();
}

// The answer as the server will write it (schema.answer_review): the first word confirms.
function answer(id, answers, word, note) {
  const ok = word === answers[0];
  note = (note || '').trim();
  held[id] = { status: ok ? 'confirmed' : 'not-right', answer: word, note, comment: ok ? note : (note ? `${word}: ${note}` : word),
    flag: (held[id] || {}).flag || false, at: new Date().toISOString().slice(0, 19) };
  saveHeld();
}

const sitAnswer = id => (held[id] && held[id].answer) || (byId(id) && byId(id).review && byId(id).review.answer) || '';

function sitView(blockId) {
  if (!blockId) {
    return `<div class="content sit"><h1 class="sit-h">Sitting</h1>${sittings.map(b => {
      const done = b.checks.filter(c => sitAnswer(c.item)).length;
      return `<div class="panel sit-block" data-go="#/sit/${encodeURIComponent(b.id)}"><div class="t">${esc(b.id)}</div>
        <div class="a">${esc(b.about || '')}</div><div class="m">sitting ${esc(b.sitting || 1)} · ~${esc(b.minutes || '?')} min · ${done} / ${b.checks.length} answered${b.marks ? ` · marks of ${esc(b.marks.date)}` : ''}</div></div>`;
    }).join('')}</div>`;
  }
  const block = sittings.find(b => b.id === blockId);
  if (!block) return `<div class="content"><div class="empty">No sitting block ${esc(blockId)}. <a href="#/sit">The blocks</a></div></div>`;
  sitCursor = Math.max(0, Math.min(sitCursor, block.checks.length - 1));
  const checks = block.checks.map((c, k) => {
    const item = byId(c.item);
    const given = sitAnswer(c.item);
    const h = held[c.item];
    return `<div class="check ${k === sitCursor ? 'cur' : ''} ${h ? 'held' : ''}" data-sitcheck="${k}">
      <div class="q"><span class="n">${k + 1}</span>${esc(c.check)}${item ? '' : ' <span class="bad mono">(no item ' + esc(c.item) + ')</span>'}</div>
      <div class="answers">${c.answers.map((w, n) => `<button class="btn big ${given === w ? (n === 0 ? 'on ok' : 'on no') : ''}" data-answer="${esc(w)}" data-item="${esc(c.item)}" data-block="${esc(block.id)}">${esc(w)}<span class="k">${n + 1}</span></button>`).join('')}</div>
      <input class="input sit-note" data-note="${esc(c.item)}" placeholder="a note (n), the MARK number if one was pressed" value="${esc((h && h.note) || '')}">
      ${!h && item && item.review ? `<div class="mono muted saved">saved: ${esc(item.review.answer || item.status)}${item.review.comment ? ' · ' + esc(item.review.comment) : ''}</div>` : ''}
    </div>`;
  }).join('');
  const marks = block.marks ? `<div class="panel marks"><h2>MARK and force lines, ${esc(block.marks.date)}</h2>${block.marks.lines.length
    ? block.marks.lines.map(l => `<div class="mono">${esc(l)}</div>`).join('') : '<div class="muted">None pressed.</div>'}</div>` : '';
  return `<div class="content sit">
    <div class="sit-head"><a href="#/sit">‹ blocks</a><h1 class="sit-h">${esc(block.id)}</h1><span class="muted">${esc(block.about || '')}</span></div>
    ${block.setup ? `<div class="setup">${prose(block.setup)}</div>` : ''}
    <div class="mono muted keys">python scripts/verify/run_checks.py --sit ${esc(block.id)} · <span class="kbd">↑</span><span class="kbd">↓</span> check · <span class="kbd">1</span>-<span class="kbd">9</span> answer · <span class="kbd">n</span> note · Save when the block is done</div>
    ${checks}
    ${marks}
  </div>`;
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
  const st = staleRun(i);
  return `<div class="row ${cur ? 'cur' : ''} ${h ? 'held' : ''} ${isFlagged(i) ? 'flagged' : ''} ${st ? 'stale' : ''}" data-open="${esc(i.id)}"${st ? ` title="shot before the item's commit: repeat ${esc(st)}"` : ''}>
    <span class="mark"></span>
    <div class="cell"><span class="t">${esc(i.title)}</span><span class="id">${esc(i.id)}</span></div>
    <span class="cell hide-n">${esc(i.area)}</span>
    <span class="cell hide-n" title="${esc(i.check_by)}">${esc(i.check_by)}</span>
    <span class="cell">${v ? `<span class="says ${v.says}">${esc(v.says)}</span>` : ''}</span>
    <span class="cell hide-n">${st ? '<span class="warn">repeat</span>' : ev ? ev + ' run' + (ev > 1 ? 's' : '') : ''}</span>
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
      ${staleRun(i) ? `<div class="stale-note">The newest evidence is older than the item's commit ${esc(i.commit || '')}: repeat <b>${esc(staleRun(i))}</b>.</div>` : ''}
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
        <span class="kbd">←</span><span class="kbd">→</span> shots · <span class="kbd">z</span> 1:1 · <span class="kbd">b</span> before / after · <span class="kbd">o</span> copy the replay command<br>
        <span class="kbd">j</span><span class="kbd">k</span> items · <span class="kbd">Esc</span> queue · <span class="kbd">?</span> all keys</div>
    </div>`;
}

// A run's shots as they were at `commit` (a kept generation once the run has shot again).
async function runInfo(run, commit) {
  const key = run + '@' + (commit || '');
  if (!runs[key]) runs[key] = await api('/api/runs/' + encodeURIComponent(run) + (commit ? '?commit=' + encodeURIComponent(commit) : ''))
    .catch(() => ({ run, shots: [], logs: [], exists: false, history: [] }));
  return runs[key];
}

const shotUrl = (run, n, commit) => `/shot/${encodeURIComponent(run)}/${n}${commit ? '?commit=' + encodeURIComponent(commit) : ''}`;

async function afterItem(id) {
  const i = byId(id);
  const box = $('#evidence');
  if (!i || !(i.evidence || []).length) return;
  const evs = [...i.evidence].sort((a, b) => String(b.date).localeCompare(String(a.date)));
  const infos = await Promise.all(evs.map(e => runInfo(e.run, e.commit)));
  if (route().id !== id) return;
  ui.replay = (infos.find(x => x.replay) || {}).replay || '';
  if (ui.pair) { await pairView(evs, infos, box); return; }
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
    return `<div class="ev"><div class="ev-head"><h2>${esc(e.run)}</h2><span class="m">${esc(e.date)}${e.commit ? ' · ' + esc(e.commit) : ''} · ${esc(e.kind || 'shot')}${info.date ? ' · shot ' + esc(info.date) : ''}${info.kept ? ' · kept from history' : ''}</span>
        ${info.replay ? `<button class="btn sm quiet" data-copy="${esc(info.replay)}">COPY REPLAY COMMAND <span class="k">O</span></button>` : ''}
        ${e.note ? `<span class="m">${esc(e.note)}</span>` : ''}${info.about ? `<span class="about">${esc(info.about)}</span>` : ''}</div>
      ${info.exists ? '' : '<div class="empty">This run\'s folder is not on this machine (out/verify is not in git).</div>'}
      ${logs ? `<div class="logbar">${logs}</div><div id="log-${esc(e.run)}"></div>` : ''}
      <div class="shots">${shown.map(s => shotHtml(e.run, s, e === latest && verdictShots.has(s.n), refs.has(e.run + '#' + s.n), info.kept ? e.commit : '')).join('')}</div>
      ${rest.length ? `<button class="btn sm quiet more" data-more="${esc(key)}">${ui.more[key] ? 'HIDE THE OTHER ' + rest.length : 'THE RUN\'S OTHER ' + rest.length + ' SHOTS'}</button>` : ''}
    </div>`;
  }).join('');
  for (const [run, name] of Object.entries(ui.logs)) showLog(run, name);
  markShot();
}

function shotHtml(run, s, isVerdict, isRef, commit, tag) {
  if (!s || !s.file) return `<div class="shot" data-run="${esc(run)}" data-n="${s ? s.n : ''}"><div class="empty">${s ? `Shot ${s.n} is not on disk.` : 'No shot to match.'}</div></div>`;
  return `<div class="shot ${isVerdict ? 'verdict-shot' : ''} ${isRef ? 'ref' : ''}" data-run="${esc(run)}" data-n="${s.n}">
    <img loading="lazy" src="${shotUrl(run, s.n, commit)}" alt="${esc(run)} shot ${s.n}">
    <div class="cap">${tag ? `<span class="tag">${esc(tag)}</span>` : ''}<span class="n">#${s.n}</span>${s.at != null ? `<span>${s.at} s</span>` : ''}<span class="lab">${esc(s.label)}</span><span>${esc(run)}</span>${s.checks ? `<span class="chk">${esc(s.checks)}</span>` : ''}</div>
  </div>`;
}

// Before and after: the newest evidence beside the one before it (another
// run, or the same run at another commit), shot for shot; with a single
// piece of evidence, its run's previous generation from history/.
async function pairView(evs, infos, box) {
  const after = { ev: evs[0], info: infos[0] };
  let before = null;
  const other = evs.findIndex((e, k) => k > 0 && (e.run !== after.ev.run || e.commit !== after.ev.commit));
  if (other > 0) before = { ev: evs[other], info: infos[other] };
  else {
    const prev = (after.info.history || []).find(h => !h.current && h.commit && h.commit !== after.ev.commit);
    if (prev) before = { ev: { run: after.ev.run, commit: prev.commit, date: prev.date }, info: await runInfo(after.ev.run, prev.commit) };
  }
  if (!before) {
    box.innerHTML = '<div class="empty">Nothing to compare: one piece of evidence and no earlier shots of its run kept. <span class="kbd">b</span> goes back.</div>';
    return;
  }
  const chosen = (after.ev.shots || []).length ? after.ev.shots : after.info.shots.map(s => s.n);
  const sameRun = before.ev.run === after.ev.run;
  const beforeChosen = (before.ev.shots || []).length ? before.ev.shots : before.info.shots.map(s => s.n);
  const label = x => `${x.ev.run} · ${x.ev.date || ''}${x.ev.commit ? ' · ' + x.ev.commit : ''}`;
  const pairs = chosen.map((n, k) => {
    const a = after.info.shots.find(s => s.n === n);
    const bn = sameRun ? n : beforeChosen[k];
    const b = before.info.shots.find(s => s.n === bn);
    return `<div class="shots pair">${shotHtml(before.ev.run, b, false, false, before.info.kept ? before.ev.commit : '', 'before')}${shotHtml(after.ev.run, a, false, false, after.info.kept ? after.ev.commit : '', 'after')}</div>`;
  });
  box.innerHTML = `<div class="ev"><div class="ev-head"><h2>Before and after</h2><span class="m">before: ${esc(label(before))}</span><span class="m">after: ${esc(label(after))}</span>
    <span class="m">${sameRun ? 'shot for shot' : 'paired in order'} · <span class="kbd">b</span> back</span></div>
    <div class="pairs">${pairs.join('')}</div></div>`;
  markShot();
}

async function copyText(text) {
  try { await navigator.clipboard.writeText(text); } catch (_) {
    // Not a secure context (the LAN): the old way.
    const t = document.createElement('textarea');
    t.value = text; document.body.appendChild(t); t.select();
    document.execCommand('copy'); t.remove();
  }
  toast('Copied: ' + text);
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
      <span class="mono">${r.answer ? 'answered ' + esc(r.answer) : r.score ? r.score + ' ' + SCORE_NAMES[r.score] : ''}${r.flag ? ' <span class="bad">FLAG</span>' : ''}</span>
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
  const reviews = valid.map(([id, r]) => r.answer ? { id, answer: r.answer, note: r.note || '', at: r.at, flag: !!r.flag }
    : { id, status: r.status, review: { at: r.at, score: r.score || 0, comment: r.comment || '', flag: !!r.flag, shots: r.shots || [] } });
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
    ['← →', 'the shot cursor'], ['z', 'the shot under the mouse (or the cursor) at 1:1'], ['b', 'before and after: the newest evidence beside the one before it'],
    ['o', 'copy the command that opens the run\'s replay in the game'], ['j k ↑ ↓', 'next and previous item (in the queue: the row)'],
    ['Enter', 'open the row (queue); save (Save page)'], ['Esc', 'back to the queue'], ['Ctrl+S', 'the Save page']];
  const bg = document.createElement('div');
  bg.className = 'help-bg';
  bg.innerHTML = `<div class="help"><h2>Keys</h2><table>${keys.map(([k, v]) => `<tr><td><span class="kbd">${esc(k)}</span></td><td>${esc(v)}</td></tr>`).join('')}</table></div>`;
  bg.onclick = () => bg.remove();
  document.body.appendChild(bg);
}

// ---------------------------------------------------------------- events
document.addEventListener('click', e => {
  const t = e.target.closest('[data-go],[data-open],[data-chip],[data-toggle],[data-collapse],[data-clear],[data-decide],[data-score],[data-flag],[data-undo],[data-save],[data-discard],[data-log],[data-more],[data-help],[data-copy],[data-answer],[data-sitcheck]');
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
  else if (d.copy) copyText(d.copy);
  else if (d.answer) {
    const check = sittings.find(b => b.id === d.block).checks.find(c => c.item === d.item);
    answer(d.item, check.answers, d.answer, (held[d.item] || {}).note);
    sitCursor = +t.closest('.check').dataset.sitcheck;
    render();
  } else if (d.sitcheck !== undefined && !e.target.closest('input')) { sitCursor = +d.sitcheck; render(); }
});

document.addEventListener('change', e => {
  const p = e.target.dataset.pref;
  if (p) { prefs[p] = e.target.value; savePrefs(); render(); }
});

document.addEventListener('input', e => {
  const noteFor = e.target.dataset && e.target.dataset.note;
  if (noteFor) {
    const h = held[noteFor];
    if (h && h.answer) {
      const block = sittings.find(b => b.checks.some(c => c.item === noteFor));
      answer(noteFor, block.checks.find(c => c.item === noteFor).answers, h.answer, e.target.value);
    } else { held[noteFor] = { ...(h || {}), status: (h || {}).status || '', note: e.target.value, at: new Date().toISOString().slice(0, 19) }; saveHeld(); }
    return;
  }
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
  if (r.view === 'sit') {
    const block = r.block && sittings && sittings.find(b => b.id === r.block);
    if (e.key === 'Escape') { go(r.block ? '#/sit' : '#/'); return; }
    if (!block) return;
    const check = block.checks[sitCursor];
    if (e.key === 'j' || e.key === 'ArrowDown') { sitCursor = Math.min(sitCursor + 1, block.checks.length - 1); render(); e.preventDefault(); }
    else if (e.key === 'k' || e.key === 'ArrowUp') { sitCursor = Math.max(sitCursor - 1, 0); render(); e.preventDefault(); }
    else if (/^[1-9]$/.test(e.key) && check && check.answers[+e.key - 1]) {
      answer(check.item, check.answers, check.answers[+e.key - 1], (held[check.item] || {}).note);
      sitCursor = Math.min(sitCursor + 1, block.checks.length - 1);
      render();
    } else if (e.key === 'n') { e.preventDefault(); const el = $$('.check')[sitCursor]; if (el) $('.sit-note', el).focus(); }
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
    case 'b': ui.pair = !ui.pair; afterItem(id); break;
    case 'o': if (ui.replay) copyText(ui.replay); else toast('No replay kept by this item\'s runs.', true); break;
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

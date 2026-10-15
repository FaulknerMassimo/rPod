// rPod's web UI (docs/PLAN.md §8.6): music uploads, the library, and the
// settings a click wheel can't reach, in whichever look the device has.
//
// No framework and no build step: the device serves these files as they
// are (web/rpod_web.c). Screens drill down like the iPod's menus -- each
// one's title is the row that opened it, the arrow keys move the
// selection the way the wheel does, Enter is Center and Escape is Menu.
'use strict';

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'text') el.textContent = v;
    else if (k === 'value') el.value = v;
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
    else if (typeof v === 'boolean' || typeof v === 'number') {
      if (k in el) el[k] = v;
      else el.setAttribute(k, String(v));
    } else el.setAttribute(k, v);
  }
  for (const kid of kids.flat(Infinity)) {
    if (kid == null || kid === false) continue;
    el.append(kid instanceof Node ? kid : String(kid));
  }
  return el;
}

const icon = (name, cls = '') => h('span', { class: `icon i-${name} ${cls}`.trim(), 'aria-hidden': 'true' });

const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });

function fmtTime(s) {
  s = Math.max(0, Math.floor(s || 0));
  const hh = Math.floor(s / 3600);
  const mm = Math.floor((s % 3600) / 60);
  const ss = String(s % 60).padStart(2, '0');
  return hh ? `${hh}:${String(mm).padStart(2, '0')}:${ss}` : `${mm}:${ss}`;
}

function fmtBytes(n) {
  if (n == null || n < 0) return '—';
  const units = ['bytes', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  while (n >= 1000 && i < units.length - 1) {
    n /= 1000;
    i++;
  }
  return i === 0 ? `${n} bytes` : `${n.toFixed(n < 10 ? 1 : 0)} ${units[i]}`;
}

function fmtDuration(s) {
  s = Math.floor(s || 0);
  const d = Math.floor(s / 86400);
  const hh = Math.floor((s % 86400) / 3600);
  const mm = Math.floor((s % 3600) / 60);
  if (d) return `${d} day${d === 1 ? '' : 's'}, ${hh} hr`;
  if (hh) return `${hh} hr ${mm} min`;
  return `${mm} min`;
}

function fmtDate(t) {
  if (!t) return '—';
  return new Date(t * 1000).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
}

function plural(n, one, many) {
  return `${n.toLocaleString()} ${n === 1 ? one : many || one + 's'}`;
}

function debounce(fn, ms) {
  let t;
  return (...args) => {
    clearTimeout(t);
    t = setTimeout(() => fn(...args), ms);
  };
}

function joinPath(a, b) {
  return a ? (b ? `${a}/${b}` : a) : b;
}

function dirname(p) {
  const i = p.lastIndexOf('/');
  return i < 0 ? '' : p.slice(0, i);
}

function basename(p) {
  return p.slice(p.lastIndexOf('/') + 1);
}

function stripExt(name) {
  const i = name.lastIndexOf('.');
  return i > 0 ? name.slice(0, i) : name;
}

function store(key, value) {
  try {
    if (value === undefined) return localStorage.getItem(key);
    localStorage.setItem(key, value);
  } catch (e) {
    // Storage blocked: remembered for this page only.
  }
  return null;
}

/* ---------------------------------------------------------------------------
 * The API (web/api.c)
 * ------------------------------------------------------------------------- */

class ApiError extends Error {
  constructor(message, status) {
    super(message);
    this.status = status;
  }
}

async function call(method, path, params) {
  const opts = { method, cache: 'no-store', headers: {} };
  if (method !== 'GET') {
    // The header a page from elsewhere can't send: what lets the server
    // tell rPod's own page apart.
    opts.headers['X-Rpod'] = '1';
    opts.headers['Content-Type'] = 'application/x-www-form-urlencoded';
    opts.body = new URLSearchParams(params || {}).toString();
  }
  let r;
  try {
    r = await fetch(path, opts);
  } catch (e) {
    throw new ApiError("Can't reach rPod", 0);
  }
  let body = null;
  try {
    body = await r.json();
  } catch (e) {
    // Not JSON: the status says enough.
  }
  if (!r.ok) throw new ApiError((body && body.error) || `Error ${r.status}`, r.status);
  return body;
}

const GET = (path, q) => call('GET', q ? `${path}?${new URLSearchParams(q)}` : path);
const POST = (path, params) => call('POST', path, params);
const coverUrl = (uri) => `/api/cover?uri=${encodeURIComponent(uri)}`;

/* ---------------------------------------------------------------------------
 * State, theme and the iPod's assets
 * ------------------------------------------------------------------------- */

const S = {
  theme: null,
  st: null, // the last /api/state
  stAt: 0, // when it arrived
  settings: null,
  reachable: true,
};

const THEMES = [
  ['glass', 'Glass'],
  ['classic', 'Classic'],
  ['classic-dark', 'Classic Dark'],
];

function look() {
  return S.theme === 'glass' ? 'glass' : 'classic';
}

function applyTheme(t) {
  if (!THEMES.some(([k]) => k === t)) t = 'glass';
  if (S.theme === t) return;
  const relook = S.theme !== null && (S.theme === 'glass') !== (t === 'glass');
  S.theme = t;
  const root = document.documentElement;
  root.dataset.theme = t;
  root.dataset.look = t === 'glass' ? 'glass' : 'classic';
  document.querySelector('meta[name="theme-color"]').setAttribute('content', t === 'classic' ? '#ffffff' : '#000000');
  store('rpod-theme', t);
  // Glass and Classic lay some screens out differently: build them again.
  if (relook) rerender();
}

// `make ipod-assets` puts the iPod's Chicago, Pixo and bitmaps under /ipod/
// (re/ipod4/tools/ipod4re/webfont.py). Without them Classic still works,
// with look-alikes.
async function loadIpodAssets() {
  let m;
  try {
    m = await GET('/ipod/manifest.json');
  } catch (e) {
    return;
  }
  const root = document.documentElement;
  const have = new Set((m.icons || []).map((i) => i.name));
  const icons = {
    arrow: 'menu_arrow',
    play: 'status_play',
    pause: 'status_pause',
    battery: 'battery_level_00',
    repeat: 'status_repeat',
    'repeat-one': 'status_repeat_one',
    shuffle: 'status_shuffle',
    'vol-low': 'volume_low',
    'vol-high': 'volume_high',
    lock: 'status_lock',
    diamond: 'scrub_diamond',
    sync: 'sync_arrows_anim',
    bell: 'alarm_bell',
  };
  for (const [k, v] of Object.entries(icons)) {
    if (have.has(v)) root.style.setProperty(`--ipod-${k}`, `url("/ipod/icons/${v}.svg")`);
  }
  for (const [key, cls] of [
    ['chicago', 'has-chicago'],
    ['pixo', 'has-pixo'],
  ]) {
    const f = m.fonts && m.fonts[key];
    if (!f) continue;
    try {
      const face = new FontFace(f.family, `url("/ipod/${encodeURIComponent(f.file)}")`, { display: 'block' });
      await face.load();
      document.fonts.add(face);
      root.classList.add(cls);
    } catch (e) {
      // A font that won't load leaves the stand-in.
    }
  }
}

/* ---------------------------------------------------------------------------
 * The title bar and the HUD
 * ------------------------------------------------------------------------- */

const tb = {
  title: document.getElementById('tb-title'),
  back: document.getElementById('back'),
  state: document.getElementById('tb-state'),
  clock: document.getElementById('tb-clock'),
  up: document.getElementById('tb-up'),
};

function setTitle(t) {
  tb.title.textContent = t;
  document.title = t === 'rPod' ? 'rPod' : `${t} – rPod`;
}

function tickClock() {
  const d = new Date();
  tb.clock.textContent = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

// The play state at the left (Classic), the iPod's turning arrows while the
// library updates, and the equalizer while something plays (Glass).
function paintTitleState() {
  const st = S.st;
  const state = st && st.player ? st.player.state : 'stop';
  const busy = (st && st.updating) || UP.busy();
  document.body.classList.toggle('playing', state === 'play');
  let cls = '';
  if (busy) cls = 'i-sync';
  else if (state === 'play') cls = 'i-play';
  else if (state === 'pause') cls = 'i-pause';
  tb.state.className = `icon tb-state ${cls}`;
  tb.state.hidden = !cls;

  const up = UP.summary();
  if (up) tb.up.textContent = look() === 'glass' ? `↑ ${up}` : up;
  else if (st && st.updating) tb.up.textContent = look() === 'glass' ? 'Updating' : '';
  else tb.up.textContent = '';
  tb.up.hidden = !tb.up.textContent;
}

let hudTimer;
function hud(msg, bad = false) {
  const el = document.getElementById('hud');
  el.textContent = msg;
  el.classList.toggle('bad', bad);
  el.classList.add('show');
  clearTimeout(hudTimer);
  hudTimer = setTimeout(() => el.classList.remove('show'), bad ? 4000 : 2600);
}

function oops(e) {
  hud(e instanceof ApiError ? e.message : String(e), true);
}

/* ---------------------------------------------------------------------------
 * Lists: rows with one selected, moved by the arrow keys like the wheel
 * ------------------------------------------------------------------------- */

let activeList = null;
const selMemory = new Map(); // route -> selected row index, for coming back

function makeRow(r) {
  if (r.header) {
    return h('div', { class: 'row header', role: 'presentation' }, h('span', { class: 'text' }, h('span', { class: 'label', text: r.header })));
  }
  const end = [];
  if (r.duration != null) end.push(h('span', { class: 'duration', text: r.duration }));
  if (r.state) end.push(r.state);
  const row = h(
    'div',
    {
      class: ['row', r.sub ? 'two' : '', r.cls || '', r.disabled ? 'disabled' : ''].join(' ').trim(),
      role: 'option',
      'aria-selected': 'false',
    },
    r.lead ? icon(r.lead, 'lead') : null,
    h('span', { class: 'text' }, h('span', { class: 'label', text: r.label }), r.sub ? h('span', { class: 'sub', text: r.sub }) : null),
    end,
    r.value != null ? h('span', { class: 'value', text: r.value }) : null,
    r.check ? icon('check', 'row-end') : null,
    r.arrow ? icon('arrow') : null,
  );
  row._spec = r;
  if (r.title) row.title = r.title;
  row.addEventListener('pointermove', (e) => {
    if (e.pointerType === 'mouse' && !row.classList.contains('sel')) select(row);
  });
  row.addEventListener('click', () => {
    if (r.disabled) return;
    select(row);
    activate(row);
  });
  return row;
}

function makeList(rows, opts = {}) {
  const list = h('div', { class: `list ${opts.panel === false ? '' : 'panel'} ${opts.cls || ''}`.trim(), role: 'listbox' });
  for (const r of rows) list.append(makeRow(r));
  if (!opts.passive) activeList = list;
  return list;
}

function rowsOf(list) {
  return [...list.querySelectorAll(':scope > .row:not(.header):not(.disabled)')];
}

function select(row, scroll = false) {
  const list = row.parentElement;
  // One selection per layer: a sheet's own, or the screen's under it.
  const scope = row.closest('.sheet') || main;
  for (const r of scope.querySelectorAll('.row.sel')) {
    if (r !== row) {
      r.classList.remove('sel');
      r.setAttribute('aria-selected', 'false');
    }
  }
  row.classList.add('sel');
  row.setAttribute('aria-selected', 'true');
  activeList = list;
  if (!list.closest('.sheet')) {
    selMemory.set(location.hash, rowsOf(list).indexOf(row));
  }
  if (scroll) row.scrollIntoView({ block: 'nearest' });
}

function activate(row) {
  const r = row._spec;
  if (r && r.onSelect && !r.disabled) r.onSelect(row);
}

// Puts the selection back where it was on this screen, or on its first row.
function restoreSelection(list) {
  const rows = rowsOf(list);
  if (!rows.length) return;
  const i = selMemory.get(location.hash);
  const row = rows[Math.min(i == null ? 0 : i, rows.length - 1)];
  select(row);
  if (i) row.scrollIntoView({ block: 'center' });
}

function moveSelection(dir) {
  const list = (sheetState && sheetState.list) || activeList;
  if (!list || !list.isConnected) return;
  const rows = rowsOf(list);
  if (!rows.length) return;
  const cur = rows.findIndex((r) => r.classList.contains('sel'));
  // Lists stop at their ends, as the device's do (PLAN §8.2).
  const next = cur < 0 ? 0 : Math.max(0, Math.min(rows.length - 1, cur + dir));
  select(rows[next], true);
}

function selectedRow() {
  const list = (sheetState && sheetState.list) || activeList;
  return list && list.isConnected ? list.querySelector(':scope > .row.sel') : null;
}

/* ---------------------------------------------------------------------------
 * Sheets: the overlay menus and questions
 * ------------------------------------------------------------------------- */

let sheetState = null;

function closeSheet(value) {
  if (!sheetState) return;
  const { el, resolve, prevActive } = sheetState;
  sheetState = null;
  el.remove();
  activeList = prevActive;
  resolve(value);
}

// rows: [{label, value, danger}] -- resolves with the picked row's value, or
// null for Menu/Escape. With `input`, an <input> sits above the rows and the
// resolved value is {pick, text}.
function sheet({ title, message, rows, input, body }) {
  if (sheetState) closeSheet(null);
  return new Promise((resolve) => {
    let field = null;
    const head = h('div', { class: 'sheet-head' }, h('h2', { text: title }), message ? h('p', { text: message }) : null);
    const parts = [head];
    if (input) {
      field = h('input', { class: 'input big', type: 'text', value: input.value || '', spellcheck: 'false', autocomplete: 'off', 'aria-label': input.label || title });
      parts.push(h('div', { class: 'card' }, field));
      field.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
          e.preventDefault();
          closeSheet({ pick: input.submit, text: field.value });
        }
      });
    }
    if (body) parts.push(body);
    const list = makeList(
      rows.map((r) => ({
        label: r.label,
        cls: r.danger ? 'danger' : '',
        onSelect: () => closeSheet(input ? { pick: r.value, text: field.value } : r.value),
      })),
      { panel: false, passive: true },
    );
    parts.push(list);
    const box = h('div', { class: 'sheet', role: 'dialog', 'aria-modal': 'true', 'aria-label': title }, parts);
    const el = h('div', { class: 'sheet-scrim' }, box);
    el.addEventListener('click', (e) => {
      if (e.target === el) closeSheet(null);
    });
    sheetState = { el, resolve, list, prevActive: activeList };
    document.body.append(el);
    const first = rowsOf(list)[0];
    if (first) select(first);
    if (field) {
      field.focus();
      if (input.selectStem) field.setSelectionRange(0, stripExt(field.value).length);
      else field.select();
    }
  });
}

/* ---------------------------------------------------------------------------
 * Routing: #/path?query, one screen at a time
 * ------------------------------------------------------------------------- */

const main = document.getElementById('screen');
let current = null; // {route, q, inst}

function parseHash() {
  const raw = location.hash.replace(/^#\/?/, '');
  const qi = raw.indexOf('?');
  const path = (qi < 0 ? raw : raw.slice(0, qi)).replace(/\/+$/, '');
  const q = Object.fromEntries(new URLSearchParams(qi < 0 ? '' : raw.slice(qi + 1)));
  return { path, q };
}

function href(path, q) {
  const qs = q && Object.keys(q).length ? `?${new URLSearchParams(q)}` : '';
  return `#/${path}${qs}`;
}

function go(path, q) {
  const target = href(path, q);
  if (location.hash === target) rerender();
  else location.hash = target;
}

function goBack() {
  if (sheetState) return closeSheet(null);
  if (!current) return;
  const parent = current.route.parent && current.route.parent(current.q);
  if (parent) go(parent[0], parent[1]);
}

function rerender() {
  const { path, q } = parseHash();
  const route = ROUTES[path] || ROUTES[''];
  if (current && current.inst && current.inst.stop) current.inst.stop();
  if (sheetState) closeSheet(null);
  activeList = null;
  main.replaceChildren();
  current = { route, q, inst: null };
  setTitle(typeof route.title === 'function' ? route.title(q) : route.title);
  tb.back.hidden = !route.parent;
  current.inst = route.render(main, q) || null;
  if (activeList) restoreSelection(activeList);
  window.scrollTo(0, 0);
  if (selMemory.get(location.hash)) {
    const sel = selectedRow();
    if (sel) sel.scrollIntoView({ block: 'center' });
  }
}

// A screen that loads before it can draw: a spinner until it has its data.
function loading(el) {
  el.replaceChildren(h('div', { class: 'empty' }, h('span', { class: 'spinner' })));
}

function failed(el, e, retry) {
  el.replaceChildren(
    h(
      'div',
      { class: 'empty' },
      h('p', { text: e instanceof ApiError ? e.message : String(e) }),
      retry ? h('button', { class: 'btn', type: 'button', onclick: retry, text: 'Try Again' }) : null,
    ),
  );
}

// Re-renders the screen in place (after a change), keeping its selection.
function refresh() {
  if (!current) return;
  const keep = selMemory.get(location.hash);
  rerender();
  if (keep != null) selMemory.set(location.hash, keep);
}

/* ---------------------------------------------------------------------------
 * Home
 * ------------------------------------------------------------------------- */

// As the device's main menu: Now Playing once something is loaded.
function hasNowPlaying() {
  const p = S.st && S.st.player;
  return !!(p && (p.title || p.uri));
}

function renderHome(el) {
  const p = S.st && S.st.player;
  if (look() === 'glass' && p && (p.title || p.uri)) {
    el.append(miniNowPlaying(p));
  }
  const rows = [{ label: 'Music', arrow: true, onSelect: () => go('music') }];
  if (hasNowPlaying()) rows.push({ label: 'Now Playing', arrow: true, onSelect: () => go('now') });
  rows.push(
    { label: 'Upload Music', value: UP.summary() || null, arrow: true, onSelect: () => go('upload') },
    { label: 'Settings', arrow: true, onSelect: () => go('settings') },
    { label: 'About', arrow: true, onSelect: () => go('about') },
  );
  el.append(makeList(rows));
  const shown = hasNowPlaying();
  return {
    state() {
      if (hasNowPlaying() !== shown) refresh();
      const card = el.querySelector('.mini-np');
      if (card) fillMini(card);
    },
  };
}

function setArt(el, uri) {
  if (el._uri === uri) return;
  el._uri = uri;
  el.classList.remove('has-art');
  el.style.backgroundImage = '';
  if (!uri) return;
  const img = new Image();
  img.onload = () => {
    if (el._uri !== uri) return;
    el.style.backgroundImage = `url("${coverUrl(uri)}")`;
    el.classList.add('has-art');
  };
  img.src = coverUrl(uri);
}

function miniNowPlaying() {
  const playBtn = h('button', { class: 'np-btn', type: 'button', 'aria-label': 'Play or pause', onclick: (e) => (e.stopPropagation(), transport('toggle')) }, icon('play'));
  const card = h(
    'div',
    { class: 'panel mini-np', onclick: () => go('now') },
    h('div', { class: 'art' }, icon('note')),
    h('div', { class: 'meta' }, h('div', { class: 't' }), h('div', { class: 'a' })),
    playBtn,
  );
  fillMini(card);
  return card;
}

function fillMini(card) {
  const p = (S.st && S.st.player) || {};
  card.querySelector('.t').textContent = p.title || basename(p.uri || '') || 'Not Playing';
  card.querySelector('.a').textContent = [p.artist, p.album].filter(Boolean).join(' · ');
  setArt(card.querySelector('.art'), p.uri);
  card.querySelector('.np-btn .icon').className = `icon i-${p.state === 'play' ? 'pause' : 'play'}`;
}

/* ---------------------------------------------------------------------------
 * Now Playing
 * ------------------------------------------------------------------------- */

async function transport(action, extra) {
  try {
    const st = await POST('/api/player', action ? { action, ...extra } : extra);
    onState(st);
  } catch (e) {
    oops(e);
  }
}

function renderNowPlaying(el) {
  const art = h('div', { class: 'np-art' }, icon('note'));
  const count = h('span', { class: 'np-count' });
  const shuffleFlag = icon('shuffle');
  const repeatFlag = icon('repeat');
  const title = h('div', { class: 'np-title' });
  const artist = h('div', { class: 'np-artist' });
  const album = h('div', { class: 'np-album' });
  const fill = h('i');
  const bar = h('div', { class: 'bar seekable', role: 'slider', 'aria-label': 'Position', tabindex: '0' }, fill);
  const elapsed = h('span');
  const remaining = h('span');
  const playIcon = icon('play');
  const vol = h('input', { class: 'slider', type: 'range', min: 0, max: 100, step: 1, 'aria-label': 'Volume' });
  const shuffleBtn = h('button', { class: 'np-btn mode', type: 'button', 'aria-label': 'Shuffle', 'aria-pressed': 'false' }, icon('shuffle'));
  const repeatBtn = h('button', { class: 'np-btn mode', type: 'button', 'aria-label': 'Repeat', 'aria-pressed': 'false' }, icon('repeat'));

  const np = h(
    'div',
    { class: 'np' },
    art,
    h('div', { class: 'np-head' }, count, h('span', { class: 'np-flags' }, repeatFlag, shuffleFlag)),
    h('div', { class: 'np-lines' }, title, artist, album),
    h('div', { class: 'np-progress' }, bar, h('div', { class: 'np-times' }, elapsed, remaining)),
    h(
      'div',
      { class: 'np-transport' },
      h('button', { class: 'np-btn', type: 'button', 'aria-label': 'Previous', onclick: () => transport('prev') }, icon('prev')),
      h('button', { class: 'np-btn big', type: 'button', 'aria-label': 'Play or pause', onclick: () => transport('toggle') }, playIcon),
      h('button', { class: 'np-btn', type: 'button', 'aria-label': 'Next', onclick: () => transport('next') }, icon('next')),
    ),
    h('div', { class: 'np-volume' }, icon('vol-low'), vol, icon('vol-high')),
    h('div', { class: 'np-modes' }, shuffleBtn, repeatBtn),
  );
  const nothing = h('div', { class: 'np-nothing', text: 'Nothing is playing.' });
  el.append(np, nothing);

  let volBusy = 0;
  const sendVol = debounce((v) => transport(null, { volume: v }), 120);
  vol.addEventListener('input', () => {
    volBusy = Date.now();
    paintSlider(vol);
    sendVol(vol.value);
  });
  shuffleBtn.addEventListener('click', () => transport(null, { random: S.st.player.random ? 0 : 1 }));
  // Repeat cycles as the iPod's did: off, all, one.
  repeatBtn.addEventListener('click', () => {
    const p = S.st.player;
    if (!p.repeat) transport(null, { repeat: 1, single: 0 });
    else if (!p.single) transport(null, { repeat: 1, single: 1 });
    else transport(null, { repeat: 0, single: 0 });
  });
  const seek = (clientX) => {
    const p = S.st && S.st.player;
    if (!p || !p.duration) return;
    const r = bar.getBoundingClientRect();
    const f = Math.max(0, Math.min(1, (clientX - r.left) / r.width));
    transport(null, { seek: Math.round(f * p.duration * 1000) });
  };
  bar.addEventListener('click', (e) => seek(e.clientX));
  bar.addEventListener('keydown', (e) => {
    const p = S.st && S.st.player;
    if (!p) return;
    if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
      e.preventDefault();
      e.stopPropagation();
      const pos = Math.max(0, elapsedNow() + (e.key === 'ArrowRight' ? 10 : -10));
      transport(null, { seek: Math.round(pos * 1000) });
    }
  });

  function elapsedNow() {
    const p = S.st && S.st.player;
    if (!p) return 0;
    let e = p.elapsed;
    if (p.state === 'play') e += (Date.now() - S.stAt) / 1000;
    return Math.min(e, p.duration || e);
  }

  function paintTime() {
    const p = S.st && S.st.player;
    if (!p) return;
    const e = elapsedNow();
    const pct = p.duration ? (100 * e) / p.duration : 0;
    fill.style.width = `${pct}%`;
    bar.setAttribute('aria-valuenow', String(Math.round(pct)));
    elapsed.textContent = fmtTime(e);
    remaining.textContent = p.duration ? `-${fmtTime(p.duration - Math.floor(e))}` : '';
  }

  function paint() {
    const p = S.st && S.st.player;
    const any = !!(p && (p.title || p.uri));
    np.hidden = !any;
    nothing.hidden = any;
    if (!any) return;
    title.textContent = p.title || basename(p.uri);
    artist.textContent = p.artist || '';
    album.textContent = p.album || '';
    count.textContent = p.pos >= 0 && p.length ? `${p.pos + 1} of ${p.length}` : '';
    setArt(art, p.uri);
    playIcon.className = `icon i-${p.state === 'play' ? 'pause' : 'play'}`;
    shuffleBtn.classList.toggle('on', p.random);
    shuffleBtn.setAttribute('aria-pressed', String(p.random));
    repeatBtn.classList.toggle('on', p.repeat);
    repeatBtn.setAttribute('aria-pressed', String(p.repeat));
    repeatBtn.firstChild.className = `icon i-${p.single && p.repeat ? 'repeat-one' : 'repeat'}`;
    shuffleFlag.hidden = !p.random;
    repeatFlag.hidden = !p.repeat;
    repeatFlag.className = `icon i-${p.single ? 'repeat-one' : 'repeat'}`;
    if (Date.now() - volBusy > 1500 && p.volume >= 0) {
      vol.value = p.volume;
      paintSlider(vol);
    }
    vol.disabled = p.volume < 0;
    paintTime();
  }

  paint();
  const timer = setInterval(paintTime, 250);
  return { state: paint, stop: () => clearInterval(timer), fastPoll: true };
}

function paintSlider(input) {
  const min = Number(input.min || 0);
  const max = Number(input.max || 100);
  const pct = max > min ? ((Number(input.value) - min) / (max - min)) * 100 : 0;
  input.style.setProperty('--pct', `${pct}%`);
}

/* ---------------------------------------------------------------------------
 * Music: the library's folders and files
 * ------------------------------------------------------------------------- */

const SORTS = [
  ['name', 'Name'],
  ['artist', 'Artist'],
  ['album', 'Album'],
  ['added', 'Recently Added'],
  ['size', 'Size'],
];

function entryLabel(e) {
  return e.dir ? e.name : (e.song && e.song.title) || stripExt(e.name);
}

function sortEntries(entries, how) {
  const dirs = entries.filter((e) => e.dir).sort((a, b) => collator.compare(a.name, b.name));
  const files = entries.filter((e) => !e.dir);
  const s = (e) => e.song || {};
  const by = {
    name: (a, b) => collator.compare(entryLabel(a), entryLabel(b)),
    artist: (a, b) =>
      collator.compare(s(a).album_artist || s(a).artist || '￿', s(b).album_artist || s(b).artist || '￿') ||
      collator.compare(s(a).album || '', s(b).album || '') ||
      (s(a).track || 0) - (s(b).track || 0) ||
      collator.compare(a.name, b.name),
    album: (a, b) =>
      collator.compare(s(a).album || '￿', s(b).album || '￿') ||
      (s(a).track || 0) - (s(b).track || 0) ||
      collator.compare(a.name, b.name),
    added: (a, b) => b.mtime - a.mtime || collator.compare(a.name, b.name),
    size: (a, b) => b.size - a.size,
  };
  files.sort(by[how] || by.name);
  return how === 'added' || how === 'size' ? [...files, ...dirs] : [...dirs, ...files];
}

function renderMusic(el, q) {
  const path = q.path || '';
  let data = null;
  let sort = store('rpod-sort') || 'name';
  let filter = '';
  const body = h('div');

  const filterInput = h('input', { class: 'input', type: 'search', placeholder: 'Filter', 'aria-label': 'Filter this folder', autocomplete: 'off' });
  const sortSelect = h(
    'select',
    { class: 'input', 'aria-label': 'Sort by' },
    SORTS.map(([k, label]) => h('option', { value: k, text: label })),
  );
  sortSelect.value = sort;
  const toolbar = h(
    'div',
    { class: 'toolbar' },
    filterInput,
    sortSelect,
    h('button', { class: 'btn primary', type: 'button', onclick: () => go('upload', path ? { dir: path } : {}) }, icon('upload'), 'Upload Here'),
    h('button', { class: 'btn', type: 'button', onclick: newFolder, text: 'New Folder' }),
    path
      ? [h('button', { class: 'btn', type: 'button', onclick: () => renameEntry(path, true), text: 'Rename Folder' }), h('button', { class: 'btn danger', type: 'button', onclick: () => deleteEntry(path, true), text: 'Delete Folder' })]
      : h('button', { class: 'btn', type: 'button', onclick: updateLibrary, text: 'Update Library' }),
  );
  el.append(toolbar, body);
  loading(body);

  filterInput.addEventListener(
    'input',
    debounce(() => {
      filter = filterInput.value.trim().toLowerCase();
      draw();
    }, 120),
  );
  filterInput.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      filterInput.blur();
      moveSelection(0);
    }
  });
  sortSelect.addEventListener('change', () => {
    sort = sortSelect.value;
    store('rpod-sort', sort);
    draw();
  });

  async function load() {
    try {
      data = await GET('/api/library', { path });
      draw();
      restoreSelection(activeList);
    } catch (e) {
      failed(body, e, load);
    }
  }

  function draw() {
    const entries = sortEntries(data.entries, sort).filter((e) => {
      if (!filter) return true;
      const s = e.song || {};
      return [e.name, s.title, s.artist, s.album].some((v) => v && v.toLowerCase().includes(filter));
    });
    const folders = data.entries.filter((e) => e.dir).length;
    const songs = data.entries.filter((e) => e.song).length;
    const others = data.entries.length - folders - songs;
    const parts = [];
    if (folders) parts.push(plural(folders, 'folder'));
    parts.push(plural(songs, 'song'));
    if (others) parts.push(plural(others, 'other file'));
    if (data.storage && data.storage.free >= 0) parts.push(`${fmtBytes(data.storage.free)} free`);
    if (!data.writable) parts.push('read-only');
    if (!data.mpd) parts.push('MPD is not running');
    const summary = h('div', { class: 'summary', text: parts.join(' · ') });

    const rows = [];
    let lastAlbum = null;
    for (const e of entries) {
      const s = e.song;
      if (sort === 'album' && !e.dir) {
        const album = (s && s.album) || 'Not in the Library Yet';
        if (album !== lastAlbum) {
          rows.push({ header: album });
          lastAlbum = album;
        }
      }
      if (e.dir) {
        rows.push({ label: e.name, lead: 'folder', arrow: true, onSelect: () => go('music', { path: joinPath(path, e.name) }) });
        continue;
      }
      const sub = s ? [s.artist, s.album].filter(Boolean).join(' · ') || e.name : `${e.name} · not in the library yet`;
      rows.push({
        label: entryLabel(e),
        sub,
        lead: s ? 'note' : 'file',
        duration: s && s.duration ? fmtTime(s.duration) : fmtBytes(e.size),
        title: e.name,
        disabled: !!e.odd_name,
        onSelect: () => fileActions(e),
      });
    }
    const list = rows.length ? makeList(rows) : null;
    body.replaceChildren(summary, list || h('div', { class: 'empty', text: filter ? 'Nothing matches.' : 'This folder is empty.' }));
    if (list) restoreSelection(list);
  }

  async function fileActions(e) {
    const rel = joinPath(path, e.name);
    const s = e.song;
    const rows = [];
    if (s) rows.push({ label: 'Play', value: 'play' });
    rows.push({ label: 'Rename…', value: 'rename' }, { label: 'Delete', value: 'delete', danger: true }, { label: 'Cancel', value: null });
    const info = [e.name, fmtBytes(e.size)];
    if (s && s.duration) info.push(fmtTime(s.duration));
    const pick = await sheet({ title: entryLabel(e), message: info.join(' · '), rows });
    if (pick === 'play') {
      try {
        onState(await POST('/api/library/play', { path: rel }));
        go('now');
      } catch (err) {
        oops(err);
      }
    } else if (pick === 'rename') renameEntry(rel, false);
    else if (pick === 'delete') deleteEntry(rel, false);
  }

  async function renameEntry(rel, isDir) {
    const old = basename(rel);
    const res = await sheet({
      title: isDir ? 'Rename Folder' : 'Rename',
      input: { value: old, submit: 'save', selectStem: !isDir, label: 'New name' },
      rows: [
        { label: 'Save', value: 'save' },
        { label: 'Cancel', value: null },
      ],
    });
    if (!res || res.pick !== 'save') return;
    const name = cleanName(res.text);
    if (!name || name === old) return;
    try {
      await POST('/api/library/rename', { path: rel, name });
      hud(`Renamed to “${name}”`);
      if (isDir) go('music', { path: joinPath(dirname(rel), name) });
      else load();
    } catch (e) {
      oops(e);
    }
  }

  async function deleteEntry(rel, isDir) {
    const name = basename(rel);
    const pick = await sheet({
      title: isDir ? `Delete “${name}” and everything in it?` : `Delete “${name}”?`,
      message: 'It comes off rPod for good.',
      rows: [
        { label: 'Cancel', value: null },
        { label: 'Delete', value: 'delete', danger: true },
      ],
    });
    if (pick !== 'delete') return;
    try {
      await POST('/api/library/delete', { path: rel });
      hud(`Deleted “${name}”`);
      if (isDir) go('music', dirname(rel) ? { path: dirname(rel) } : {});
      else load();
    } catch (e) {
      oops(e);
    }
  }

  async function newFolder() {
    const res = await sheet({
      title: 'New Folder',
      input: { value: '', submit: 'make', label: 'Folder name' },
      rows: [
        { label: 'Create', value: 'make' },
        { label: 'Cancel', value: null },
      ],
    });
    if (!res || res.pick !== 'make') return;
    const name = cleanName(res.text);
    if (!name) return;
    try {
      await POST('/api/library/mkdir', { path: joinPath(path, name) });
      go('music', { path: joinPath(path, name) });
    } catch (e) {
      oops(e);
    }
  }

  load();
  const unsub = UP.onDone(() => load());
  return { stop: unsub, dropDir: path };
}

async function updateLibrary() {
  try {
    await POST('/api/library/update', {});
    hud('Updating the library…');
    poll();
  } catch (e) {
    oops(e);
  }
}

/* ---------------------------------------------------------------------------
 * Uploads: files to PUT /api/library/upload, two at a time
 * ------------------------------------------------------------------------- */

// What the server takes (rpod_lib_upload_name_ok() in web/library.c).
const UPLOAD_EXTS = new Set('flac mp3 m4a mp4 aac alac ogg oga opus wav wv ape aif aiff aifc mpc dsf dff jpg jpeg png cue lrc'.split(' '));
const PARALLEL = 2;

// A name that's safe on the device's exFAT music partition as well as here:
// no reserved characters, no leading dot (hidden) or trailing dot/space,
// at most 255 bytes. Mirrors rpod_lib_name_ok().
function cleanName(n) {
  n = String(n || '')
    .normalize('NFC')
    .replace(/[\u0000-\u001f\u007f"*/:<>?\\|]/g, '_')
    .replace(/[ .]+$/, '')
    .trim();
  if (n.startsWith('.')) n = `_${n.slice(1)}`;
  const enc = new TextEncoder();
  if (enc.encode(n).length > 255) {
    const dot = n.lastIndexOf('.');
    const ext = dot > 0 && n.length - dot <= 6 ? n.slice(dot) : '';
    let stem = ext ? n.slice(0, dot) : n;
    while (enc.encode(stem + ext).length > 255) stem = stem.slice(0, -1);
    n = stem + ext;
  }
  return n;
}

const UP = {
  items: [],
  overwrite: store('rpod-overwrite') === '1',
  sending: 0,
  listeners: new Set(),
  doneListeners: new Set(),
  songsBefore: null,
  dirs: new Set(),
  rate: { t: 0, bytes: 0, bps: 0 },
  painting: false,

  busy() {
    return this.items.some((i) => i.status === 'queued' || i.status === 'sending');
  },
  totals() {
    let n = 0;
    let done = 0;
    let bytes = 0;
    let sent = 0;
    for (const i of this.items) {
      if (i.status === 'skipped') continue;
      n++;
      bytes += i.size;
      if (i.status === 'done' || i.status === 'exists' || i.status === 'failed') {
        done++;
        sent += i.size;
      } else if (i.status === 'sending') sent += i.loaded;
    }
    return { n, done, bytes, sent };
  },
  summary() {
    if (!this.busy()) return '';
    const t = this.totals();
    return `${t.done} of ${t.n}`;
  },
  onChange(fn) {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  },
  onDone(fn) {
    this.doneListeners.add(fn);
    return () => this.doneListeners.delete(fn);
  },
  changed() {
    if (this.painting) return;
    this.painting = true;
    requestAnimationFrame(() => {
      this.painting = false;
      for (const fn of this.listeners) fn();
      paintTitleState();
    });
  },

  // files: [{file, rel}], rel being the path within what was picked or
  // dropped ("Album/01 Song.flac"); dest: the library folder it goes in.
  async add(files, dest) {
    if (!this.busy()) {
      this.songsBefore = null;
      GET('/api/library/stats')
        .then((s) => (this.songsBefore = s.songs))
        .catch(() => {});
    }
    let added = 0;
    let ignored = 0;
    for (const { file, rel } of files) {
      const parts = rel.split('/').filter(Boolean).map(cleanName).filter(Boolean);
      const name = parts.pop() || cleanName(file.name);
      const ext = name.slice(name.lastIndexOf('.') + 1).toLowerCase();
      // Hidden files and a desktop's droppings (.DS_Store, Thumbs.db...) never count.
      if (file.name.startsWith('.') || /^(thumbs\.db|desktop\.ini)$/i.test(file.name)) continue;
      const dir = parts.reduce((d, p) => joinPath(d, p), dest);
      const item = { file, dir, name, size: file.size, loaded: 0, status: 'queued', error: '' };
      if (!UPLOAD_EXTS.has(ext) || name.lastIndexOf('.') <= 0) {
        item.status = 'skipped';
        item.error = 'Not music';
        ignored++;
      } else added++;
      this.items.push(item);
      this.dirs.add(dest);
    }
    if (!added) hud(ignored ? 'None of those are music files rPod plays.' : 'Nothing to upload.', true);
    this.changed();
    this.pump();
    return added;
  },

  pump() {
    while (this.sending < PARALLEL) {
      const item = this.items.find((i) => i.status === 'queued');
      if (!item) break;
      this.send(item);
    }
    if (!this.busy() && this.sending === 0) this.finish();
  },

  send(item) {
    this.sending++;
    item.status = 'sending';
    const xhr = new XMLHttpRequest();
    item.xhr = xhr;
    const qs = new URLSearchParams({ dir: item.dir, name: item.name, overwrite: this.overwrite ? '1' : '0' });
    xhr.open('PUT', `/api/library/upload?${qs}`);
    xhr.setRequestHeader('X-Rpod', '1');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.upload.onprogress = (e) => {
      item.loaded = e.loaded;
      this.measure();
      this.changed();
    };
    const end = (status, error = '') => {
      item.status = status;
      item.error = error;
      item.loaded = status === 'done' ? item.size : item.loaded;
      item.xhr = null;
      item.file = status === 'failed' ? item.file : null; // let it go once it's there
      this.sending--;
      this.changed();
      this.pump();
    };
    xhr.onload = () => {
      let msg = '';
      try {
        msg = JSON.parse(xhr.responseText).error || '';
      } catch (e) {
        // no JSON body
      }
      if (xhr.status === 201) end('done');
      else if (xhr.status === 409) end('exists', 'Already there');
      else if (xhr.status === 507 || xhr.status === 403) {
        end('failed', msg || 'Failed');
        this.cancel(msg); // no space, or not writable: the rest would fail too
      } else end('failed', msg || `Error ${xhr.status}`);
    };
    xhr.onerror = () => end('failed', 'Connection lost');
    xhr.onabort = () => end('failed', 'Cancelled');
    xhr.send(item.file);
  },

  measure() {
    const now = performance.now();
    const sent = this.totals().sent;
    if (!this.rate.t) {
      this.rate = { t: now, bytes: sent, bps: 0 };
      return;
    }
    const dt = (now - this.rate.t) / 1000;
    if (dt >= 1) {
      const bps = (sent - this.rate.bytes) / dt;
      this.rate = { t: now, bytes: sent, bps: this.rate.bps ? this.rate.bps * 0.6 + bps * 0.4 : bps };
    }
  },

  cancel(reason) {
    for (const i of this.items) {
      if (i.status === 'queued') {
        i.status = 'failed';
        i.error = reason || 'Cancelled';
      }
    }
    for (const i of this.items) if (i.xhr) i.xhr.abort();
    this.changed();
  },

  retry() {
    for (const i of this.items) {
      if (i.status === 'failed' && i.file) {
        i.status = 'queued';
        i.error = '';
        i.loaded = 0;
      }
    }
    this.changed();
    this.pump();
  },

  clear() {
    this.items = this.items.filter((i) => i.status === 'queued' || i.status === 'sending');
    this.changed();
  },

  // Everything's in: have MPD read it, then say what arrived.
  async finish() {
    if (this.finishing || !this.items.length) return;
    const fresh = this.items.filter((i) => i.status === 'done' && !i.counted);
    if (!fresh.length) return;
    this.finishing = true;
    fresh.forEach((i) => (i.counted = true));
    this.rate = { t: 0, bytes: 0, bps: 0 };
    try {
      for (const d of this.dirs) await POST('/api/library/update', { path: d });
      this.dirs.clear();
      let stats;
      for (let tries = 0; tries < 120; tries++) {
        await new Promise((r) => setTimeout(r, 1000));
        stats = await GET('/api/library/stats');
        if (!stats.updating) break;
      }
      const added = this.songsBefore != null && stats ? stats.songs - this.songsBefore : null;
      hud(added > 0 ? `${plural(added, 'new song')} in the library` : `Uploaded ${plural(fresh.length, 'file')}`);
    } catch (e) {
      hud(`Uploaded ${plural(fresh.length, 'file')}`);
    } finally {
      this.finishing = false;
      for (const fn of this.doneListeners) fn();
      poll();
    }
  },
};

window.addEventListener('beforeunload', (e) => {
  if (UP.busy()) {
    e.preventDefault();
    e.returnValue = '';
  }
});

function pickFiles(folder, dest) {
  const input = h('input', { type: 'file', multiple: true, hidden: true });
  if (folder) input.webkitdirectory = true;
  else input.accept = [...UPLOAD_EXTS].map((x) => `.${x}`).join(',');
  input.addEventListener('change', () => {
    const files = [...input.files].map((f) => ({ file: f, rel: f.webkitRelativePath || f.name }));
    input.remove();
    if (files.length) UP.add(files, dest);
  });
  document.body.append(input);
  input.click();
}

// Folders dropped from a desktop arrive as entries to walk; the entries
// have to be taken from the event before anything awaits.
async function filesFromDrop(dt) {
  const entries = [...dt.items]
    .filter((i) => i.kind === 'file')
    .map((i) => (i.webkitGetAsEntry ? i.webkitGetAsEntry() : null))
    .filter(Boolean);
  if (!entries.length) return [...dt.files].map((f) => ({ file: f, rel: f.name }));
  const out = [];
  const walk = async (entry, prefix) => {
    if (entry.isFile) {
      const file = await new Promise((res, rej) => entry.file(res, rej));
      out.push({ file, rel: prefix + file.name });
    } else if (entry.isDirectory) {
      const reader = entry.createReader();
      for (;;) {
        const batch = await new Promise((res, rej) => reader.readEntries(res, rej));
        if (!batch.length) break;
        for (const e of batch) await walk(e, `${prefix}${entry.name}/`);
      }
    }
  };
  for (const e of entries) await walk(e, '');
  return out;
}

const STATUS_TEXT = { queued: 'Waiting', done: 'Done', exists: 'Already there', skipped: 'Not music' };

function renderUpload(el, q) {
  const dest = q.dir || '';
  const zone = h(
    'div',
    { class: 'dropzone' },
    icon('upload'),
    h('div', { text: 'Drop songs or album folders here' }),
    h('div', { class: 'dest', text: `Into ${dest ? `Music › ${dest.split('/').join(' › ')}` : 'Music'}` }),
    h(
      'div',
      { class: 'buttons' },
      h('button', { class: 'btn primary', type: 'button', onclick: () => pickFiles(false, dest), text: 'Choose Files…' }),
      h('button', { class: 'btn', type: 'button', onclick: () => pickFiles(true, dest), text: 'Choose Folder…' }),
    ),
    h('div', { class: 'hint', text: 'FLAC, MP3, AAC, ALAC, Ogg, Opus, WAV, AIFF and more, plus cover images.' }),
  );
  const options = makeList([
    {
      label: 'Replace Existing Files',
      value: UP.overwrite ? 'On' : 'Off',
      onSelect: (row) => {
        UP.overwrite = !UP.overwrite;
        store('rpod-overwrite', UP.overwrite ? '1' : '0');
        row.querySelector('.value').textContent = UP.overwrite ? 'On' : 'Off';
      },
    },
    { label: 'See the Folder', arrow: true, onSelect: () => go('music', dest ? { path: dest } : {}) },
  ]);
  const status = h('div', { class: 'panel' });
  const queue = h('div');
  el.append(zone, options, status, queue);

  ['dragenter', 'dragover'].forEach((t) =>
    zone.addEventListener(t, (e) => {
      if (![...e.dataTransfer.types].includes('Files')) return;
      e.preventDefault();
      zone.classList.add('over');
    }),
  );
  zone.addEventListener('dragleave', () => zone.classList.remove('over'));

  const rowsByItem = new Map();
  let shownCount = -1;

  function stateEl(i) {
    if (i.status === 'sending') return h('span', { class: 'state', text: `${Math.floor((100 * i.loaded) / (i.size || 1))}%` });
    if (i.status === 'failed') return h('span', { class: 'state bad', text: i.error || 'Failed' });
    return h('span', { class: 'state', text: STATUS_TEXT[i.status] || '' });
  }

  function paint() {
    const t = UP.totals();
    if (!UP.items.length) {
      status.hidden = true;
      queue.replaceChildren();
      rowsByItem.clear();
      shownCount = 0;
      return;
    }
    status.hidden = false;
    const busy = UP.busy();
    const pct = t.bytes ? (100 * t.sent) / t.bytes : 100;
    const bar = h('div', { class: 'bar' }, h('i'));
    bar.firstChild.style.width = `${pct}%`;
    let speed = '';
    if (busy && UP.rate.bps > 0) {
      const left = (t.bytes - t.sent) / UP.rate.bps;
      speed = `${fmtBytes(UP.rate.bps)}/s · ${left < 60 ? 'under a minute' : fmtDuration(left)} left`;
    }
    const failedN = UP.items.filter((i) => i.status === 'failed').length;
    const lines = [
      h('div', { class: 'line' }, h('span', { text: busy ? `Uploading ${t.done + 1 > t.n ? t.n : t.done + 1} of ${t.n}` : `${plural(t.done, 'file')} done` }), h('span', { text: `${fmtBytes(t.sent)} of ${fmtBytes(t.bytes)}` })),
      bar,
    ];
    if (speed) lines.push(h('div', { class: 'note', text: speed }));
    const buttons = h('div', { class: 'buttons' });
    if (busy) buttons.append(h('button', { class: 'btn danger', type: 'button', onclick: () => UP.cancel('Cancelled'), text: 'Cancel' }));
    if (!busy && failedN && UP.items.some((i) => i.status === 'failed' && i.file)) buttons.append(h('button', { class: 'btn', type: 'button', onclick: () => UP.retry(), text: `Retry ${failedN}` }));
    if (!busy) buttons.append(h('button', { class: 'btn', type: 'button', onclick: () => UP.clear(), text: 'Clear List' }));
    status.replaceChildren(h('div', { class: 'upload-status' }, lines, buttons));

    // Rows are built once per item and updated in place.
    if (shownCount !== UP.items.length || rowsByItem.size !== UP.items.length) {
      rowsByItem.clear();
      const list = makeList(
        UP.items.map((i) => ({
          label: i.name,
          sub: i.dir ? `Music › ${i.dir.split('/').join(' › ')} · ${fmtBytes(i.size)}` : `Music · ${fmtBytes(i.size)}`,
          lead: 'note',
          state: stateEl(i),
          onSelect: () => {},
        })),
        { passive: true },
      );
      rowsOf(list).forEach((row, k) => rowsByItem.set(UP.items[k], row));
      queue.replaceChildren(list);
      shownCount = UP.items.length;
    }
    for (const [i, row] of rowsByItem) {
      const old = row.querySelector('.state');
      const fresh = stateEl(i);
      if (old.textContent !== fresh.textContent || old.className !== fresh.className) old.replaceWith(fresh);
      let mb = row.querySelector('.minibar');
      if (i.status === 'sending') {
        if (!mb) {
          mb = h('span', { class: 'minibar' }, h('i'));
          row.append(mb);
        }
        mb.firstChild.style.width = `${(100 * i.loaded) / (i.size || 1)}%`;
      } else if (mb) mb.remove();
    }
  }

  paint();
  const unsub = UP.onChange(paint);
  return { stop: unsub, dropDir: dest };
}

/* ---------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------- */

const BACKLIGHT = [
  [10, '10 Seconds'],
  [20, '20 Seconds'],
  [30, '30 Seconds'],
  [60, '1 Minute'],
  [120, '2 Minutes'],
  [0, 'Always On'],
];
const SLEEP = [
  [0, 'Off'],
  [15, '15 Minutes'],
  [30, '30 Minutes'],
  [60, '60 Minutes'],
  [90, '90 Minutes'],
  [120, '120 Minutes'],
];
const REPLAY_GAIN = [
  ['off', 'Off'],
  ['track', 'Track'],
  ['album', 'Album'],
  ['auto', 'Auto'],
];

function labelFor(table, v) {
  const hit = table.find(([k]) => k === v);
  return hit ? hit[1] : v == null ? '—' : String(v);
}

// Loads the settings, then draws `fn(settings)` into el -- again after
// every change, so what's shown is what rPod now has.
function settingsScreen(el, fn) {
  const body = h('div');
  el.append(body);
  let alive = true;
  const load = async () => {
    try {
      S.settings = await GET('/api/settings');
      if (!alive) return;
      if (S.settings.theme) applyTheme(S.settings.theme);
      body.replaceChildren();
      const extra = fn(body, S.settings, load);
      if (activeList && activeList.isConnected) restoreSelection(activeList);
      return extra;
    } catch (e) {
      if (alive) failed(body, e, load);
    }
  };
  if (S.settings) {
    fn(body, S.settings, load);
  } else loading(body);
  load();
  return { stop: () => (alive = false) };
}

async function setSetting(params, reload) {
  try {
    S.settings = await POST('/api/settings', params);
    if (reload) reload();
    return true;
  } catch (e) {
    oops(e);
    if (reload) reload();
    return false;
  }
}

function needsRpod(s) {
  return s.ui ? null : h('p', { class: 'note', text: 'rPod itself isn’t running, so its own settings can’t change right now.' });
}

function renderSettings(el) {
  return settingsScreen(el, (body, s) => {
    const enabled = (s.outputs || []).filter((o) => o.enabled && !/visuali[sz]er/i.test(o.name)).map((o) => o.name);
    const sleepLeft = s.sleep_remaining_ms ? Math.ceil(s.sleep_remaining_ms / 60000) : 0;
    body.append(
      needsRpod(s) || '',
      makeList([
        { label: 'Theme', value: labelFor(THEMES, s.theme), arrow: true, onSelect: () => go('settings/theme') },
        { label: 'Backlight', value: s.ui ? labelFor(BACKLIGHT, s.backlight) : '—', arrow: true, onSelect: () => go('settings/backlight') },
        { label: 'Sleep Timer', value: s.ui ? (sleepLeft ? `${sleepLeft} min left` : 'Off') : '—', arrow: true, onSelect: () => go('settings/sleep') },
        { label: 'Alarm Clock', value: s.alarm ? (s.alarm.on ? s.alarm.time : 'Off') : '—', arrow: true, onSelect: () => go('settings/alarm') },
        { label: 'Audio Output', value: s.mpd ? enabled.join(', ') || 'None' : '—', arrow: true, onSelect: () => go('settings/output') },
        { label: 'Playback', value: s.mpd && s.crossfade ? `Crossfade ${s.crossfade} s` : null, arrow: true, onSelect: () => go('settings/playback') },
        { label: 'Click Wheel', arrow: true, onSelect: () => go('settings/wheel') },
        { label: 'ListenBrainz', value: s.listenbrainz && s.listenbrainz.on != null ? (s.listenbrainz.on ? 'On' : 'Off') : '—', arrow: true, onSelect: () => go('settings/listenbrainz') },
      ]),
    );
  });
}

// A list of choices with the current one checked, like the device's.
function choiceScreen(el, { get, choices, set, note }) {
  return settingsScreen(el, (body, s, reload) => {
    body.append(
      needsRpod(s) || '',
      note ? note(s) || '' : '',
      makeList(
        choices(s).map(([value, label]) => ({
          label,
          check: get(s) === value,
          onSelect: async (row) => {
            for (const r of row.parentElement.querySelectorAll('.row')) {
              const c = r.querySelector('.i-check');
              if (c) c.remove();
            }
            row.insertBefore(icon('check', 'row-end'), null);
            await set(value, reload);
          },
        })),
      ),
    );
  });
}

function renderTheme(el) {
  return choiceScreen(el, {
    get: (s) => s.theme,
    choices: () => THEMES,
    set: async (v, reload) => {
      const was = S.theme;
      applyTheme(v); // at once here; rPod rebuilds its own screens alike
      if (!(await setSetting({ theme: v }, reload))) applyTheme(was);
    },
  });
}

function renderBacklight(el) {
  return choiceScreen(el, {
    get: (s) => s.backlight,
    choices: () => BACKLIGHT,
    set: (v, reload) => setSetting({ backlight: v }, reload),
  });
}

function renderSleep(el) {
  const inst = choiceScreen(el, {
    get: (s) => (s.sleep_remaining_ms ? s.sleep : 0),
    choices: () => SLEEP,
    note: (s) => (s.sleep_remaining_ms ? h('p', { class: 'note', text: `Sleeping in ${plural(Math.ceil(s.sleep_remaining_ms / 60000), 'minute')}.` }) : null),
    set: (v, reload) => setSetting({ sleep: v }, reload),
  });
  const t = setInterval(() => refresh(), 30000);
  return { stop: () => (clearInterval(t), inst.stop()) };
}

function renderAlarm(el) {
  return settingsScreen(el, (body, s, reload) => {
    if (!s.alarm) {
      body.append(needsRpod(s) || '');
      return;
    }
    const sound = s.alarm.sound || 'Shuffle Songs';
    body.append(
      makeList([
        { label: 'Alarm', value: s.alarm.on ? 'On' : 'Off', onSelect: () => setSetting({ alarm: s.alarm.on ? 'off' : 'on' }, reload) },
        { label: 'Time', value: s.alarm.time, arrow: true, onSelect: () => go('settings/alarm/time') },
        { label: 'Sound', value: sound, arrow: true, onSelect: () => go('settings/alarm/sound') },
      ]),
      s.alarm.ringing ? h('p', { class: 'note', text: 'It’s going off right now.' }) : '',
    );
  });
}

function renderAlarmTime(el) {
  return settingsScreen(el, (body, s) => {
    if (!s.alarm) {
      body.append(needsRpod(s) || '');
      return;
    }
    const input = h('input', { class: 'input time-input', type: 'time', value: s.alarm.time, required: true, 'aria-label': 'Alarm time' });
    const save = async () => {
      if (!input.value) return;
      // Setting the time turns the alarm on, as on the device.
      if (await setSetting({ alarm_time: input.value, alarm: 'on' })) {
        hud(`Alarm set for ${input.value}`);
        go('settings/alarm');
      }
    };
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') save();
    });
    body.append(
      h(
        'div',
        { class: 'panel boxed card' },
        h('div', { class: 'field-row' }, input),
        h('div', { class: 'buttons' }, h('button', { class: 'btn primary', type: 'button', onclick: save, text: 'Set Alarm' })),
        h('p', { class: 'note', text: 'Every day, faded in from quiet. The time is rPod’s, which is right once it has been online.' }),
      ),
    );
    input.focus();
  });
}

function renderAlarmSound(el) {
  return choiceScreen(el, {
    get: (s) => (s.alarm ? s.alarm.sound : null),
    choices: (s) => [['', 'Shuffle Songs'], ...(s.playlists || []).map((p) => [p, p])],
    set: (v, reload) => setSetting({ alarm_sound: v }, reload),
  });
}

function renderOutputs(el) {
  return settingsScreen(el, (body, s, reload) => {
    if (!s.mpd) {
      body.append(h('p', { class: 'note', text: 'MPD isn’t running.' }));
      return;
    }
    body.append(
      makeList(
        (s.outputs || []).map((o) => ({
          label: o.name,
          value: o.enabled ? 'On' : 'Off',
          onSelect: () => setSetting({ [`output.${o.id}`]: o.enabled ? 0 : 1 }, reload),
        })),
      ),
    );
  });
}

// A labelled slider row: name and value, the slider, an optional line about it.
function sliderSetting({ name, min, max, step, value, fmt, desc, onInput, onCommit }) {
  const val = h('span', { class: 'val', text: fmt(value) });
  const input = h('input', { class: 'slider', type: 'range', min, max, step, value, 'aria-label': name });
  paintSlider(input);
  input.addEventListener('input', () => {
    const v = Number(input.value);
    val.textContent = fmt(v);
    paintSlider(input);
    if (onInput) onInput(v);
  });
  input.addEventListener('change', () => onCommit && onCommit(Number(input.value)));
  return h('div', { class: 'setting' }, h('span', { class: 'name', text: name }), val, input, desc ? h('span', { class: 'desc', text: desc }) : null);
}

function renderPlayback(el) {
  return settingsScreen(el, (body, s, reload) => {
    if (!s.mpd) {
      body.append(h('p', { class: 'note', text: 'MPD isn’t running.' }));
      return;
    }
    const sendVol = debounce((v) => POST('/api/player', { volume: v }).catch(oops), 150);
    body.append(
      h(
        'div',
        { class: 'panel boxed' },
        sliderSetting({
          name: 'Crossfade',
          min: 0,
          max: 12,
          step: 1,
          value: s.crossfade || 0,
          fmt: (v) => (v ? `${v} s` : 'Off'),
          desc: 'Blends the end of each song into the next. Off keeps albums gapless.',
          onCommit: (v) => setSetting({ crossfade: v }),
        }),
        s.volume != null && s.volume >= 0
          ? sliderSetting({ name: 'Volume', min: 0, max: 100, step: 1, value: s.volume, fmt: (v) => `${v}%`, onInput: sendVol })
          : null,
      ),
      h('h2', { class: 'section-title', text: 'Sound Check' }),
      makeList(
        REPLAY_GAIN.map(([k, label]) => ({
          label,
          check: s.replay_gain === k,
          onSelect: () => setSetting({ replay_gain: k }, reload),
        })),
      ),
      h('p', { class: 'note', text: 'Evens out loudness with the ReplayGain tags in your files: per track, per album, or Auto (album when playing one in order).' }),
    );
  });
}

/* --- Click Wheel: the acceleration curve, tuned live --------------------------- */

const WHEEL = {
  step_ticks: {
    name: 'Slow Scrolling',
    step: 0.5,
    fmt: (v) => `${v} per row`,
    desc: (v) => `How far a slow turn goes for one row: ${Math.round(96 / v)} rows a full turn (96 positions).`,
  },
  accel_v0: { name: 'Speeds Up From', step: 5, fmt: speed, desc: () => 'Turning faster than this starts to skip rows.' },
  accel_v1: { name: 'Full Speed At', step: 5, fmt: speed, desc: () => 'From here on, a turn moves rows at the top multiplier.' },
  accel_max: { name: 'Top Multiplier', step: 0.5, fmt: (v) => `×${v}`, desc: () => 'How many times further a flick goes than a slow turn.' },
  scrub_v: {
    name: 'Letters From',
    step: 5,
    fmt: (v) => (v ? speed(v) : 'Full speed'),
    desc: () => 'In a long A–Z list, turning faster than this jumps a letter at a time. All the way left follows Full Speed At.',
  },
  scrub_ticks: { name: 'Letter Step', step: 1, fmt: (v) => `${v} per letter`, desc: (v) => `${(96 / v).toFixed(1)} letters a full turn.` },
};

function speed(v) {
  return `${(v / 96).toFixed(2)} turns/s`;
}

function wheelCurve(vals) {
  const W = 560;
  const H = 214;
  const pad = { l: 44, r: 14, t: 12, b: 44 };
  const v0 = vals.accel_v0;
  const v1 = Math.max(vals.accel_v1, v0 + 1);
  const scrub = vals.scrub_v || v1;
  const xmax = Math.max(3, Math.ceil(((Math.max(v1, scrub) / 96) * 1.35) * 2) / 2);
  const rowsPerTurn = (tps) => {
    const v = tps * 96;
    const t = Math.max(0, Math.min(1, (v - v0) / (v1 - v0)));
    return (96 / vals.step_ticks) * (1 + (vals.accel_max - 1) * t * t);
  };
  const ymaxRaw = rowsPerTurn(xmax);
  const ystep = ymaxRaw > 400 ? 100 : ymaxRaw > 160 ? 50 : ymaxRaw > 60 ? 20 : 10;
  const ymax = Math.ceil(ymaxRaw / ystep) * ystep;
  const X = (x) => pad.l + (x / xmax) * (W - pad.l - pad.r);
  const Y = (y) => H - pad.b - (y / ymax) * (H - pad.t - pad.b);
  const NS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  svg.setAttribute('class', 'curve');
  svg.setAttribute('role', 'img');
  svg.setAttribute('aria-label', 'Rows moved per turn of the wheel, by how fast it turns');
  const add = (tag, attrs, text) => {
    const e = document.createElementNS(NS, tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    if (text != null) e.textContent = text;
    svg.append(e);
    return e;
  };
  for (let x = 0; x <= xmax + 1e-9; x += xmax > 6 ? 1 : 0.5) {
    add('line', { class: 'grid', x1: X(x), x2: X(x), y1: Y(0), y2: Y(ymax) });
    if (Number.isInteger(x)) add('text', { class: 'axis-label', x: X(x), y: H - pad.b + 18, 'text-anchor': 'middle' }, x);
  }
  for (let y = 0; y <= ymax; y += ystep) {
    add('line', { class: 'grid', x1: X(0), x2: X(xmax), y1: Y(y), y2: Y(y) });
    add('text', { class: 'axis-label', x: pad.l - 8, y: Y(y) + 4, 'text-anchor': 'end' }, y);
  }
  add('text', { class: 'axis-label', x: (W + pad.l - pad.r) / 2, y: H - 4, 'text-anchor': 'middle' }, 'turns a second');
  add('text', { class: 'axis-label', x: pad.l + 6, y: pad.t + 10 }, 'rows a turn');
  let d = '';
  for (let i = 0; i <= 120; i++) {
    const x = (i / 120) * xmax;
    d += `${i ? 'L' : 'M'}${X(x).toFixed(1)} ${Y(rowsPerTurn(x)).toFixed(1)}`;
  }
  add('path', { class: 'line', d });
  const sx = X(scrub / 96);
  add('line', { class: 'scrub', x1: sx, x2: sx, y1: Y(0), y2: Y(ymax) });
  add('text', { class: 'scrub-label', x: sx + 5, y: pad.t + 26 }, 'A–Z');
  return svg;
}

function renderWheel(el) {
  return settingsScreen(el, (body, s, reload) => {
    const params = s.wheel || [];
    if (!s.ui || !params.length) {
      body.append(needsRpod(s) || h('p', { class: 'note', text: 'rPod didn’t say how its wheel is tuned.' }));
      return;
    }
    const vals = Object.fromEntries(params.map((p) => [p.key, p.value]));
    const curveBox = h('div', { class: 'card' }, wheelCurve(vals));
    const redraw = () => curveBox.replaceChildren(wheelCurve(vals));
    const pending = {};
    const flush = debounce(() => {
      const send = { ...pending };
      for (const k of Object.keys(pending)) delete pending[k];
      setSetting(send);
    }, 180);
    const rows = params.map((p) => {
      const meta = WHEEL[p.key] || { name: p.key, step: 0.1, fmt: String, desc: () => '' };
      const desc = h('span', { class: 'desc' });
      const setDesc = (v) => {
        const def = meta.fmt(p.reset);
        desc.textContent = `${meta.desc(v)} Default: ${def}.`;
      };
      const row = sliderSetting({
        name: meta.name,
        min: p.min,
        max: p.max,
        step: meta.step,
        value: p.value,
        fmt: meta.fmt,
        onInput: (v) => {
          vals[p.key] = v;
          setDesc(v);
          redraw();
          // Live: the wheel follows while the slider moves.
          pending[`wheel.${p.key}`] = v;
          flush();
        },
      });
      setDesc(p.value);
      row.append(desc);
      return row;
    });
    body.append(
      h('div', { class: 'panel boxed' }, curveBox),
      h('div', { class: 'panel boxed' }, rows),
      h(
        'div',
        { class: 'buttons' },
        h('button', {
          class: 'btn',
          type: 'button',
          text: 'Reset to Defaults',
          onclick: async () => {
            try {
              S.settings = await POST('/api/settings/wheel/reset', {});
              hud('The wheel is back to its defaults');
              reload();
            } catch (e) {
              oops(e);
            }
          },
        }),
      ),
      h('p', { class: 'note', text: 'Changes reach the wheel as you move a slider, and rPod keeps them. Turn the wheel to try them out.' }),
    );
  });
}

/* --- ListenBrainz -------------------------------------------------------------- */

function renderListenBrainz(el) {
  return settingsScreen(el, (body, s, reload) => {
    const lb = s.listenbrainz || {};
    const field = h('input', { class: 'input', type: 'password', placeholder: 'Your user token', autocomplete: 'off', spellcheck: 'false', 'aria-label': 'ListenBrainz user token' });
    const save = async () => {
      const token = field.value.trim();
      if (!token) return;
      let user = '';
      try {
        const r = await POST('/api/listenbrainz/check', { token });
        if (!r.valid) {
          hud('ListenBrainz doesn’t know that token.', true);
          return;
        }
        user = r.user;
      } catch (e) {
        if (e.status !== 502) return oops(e);
        const pick = await sheet({
          title: 'Can’t check the token',
          message: `${e.message} It can be saved anyway, and used once rPod is online.`,
          rows: [
            { label: 'Save Anyway', value: 'save' },
            { label: 'Cancel', value: null },
          ],
        });
        if (pick !== 'save') return;
      }
      if (await setSetting({ listenbrainz_token: token }, reload)) hud(user ? `Scrobbling as ${user}` : 'Token saved');
    };
    field.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') save();
    });
    body.append(
      needsRpod(s) || '',
      makeList(
        [
          { label: 'Scrobbling', value: lb.on == null ? '—' : lb.on ? 'On' : 'Off', onSelect: () => field.focus() },
          { label: 'Waiting to Send', value: plural(lb.queued || 0, 'listen'), onSelect: () => {} },
        ],
        {},
      ),
      h(
        'div',
        { class: 'panel boxed card' },
        h('h2', { text: lb.on ? 'Change Token' : 'Connect' }),
        h(
          'p',
          { class: 'note' },
          'Paste the user token from ',
          h('a', { href: 'https://listenbrainz.org/settings/', target: '_blank', rel: 'noreferrer noopener', text: 'listenbrainz.org/settings' }),
          '. Listens played offline are kept and sent once rPod is online.',
        ),
        h('div', { class: 'field-row' }, h('div', { class: 'grow' }, field)),
        h(
          'div',
          { class: 'buttons' },
          h('button', { class: 'btn primary', type: 'button', onclick: save, text: 'Check and Save', disabled: !s.ui }),
          lb.on
            ? h('button', {
                class: 'btn danger',
                type: 'button',
                text: 'Turn Off',
                onclick: async () => {
                  if (await setSetting({ listenbrainz_token: '' }, reload)) hud('Scrobbling is off');
                },
              })
            : null,
        ),
      ),
    );
  });
}

/* ---------------------------------------------------------------------------
 * About: the iPod's About screen, values right-aligned
 * ------------------------------------------------------------------------- */

function renderAbout(el) {
  const body = h('div');
  el.append(body);
  loading(body);
  let alive = true;
  (async function load() {
    try {
      const a = await GET('/api/about');
      if (!alive) return;
      const m = a.mpd || {};
      const rows = [];
      const add = (label, value) => value != null && value !== '' && rows.push({ label, value: String(value), onSelect: () => {} });
      rows.push({ header: 'Library' });
      add('Songs', m.songs != null ? m.songs.toLocaleString() : null);
      add('Albums', m.albums != null ? m.albums.toLocaleString() : null);
      add('Artists', m.artists != null ? m.artists.toLocaleString() : null);
      add('Playing Time', m.playtime != null ? fmtDuration(m.playtime) : null);
      add('Capacity', a.music ? fmtBytes(a.music.total) : null);
      add('Available', a.music ? fmtBytes(a.music.free) : null);
      add('Updated', m.db_update ? fmtDate(m.db_update) : null);
      rows.push({ header: 'rPod' });
      add('Version', a.version);
      add('Model', a.model);
      add('Name', a.hostname);
      const ifName = (n) => (/^wl/.test(n) ? 'Wi-Fi' : /^(eth|en)/.test(n) ? 'Ethernet' : /^usb/.test(n) ? 'USB' : n);
      for (const ad of a.addresses || []) add(ifName(ad.interface), ad.address);
      add('Up For', a.uptime != null ? fmtDuration(a.uptime) : null);
      add('Memory Free', a.mem_available >= 0 ? `${fmtBytes(a.mem_available)} of ${fmtBytes(a.mem_total)}` : null);
      add('Temperature', a.temperature != null ? `${a.temperature.toFixed(1)} °C` : null);
      add('Load', a.load ? a.load.map((x) => x.toFixed(2)).join('  ') : null);
      add('System Storage', a.system ? `${fmtBytes(a.system.free)} free of ${fmtBytes(a.system.total)}` : null);
      add('Kernel', a.kernel);
      add('MPD', m.version);
      add('rPod App', a.ui ? 'Running' : 'Not running');
      body.replaceChildren(makeList(rows));
      restoreSelection(activeList);
    } catch (e) {
      if (alive) failed(body, e, load);
    }
  })();
  return { stop: () => (alive = false) };
}

/* ---------------------------------------------------------------------------
 * The screens, by route
 * ------------------------------------------------------------------------- */

const ROUTES = {
  '': { title: 'rPod', render: renderHome },
  now: { title: 'Now Playing', parent: () => [''], render: renderNowPlaying },
  music: {
    title: (q) => (q.path ? basename(q.path) : 'Music'),
    parent: (q) => (q.path ? ['music', dirname(q.path) ? { path: dirname(q.path) } : {}] : ['']),
    render: renderMusic,
  },
  upload: { title: 'Upload Music', parent: () => [''], render: renderUpload },
  settings: { title: 'Settings', parent: () => [''], render: renderSettings },
  'settings/theme': { title: 'Theme', parent: () => ['settings'], render: renderTheme },
  'settings/backlight': { title: 'Backlight', parent: () => ['settings'], render: renderBacklight },
  'settings/sleep': { title: 'Sleep Timer', parent: () => ['settings'], render: renderSleep },
  'settings/alarm': { title: 'Alarm Clock', parent: () => ['settings'], render: renderAlarm },
  'settings/alarm/time': { title: 'Alarm Time', parent: () => ['settings/alarm'], render: renderAlarmTime },
  'settings/alarm/sound': { title: 'Sound', parent: () => ['settings/alarm'], render: renderAlarmSound },
  'settings/output': { title: 'Audio Output', parent: () => ['settings'], render: renderOutputs },
  'settings/playback': { title: 'Playback', parent: () => ['settings'], render: renderPlayback },
  'settings/wheel': { title: 'Click Wheel', parent: () => ['settings'], render: renderWheel },
  'settings/listenbrainz': { title: 'ListenBrainz', parent: () => ['settings'], render: renderListenBrainz },
  about: { title: 'About', parent: () => [''], render: renderAbout },
};

/* ---------------------------------------------------------------------------
 * Polling rPod's state
 * ------------------------------------------------------------------------- */

const backdrop = document.getElementById('backdrop');

function onState(st) {
  if (!st) return;
  const prevUri = S.st && S.st.player ? S.st.player.uri : null;
  S.st = st;
  S.stAt = Date.now();
  applyTheme(st.theme);
  const uri = st.player ? st.player.uri : null;
  if (uri !== prevUri) setArt(backdrop, uri);
  paintTitleState();
  if (current && current.inst && current.inst.state) current.inst.state(st);
}

// Glass's backdrop, the cover blurred, fades in once the art has loaded.
new MutationObserver(() => {
  const on = backdrop.classList.contains('has-art');
  if (backdrop.classList.contains('on') !== on) backdrop.classList.toggle('on', on);
}).observe(backdrop, { attributes: true, attributeFilter: ['class'] });

let pollTimer = null;
async function poll() {
  clearTimeout(pollTimer);
  try {
    onState(await GET('/api/state'));
    if (!S.reachable) hud('rPod is back');
    S.reachable = true;
  } catch (e) {
    if (S.reachable) hud("Can't reach rPod", true);
    S.reachable = false;
  }
  const fast = current && current.inst && current.inst.fastPoll;
  const delay = document.hidden ? 15000 : fast || UP.busy() ? 1000 : 2500;
  pollTimer = setTimeout(poll, delay);
}

/* ---------------------------------------------------------------------------
 * Keys: the wheel, its buttons, and Menu
 * ------------------------------------------------------------------------- */

function typing(t) {
  return t instanceof Element && (t.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(t.tagName));
}

document.addEventListener('keydown', (e) => {
  if (e.altKey || e.ctrlKey || e.metaKey) return;
  const t = e.target;
  if (e.key === 'Escape') {
    if (sheetState) return closeSheet(null);
    if (typing(t)) return t.blur();
    e.preventDefault();
    return goBack();
  }
  if (typing(t)) return;
  const onButton = t instanceof Element && (t.tagName === 'BUTTON' || t.tagName === 'A' || t.getAttribute('role') === 'slider');
  switch (e.key) {
    case 'ArrowDown':
    case 'ArrowUp':
      e.preventDefault();
      moveSelection(e.key === 'ArrowDown' ? 1 : -1);
      break;
    case 'Enter':
    case 'ArrowRight': {
      if (onButton && e.key === 'Enter') return;
      const row = selectedRow();
      if (row && (e.key === 'Enter' || row._spec.arrow)) {
        e.preventDefault();
        activate(row);
      }
      break;
    }
    case 'ArrowLeft':
    case 'Backspace':
      if (onButton && e.key === 'ArrowLeft') return;
      e.preventDefault();
      goBack();
      break;
    case ' ':
      if (onButton) return;
      e.preventDefault();
      transport('toggle');
      break;
    case 'n':
    case 'N':
      transport('next');
      break;
    case 'p':
    case 'P':
      transport('prev');
      break;
    default:
  }
});

tb.back.addEventListener('click', goBack);

/* ---------------------------------------------------------------------------
 * Dropping files anywhere uploads them into the folder in view
 * ------------------------------------------------------------------------- */

const dropOverlay = document.getElementById('drop-overlay');
let dragDepth = 0;

function dropDir() {
  return (current && current.inst && current.inst.dropDir) || '';
}

document.addEventListener('dragenter', (e) => {
  if (!e.dataTransfer || ![...e.dataTransfer.types].includes('Files')) return;
  e.preventDefault();
  dragDepth++;
  const d = dropDir();
  document.getElementById('drop-msg').textContent = `Drop to upload into ${d ? `Music › ${d.split('/').join(' › ')}` : 'Music'}`;
  dropOverlay.hidden = false;
});
document.addEventListener('dragover', (e) => {
  if (e.dataTransfer && [...e.dataTransfer.types].includes('Files')) e.preventDefault();
});
document.addEventListener('dragleave', () => {
  if (--dragDepth <= 0) {
    dragDepth = 0;
    dropOverlay.hidden = true;
  }
});
document.addEventListener('drop', async (e) => {
  if (!e.dataTransfer || ![...e.dataTransfer.types].includes('Files')) return;
  e.preventDefault();
  dragDepth = 0;
  dropOverlay.hidden = true;
  document.querySelectorAll('.dropzone.over').forEach((z) => z.classList.remove('over'));
  const dest = dropDir();
  const files = await filesFromDrop(e.dataTransfer);
  if (!files.length) return;
  const added = await UP.add(files, dest);
  if (added && !(current && current.route === ROUTES.upload)) go('upload', dest ? { dir: dest } : {});
});

/* ---------------------------------------------------------------------------
 * Start
 * ------------------------------------------------------------------------- */

(async function start() {
  applyTheme(document.documentElement.dataset.theme || 'glass');
  tickClock();
  setInterval(tickClock, 10000);
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) poll();
  });
  window.addEventListener('hashchange', rerender);
  await poll();
  if (S.st && S.st.ipod_assets) await loadIpodAssets();
  rerender();
})();

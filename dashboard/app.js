// IoT Terrarium dashboard. Served by the receiver board (terrarium_receiver) at http://192.168.4.1.
//
// Modes:
//   on the board            talks to the board's JSON API (see terrarium_receiver.ino for the endpoints)
//   ?board=192.168.4.1      a local copy of this folder talking to a real board (for development)
//   demo                    simulated data from demo.js: on localhost, file://, GitHub Pages, or with ?demo
'use strict';

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

const params = new URLSearchParams(location.search);
const BOARD = params.get('board');
const API_BASE = BOARD ? `http://${BOARD}` : '';
const DEMO = params.has('demo') ||
  (!BOARD && (location.protocol === 'file:' || /^(localhost|127\.0\.0\.1|\[::1\])$|github\.io$/.test(location.hostname)));

const STATUS_POLL_MS = 3000;
const HISTORY_POLL_MS = 60000;
const SPARK_HOURS = 2;
// Smallest vertical span a sparkline shows, so sensor jitter on a steady reading doesn't look dramatic.
const SPARK_MIN_SPAN = { humidity: 20, temperature: 3, light: 30, water: 20, h2: 200 };

// History rows from the board are [uptime, humidity, temperature, light, water, h2, rssi].
const METRICS = {
  humidity: {
    label: 'Humidity', unit: '%', column: 1, decimals: 0, domain: [0, 100],
    icon: '<path d="M12 3.5c3.2 4 5.5 7.2 5.5 10a5.5 5.5 0 0 1-11 0c0-2.8 2.3-6 5.5-10z"/>',
  },
  temperature: {
    label: 'Temperature', unit: '°C', column: 2, decimals: 1,
    icon: '<path d="M10 14.5V5a2 2 0 0 1 4 0v9.5a4 4 0 1 1-4 0z"/><path d="M12 9v7"/>',
  },
  light: {
    label: 'Light', unit: '%', column: 3, decimals: 0, domain: [0, 100],
    icon: '<circle cx="12" cy="12" r="3.6"/><path d="M12 3v2.2M12 18.8V21M3 12h2.2M18.8 12H21M5.6 5.6l1.6 1.6M16.8 16.8l1.6 1.6M5.6 18.4l1.6-1.6M16.8 7.2l1.6-1.6"/>',
  },
  water: {
    label: 'Water', unit: '%', column: 4, decimals: 0, domain: [0, 100],
    icon: '<path d="M3 9c1.5-1.3 3-1.3 4.5 0s3 1.3 4.5 0 3-1.3 4.5 0 3 1.3 4.5 0M3 14c1.5-1.3 3-1.3 4.5 0s3 1.3 4.5 0 3-1.3 4.5 0 3 1.3 4.5 0M3 19c1.5-1.3 3-1.3 4.5 0s3 1.3 4.5 0 3-1.3 4.5 0 3 1.3 4.5 0"/>',
  },
  h2: {
    label: 'Hydrogen', unit: 'ppm', column: 5, decimals: 0,
    icon: 'H₂',
  },
};
const RSSI = { label: 'Signal', unit: 'dBm', column: 6, decimals: 0 };
const METRIC_KEYS = Object.keys(METRICS);

const DEFAULT_SETTINGS = {
  name: 'Terrarium',
  units: 'C',
  ranges: {
    humidity: { min: 50, max: 80 },
    temperature: { min: 21, max: 29 },   // always stored in °C
    light: { min: null, max: null },
    water: { min: 20, max: null },
  },
  h2Alert: 100,
};

const state = {
  status: null,
  ref: null,            // {uptime, ms, boot}: maps the board's uptime seconds to this device's clock
  failures: 0,
  history: [],
  historyBoot: null,
  historyLoading: false,
  photos: [],
  photosKey: '',
  settings: clone(DEFAULT_SETTINGS),
  clockSent: false,
  view: 'overview',
  range: 6,
  shown: new Set(METRIC_KEYS),
  snapRequestedAt: 0,
  lightboxIndex: -1,
};

// ---------------------------------------------------------------- utilities

function clone(o) { return JSON.parse(JSON.stringify(o)); }
function esc(s) { return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }
function clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)); }

function mergeSettings(saved) {
  const s = clone(DEFAULT_SETTINGS);
  if (!saved || typeof saved !== 'object') return s;
  if (typeof saved.name === 'string' && saved.name.trim()) s.name = saved.name.trim().slice(0, 40);
  if (saved.units === 'C' || saved.units === 'F') s.units = saved.units;
  for (const k of Object.keys(s.ranges)) {
    const r = saved.ranges && saved.ranges[k];
    if (!r) continue;
    for (const end of ['min', 'max']) {
      if (r[end] === null || Number.isFinite(r[end])) s.ranges[k][end] = r[end];
    }
  }
  if (Number.isFinite(saved.h2Alert)) s.h2Alert = saved.h2Alert;
  return s;
}

const toF = c => c * 9 / 5 + 32;
const toC = f => (f - 32) * 5 / 9;

// Value in display units (temperature may be °F).
function display(key, v) {
  if (v === null || v === undefined) return null;
  return key === 'temperature' && state.settings.units === 'F' ? toF(v) : v;
}
function unitOf(key) {
  if (key === 'temperature') return state.settings.units === 'F' ? '°F' : '°C';
  return (METRICS[key] || RSSI).unit;
}
function fmt(key, v) {
  const d = display(key, v);
  if (d === null || !Number.isFinite(d)) return '–';
  return d.toFixed((METRICS[key] || RSSI).decimals);
}

function fmtAgo(seconds) {
  if (seconds < 5) return 'just now';
  if (seconds < 60) return `${Math.round(seconds)} s ago`;
  if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
  if (seconds < 86400) return `${Math.round(seconds / 3600)} h ago`;
  return `${Math.round(seconds / 86400)} d ago`;
}
function fmtDuration(seconds) {
  const d = Math.floor(seconds / 86400), h = Math.floor(seconds % 86400 / 3600), m = Math.floor(seconds % 3600 / 60);
  return d ? `${d} d ${h} h` : h ? `${h} h ${m} min` : `${m} min`;
}
function fmtTime(ms) { return new Date(ms).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }); }
function fmtDateTime(ms) {
  const d = new Date(ms), today = new Date();
  const sameDay = d.toDateString() === today.toDateString();
  return sameDay ? `Today ${fmtTime(ms)}`
    : d.toLocaleString([], { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' });
}

// Board uptime (seconds) -> this device's clock (ms). Only valid for the board's current power-up.
function uptimeToMs(t, boot) {
  const r = state.ref;
  if (!r || (boot !== undefined && boot !== r.boot)) return null;
  return r.ms - (r.uptime - t) * 1000;
}
function boardNowUptime() {
  const r = state.ref;
  return r ? r.uptime + (Date.now() - r.ms) / 1000 : 0;
}
function photoTime(p) {
  if (p.epoch) return p.epoch * 1000;
  return p.uptime ? uptimeToMs(p.uptime, p.boot) : null;
}

// Local time as "2026-11-06 14:10" (for CSV) or "20261106-1410" (for file names).
function localStamp(ms, compact = false) {
  const d = new Date(ms), p = n => String(n).padStart(2, '0');
  const date = [d.getFullYear(), p(d.getMonth() + 1), p(d.getDate())], time = [p(d.getHours()), p(d.getMinutes())];
  return compact ? `${date.join('')}-${time.join('')}` : `${date.join('-')} ${time.join(':')}`;
}

let toastTimer;
function toast(msg) {
  const el = $('#toast');
  el.textContent = msg;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 3200);
}

// ---------------------------------------------------------------- API

async function request(path, { method = 'GET', body } = {}) {
  if (DEMO) return window.TerrariumDemo.request(path, method, body);
  const res = await fetch(API_BASE + path, {
    method,
    body,
    cache: 'no-store',
    // text/plain keeps cross-origin POSTs "simple" (no preflight) when developing with ?board=
    headers: body !== undefined ? { 'Content-Type': 'text/plain' } : undefined,
  });
  const text = await res.text();
  if (!res.ok) {
    const err = new Error(text || res.statusText);
    err.status = res.status;
    throw err;
  }
  return (res.headers.get('content-type') || '').includes('json') ? JSON.parse(text) : text;
}

const photoUrl = n => DEMO ? window.TerrariumDemo.photoUrl(n) : `${API_BASE}/photo.jpg?n=${n}`;

async function refreshStatus() {
  try {
    const s = await request('/api/status');
    state.status = s;
    state.ref = { uptime: s.uptime, ms: Date.now(), boot: s.boot };
    state.failures = 0;

    // The board has no clock of its own: give it ours so new photos get real timestamps.
    if (!state.clockSent && (!s.clock || Math.abs(s.clock * 1000 - Date.now()) > 120000)) {
      state.clockSent = true;
      request('/api/time', { method: 'POST', body: String(Math.floor(Date.now() / 1000)) }).catch(() => {});
    }
    if (s.boot !== state.historyBoot) refreshHistory(true);
    const photosKey = `${s.boot}:${s.photo.latest}:${s.photo.count}`;
    if (photosKey !== state.photosKey) refreshPhotos(photosKey);
  } catch (e) {
    state.failures++;
  }
  renderStatus();
}

async function refreshHistory(reset = false) {
  if (state.historyLoading) return;
  state.historyLoading = true;
  try {
    const after = !reset && state.history.length ? state.history[state.history.length - 1][0] : 0;
    const h = await request(`/api/history?after=${after}`);
    if (reset || h.boot !== state.historyBoot) state.history = [];
    state.historyBoot = h.boot;
    state.history.push(...h.rows);
    const cutoff = h.uptime - 24 * 3600;
    while (state.history.length && state.history[0][0] < cutoff) state.history.shift();
  } catch (e) {
    // keep what we have; the next poll retries
  } finally {
    state.historyLoading = false;
  }
  renderMetrics();
  if (state.view === 'history') renderHistory();
}

async function refreshPhotos(key) {
  try {
    const p = await request('/api/photos');
    state.photos = p.photos;
    state.photosKey = key;
  } catch (e) {
    return;
  }
  renderPhotos();
}

async function loadSettings() {
  try {
    state.settings = mergeSettings(await request('/api/settings'));
  } catch (e) {
    state.settings = clone(DEFAULT_SETTINGS);
  }
  fillSettingsForm();
  renderAll();
}

// ---------------------------------------------------------------- evaluation & alerts

function currentValue(key, r) {
  if (!r) return null;
  if (key === 'h2') return r.h2.state === 'ok' ? r.h2.ppm : r.h2.state === 'low' ? 0 : null;
  return r[key];
}

// -> {level: ok|warn|bad|info|none, label, message?}
function evaluate(key, r) {
  if (!r) return { level: 'none', label: 'No data' };
  const m = METRICS[key];
  if (key === 'h2') {
    const { state: st, ppm } = r.h2;
    if (st === 'warming') return { level: 'info', label: 'Warming up', message: 'Hydrogen sensor is warming up (about a minute after power-up).' };
    if (st === 'nosignal') return { level: 'bad', label: 'No signal', message: 'Hydrogen sensor (MQ-8) isn\'t responding. Check its wiring and 5 V supply.' };
    if (st === 'ok' && ppm >= state.settings.h2Alert)
      return { level: 'bad', label: 'Detected', message: `Hydrogen detected: ${ppm} ppm (alert at ${state.settings.h2Alert} ppm).` };
    return { level: 'ok', label: st === 'low' ? 'None' : 'Normal' };
  }
  const v = r[key];
  if (v === null || v === undefined) {
    if (key === 'water') return { level: 'none', label: 'No sensor' };
    return { level: 'bad', label: 'Sensor error', message: `${m.label} sensor isn't responding. Check its wiring.` };
  }
  const range = state.settings.ranges[key];
  if (!range || (range.min === null && range.max === null)) return { level: 'info', label: 'Monitoring' };
  const target = targetText(key);
  if (range.min !== null && v < range.min)
    return { level: 'warn', label: 'Low', message: `${m.label} is low: ${fmt(key, v)} ${unitOf(key)} (target ${target}).` };
  if (range.max !== null && v > range.max)
    return { level: 'warn', label: 'High', message: `${m.label} is high: ${fmt(key, v)} ${unitOf(key)} (target ${target}).` };
  return { level: 'ok', label: 'In range' };
}

function targetText(key) {
  if (key === 'h2') return `alert at ${state.settings.h2Alert} ppm`;
  const r = state.settings.ranges[key];
  if (!r || (r.min === null && r.max === null)) return '';
  const u = unitOf(key);
  if (r.min !== null && r.max !== null) return `${fmt(key, r.min)}–${fmt(key, r.max)} ${u}`;
  return r.min !== null ? `above ${fmt(key, r.min)} ${u}` : `below ${fmt(key, r.max)} ${u}`;
}

function collectAlerts() {
  const s = state.status;
  if (!s) return [];
  const alerts = [];
  if (!s.link.hasData) {
    alerts.push({ level: 'info', message: 'Waiting for the first reading from the terrarium. It sends one every 10 seconds.' });
    return alerts;
  }
  if (s.link.lost) {
    alerts.push({ level: 'bad', message: `No data from the terrarium for ${fmtAgo(s.link.age).replace(' ago', '')}. The readings below are the last ones received.` });
  }
  for (const key of METRIC_KEYS) {
    const e = evaluate(key, s.reading);
    if (e.message && e.level !== 'ok') alerts.push(e);
  }
  return alerts;
}

const ALERT_ICON = {
  bad: '<svg viewBox="0 0 24 24"><path d="M12 4l9 16H3z"/><path d="M12 10v4M12 17.2v.1"/></svg>',
  warn: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7.5v5.5M12 16.2v.1"/></svg>',
  info: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 11v5.5M12 7.8v.1"/></svg>',
};

// ---------------------------------------------------------------- rendering: header & overview

function renderStatus() {
  const s = state.status;
  const offline = !DEMO && state.failures >= 2;
  $('#offlineBanner').hidden = !offline;

  const pill = $('#linkPill');
  if (!s) {
    pill.className = 'pill';
    $('#linkText').textContent = offline ? 'Offline' : 'Connecting';
  } else if (offline) {
    pill.className = 'pill bad';
    $('#linkText').textContent = 'Board offline';
  } else if (!s.link.hasData) {
    pill.className = 'pill';
    $('#linkText').textContent = 'Waiting for data';
  } else if (s.link.lost) {
    pill.className = 'pill bad';
    $('#linkText').textContent = 'Link lost';
  } else {
    pill.className = 'pill ok';
    $('#linkText').textContent = 'Live';
  }
  $('#subtitle').textContent = s && s.link.hasData
    ? `Updated ${fmtAgo(s.link.age)} · reading #${s.link.count}`
    : s ? 'Board online, waiting for the sender' : 'Connecting to the board…';

  $('#alerts').innerHTML = collectAlerts()
    .map(a => `<div class="alert ${a.level}">${ALERT_ICON[a.level] || ALERT_ICON.info}<span>${esc(a.message)}</span></div>`)
    .join('');

  renderMetrics();
  renderLink();
  renderSnap();
  renderBoard();
}

function renderMetrics() {
  const s = state.status;
  const r = s ? s.reading : null;
  const end = Date.now(), start = end - SPARK_HOURS * 3600e3;
  $('#metrics').innerHTML = METRIC_KEYS.map(key => {
    const m = METRICS[key];
    const e = evaluate(key, r);
    let value, text = false;
    if (key === 'h2' && r) {
      const st = r.h2.state;
      if (st === 'ok') value = String(r.h2.ppm);
      else if (st === 'low') value = '<100';
      else { value = st === 'warming' ? 'Warming' : 'No signal'; text = true; }
    } else {
      value = fmt(key, currentValue(key, r));
    }
    const icon = m.icon.startsWith('<') ? `<svg viewBox="0 0 24 24">${m.icon}</svg>` : m.icon;
    const target = targetText(key);
    const foot = key === 'h2' && r && r.h2.state === 'low' ? 'Below the 100 ppm detection limit' : target ? `Target ${target}` : 'No target set';
    return `<button class="metric" data-metric="${key}" style="--metric: var(--c-${key})" aria-label="${m.label}: ${esc(value)} ${esc(text ? '' : unitOf(key))}, ${e.label}. Open history.">
      <div class="metric-head"><span class="metric-icon">${icon}</span>${m.label}<span class="chip ${e.level === 'none' ? '' : e.level}">${e.label}</span></div>
      <div class="metric-value"><span class="num${text ? ' text' : ''}">${esc(value)}</span>${text || value === '–' ? '' : `<span class="unit">${unitOf(key)}</span>`}</div>
      <div class="metric-foot">${esc(foot)}</div>
      ${sparkline(key, start, end)}
    </button>`;
  }).join('');
}

function seriesFor(key, startMs, endMs) {
  const col = (METRICS[key] || RSSI).column;
  const pts = [];
  for (const row of state.history) {
    const t = uptimeToMs(row[0]);
    if (t === null || t < startMs || t > endMs) continue;
    pts.push([t, row[col]]);
  }
  return pts;
}

function sparkline(key, start, end) {
  const pts = seriesFor(key, start, end).filter(p => p[1] !== null);
  if (pts.length < 2) return '<svg class="spark" aria-hidden="true"></svg>';
  const W = 200, H = 40, pad = 4;
  let lo = Math.min(...pts.map(p => p[1])), hi = Math.max(...pts.map(p => p[1]));
  const minSpan = SPARK_MIN_SPAN[key] || 1;
  if (hi - lo < minSpan) {
    const mid = (hi + lo) / 2;
    [lo, hi] = [mid - minSpan / 2, mid + minSpan / 2];
  }
  const x = t => (t - start) / (end - start) * W;
  const y = v => H - pad - (v - lo) / (hi - lo) * (H - pad * 2);
  const line = pts.map((p, i) => `${i ? 'L' : 'M'}${x(p[0]).toFixed(1)},${y(p[1]).toFixed(1)}`).join('');
  const area = `${line}L${x(pts[pts.length - 1][0]).toFixed(1)},${H}L${x(pts[0][0]).toFixed(1)},${H}Z`;
  return `<svg class="spark" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" aria-hidden="true">
    <path d="${area}" fill="var(--metric)" opacity="0.12"/>
    <path d="${line}" fill="none" stroke="var(--metric)" stroke-width="2" vector-effect="non-scaling-stroke" stroke-linejoin="round"/>
  </svg>`;
}

function signalQuality(rssi, snr) {
  if (rssi > -80 && snr > 5) return { bars: 4, label: 'Excellent' };
  if (rssi > -100 && snr > 0) return { bars: 3, label: 'Good' };
  if (rssi > -115 && snr > -10) return { bars: 2, label: 'Fair' };
  return { bars: 1, label: 'Weak' };
}

function renderLink() {
  const s = state.status;
  const bars = $$('#signalBars i');
  if (!s || !s.link.hasData) {
    bars.forEach(b => b.classList.remove('on'));
    $('#signalLabel').textContent = s ? 'No signal yet' : '–';
    $('#signalDetail').textContent = 'LoRa 915 MHz';
    $('#linkStats').innerHTML = '';
    return;
  }
  const L = s.link;
  const q = L.lost ? { bars: 0, label: 'Link lost' } : signalQuality(L.rssi, L.snr);
  bars.forEach((b, i) => b.classList.toggle('on', i < q.bars));
  $('#signalBars').classList.toggle('weak', q.bars <= 2);
  $('#signalLabel').textContent = q.label;
  $('#signalDetail').textContent = `${L.rssi} dBm · SNR ${L.snr} dB · LoRa 915 MHz`;
  const total = L.received + L.missed;
  const loss = total ? (L.missed / total * 100) : 0;
  $('#linkStats').innerHTML = `
    <dt>Last reading</dt><dd>${fmtAgo(L.age)}</dd>
    <dt>Readings received</dt><dd>${L.received.toLocaleString()}</dd>
    <dt>Missed</dt><dd>${L.missed.toLocaleString()} (${loss === 0 ? '0' : loss < 0.1 ? '<0.1' : loss.toFixed(loss < 10 ? 1 : 0)}%)</dd>
    <dt>Photos stored</dt><dd>${s.photo.count}</dd>`;
}

function renderSnap() {
  const p = state.status ? state.status.photo : null;
  const waiting = p && (p.pending || (state.snapRequestedAt && Date.now() - state.snapRequestedAt < 8000)) && !p.inProgress;
  for (const box of $$('[data-snap]')) {
    const btn = $('[data-snap-btn]', box), bar = $('[data-snap-progress]', box), text = $('[data-snap-text]', box);
    btn.disabled = !p || p.inProgress || waiting || !state.status.link.hasData;
    bar.hidden = !(p && (p.inProgress || waiting));
    bar.classList.toggle('indeterminate', !!waiting);
    $('.bar', bar).style.width = p && p.inProgress && p.total ? `${p.got / p.total * 100}%` : '';
    if (p && p.inProgress) {
      btn.textContent = 'Receiving photo…';
      text.textContent = `Received ${p.got} of ${p.total} pieces over LoRa`;
    } else if (waiting) {
      btn.textContent = 'Waiting for the camera…';
      text.textContent = 'The camera takes the photo, then it arrives in about 30 seconds.';
    } else {
      btn.textContent = 'Take photo now';
      text.textContent = state.status && !state.status.link.hasData ? 'Available once the terrarium is sending data.' : '';
    }
  }
}

// ---------------------------------------------------------------- photos

function photoCaption(p) {
  const t = photoTime(p);
  const when = t ? fmtDateTime(t) : 'time unknown (taken before the clock was set)';
  return `Photo #${p.n} · ${when} · ${(p.bytes / 1024).toFixed(1)} KB`;
}

function renderPhotos() {
  const photos = state.photos;
  const latest = photos[photos.length - 1];
  for (const [img, empty, caption] of [['#latestPhoto', '#latestPhotoEmpty', '#latestPhotoCaption'],
                                        ['#cameraPhoto', '#cameraPhotoEmpty', '#cameraCaption']]) {
    if (latest) {
      const src = photoUrl(latest.n);
      if ($(img).getAttribute('src') !== src) $(img).src = src;
      $(caption).textContent = photoCaption(latest);
    } else {
      $(caption).textContent = 'Photos arrive every 5 minutes, or tap "Take photo now".';
    }
    $(img).hidden = !latest;
    $(empty).hidden = !!latest;
  }
  $('#photoCount').textContent = photos.length ? `${photos.length} kept on the board` : '';
  $('#gallery').innerHTML = photos.length
    ? photos.slice().reverse().map((p, i) => {
      const t = photoTime(p);
      return `<button class="thumb" data-index="${photos.length - 1 - i}" aria-label="Open photo ${p.n}">
        <img src="${photoUrl(p.n)}" alt="" loading="lazy"><span>${t ? fmtTime(t) : '#' + p.n}</span></button>`;
    }).join('')
    : '<p class="gallery-empty">No photos yet.</p>';
}

function openLightbox(index) {
  if (!state.photos.length) return;
  state.lightboxIndex = clamp(index, 0, state.photos.length - 1);
  const p = state.photos[state.lightboxIndex];
  $('#lightboxImg').src = photoUrl(p.n);
  $('#lightboxImg').alt = `Terrarium photo ${p.n}`;
  $('#lightboxCaption').textContent = photoCaption(p);
  const t = photoTime(p);
  $('#lightboxDownload').href = photoUrl(p.n);
  $('#lightboxDownload').download = `terrarium-${p.n}${t ? '-' + localStamp(t, true) : ''}.jpg`;
  $('#lightboxPrev').disabled = state.lightboxIndex === 0;
  $('#lightboxNext').disabled = state.lightboxIndex === state.photos.length - 1;
  const dlg = $('#lightbox');
  if (!dlg.open) dlg.showModal();
}

async function takePhoto() {
  state.snapRequestedAt = Date.now();
  renderSnap();
  try {
    await request('/api/snap', { method: 'POST', body: '' });
    toast('Asked the camera for a photo');
  } catch (e) {
    state.snapRequestedAt = 0;
    toast(e.status === 409 ? 'A photo is already on its way' : 'Couldn\'t reach the board');
  }
  refreshStatus();
}

// ---------------------------------------------------------------- history charts

const charts = [];   // {el, key, x, xinv, y, pts, t0, t1, H}

function niceStep(span, target) {
  const raw = span / target, mag = 10 ** Math.floor(Math.log10(raw));
  return [1, 2, 2.5, 5, 10].map(f => f * mag).find(s => s >= raw) || 10 * mag;
}

function yDomain(key, values) {
  const m = METRICS[key] || RSSI;
  if (m.domain) return m.domain;
  let lo = Math.min(...values), hi = Math.max(...values);
  if (key === 'h2') return [0, Math.max(200, hi * 1.2)];
  const r = state.settings.ranges[key];
  if (r) { if (r.min !== null) lo = Math.min(lo, display(key, r.min)); if (r.max !== null) hi = Math.max(hi, display(key, r.max)); }
  const pad = Math.max((hi - lo) * 0.15, key === 'temperature' ? 1 : 2);
  return [lo - pad, hi + pad];
}

function renderHistory() {
  const end = Date.now(), start = end - state.range * 3600e3;
  const n = state.history.length;
  const coveredMin = n ? Math.round((state.history[n - 1][0] - state.history[0][0]) / 60) : 0;
  $('#historyNote').textContent = n
    ? `One point per minute from the board's memory · ${fmtDuration(coveredMin * 60)} recorded since it last started`
    : 'History builds up while the receiver board runs: one point per minute, up to 24 hours.';

  charts.length = 0;
  const keys = METRIC_KEYS.filter(k => state.shown.has(k));
  $('#charts').innerHTML = keys.map(key =>
    `<article class="card chart-card" style="--metric: var(--c-${key})">
      <div class="chart-head"><h2>${METRICS[key].label}</h2><span class="chart-summary" data-summary="${key}"></span></div>
      <div class="chart" data-chart="${key}"></div>
    </article>`).join('') || '<p class="muted">Pick at least one sensor above.</p>';

  for (const key of keys) drawChart($(`[data-chart="${key}"]`), key, start, end);
}

function drawChart(el, key, t0, t1) {
  const raw = seriesFor(key, t0, t1);
  const pts = raw.filter(p => p[1] !== null).map(p => [p[0], display(key, p[1])]);
  const summary = $(`[data-summary="${key}"]`);
  if (pts.length < 2) {
    el.innerHTML = '<div class="chart-empty">No data in this time range yet</div>';
    summary.textContent = '';
    return;
  }
  const vals = pts.map(p => p[1]);
  const u = unitOf(key), dec = METRICS[key].decimals;
  summary.textContent = `min ${Math.min(...vals).toFixed(dec)} · max ${Math.max(...vals).toFixed(dec)} ${u}`;

  const W = Math.max(280, el.clientWidth), H = W < 520 ? 150 : 180;
  const m = { l: 38, r: 10, t: 10, b: 22 };
  const [lo, hi] = yDomain(key, vals);
  const x = t => m.l + (t - t0) / (t1 - t0) * (W - m.l - m.r);
  const xinv = px => t0 + (px - m.l) / (W - m.l - m.r) * (t1 - t0);
  const y = v => m.t + (1 - (v - lo) / (hi - lo)) * (H - m.t - m.b);

  // Path, broken wherever more than 3 minutes of data is missing.
  let line = '', area = '', seg = [];
  const flush = () => {
    if (seg.length > 1) {
      const d = seg.map((p, i) => `${i ? 'L' : 'M'}${x(p[0]).toFixed(1)},${y(p[1]).toFixed(1)}`).join('');
      line += d;
      area += `${d}L${x(seg[seg.length - 1][0]).toFixed(1)},${H - m.b}L${x(seg[0][0]).toFixed(1)},${H - m.b}Z`;
    }
    seg = [];
  };
  let prevT = null;
  for (const p of raw) {
    if (p[1] === null || (prevT !== null && p[0] - prevT > 180e3)) flush();
    if (p[1] !== null) seg.push([p[0], display(key, p[1])]);
    prevT = p[0];
  }
  flush();

  // Axes
  const step = niceStep(hi - lo, 4);
  let grid = '';
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) {
    grid += `<line class="grid" x1="${m.l}" x2="${W - m.r}" y1="${y(v).toFixed(1)}" y2="${y(v).toFixed(1)}"/>
      <text x="${m.l - 6}" y="${(y(v) + 4).toFixed(1)}" text-anchor="end">${+v.toFixed(2)}</text>`;
  }
  const minuteSteps = [5, 10, 15, 30, 60, 120, 180, 240, 360];
  const xStep = minuteSteps.find(s => (t1 - t0) / (s * 60e3) <= Math.max(3, Math.floor(W / 90))) * 60e3;
  const tz = new Date().getTimezoneOffset() * 60e3;
  for (let t = Math.ceil((t0 - tz) / xStep) * xStep + tz; t <= t1; t += xStep) {
    grid += `<text x="${x(t).toFixed(1)}" y="${H - 4}" text-anchor="middle">${fmtTime(t)}</text>`;
  }

  // Target band
  let band = '';
  const r = state.settings.ranges[key];
  const bandLo = key === 'h2' ? null : r && r.min !== null ? display(key, r.min) : null;
  const bandHi = key === 'h2' ? display(key, state.settings.h2Alert) : r && r.max !== null ? display(key, r.max) : null;
  if (bandLo !== null || bandHi !== null) {
    const top = y(clamp(bandHi === null ? hi : bandHi, lo, hi)), bottom = y(clamp(bandLo === null ? lo : bandLo, lo, hi));
    if (key !== 'h2') band = `<rect class="band" x="${m.l}" y="${top}" width="${W - m.l - m.r}" height="${Math.max(0, bottom - top)}"/>`;
    for (const edge of [bandLo, bandHi]) {
      if (edge !== null && edge > lo && edge < hi) band += `<line class="band-edge" x1="${m.l}" x2="${W - m.r}" y1="${y(edge)}" y2="${y(edge)}"/>`;
    }
  }

  el.innerHTML = `<svg viewBox="0 0 ${W} ${H}" height="${H}" role="img" aria-label="${METRICS[key].label} over the last ${state.range} hours, from ${Math.min(...vals).toFixed(dec)} to ${Math.max(...vals).toFixed(dec)} ${u}">
    ${grid}${band}
    <path class="area" d="${area}"/><path class="line" d="${line}"/>
    <g class="cursor" visibility="hidden">
      <line class="cursor-line" y1="${m.t}" y2="${H - m.b}"/>
      <circle class="cursor-dot" r="4.5"/>
      <rect class="tip-bg" rx="6" height="22"/><text class="tip" y="${m.t + 15}"></text>
    </g>
  </svg>`;

  const chart = { el, key, x, xinv, y, pts, W, m, H, u, dec };
  charts.push(chart);
  const move = ev => {
    const rect = el.getBoundingClientRect();
    const px = (ev.clientX - rect.left) / rect.width * W;
    setCursor(px < m.l || px > W - m.r ? null : xinv(px));
  };
  el.addEventListener('pointermove', move);
  el.addEventListener('pointerdown', move);
  el.addEventListener('pointerleave', () => setCursor(null));
}

// One cursor time shared by every chart, so readings line up across sensors.
function setCursor(t) {
  for (const c of charts) {
    const g = $('.cursor', c.el);
    if (!g) continue;
    if (t === null) { g.setAttribute('visibility', 'hidden'); continue; }
    // nearest point within 3 minutes
    let best = null, bestD = Infinity;
    let lo = 0, hi = c.pts.length - 1;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (c.pts[mid][0] < t) lo = mid + 1; else hi = mid; }
    for (const i of [lo - 1, lo]) {
      if (i < 0 || i >= c.pts.length) continue;
      const d = Math.abs(c.pts[i][0] - t);
      if (d < bestD) { bestD = d; best = c.pts[i]; }
    }
    if (!best || bestD > 180e3) { g.setAttribute('visibility', 'hidden'); continue; }
    const cx = c.x(best[0]), cy = c.y(best[1]);
    g.setAttribute('visibility', 'visible');
    $('.cursor-line', g).setAttribute('x1', cx);
    $('.cursor-line', g).setAttribute('x2', cx);
    $('.cursor-dot', g).setAttribute('cx', cx);
    $('.cursor-dot', g).setAttribute('cy', cy);
    const text = $('.tip', g);
    text.textContent = `${fmtTime(best[0])} · ${best[1].toFixed(c.dec)} ${c.u}`;
    const w = text.getComputedTextLength() + 16;
    const tx = clamp(cx - w / 2, c.m.l, c.W - c.m.r - w);
    text.setAttribute('x', tx + 8);
    const bg = $('.tip-bg', g);
    bg.setAttribute('x', tx);
    bg.setAttribute('y', c.m.t);
    bg.setAttribute('width', w);
  }
}

function exportCsv() {
  const end = Date.now(), start = end - state.range * 3600e3;
  const tUnit = state.settings.units === 'F' ? 'F' : 'C';
  const lines = [`local_time,humidity_pct,temperature_${tUnit},light_pct,water_pct,h2_ppm,rssi_dbm`];
  for (const row of state.history) {
    const t = uptimeToMs(row[0]);
    if (t === null || t < start || t > end) continue;
    const cell = (key, v) => v === null ? '' : display(key, v).toFixed(key === 'temperature' ? 1 : 0);
    lines.push([localStamp(t), cell('humidity', row[1]), cell('temperature', row[2]), cell('light', row[3]),
      cell('water', row[4]), cell('h2', row[5]), row[6] === null ? '' : row[6]].join(','));
  }
  if (lines.length === 1) { toast('No history in this time range yet'); return; }
  const blob = new Blob([lines.join('\n') + '\n'], { type: 'text/csv' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `terrarium-history-${localStamp(Date.now(), true)}.csv`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  toast(`Exported ${lines.length - 1} readings`);
}

// ---------------------------------------------------------------- settings & board info

const RANGE_KEYS = ['humidity', 'temperature', 'light', 'water'];

function fillSettingsForm() {
  const f = $('#settingsForm'), s = state.settings;
  f.name.value = s.name;
  f.h2Alert.value = s.h2Alert;
  $$('#unitPicker button').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.unit === s.units)));
  renderRangeFields();
}

function renderRangeFields() {
  const unitsF = $('#unitPicker [aria-pressed="true"]').dataset.unit === 'F';
  const val = (key, v) => v === null ? '' : +(key === 'temperature' && unitsF ? toF(v) : v).toFixed(1);
  $('#rangeFields').innerHTML = '<span></span><span class="head">Min</span><span class="head">Max</span>' +
    RANGE_KEYS.map(key => {
      const r = state.settings.ranges[key];
      const u = key === 'temperature' ? (unitsF ? '°F' : '°C') : METRICS[key].unit;
      return `<label for="min-${key}" style="--metric: var(--c-${key})">${METRICS[key].label} <span class="unit-hint">${u}</span></label>
        <input id="min-${key}" name="min-${key}" type="number" step="0.5" inputmode="decimal" value="${val(key, r.min)}" aria-label="${METRICS[key].label} minimum">
        <input id="max-${key}" name="max-${key}" type="number" step="0.5" inputmode="decimal" value="${val(key, r.max)}" aria-label="${METRICS[key].label} maximum">`;
  }).join('');
}

async function saveSettings(ev) {
  ev.preventDefault();
  const f = $('#settingsForm');
  const units = $('#unitPicker [aria-pressed="true"]').dataset.unit;
  const next = { name: f.name.value.trim() || 'Terrarium', units, ranges: {}, h2Alert: Number(f.h2Alert.value) };
  let error = '';
  for (const key of RANGE_KEYS) {
    const read = end => {
      const raw = f[`${end}-${key}`].value.trim();
      if (raw === '') return null;
      const v = Number(raw);
      if (!Number.isFinite(v)) { error = `${METRICS[key].label}: enter a number or leave the box empty.`; return null; }
      return key === 'temperature' && units === 'F' ? +toC(v).toFixed(2) : v;
    };
    const min = read('min'), max = read('max');
    if (min !== null && max !== null && min >= max) error = `${METRICS[key].label}: the minimum must be below the maximum.`;
    next.ranges[key] = { min, max };
  }
  if (!Number.isFinite(next.h2Alert) || next.h2Alert < 100) error = 'Hydrogen alert: the MQ-8 can\'t measure below 100 ppm.';
  $('#settingsError').textContent = error;
  if (error) return;
  try {
    state.settings = mergeSettings(await request('/api/settings', { method: 'POST', body: JSON.stringify(next) }));
    fillSettingsForm();
    renderAll();
    toast(DEMO ? 'Saved (demo: in this browser only)' : 'Saved on the board');
  } catch (e) {
    $('#settingsError').textContent = 'Couldn\'t save: the board didn\'t respond. Check you\'re on its WiFi.';
  }
}

function renderBoard() {
  const s = state.status;
  if (!s) return;
  $('#navFoot').textContent = `${DEMO ? 'Demo · ' : ''}firmware ${s.fw}`;
  $('#boardStats').innerHTML = `
    <dt>Firmware</dt><dd>${esc(s.fw)}${DEMO ? ' (demo)' : ''}</dd>
    <dt>Running for</dt><dd>${fmtDuration(boardNowUptime())}</dd>
    <dt>Power-ups</dt><dd>${s.boot}</dd>
    <dt>Clock</dt><dd>${s.clock ? 'Set from this device' : 'Not set yet'}</dd>
    <dt>History in memory</dt><dd>${fmtDuration(s.history.samples * 60)} of 24 h</dd>
    <dt>Readings received</dt><dd>${s.link.received.toLocaleString()}</dd>`;
  const pct = s.storage.totalKB ? s.storage.usedKB / s.storage.totalKB * 100 : 0;
  $('#storageText').textContent = `Storage: ${s.storage.usedKB} KB of ${s.storage.totalKB} KB used (photos and settings)`;
  $('#storageBar').style.width = `${pct}%`;
}

// ---------------------------------------------------------------- navigation

function showView(view) {
  state.view = view;
  for (const v of ['overview', 'history', 'camera', 'settings']) $(`#view-${v}`).hidden = v !== view;
  $$('.nav-btn').forEach(b => {
    if (b.dataset.view === view) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  });
  if (view === 'history') renderHistory();
  if (history.replaceState) history.replaceState(null, '', `${location.search}#${view}`);
  window.scrollTo({ top: 0 });
}

function renderAll() {
  document.title = state.settings.name;
  $('#title').textContent = state.settings.name;
  renderStatus();
  renderPhotos();
  if (state.view === 'history') renderHistory();
}

function wireEvents() {
  $$('.nav-btn').forEach(b => b.addEventListener('click', () => showView(b.dataset.view)));
  $$('[data-goto]').forEach(b => b.addEventListener('click', () => showView(b.dataset.goto)));
  $('#metrics').addEventListener('click', ev => {
    const card = ev.target.closest('.metric');
    if (!card) return;
    state.shown = new Set([card.dataset.metric]);
    renderChips();
    showView('history');
  });
  $$('[data-snap-btn]').forEach(b => b.addEventListener('click', takePhoto));

  $('#rangePicker').addEventListener('click', ev => {
    const b = ev.target.closest('button');
    if (!b) return;
    state.range = Number(b.dataset.range);
    $$('#rangePicker button').forEach(x => x.setAttribute('aria-pressed', String(x === b)));
    renderHistory();
  });
  $('#metricChips').addEventListener('click', ev => {
    const b = ev.target.closest('button');
    if (!b) return;
    const key = b.dataset.metric;
    if (state.shown.has(key)) state.shown.delete(key); else state.shown.add(key);
    renderChips();
    renderHistory();
  });
  $('#exportCsv').addEventListener('click', exportCsv);

  const openFrom = () => openLightbox(state.photos.length - 1);
  $('#latestPhotoBtn').addEventListener('click', openFrom);
  $('#cameraPhotoBtn').addEventListener('click', openFrom);
  $('#gallery').addEventListener('click', ev => {
    const t = ev.target.closest('.thumb');
    if (t) openLightbox(Number(t.dataset.index));
  });
  $('#lightboxClose').addEventListener('click', () => $('#lightbox').close());
  $('#lightboxPrev').addEventListener('click', () => openLightbox(state.lightboxIndex - 1));
  $('#lightboxNext').addEventListener('click', () => openLightbox(state.lightboxIndex + 1));
  $('#lightbox').addEventListener('click', ev => { if (ev.target.id === 'lightbox') $('#lightbox').close(); });
  $('#lightbox').addEventListener('keydown', ev => {
    if (ev.key === 'ArrowLeft') openLightbox(state.lightboxIndex - 1);
    if (ev.key === 'ArrowRight') openLightbox(state.lightboxIndex + 1);
  });

  $('#settingsForm').addEventListener('submit', saveSettings);
  $('#unitPicker').addEventListener('click', ev => {
    const b = ev.target.closest('button');
    if (!b) return;
    // Keep what's typed, converted to the new unit.
    const wasF = $('#unitPicker [aria-pressed="true"]').dataset.unit === 'F';
    const f = $('#settingsForm');
    const conv = [f['min-temperature'], f['max-temperature']].map(i => i.value.trim() === '' ? null : Number(i.value));
    $$('#unitPicker button').forEach(x => x.setAttribute('aria-pressed', String(x === b)));
    const nowF = b.dataset.unit === 'F';
    if (wasF !== nowF) {
      ['min-temperature', 'max-temperature'].forEach((name, i) => {
        if (conv[i] === null || !Number.isFinite(conv[i])) return;
        f[name].value = +(nowF ? toF(conv[i]) : toC(conv[i])).toFixed(1);
      });
      $('label[for="min-temperature"] .unit-hint').textContent = nowF ? '°F' : '°C';
    }
  });
  $('#resetSettings').addEventListener('click', () => {
    state.settings = mergeSettings({ name: state.settings.name, units: state.settings.units });
    fillSettingsForm();
    $('#settingsError').textContent = '';
    toast('Defaults restored. Save to keep them.');
  });

  let resizeTimer;
  new ResizeObserver(() => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => { if (state.view === 'history') renderHistory(); }, 150);
  }).observe($('#main'));

  document.addEventListener('visibilitychange', () => { if (!document.hidden) { refreshStatus(); refreshHistory(); } });
}

function renderChips() {
  $('#metricChips').innerHTML = METRIC_KEYS.map(key =>
    `<button class="chip-toggle" data-metric="${key}" style="--metric: var(--c-${key})" aria-pressed="${state.shown.has(key)}">${METRICS[key].label}</button>`).join('');
}

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = resolve;
    s.onerror = reject;
    document.head.append(s);
  });
}

async function start() {
  if (DEMO) {
    await loadScript('demo.js');
    $('#demoBanner').hidden = false;
  }
  wireEvents();
  renderChips();
  const initial = location.hash.slice(1);
  showView(['overview', 'history', 'camera', 'settings'].includes(initial) ? initial : 'overview');
  await loadSettings();
  await refreshStatus();
  setInterval(() => { if (!document.hidden) refreshStatus(); }, STATUS_POLL_MS);
  setInterval(() => { if (!document.hidden) refreshHistory(); }, HISTORY_POLL_MS);
}

start();

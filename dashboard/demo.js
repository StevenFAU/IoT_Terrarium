// Demo mode for the dashboard: answers the same API as terrarium_receiver with simulated data, so the dashboard can
// be tried on any computer or on GitHub Pages without the hardware. Only loaded when app.js decides it's in demo mode;
// it is not packed into the board's firmware.
'use strict';

(() => {
  const BOOT = 7;
  const START = Date.now() - 26 * 3600e3;           // the pretend board was switched on 26 hours ago
  const REFILL = Date.now() - 23.5 * 3600e3;        // water was topped up then, so it's running low now
  const H2_SPIKE = Date.now() - 5 * 3600e3;         // one hydrogen reading 5 hours ago
  const PHOTO_EVERY = 5 * 60e3;
  const PHOTO_KEEP = 40;
  const SETTINGS_KEY = 'terrarium-demo-settings';

  const uptimeAt = ms => Math.floor((ms - START) / 1000);
  const msAt = uptime => START + uptime * 1000;

  // Deterministic noise, so a reading at a given minute is the same every time it's asked for.
  const noise = t => {
    const x = Math.sin(t * 12.9898 + 78.233) * 43758.5453;
    return (x - Math.floor(x)) * 2 - 1;
  };
  const smooth = (a, b, x) => { const k = Math.min(1, Math.max(0, (x - a) / (b - a))); return k * k * (3 - 2 * k); };
  const hourOf = ms => { const d = new Date(ms); return d.getHours() + d.getMinutes() / 60; };
  const lightsOn = h => smooth(6.5, 7.5, h) * (1 - smooth(19, 20, h));   // terrarium lamp 7:00-19:30

  function sample(ms) {
    const h = hourOf(ms), m = Math.floor(ms / 60e3);
    const on = lightsOn(h), warm = lightsOn(h - 1.2);
    const mist = [8, 17].reduce((acc, at) => acc + (h >= at ? 15 * Math.exp(-(h - at) / 0.6) : 0), 0);
    const hrsSinceRefill = (ms - REFILL) / 3600e3;
    const spike = Math.abs(ms - H2_SPIKE) < 8 * 60e3 ? Math.round(150 * (1 - Math.abs(ms - H2_SPIKE) / (8 * 60e3)) + 80) : 0;
    return {
      humidity: +(71 - 11 * on + mist + noise(m) * 1.2).toFixed(1),
      temperature: +(21.6 + 4.4 * warm + noise(m + 1) * 0.15).toFixed(1),
      light: Math.round(Math.max(0, 3 + 79 * on + noise(m + 2) * 3)),
      water: Math.round(Math.max(4, 94 - 3.25 * Math.max(0, hrsSinceRefill) + noise(m + 3))),
      h2: spike >= 100 ? spike : 0,
      rssi: Math.round(-47 + noise(m + 4) * 3),
    };
  }

  let settings = {};
  try { settings = JSON.parse(localStorage.getItem(SETTINGS_KEY) || '{}'); } catch (e) { settings = {}; }

  // ---- photos ----
  const extraPhotos = [];        // times of photos taken with "Take photo now"
  let snap = null;               // {requested, start, total}
  const firstPhotoMs = START + 30e3;

  // Every 5 minutes since power-up, plus any taken on request, numbered in order like the board does.
  function photoList() {
    const count = Math.floor((Date.now() - firstPhotoMs) / PHOTO_EVERY) + 1;
    const times = [];
    for (let i = 0; i < count; i++) times.push(firstPhotoMs + i * PHOTO_EVERY);
    times.push(...extraPhotos);
    times.sort((a, b) => a - b);
    return times.map((ms, i) => ({ n: i + 1, ms })).slice(-PHOTO_KEEP);
  }

  const imageCache = new Map();
  function drawPhoto(n, ms) {
    const c = document.createElement('canvas');
    c.width = 320;
    c.height = 240;
    const g = c.getContext('2d');
    const on = lightsOn(hourOf(ms));
    const sky = g.createLinearGradient(0, 0, 0, 240);
    sky.addColorStop(0, `rgb(${40 + 150 * on},${55 + 165 * on},${70 + 140 * on})`);
    sky.addColorStop(1, `rgb(${20 + 90 * on},${30 + 110 * on},${30 + 80 * on})`);
    g.fillStyle = sky;
    g.fillRect(0, 0, 320, 240);
    // glass box and soil
    g.fillStyle = `rgba(90,60,35,${0.6 + 0.4 * on})`;
    g.fillRect(30, 170, 260, 50);
    g.strokeStyle = `rgba(255,255,255,${0.25 + 0.35 * on})`;
    g.lineWidth = 3;
    g.strokeRect(30, 40, 260, 180);
    // plants, slightly different in every photo
    let seed = n * 9301 + 49297;
    const rnd = () => { seed = (seed * 9301 + 49297) % 233280; return seed / 233280; };
    for (let i = 0; i < 7; i++) {
      const x = 55 + i * 35 + rnd() * 10, hgt = 50 + rnd() * 70;
      g.fillStyle = `rgb(${30 + 40 * on},${90 + 110 * on + rnd() * 30},${40 + 40 * on})`;
      g.beginPath();
      g.ellipse(x, 170 - hgt / 2, 10 + rnd() * 8, hgt / 2, (rnd() - 0.5) * 0.6, 0, Math.PI * 2);
      g.fill();
    }
    // water dish
    g.fillStyle = `rgba(80,150,200,${0.5 + 0.4 * on})`;
    g.beginPath();
    g.ellipse(230, 185, 30, 9, 0, 0, Math.PI * 2);
    g.fill();
    g.fillStyle = 'rgba(255,255,255,0.85)';
    g.font = '600 12px system-ui, sans-serif';
    g.fillText(`Demo photo #${n}`, 10, 18);
    g.fillText(new Date(ms).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }), 250, 18);
    return c.toDataURL('image/jpeg', 0.8);
  }

  function photoUrl(n) {
    if (!imageCache.has(n)) {
      const p = photoList().find(x => x.n === n);
      imageCache.set(n, drawPhoto(n, p ? p.ms : Date.now()));
    }
    return imageCache.get(n);
  }

  function snapProgress() {
    if (!snap) return null;
    const now = Date.now();
    if (now - snap.requested < 2500) return { pending: true, got: 0, total: 0 };
    const got = Math.min(snap.total, Math.floor((now - snap.requested - 2500) / 2000));
    if (got >= snap.total) {
      extraPhotos.push(now);
      snap = null;
      return null;
    }
    return { pending: false, got, total: snap.total };
  }

  // ---- API ----

  function status() {
    const now = Date.now();
    const uptime = uptimeAt(now);
    const count = Math.floor(uptime / 10);
    const s = sample(now);
    const prog = snapProgress();
    const photos = photoList();
    const hist = Math.min(1440, Math.floor(uptime / 60));
    return {
      fw: '2.0', name: 'Terrarium', boot: BOOT, uptime, clock: Math.floor(now / 1000),
      link: { hasData: true, count, age: Math.floor(uptime % 10), lost: false, rssi: s.rssi, snr: +(9.5 + noise(count) * 0.8).toFixed(1),
        received: count - 3, missed: 3, intervalS: 10 },
      reading: { humidity: s.humidity, temperature: s.temperature, light: s.light, water: s.water,
        h2: s.h2 ? { state: 'ok', ppm: s.h2 } : { state: 'low', ppm: null } },
      photo: { inProgress: !!(prog && !prog.pending), got: prog ? prog.got : 0, total: prog ? prog.total : 0,
        pending: !!(prog && prog.pending), latest: photos.length ? photos[photos.length - 1].n : 0, count: photos.length },
      storage: { usedKB: 96 + photos.length * 3, totalKB: 1536 },
      history: { samples: hist, max: 1440 },
    };
  }

  function history(after) {
    const now = Date.now();
    const uptime = uptimeAt(now);
    const rows = [];
    const firstMinute = Math.max(1, Math.floor(uptime / 60) - 1439);
    for (let m = firstMinute; m <= Math.floor(uptime / 60); m++) {
      const t = m * 60;
      if (t <= after) continue;
      // a short gap, as if the sender had been unplugged for a few minutes
      const ms = msAt(t);
      if (Math.abs(ms - (now - 9 * 3600e3)) < 4 * 60e3) continue;
      const s = sample(ms);
      rows.push([t, s.humidity, s.temperature, s.light, s.water, s.h2, s.rssi]);
    }
    return { uptime, boot: BOOT, periodS: 60, fields: ['t', 'humidity', 'temperature', 'light', 'water', 'h2', 'rssi'], rows };
  }

  function photos() {
    return {
      boot: BOOT, uptime: uptimeAt(Date.now()),
      photos: photoList().map(p => ({ n: p.n, epoch: Math.floor(p.ms / 1000), boot: BOOT, uptime: uptimeAt(p.ms),
        bytes: 2400 + Math.round((noise(p.n) + 1) * 300) })),
    };
  }

  function delay(v) { return new Promise(resolve => setTimeout(() => resolve(v), 80 + Math.random() * 120)); }

  async function request(path, method, body) {
    const [route, query] = path.split('?');
    if (route === '/api/status') return delay(status());
    if (route === '/api/history') return delay(history(Number(new URLSearchParams(query).get('after')) || 0));
    if (route === '/api/photos') return delay(photos());
    if (route === '/api/time') return delay({ clock: Math.floor(Date.now() / 1000) });
    if (route === '/api/settings' && method === 'GET') return delay(settings);
    if (route === '/api/settings' && method === 'POST') {
      settings = JSON.parse(body);
      try { localStorage.setItem(SETTINGS_KEY, body); } catch (e) { /* private mode: keep it for this visit */ }
      return delay(settings);
    }
    if (route === '/api/snap') {
      if (snap) { const err = new Error('A photo is already on its way'); err.status = 409; throw err; }
      snap = { requested: Date.now(), total: 14 };
      return delay('Requested');
    }
    const err = new Error('Not found');
    err.status = 404;
    throw err;
  }

  window.TerrariumDemo = { request, photoUrl };
})();

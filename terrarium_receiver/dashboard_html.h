// Phone dashboard served by terrarium_receiver at http://192.168.4.1
// Polls /api/status every 2 s; photos come from /photo.jpg?n=<number>.
#pragma once

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Terrarium</title>
<style>
  :root { --bg:#f4f7f6; --card:#fff; --ink:#1f2933; --muted:#5f6b76; --accent:#0b6e4f; --warn:#c0392b; --line:#dde5e2; }
  @media (prefers-color-scheme: dark) {
    :root { --bg:#111816; --card:#1b2421; --ink:#e7eeeb; --muted:#9aa9a3; --accent:#3ccf91; --warn:#ff6b5b; --line:#2a3632; }
  }
  * { box-sizing: border-box; }
  body { margin:0; font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif; background:var(--bg); color:var(--ink); }
  header { padding:16px 16px 8px; display:flex; align-items:baseline; justify-content:space-between; gap:8px; }
  h1 { font-size:22px; margin:0; }
  #link { font-size:13px; color:var(--muted); text-align:right; }
  #link.lost { color:var(--warn); font-weight:600; }
  main { padding:0 16px 24px; max-width:720px; margin:0 auto; }
  .grid { display:grid; grid-template-columns:repeat(2, 1fr); gap:10px; }
  .card { background:var(--card); border:1px solid var(--line); border-radius:14px; padding:12px 14px; }
  .label { font-size:12px; color:var(--muted); text-transform:uppercase; letter-spacing:.04em; }
  .value { font-size:28px; font-weight:650; margin-top:2px; }
  .unit { font-size:15px; font-weight:500; color:var(--muted); margin-left:2px; }
  .wide { grid-column:1 / -1; }
  h2 { font-size:15px; margin:20px 0 8px; }
  #photo { width:100%; aspect-ratio:4/3; object-fit:cover; border-radius:14px; background:var(--line); display:block; }
  #photoInfo { font-size:13px; color:var(--muted); margin-top:6px; }
  button { width:100%; margin-top:10px; padding:14px; font-size:16px; font-weight:600; border:0; border-radius:12px;
           background:var(--accent); color:#fff; }
  button:disabled { opacity:.55; }
  #gallery { display:flex; gap:8px; overflow-x:auto; padding-bottom:6px; }
  #gallery img { width:96px; height:72px; object-fit:cover; border-radius:8px; flex:0 0 auto; border:2px solid transparent; }
  #gallery img.sel { border-color:var(--accent); }
  .stats { font-size:13px; color:var(--muted); margin-top:16px; line-height:1.6; }
</style>
</head>
<body>
<header><h1>Terrarium</h1><div id="link">Connecting…</div></header>
<main>
  <div class="grid">
    <div class="card"><div class="label">Humidity</div><div class="value"><span id="hum">–</span><span class="unit">%</span></div></div>
    <div class="card"><div class="label">Temperature</div><div class="value"><span id="temp">–</span><span class="unit">°C</span></div></div>
    <div class="card"><div class="label">Light</div><div class="value"><span id="light">–</span><span class="unit">%</span></div></div>
    <div class="card"><div class="label">Water / soil</div><div class="value"><span id="soil">–</span><span class="unit">%</span></div></div>
    <div class="card wide"><div class="label">Hydrogen (MQ-8)</div><div class="value" id="h2" style="font-size:22px">–</div></div>
  </div>

  <h2>Camera</h2>
  <img id="photo" alt="Latest terrarium photo">
  <div id="photoInfo">No photos yet</div>
  <button id="snap">Take photo now</button>

  <h2>Saved photos</h2>
  <div id="gallery"></div>

  <div class="stats" id="stats"></div>
</main>
<script>
const $ = id => document.getElementById(id);
let shownPhoto = 0, latestPhoto = 0, pinned = false;

function show(n, isLatest) {
  if (!n || n === shownPhoto) return;
  shownPhoto = n;
  $('photo').src = '/photo.jpg?n=' + n;
  $('photoInfo').textContent = 'Photo #' + n + (isLatest ? ' (latest)' : '');
  document.querySelectorAll('#gallery img').forEach(i => i.classList.toggle('sel', +i.dataset.n === n));
}

function renderGallery(photos) {
  const g = $('gallery');
  if (g.childElementCount === photos.length && +(g.firstChild && g.firstChild.dataset.n) === photos[photos.length - 1]) return;
  g.innerHTML = '';
  photos.slice().reverse().forEach(n => {
    const img = document.createElement('img');
    img.src = '/photo.jpg?n=' + n; img.dataset.n = n; img.loading = 'lazy';
    img.onclick = () => { pinned = n !== latestPhoto; show(n, n === latestPhoto); };
    g.appendChild(img);
  });
}

async function refresh() {
  try {
    const s = await (await fetch('/api/status', {cache: 'no-store'})).json();
    const link = $('link');
    if (!s.hasData) { link.textContent = 'Waiting for sender…'; link.className = ''; }
    else {
      link.textContent = (s.linkLost ? 'Link lost · ' : '') + 'updated ' + s.ageS + ' s ago';
      link.className = s.linkLost ? 'lost' : '';
      $('hum').textContent = s.humidity ? Math.round(+s.humidity) : '–';
      $('temp').textContent = s.temperature || '–';
      $('light').textContent = s.light || '–';
      $('soil').textContent = s.soil || '–';
      $('h2').textContent = s.h2 || '–';
    }
    latestPhoto = s.photos.length ? s.photos[s.photos.length - 1] : 0;
    renderGallery(s.photos);
    if (!pinned) show(latestPhoto, true);

    const btn = $('snap');
    btn.disabled = s.photoInProgress || s.snapPending;
    btn.textContent = s.photoInProgress ? (s.photoStatus || 'Receiving photo…')
                    : s.snapPending ? 'Waiting for the camera…' : 'Take photo now';
    $('stats').textContent = s.hasData
      ? 'Packet #' + s.count + ' · signal ' + s.rssi + ' dBm, SNR ' + s.snr + ' dB · received ' + s.received +
        ', missed ' + s.missed + ' · ' + s.photos.length + ' photos saved'
      : '';
  } catch (e) {
    $('link').textContent = 'Dashboard not reachable'; $('link').className = 'lost';
  }
}

$('snap').onclick = async () => {
  $('snap').disabled = true;
  pinned = false;
  try { await fetch('/api/snap', {method: 'POST'}); } catch (e) {}
  refresh();
};

refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)rawliteral";

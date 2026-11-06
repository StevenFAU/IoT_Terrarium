// Close-up dashboard served by terrarium_cam at http://192.168.4.1 (live video comes from port 81).
#pragma once

const char CAM_DASHBOARD_HTML[] = R"rawliteral(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Terrarium Cam</title>
<style>
  :root { --bg:#f4f7f6; --card:#fff; --ink:#1f2933; --muted:#5f6b76; --accent:#0b6e4f; --line:#dde5e2; }
  @media (prefers-color-scheme: dark) {
    :root { --bg:#111816; --card:#1b2421; --ink:#e7eeeb; --muted:#9aa9a3; --accent:#3ccf91; --line:#2a3632; }
  }
  * { box-sizing: border-box; }
  body { margin:0; font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif; background:var(--bg); color:var(--ink); }
  header { padding:16px 16px 8px; display:flex; align-items:baseline; justify-content:space-between; }
  h1 { font-size:22px; margin:0; }
  #age { font-size:13px; color:var(--muted); }
  main { padding:0 16px 24px; max-width:720px; margin:0 auto; }
  #live { width:100%; aspect-ratio:4/3; object-fit:cover; border-radius:14px; background:#000; display:block; }
  .row { display:flex; gap:10px; margin-top:10px; }
  button, a.btn { flex:1; padding:14px; font-size:16px; font-weight:600; border:0; border-radius:12px; text-align:center;
                  background:var(--accent); color:#fff; text-decoration:none; }
  button.secondary { background:var(--card); color:var(--ink); border:1px solid var(--line); }
  #shot { width:100%; border-radius:14px; margin-top:10px; display:none; }
  .grid { display:grid; grid-template-columns:repeat(2, 1fr); gap:10px; margin-top:16px; }
  .card { background:var(--card); border:1px solid var(--line); border-radius:14px; padding:12px 14px; }
  .label { font-size:12px; color:var(--muted); text-transform:uppercase; letter-spacing:.04em; }
  .value { font-size:26px; font-weight:650; margin-top:2px; }
  .unit { font-size:15px; font-weight:500; color:var(--muted); margin-left:2px; }
  .wide { grid-column:1 / -1; }
</style>
</head>
<body>
<header><h1>Terrarium Cam</h1><div id="age">Live</div></header>
<main>
  <img id="live" alt="Live video">
  <div class="row">
    <button id="toggle" class="secondary">Pause video</button>
    <button id="capture">Full-size photo</button>
  </div>
  <img id="shot" alt="Full-size photo">
  <div class="row" id="saveRow" style="display:none"><a class="btn" id="save" download="terrarium.jpg">Save photo</a></div>

  <div class="grid">
    <div class="card"><div class="label">Humidity</div><div class="value"><span id="hum">–</span><span class="unit">%</span></div></div>
    <div class="card"><div class="label">Temperature</div><div class="value"><span id="temp">–</span><span class="unit">°C</span></div></div>
    <div class="card"><div class="label">Light</div><div class="value"><span id="light">–</span><span class="unit">%</span></div></div>
    <div class="card"><div class="label">Water / soil</div><div class="value"><span id="soil">–</span><span class="unit">%</span></div></div>
    <div class="card wide"><div class="label">Hydrogen (MQ-8)</div><div class="value" id="h2" style="font-size:22px">–</div></div>
  </div>
</main>
<script>
const $ = id => document.getElementById(id);
const streamUrl = location.protocol + '//' + location.hostname + ':81/stream';
let playing = true;

function play(on) {
  playing = on;
  $('live').src = on ? streamUrl : '';
  $('toggle').textContent = on ? 'Pause video' : 'Resume video';
}
$('toggle').onclick = () => play(!playing);

$('capture').onclick = async () => {
  $('capture').disabled = true; $('capture').textContent = 'Taking photo…';
  try {
    const blob = await (await fetch('/capture.jpg', {cache: 'no-store'})).blob();
    const url = URL.createObjectURL(blob);
    $('shot').src = url; $('shot').style.display = 'block';
    $('save').href = url; $('saveRow').style.display = 'flex';
  } catch (e) { alert('Photo failed, try again'); }
  $('capture').disabled = false; $('capture').textContent = 'Full-size photo';
};

const h2Text = v => v === 'W' ? 'warming up' : v === 'E' ? 'no signal' : v === 'L' ? '<100 ppm' : v ? v + ' ppm' : '–';

async function refresh() {
  try {
    const s = await (await fetch('/api/status', {cache: 'no-store'})).json();
    if (!s.hasData) { $('age').textContent = 'No readings yet'; return; }
    $('age').textContent = 'Readings ' + s.ageS + ' s ago';
    $('hum').textContent = s.humidity ? Math.round(+s.humidity) : '–';
    $('temp').textContent = s.temperature || '–';
    $('light').textContent = s.light || '–';
    $('soil').textContent = s.soil || '–';
    $('h2').textContent = h2Text(s.h2);
  } catch (e) { $('age').textContent = 'Not reachable'; }
}

play(true);
refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)rawliteral";

/**
 * @file    web_page.h
 * @brief   Page web servie par le XIAO ESP32S3 (point d'acces Wi-Fi)
 *
 * @details Page autonome (HTML + CSS + JS, aucune ressource externe : le
 *          telephone n'a pas Internet quand il est connecte au XIAO).
 *          Elle interroge /status chaque seconde et envoie un ping via
 *          POST /ping. Les sons et vibrations sont generes par le telephone.
 */
#pragma once

static const char WEB_PAGE_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MeshCaching</title>
<style>
  :root { --bg:#111418; --card:#1c2128; --txt:#e8ecf1; --dim:#8b95a1; --lvl:#3a4350; }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--txt);
         font-family:system-ui,-apple-system,Roboto,sans-serif; }
  main { max-width:480px; margin:0 auto; padding:12px 16px 24px; }
  header { display:flex; justify-content:space-between; align-items:baseline; }
  h1 { font-size:1.1rem; margin:0; }
  #conn { font-size:.8rem; color:var(--dim); }
  #conn.off { color:#ff6b6b; }
  #big { margin-top:12px; border-radius:16px; padding:18px 12px; text-align:center;
         background:var(--lvl); transition:background .4s; }
  #rssi { font-size:5.5rem; font-weight:800; line-height:1; font-variant-numeric:tabular-nums; }
  #rssi small { font-size:1.4rem; font-weight:600; }
  #lvl { font-size:1.5rem; font-weight:700; margin-top:4px; }
  #trend { font-size:1rem; margin-top:4px; min-height:1.2em; }
  .grid { display:grid; grid-template-columns:1fr 1fr 1fr; gap:8px; margin-top:12px; }
  .cell { background:var(--card); border-radius:12px; padding:10px; text-align:center; }
  .cell b { display:block; font-size:1.25rem; font-variant-numeric:tabular-nums; }
  .cell span { font-size:.75rem; color:var(--dim); }
  #age.stale { color:#ff9f43; }
  canvas { width:100%; height:170px; margin-top:12px; background:var(--card); border-radius:12px; }
  button { width:100%; margin-top:12px; padding:16px; font-size:1.2rem; font-weight:700;
           border:0; border-radius:12px; background:#2e86de; color:#fff; }
  button:disabled { background:#3a4350; color:var(--dim); }
  #pingMsg { text-align:center; color:var(--dim); font-size:.9rem; min-height:1.3em; margin-top:6px; }
  .opts { display:flex; gap:20px; justify-content:center; margin-top:10px; color:var(--dim); }
  .opts label { display:flex; align-items:center; gap:6px; }
  footer { margin-top:14px; font-size:.75rem; color:var(--dim); text-align:center; }
</style>
</head>
<body>
<main>
  <header><h1>Répéteur <span id="target">----</span></h1><span id="conn">connexion…</span></header>

  <section id="big">
    <div id="rssi">--<small> dBm</small></div>
    <div id="lvl">En attente de paquets…</div>
    <div id="trend"></div>
  </section>

  <div class="grid">
    <div class="cell"><b id="snr">--</b><span>SNR (dB)</span></div>
    <div class="cell"><b id="age">--</b><span>dernier paquet</span></div>
    <div class="cell"><b id="count">0</b><span>détections</span></div>
  </div>

  <canvas id="chart"></canvas>

  <button id="ping">Ping le répéteur</button>
  <div id="pingMsg">Touchez la page une fois pour activer vibration et bip.</div>

  <div class="opts">
    <label><input type="checkbox" id="vib" checked> Vibration</label>
    <label><input type="checkbox" id="beep"> Bip</label>
  </div>

  <footer><span id="why"></span> · <span id="rx">0</span> paquets entendus au total</footer>
</main>

<script>
const $ = id => document.getElementById(id);
const HIST_MS = 10 * 60 * 1000;   // le graphique montre les 10 dernieres minutes
let lastSeq = -1, prevRssi = null, hist = [], audio = null;

// Niveau "chaud / froid" en fonction du RSSI
function level(r) {
  if (r > -70)  return ['Brûlant', '#c0392b'];
  if (r > -85)  return ['Chaud',   '#e67e22'];
  if (r > -100) return ['Tiède',   '#b7950b'];
  if (r > -112) return ['Froid',   '#2471a3'];
  return ['Glacial', '#6c3483'];
}

function fmtAge(s) {
  if (s < 60) return s + ' s';
  if (s < 3600) return Math.floor(s / 60) + ' min ' + String(s % 60).padStart(2, '0');
  return Math.floor(s / 3600) + ' h ' + String(Math.floor(s / 60) % 60).padStart(2, '0');
}

// Bip dont la hauteur monte avec le RSSI (-120 dBm grave -> -50 dBm aigu)
function beep(r) {
  if (!audio) return;
  const o = audio.createOscillator(), g = audio.createGain();
  const k = Math.min(1, Math.max(0, (r + 120) / 70));
  o.frequency.value = 300 + k * 1200;
  g.gain.setValueAtTime(0.25, audio.currentTime);
  g.gain.exponentialRampToValueAtTime(0.001, audio.currentTime + 0.25);
  o.connect(g); g.connect(audio.destination);
  o.start(); o.stop(audio.currentTime + 0.25);
}

// L'audio d'un navigateur ne peut demarrer qu'apres un geste de l'utilisateur
$('beep').addEventListener('change', e => {
  if (e.target.checked) {
    audio = audio || new (window.AudioContext || window.webkitAudioContext)();
    audio.resume();
    beep(-85);
  }
});

function drawChart() {
  const c = $('chart'), dpr = window.devicePixelRatio || 1;
  c.width = c.clientWidth * dpr; c.height = c.clientHeight * dpr;
  const x = c.getContext('2d'), W = c.width, H = c.height, now = Date.now();
  const yOf = r => H - (Math.min(-40, Math.max(-130, r)) + 130) / 90 * H;
  x.font = (10 * dpr) + 'px sans-serif';
  x.strokeStyle = '#2c333d'; x.fillStyle = '#8b95a1'; x.lineWidth = dpr;
  for (const r of [-120, -100, -80, -60]) {
    x.beginPath(); x.moveTo(0, yOf(r)); x.lineTo(W, yOf(r)); x.stroke();
    x.fillText(r, 4 * dpr, yOf(r) - 3 * dpr);
  }
  hist = hist.filter(p => now - p.t < HIST_MS);
  if (!hist.length) return;
  const xOf = t => W - (now - t) / HIST_MS * W;
  x.strokeStyle = '#8b95a1'; x.beginPath();
  hist.forEach((p, i) => i ? x.lineTo(xOf(p.t), yOf(p.r)) : x.moveTo(xOf(p.t), yOf(p.r)));
  x.stroke();
  for (const p of hist) {
    x.fillStyle = level(p.r)[1];
    x.beginPath(); x.arc(xOf(p.t), yOf(p.r), 4 * dpr, 0, 7); x.fill();
  }
}

async function poll() {
  try {
    const ctl = new AbortController();
    const to = setTimeout(() => ctl.abort(), 2500);
    const s = await (await fetch('/status', { signal: ctl.signal, cache: 'no-store' })).json();
    clearTimeout(to);

    $('conn').textContent = 'connecté'; $('conn').className = '';
    $('target').textContent = s.target;
    $('rx').textContent = s.rx;
    $('count').textContent = s.seq;

    if (s.has) {
      const [name, color] = level(s.rssi);
      $('rssi').innerHTML = Math.round(s.rssi) + '<small> dBm</small>';
      $('lvl').textContent = name;
      $('big').style.background = color;
      $('snr').textContent = s.snr.toFixed(1);
      $('age').textContent = fmtAge(s.age);
      $('age').className = s.age > 120 ? 'stale' : '';
      $('why').textContent = 'dernière détection : ' + s.why;

      if (s.seq !== lastSeq) {
        // Nouveau paquet du repeteur (on ignore le tout premier affichage)
        if (lastSeq >= 0) {
          if (prevRssi !== null) {
            const d = Math.round(s.rssi - prevRssi);
            $('trend').textContent = d > 0 ? '▲ +' + d + ' dB, plus chaud' : d < 0 ? '▼ ' + d + ' dB, plus froid' : '= stable';
          }
          if ($('vib').checked && navigator.vibrate) navigator.vibrate(s.rssi > -85 ? [80, 60, 80, 60, 80] : [200]);
          if ($('beep').checked) beep(s.rssi);
        }
        hist.push({ t: Date.now() - s.age * 1000, r: s.rssi });
        prevRssi = s.rssi;
        lastSeq = s.seq;
      }
    }
    $('ping').disabled = s.pingWait > 0;
    $('ping').textContent = s.pingWait > 0 ? 'Ping possible dans ' + s.pingWait + ' s' : 'Ping le répéteur';
  } catch (e) {
    $('conn').textContent = 'connexion perdue'; $('conn').className = 'off';
  }
  drawChart();
}

$('ping').addEventListener('click', async () => {
  $('ping').disabled = true;
  try {
    const r = await (await fetch('/ping', { method: 'POST' })).json();
    $('pingMsg').textContent = r.ok ? 'Ping envoyé, réponse attendue sous 10 s…' : 'Trop tôt : réessayez dans ' + r.wait + ' s';
  } catch (e) {
    $('pingMsg').textContent = 'Échec de l’envoi';
  }
});

poll();
setInterval(poll, 1000);
window.addEventListener('resize', drawChart);
</script>
</body>
</html>
)rawliteral";

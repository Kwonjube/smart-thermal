// KioskPage.h - the party kiosk: a clean screen for guests to type a
// message, pick a style and decoration, add a photo from the camera or a
// file, and print. Set up from Settings > Kiosk; started per screen with
// "Start kiosk on this screen" (remembered in that browser only). The
// padlock button returns to the normal page - deliberately no PIN.
//
// Everything is drawn by the browser and printed as images, the same way
// as the Text card's styles. The camera needs a secure page or a one-off
// browser setting on the kiosk computer (see the handover).

#pragma once
#include <Arduino.h>

static const char KIOSK_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset=utf-8>
<script>
// Opened through the website, every "/..." request goes under /p/<code>/.
window.BASE = (location.pathname.match(/^\/p\/[a-z0-9]{6}(?=\/)/) || [''])[0];
if (window.BASE) { const f = window.fetch.bind(window);
  window.fetch = (u, o) => f(typeof u === 'string' && u[0] === '/' && u[1] !== '/' ? window.BASE + u : u, o); }
</script>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Print a message</title>
<style>
:root{--ink:#222;--soft:#6b6b6b;--line:#ddd;--accent:#1b7f5a;--bad:#b3261e}
*{box-sizing:border-box}
body{margin:0;font:18px -apple-system,system-ui,Helvetica,Arial,sans-serif;color:var(--ink);background:#faf8f5}
main{max-width:1180px;margin:0 auto;padding:28px 28px 70px;display:grid;grid-template-columns:1fr 440px;gap:28px}
@media (max-width:900px){main{grid-template-columns:1fr}}
h1{font-size:40px;margin:0 0 6px;grid-column:1/-1;text-align:center}
#sub{grid-column:1/-1;text-align:center;color:var(--soft);white-space:pre-line;margin:0 0 6px;font-size:19px}
.card{background:#fff;border:1px solid var(--line);border-radius:14px;padding:20px}
textarea{width:100%;min-height:150px;font:22px -apple-system,system-ui,sans-serif;padding:14px;border:1px solid #ccc;border-radius:10px;resize:vertical}
label{display:block;font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--soft);margin:14px 0 6px}
select{width:100%;font-size:18px;padding:10px;border:1px solid #ccc;border-radius:10px;background:#fff}
.row{display:flex;gap:12px;flex-wrap:wrap}.row>*{flex:1 1 140px}
button{font-size:19px;padding:14px 18px;border-radius:12px;border:0;background:var(--accent);color:#fff;cursor:pointer}
button.ghost{background:#fff;color:var(--ink);border:1px solid #ccc}
button:disabled{opacity:.45;cursor:default}
#print{width:100%;font-size:24px;padding:18px;margin-top:18px}
.tick{display:flex;align-items:center;gap:8px;text-transform:none;letter-spacing:0;font-size:17px;color:var(--ink);margin-top:12px}
.tick input{width:22px;height:22px}
#pv{background:#fff;border:1px solid var(--line);border-radius:14px;padding:16px;text-align:center;align-self:start;position:sticky;top:16px}
#pv canvas{width:100%;image-rendering:pixelated;display:block;margin:0 auto}
#pvphoto{margin-top:8px}
#pvnote{font-size:14px;color:var(--soft);margin-top:8px}
#status{min-height:28px;margin-top:12px;font-size:18px;text-align:center}
#alert{display:none;grid-column:1/-1;background:#fbe5e2;color:#5a1a12;border-radius:12px;padding:14px 18px;font-size:18px}
#alert.info{background:#eef2f7;color:#223}
footer{position:fixed;left:0;right:0;bottom:0;display:flex;justify-content:space-between;align-items:center;padding:10px 18px;font-size:13px;color:var(--soft);background:#faf8f5cc}
#exit{font-size:13px;padding:6px 10px;background:none;color:var(--soft);border:1px solid var(--line)}
#cam{display:none;position:fixed;inset:0;background:#000d;z-index:9;flex-direction:column;align-items:center;justify-content:center;gap:18px}
#cam video{max-width:90vw;max-height:70vh;border-radius:12px;transform:scaleX(-1);background:#000}
#count{position:absolute;font-size:160px;color:#fff;font-weight:700;text-shadow:0 0 30px #000}
#photoRow{display:none;align-items:center;gap:10px;margin-top:10px;font-size:16px}
</style></head><body>
<main>
  <h1 id="title">Leave a message!</h1>
  <p id="sub"></p>
  <div id="alert"></div>
  <section class="card">
    <textarea id="msg" placeholder="Type your message here&hellip;" maxlength="400"></textarea>
    <div class="row">
      <div><label for="theme">Style</label><select id="theme"></select></div>
      <div><label for="deco">Decoration</label><select id="deco"></select></div>
      <div style="flex:0 1 110px"><label for="size">Size</label>
        <select id="size"><option value="xs">XS</option><option value="s">S</option>
        <option value="m" selected>M</option><option value="l">L</option><option value="xl">XL</option></select></div>
    </div>
    <div class="row" style="margin-top:18px">
      <button class="ghost" id="snap">&#128247; Take photo</button>
      <button class="ghost" id="upload">Upload image</button>
      <input type="file" id="file" accept="image/*" style="display:none">
    </div>
    <div class="row" id="photoRow">
      <span>Photo added &mdash; it prints after your message.</span>
      <button class="ghost" id="nophoto" style="flex:0 0 auto;padding:8px 12px;font-size:15px">Remove photo</button>
    </div>
    <label class="tick"><input type="checkbox" id="cutBetween"> Cut between message and photo</label>
    <button id="print" disabled>Print</button>
    <div id="status"></div>
  </section>
  <aside id="pv">
    <canvas id="pvslip"></canvas>
    <canvas id="pvphoto"></canvas>
    <div id="pvnote">This is exactly what prints.</div>
  </aside>
</main>
<footer><span id="foot">Available to hire</span>
  <button id="exit" title="Back to the normal page">&#128274; Exit kiosk</button></footer>
<div id="cam"><video id="video" autoplay playsinline muted></video><div id="count"></div>
  <div class="row" style="width:auto">
    <button id="shoot">&#128247; Take photo</button><button class="ghost" id="camx">Cancel</button></div></div>
<script>
(() => {
  const el = id => document.getElementById(id);
  // ---- The slip: theme, decoration, fixed title, message ----------------
  const SANS = 'Helvetica, Arial, sans-serif', SERIF = 'Georgia, "Times New Roman", serif',
        MONO = '"Courier New", Courier, monospace';
  const THEMES = {
    love:    { name: 'Love note',  body: s => 'italic ' + s + 'px ' + SERIF, head: s => 'italic bold ' + s + 'px ' + SERIF },
    classic: { name: 'Classic',    body: s => s + 'px ' + SANS,              head: s => 'bold ' + s + 'px ' + SANS },
    bold:    { name: 'Big & bold', body: s => 'bold ' + s + 'px ' + SANS,    head: s => 'bold ' + s + 'px ' + SANS },
    type:    { name: 'Typewriter', body: s => s + 'px ' + MONO,              head: s => 'bold ' + s + 'px ' + MONO },
  };
  const DECOS = {
    none: 'None', hearts: 'Hearts', stars: 'Stars', sparkles: 'Sparkles', dots: 'Dots',
    flowers: 'Flowers', balloons: 'Balloons', confetti: 'Confetti', waves: 'Waves',
    zigzag: 'Zigzag', double: 'Double line', frame: 'Double frame', dotframe: 'Dotted frame',
  };
  const SIZES = { xs: 22, s: 26, m: 32, l: 42, xl: 56 };

  function wrap(cx, text, maxW){
    const out = []; let line = '';
    for (const word of text.split(/\s+/).filter(Boolean)) {
      const t = line ? line + ' ' + word : word;
      if (cx.measureText(t).width <= maxW) { line = t; continue; }
      if (line) out.push(line);
      if (cx.measureText(word).width <= maxW) { line = word; continue; }
      let part = '';
      for (const ch of word) {
        if (cx.measureText(part + ch).width > maxW) { out.push(part); part = ch; } else part += ch;
      }
      line = part;
    }
    if (line) out.push(line);
    return out.length ? out : [''];
  }

  // Shapes, all filled black: crisp on thermal paper, unlike emoji.
  const shape = {
    heart(cx, x, y, s){
      cx.beginPath(); cx.moveTo(x, y + s * 0.35);
      cx.bezierCurveTo(x - s * 0.9, y - s * 0.25, x - s * 0.35, y - s * 0.85, x, y - s * 0.35);
      cx.bezierCurveTo(x + s * 0.35, y - s * 0.85, x + s * 0.9, y - s * 0.25, x, y + s * 0.35);
      cx.fill();
    },
    star(cx, x, y, s, points, inner){
      cx.beginPath();
      for (let i = 0; i < points * 2; i++) {
        const r = i % 2 ? s * inner : s, a = -Math.PI / 2 + i * Math.PI / points;
        cx.lineTo(x + r * Math.cos(a), y + r * Math.sin(a));
      }
      cx.closePath(); cx.fill();
    },
    dot(cx, x, y, s){ cx.beginPath(); cx.arc(x, y, s, 0, Math.PI * 2); cx.fill(); },
    flower(cx, x, y, s){
      for (let i = 0; i < 5; i++) {
        const a = i * Math.PI * 2 / 5 - Math.PI / 2;
        shape.dot(cx, x + Math.cos(a) * s * 0.55, y + Math.sin(a) * s * 0.55, s * 0.42);
      }
      cx.fillStyle = '#fff'; shape.dot(cx, x, y, s * 0.28); cx.fillStyle = '#000';
    },
    balloon(cx, x, y, s){
      cx.beginPath(); cx.ellipse(x, y - s * 0.2, s * 0.62, s * 0.8, 0, 0, Math.PI * 2); cx.fill();
      cx.beginPath(); cx.moveTo(x, y + s * 0.6); cx.lineTo(x - s * 0.15, y + s * 0.8); cx.lineTo(x + s * 0.15, y + s * 0.8); cx.fill();
      cx.lineWidth = 2; cx.beginPath(); cx.moveTo(x, y + s * 0.8);
      cx.quadraticCurveTo(x - s * 0.4, y + s * 1.2, x, y + s * 1.6); cx.stroke();
    },
  };

  // A band of symbols across the slip; returns its height.
  function band(cx, W, y, kind, s, flip){
    const row = (fn, n) => { for (let i = 0; i < n; i++) fn(W / 2 + (i - (n - 1) / 2) * (W - 80) / (n - 1), y + s); };
    switch (kind) {
      case 'hearts':   row((x, yy) => shape.heart(cx, x, yy, s * 1.0), 7); return s * 2.2;
      case 'stars':    row((x, yy) => shape.star(cx, x, yy, s * 0.7, 5, 0.45), 7); return s * 2.2;
      case 'sparkles': row((x, yy) => shape.star(cx, x, yy, (x / 37 | 0) % 2 ? s * 0.75 : s * 0.45, 4, 0.25), 9); return s * 2.2;
      case 'dots':     row((x, yy) => shape.dot(cx, x, yy, s * 0.22), 15); return s * 1.6;
      case 'flowers':  row((x, yy) => shape.flower(cx, x, yy, s * 0.75), 6); return s * 2.3;
      case 'balloons': if (flip) return 0;
                       row((x, yy) => shape.balloon(cx, x, yy, s * 0.8), 5); return s * 3.0;
      case 'confetti': {
        let seed = flip ? 7 : 3;
        const rnd = () => (seed = (seed * 16807) % 2147483647) / 2147483647;
        for (let i = 0; i < 26; i++) {
          const x = 30 + rnd() * (W - 60), yy = y + 6 + rnd() * s * 1.6, t = rnd();
          cx.save(); cx.translate(x, yy); cx.rotate(rnd() * Math.PI);
          if (t < 0.4) cx.fillRect(-s * 0.25, -s * 0.08, s * 0.5, s * 0.16);
          else if (t < 0.7) shape.dot(cx, 0, 0, s * 0.13);
          else shape.star(cx, 0, 0, s * 0.25, 5, 0.45);
          cx.restore();
        }
        return s * 2.0;
      }
      case 'waves': case 'zigzag': {
        cx.lineWidth = 4; cx.beginPath();
        for (let x = 30; x <= W - 30; x += 2) {
          const t = (x - 30) / (s * 1.2);
          const yy = kind === 'waves' ? y + s * 0.6 + Math.sin(t * Math.PI) * s * 0.3
                                      : y + s * 0.6 + (Math.abs((t % 2) - 1) - 0.5) * s * 0.6;
          x === 30 ? cx.moveTo(x, yy) : cx.lineTo(x, yy);
        }
        cx.stroke(); return s * 1.3;
      }
      case 'double':
        cx.fillRect(30, y + s * 0.35, W - 60, 4); cx.fillRect(30, y + s * 0.35 + 10, W - 60, 2);
        return s * 1.0;
      default: return 0;
    }
  }

  // The whole message slip on a tall canvas; returns its height.
  // opts: { theme, deco, size, title, message, date }
  function drawSlip(cx, W, o){
    cx.fillStyle = '#fff'; cx.fillRect(0, 0, W, cx.canvas.height);
    cx.fillStyle = '#000'; cx.strokeStyle = '#000'; cx.textBaseline = 'top';
    const th = THEMES[o.theme] || THEMES.classic, base = SIZES[o.size] || 32;
    const framed = o.deco === 'frame' || o.deco === 'dotframe';
    const pad = framed ? 40 : 22, s = 26;
    let y = framed ? 34 : 14;
    if (!framed) y += band(cx, W, y, o.deco, s, false);
    const text = (str, font, size) => {
      cx.font = font;
      for (const l of wrap(cx, str, W - 2 * pad)) {
        cx.fillText(l, (W - cx.measureText(l).width) / 2, y);
        y += Math.round(size * 1.3);
      }
    };
    if (o.title) { y += 6; text(o.title, th.head(Math.round(base * 1.2)), Math.round(base * 1.2)); y += 8; }
    for (const raw of (o.message || '').split('\n')) {
      if (!raw.trim()) { y += Math.round(base * 0.6); continue; }
      text(raw.trim(), th.body(base), base);
    }
    if (o.date) {
      const d = new Date().toLocaleDateString(undefined, { day: 'numeric', month: 'long', year: 'numeric' });
      y += 6; text(d, th.body(Math.round(base * 0.55)), Math.round(base * 0.55));
    }
    y += 6;
    if (!framed) y += band(cx, W, y, o.deco, s, true);
    else {
      y += 20;
      if (o.deco === 'frame') {
        cx.lineWidth = 4; cx.strokeRect(12, 12, W - 24, y - 12);
        cx.lineWidth = 2; cx.strokeRect(20, 20, W - 40, y - 28);
      } else {
        const step = 16;
        for (let x = 14; x <= W - 14; x += step) { shape.dot(cx, x, 14, 3.5); shape.dot(cx, x, y, 3.5); }
        for (let yy = 14; yy <= y; yy += step) { shape.dot(cx, 14, yy, 3.5); shape.dot(cx, W - 14, yy, 3.5); }
      }
      y += 12;
    }
    return Math.ceil(y + 10);
  }

  const W = 576;
  let cfg = null, photo = null, busy = false, lastPrinted = 0;
  const img = { gamma: 0.6, brightness: 0, contrast: 0, threshold: 128 };

  async function api(url, opt){
    const r = await fetch(url, opt);
    const t = await r.text().catch(() => '');
    let j = null; try { j = t ? JSON.parse(t) : null; } catch (e) {}
    if (!r.ok) throw new Error((j && j.error) || ('HTTP ' + r.status + (t ? ': ' + t.trim().slice(0, 120) : '')));
    return j;
  }
  const say = (t, bad) => { el('status').textContent = t; el('status').style.color = bad ? 'var(--bad)' : ''; };

  // ---- Rendering -----------------------------------------------------
  function toBits(cv, h, thresh){
    const px = cv.getContext('2d').getImageData(0, 0, W, h).data, rb = W / 8;
    const data = new Uint8Array(rb * h);
    for (let y = 0; y < h; y++) for (let x = 0; x < W; x++) {
      const o = (y * W + x) * 4;
      if (0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2] < thresh) data[y * rb + (x >> 3)] |= 0x80 >> (x & 7);
    }
    return { data, rows: h };
  }
  function show(canvas, r){
    canvas.width = W; canvas.height = r.rows;
    const cx = canvas.getContext('2d'), im = cx.createImageData(W, r.rows), rb = W / 8;
    for (let y = 0; y < r.rows; y++) for (let x = 0; x < W; x++) {
      const on = r.data[y * rb + (x >> 3)] & (0x80 >> (x & 7)), o = (y * W + x) * 4;
      im.data[o] = im.data[o + 1] = im.data[o + 2] = on ? 0 : 255; im.data[o + 3] = 255;
    }
    cx.putImageData(im, 0, 0);
  }
  function slipOpts(){
    return { theme: el('theme').value, deco: el('deco').value, size: el('size').value,
             title: cfg ? cfg.printTitle : '', message: el('msg').value, date: cfg && cfg.date };
  }
  function renderSlip(){
    const o = slipOpts();
    if (!o.message.trim() && !o.title) return null;
    const cv = document.createElement('canvas'); cv.width = W; cv.height = 4000;
    const h = Math.min(4000, drawSlip(cv.getContext('2d', { willReadFrequently: true }), W, o));
    return toBits(cv, h, 160);
  }
  // A photo, Atkinson-dithered at full width with the unit's own settings.
  function renderPhoto(){
    if (!photo) return null;
    const k = W / photo.width, rows = Math.min(1100, Math.round(photo.height * k));
    const cv = document.createElement('canvas'); cv.width = W; cv.height = rows;
    const cx = cv.getContext('2d', { willReadFrequently: true });
    cx.fillStyle = '#fff'; cx.fillRect(0, 0, W, rows);
    cx.imageSmoothingQuality = 'high';
    cx.drawImage(photo, 0, 0, W, Math.round(photo.height * k));
    const px = cx.getImageData(0, 0, W, rows).data, n = W * rows, g = new Float32Array(n);
    const cf = (100 + img.contrast) / 100;
    for (let i = 0; i < n; i++) {
      let v = 0.299 * px[i * 4] + 0.587 * px[i * 4 + 1] + 0.114 * px[i * 4 + 2];
      v = (v + img.brightness - 128) * cf + 128; v = v < 0 ? 0 : v > 255 ? 255 : v;
      g[i] = 255 * Math.pow(v / 255, img.gamma);
    }
    const rb = W / 8, data = new Uint8Array(rb * rows);
    const add = (i, e) => { if (i < n) g[i] += e; };
    for (let y = 0; y < rows; y++) for (let x = 0; x < W; x++) {
      const i = y * W + x, old = g[i], black = old < img.threshold;
      if (black) data[y * rb + (x >> 3)] |= 0x80 >> (x & 7);
      const e = (old - (black ? 0 : 255)) / 8;
      if (x + 1 < W) add(i + 1, e); if (x + 2 < W) add(i + 2, e);
      if (y + 1 < rows) { if (x > 0) add(i + W - 1, e); add(i + W, e); if (x + 1 < W) add(i + W + 1, e); }
      if (y + 2 < rows) add(i + 2 * W, e);
    }
    return { data, rows };
  }

  let pvTimer = null;
  function preview(){
    const s = renderSlip(), p = renderPhoto();
    el('pvslip').style.display = s ? 'block' : 'none';
    el('pvphoto').style.display = p ? 'block' : 'none';
    if (s) show(el('pvslip'), s);
    if (p) show(el('pvphoto'), p);
    el('photoRow').style.display = photo ? 'flex' : 'none';
    el('print').disabled = busy || (!el('msg').value.trim() && !photo);
  }
  const later = () => { clearTimeout(pvTimer); pvTimer = setTimeout(preview, 150); };

  // ---- Printing -------------------------------------------------------
  function coverage(d){ let s = 0; for (const b of d) { let v = b; while (v) { s += v & 1; v >>= 1; } } return Math.round(s * 100 / (d.length * 8)); }
  // Long slips are split at white rows: the unit holds one image at a time.
  function parts(r){
    const rb = W / 8, max = Math.floor(48 * 1024 / rb), out = [];
    const white = y => { for (let i = y * rb; i < (y + 1) * rb; i++) if (r.data[i]) return false; return true; };
    let a = 0;
    while (a < r.rows) {
      let b = Math.min(r.rows, a + max);
      if (b < r.rows) { let y = b; while (y > a + 32 && !white(y)) y--; if (y > a + 32) b = y; }
      out.push(r.data.slice(a * rb, b * rb)); a = b;
    }
    return out;
  }
  async function waitDone(id){
    for (let i = 0; i < 400; i++) {
      await new Promise(r => setTimeout(r, 1000));
      try {
        const j = await api('/api/jobs?limit=25');
        const x = (j.jobs || []).find(q => q.id === id);
        if (x && (x.status === 'done' || x.status === 'failed')) return x.status;
      } catch (e) {}
    }
    return 'failed';
  }
  async function send(d, cut, tail){
    const fd = new FormData();
    fd.append('data', new Blob([d], { type: 'application/octet-stream' }), 'raster.bin');
    const rows = d.length / (W / 8);
    // Busy printer or full memory: wait and try again.
    for (let a = 0; ; a++) {
      try {
        return (await api('/api/print/raster?width=' + W + '&rows=' + rows + '&coverage=' + coverage(d)
          + '&cut=' + (cut ? 1 : 0) + '&tail=' + (tail ? 1 : 0), { method: 'POST', body: fd })).jobId;
      } catch (e) {
        if (a > 30 || !/memory|queue full|HTTP 5|fetch|network|load failed/i.test(e.message)) throw e;
        await new Promise(r => setTimeout(r, 3000));
      }
    }
  }
  async function printAll(){
    if (busy) return;
    const s = renderSlip(), p = renderPhoto();
    if (!s && !p) return;
    busy = true; preview(); say('Printing\u2026');
    try {
      let last = 0;
      if (s) {
        const list = parts(s);
        for (let i = 0; i < list.length; i++) {
          const end = i === list.length - 1;
          last = await send(list[i], end && (!p || el('cutBetween').checked), false);
          if (!end || p) if (await waitDone(last) !== 'done') throw new Error('the printer stopped');
        }
      }
      if (p) last = await send(p.data, true, true);
      say('Printed! \uD83C\uDF89 Pin it on the board.');
      el('msg').value = ''; photo = null;
      lastPrinted = Date.now();
    } catch (e) { say('Sorry \u2014 ' + e.message + '. Try again in a moment.', true); }
    busy = false; preview();
  }

  // ---- Photos: camera, upload, paste ---------------------------------
  let stream = null;
  function camClose(){
    if (stream) stream.getTracks().forEach(t => t.stop());
    stream = null; el('cam').style.display = 'none'; el('count').textContent = '';
  }
  async function camOpen(){
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      say('The camera is blocked by this browser on this page. See the kiosk set-up steps.', true); return;
    }
    try {
      stream = await navigator.mediaDevices.getUserMedia({ video: { width: { ideal: 1280 }, height: { ideal: 960 } }, audio: false });
      el('video').srcObject = stream; el('cam').style.display = 'flex';
    } catch (e) { say('Could not open the camera (' + e.name + ').', true); }
  }
  async function shoot(){
    el('shoot').disabled = true;
    for (const n of [3, 2, 1]) { el('count').textContent = n; await new Promise(r => setTimeout(r, 900)); }
    el('count').textContent = '';
    const v = el('video'), c = document.createElement('canvas');
    c.width = v.videoWidth; c.height = v.videoHeight;
    const cx = c.getContext('2d');
    cx.translate(c.width, 0); cx.scale(-1, 1);       // as people saw themselves
    cx.drawImage(v, 0, 0);
    photo = c; camClose(); el('shoot').disabled = false; preview();
  }
  async function useFile(f){
    if (!f || !f.type.startsWith('image/')) return;
    try {
      const b = await createImageBitmap(f, { imageOrientation: 'from-image' });
      const c = document.createElement('canvas'); c.width = b.width; c.height = b.height;
      c.getContext('2d').drawImage(b, 0, 0); photo = c; preview();
    } catch (e) { say('Could not open that image.', true); }
  }

  // ---- Printer state ---------------------------------------------------
  async function status(){
    try {
      const s = await api('/api/printer/status'), a = el('alert');
      if (s.alert && s.alert.code) { a.className = ''; a.textContent = s.alert.message; a.style.display = 'block'; }
      else if (s.restingMs > 1500) { a.className = 'info'; a.textContent = 'The printer is resting \u2014 next print in ' + Math.ceil(s.restingMs / 1000) + ' s.'; a.style.display = 'block'; }
      else a.style.display = 'none';
    } catch (e) {}
    setTimeout(status, 3000);
  }

  // ---- Start -----------------------------------------------------------
  document.addEventListener('DOMContentLoaded', async () => {
    el('exit').onclick = () => { try { localStorage.removeItem('kiosk'); } catch (e) {} location.href = (window.BASE || '') + '/'; };
    try {
      cfg = await api('/api/kiosk');
      const s = ((await api('/api/printer/status').catch(() => ({}))) || {}).image || {};
      if (typeof s.gamma === 'number') img.gamma = s.gamma / 100;
      for (const k of ['brightness', 'contrast', 'threshold']) if (typeof s[k] === 'number') img[k] = s[k];
    } catch (e) { cfg = { title: 'Leave a message!', subtitle: '', printTitle: '', footer: 'Available to hire', themes: Object.keys(THEMES), decos: Object.keys(DECOS) }; }
    el('title').textContent = cfg.title || '';
    el('sub').textContent = cfg.subtitle || '';
    el('foot').textContent = cfg.footer || '';
    document.title = cfg.title || 'Print a message';
    const fill = (sel, all, allowed, def) => {
      for (const k of Object.keys(all)) if (!allowed || allowed.includes(k)) {
        const o = document.createElement('option'); o.value = k; o.textContent = all[k].name || all[k]; sel.appendChild(o);
      }
      if (def && [...sel.options].some(o => o.value === def)) sel.value = def;
    };
    fill(el('theme'), THEMES, cfg.themes, cfg.defTheme);
    fill(el('deco'), DECOS, cfg.decos, cfg.defDeco);

    el('msg').addEventListener('input', later);
    ['theme', 'deco', 'size'].forEach(id => el(id).addEventListener('change', preview));
    el('print').onclick = printAll;
    el('snap').onclick = camOpen;
    el('shoot').onclick = shoot;
    el('camx').onclick = camClose;
    el('upload').onclick = () => el('file').click();
    el('file').onchange = e => { useFile(e.target.files[0]); e.target.value = ''; };
    el('nophoto').onclick = () => { photo = null; preview(); };
    document.addEventListener('paste', e => {
      const f = [...((e.clipboardData && e.clipboardData.items) || [])].find(i => i.type.startsWith('image/'));
      if (f) { e.preventDefault(); useFile(f.getAsFile()); }
    });
    // A new guest: clear the "Printed!" note once someone starts typing.
    el('msg').addEventListener('focus', () => { if (Date.now() - lastPrinted > 3000) say(''); });
    preview(); status();
    el('msg').focus();
  });
})();
</script>
</body></html>)HTML";

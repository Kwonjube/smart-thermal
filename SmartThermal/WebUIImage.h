// WebUIImage.h — Sections 9, 12, 13.
//
// The whole image pipeline runs in the browser: decode, orient, rotate,
// resize, grayscale, brightness/contrast, gamma, Atkinson dither and
// bit-packing.
// The ESP32 receives a finished 1-bit raster and relays it to the printer.
//
// This is not a workaround for the WROOM-32's lack of PSRAM so much as the
// better design: the browser has a real CPU, a real image decoder and a
// canvas, so the preview is exactly what the printer will produce.
//
// GAMMA DEFAULT IS 0.60, MEASURED ON PAPER.
// An earlier guess of 1.25 was wrong. It assumed the printhead was losing
// isolated dots; side-by-side prints at 0.6, 1.0, 1.25 and 1.5 showed the
// opposite problem. With this printer's density set to Dark, each dot
// bleeds past its cell, so midtones fill in and anything above 1.0 prints
// as mud. A gamma below 1 lifts midtones to compensate.
//
// The value is loaded from /api/settings at page load, so it is tunable
// without reflashing.

#pragma once
#include <Arduino.h>

static const char WEB_UI_IMAGE[] PROGMEM = R"HTML(
<div class="card lv3">
  <label>Photos</label>
  <div class="drop" id="drop">Drop photos, paste, or tap to choose
    <input type="file" id="ifile" accept="image/*" multiple style="display:none">
  </div>
  <div class="row" id="inav" style="display:none;margin-top:8px;align-items:center">
    <button class="ghost" id="iprev" style="flex:0 0 auto;padding:6px 12px">&lsaquo;</button>
    <span class="meta" id="iname" style="flex:1 1 auto;text-align:center;margin:0"></span>
    <button class="ghost" id="inext" style="flex:0 0 auto;padding:6px 12px">&rsaquo;</button>
    <button class="ghost" id="idrop" style="flex:0 0 auto;padding:6px 10px;font-size:12px">Remove</button>
  </div>

  <div id="pvwrap"><canvas id="preview"></canvas></div>
  <div class="meta lv4" id="ipv"></div>
  <div class="meta lv4" id="icov"></div>

  <div class="row">
    <div style="flex:1 1 110px">
      <label for="iwidth">Width</label>
      <select id="iwidth">
        <option value="1">Full</option><option value="0.75">75%</option>
        <option value="0.5">50%</option><option value="0.25">25%</option>
      </select>
    </div>
    <div style="flex:1 1 130px">
      <label for="imode">Type</label>
      <select id="imode"><option value="photo">Photo</option>
        <option value="text">Text &amp; drawings</option></select>
    </div>
    <div style="flex:1 1 100px">
      <label for="irot">Rotate</label>
      <select id="irot"><option value="0">0&deg;</option><option value="90">90&deg;</option>
        <option value="180">180&deg;</option><option value="270">270&deg;</option></select>
    </div>
  </div>

  <div class="sliders lv4">
    <label for="ibright">Brightness <span id="vbright">0</span></label>
    <input type="range" id="ibright" min="-100" max="100" value="0">
    <label for="icontr">Contrast <span id="vcontr">0</span></label>
    <input type="range" id="icontr" min="-100" max="100" value="0">
    <label for="igamma">Gamma <span id="vgamma">0.60</span></label>
    <input type="range" id="igamma" min="40" max="220" value="60">
    <label for="ithresh">Threshold <span id="vthresh">128</span></label>
    <input type="range" id="ithresh" min="16" max="240" value="128">
  </div>
  <div class="meta lv4">Settings apply to every photo in the list.</div>

  <div class="row" style="margin-top:12px">
    <label class="lv4" style="flex:0 0 auto;margin:0;text-transform:none;letter-spacing:0;
                  display:flex;align-items:center;gap:6px;color:var(--ink);font-size:13px">
      <input type="checkbox" id="iinv" style="width:auto"> Invert
    </label>
    <label style="flex:0 0 auto;margin:0;text-transform:none;letter-spacing:0;
                  display:flex;align-items:center;gap:6px;color:var(--ink);font-size:13px">
      <input type="checkbox" id="icut" checked style="width:auto"> Cut after each
    </label>
  </div>

  <div class="row" style="margin-top:12px">
    <button id="iprint" disabled>Print photo</button>
    <button class="ghost" id="icrop" style="flex:0 0 auto;display:none">Crop</button>
    <button class="ghost" id="iocr" style="flex:0 0 auto;display:none">Extract text</button>
    <button class="ghost" id="ireset" style="flex:0 0 auto">Reset</button>
  </div>
  <div class="meta" id="iprog"></div>
</div>

<script>
(() => {
  // Hard ceiling in WebApi.h. The live value from /api/printer/status is
  // lower and more honest, because an image needs one contiguous
  // allocation and the heap fragments as features are added.
  const HARD_MAX = 128 * 1024;
  let maxBytes = HARD_MAX;

  // Pictures are kept at most KEEP_W pixels wide and KEEP_MP megapixels.
  // Width, not the long side: a tall receipt shrunk by its height lost all
  // its small print (3950 px tall became 175 px wide). 1200 is twice the
  // printer's 576 dots; 8 MP keeps a list of photos within a phone's memory.
  const KEEP_W = 1200, KEEP_MP = 8e6;

  const DEFAULTS = { gamma: 60, threshold: 128, brightness: 0, contrast: 0 };

  const POPCOUNT = new Uint8Array(256);
  for(let i = 0; i < 256; i++) POPCOUNT[i] = (i & 1) + POPCOUNT[i >> 1];
  function coverageOf(buf){
    if(!buf.length) return 0;
    let set = 0;
    for(let i = 0; i < buf.length; i++) set += POPCOUNT[buf[i]];
    return Math.round(set * 100 / (buf.length * 8));
  }

  // The firmware's fixed pacing table (PrintQueue.cpp), for the estimate
  // shown under the preview. The firmware decides the real rest.
  const PACE = [[30,1100],[50,1000],[66,850],[80,700],[95,600]];
  function limitFor(c){
    if(c <= PACE[0][0]) return PACE[0][1];
    for(let i = 1; i < PACE.length; i++)
      if(c <= PACE[i][0]){
        const [c0,l0] = PACE[i-1], [c1,l1] = PACE[i];
        return l0 + (l1 - l0) * (c - c0) / (c1 - c0);
      }
    return PACE[PACE.length-1][1];
  }
  const ROWS_PER_SEC = 748;
  function restMsFor(rows, cov){
    const printMs = rows * 1000 / ROWS_PER_SEC, lim = limitFor(cov);
    const p = cov * 100;
    return p <= lim ? 0 : Math.round(printMs * (p - lim) / lim);
  }

  let photos = [];        // [{ name, img }] img is a downscaled canvas
  let cur    = 0;
  let packed = null;      // the current photo, packed
  let timer  = null;
  let running = false, cancel = false;

  const el = id => document.getElementById(id);

  function targetWidth(){
    let w = Math.floor(window.PRINTER.printableDots * +el('iwidth').value);
    w -= (w % 8);
    return Math.max(8, w);
  }

  // A document, screenshot or drawing is mostly one brightness (its
  // paper); a photo spreads across many. Over half the pixels within a
  // narrow band of the commonest brightness means "text & drawings".
  function looksLikeText(img){
    const k = Math.min(1, 200 / Math.max(img.width, img.height));
    const c = document.createElement('canvas');
    c.width = Math.max(1, Math.round(img.width * k)); c.height = Math.max(1, Math.round(img.height * k));
    const cx = c.getContext('2d', { willReadFrequently: true });
    cx.drawImage(img, 0, 0, c.width, c.height);
    const d = cx.getImageData(0, 0, c.width, c.height).data, h = new Uint32Array(256);
    for(let i = 0; i < d.length; i += 4) h[(0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2]) | 0]++;
    let peak = 0; for(let v = 1; v < 256; v++) if(h[v] > h[peak]) peak = v;
    let near = 0; for(let v = Math.max(0, peak - 20); v <= Math.min(255, peak + 20); v++) near += h[v];
    return near / (d.length / 4) > 0.55;
  }

  async function decode(file){
    let src;
    try{
      // from-image honours EXIF rotation, so phone photos land upright.
      src = await createImageBitmap(file, { imageOrientation: 'from-image' });
    }catch(e){
      src = await new Promise((res, rej) => {
        const img = new Image();
        img.onload  = () => res(img);
        img.onerror = () => rej(new Error('could not decode ' + (file.name || 'image')));
        img.src = URL.createObjectURL(file);
      });
    }
    const k  = Math.min(1, KEEP_W / src.width,
                        Math.sqrt(KEEP_MP / (src.width * src.height)));
    const cv = document.createElement('canvas');
    cv.width  = Math.max(1, Math.round(src.width * k));
    cv.height = Math.max(1, Math.round(src.height * k));
    const cx = cv.getContext('2d');
    cx.imageSmoothingQuality = 'high';
    cx.drawImage(src, 0, 0, cv.width, cv.height);
    if(src.close) src.close();
    return cv;
  }

  async function addFiles(list){
    const files = [...list].filter(f => f.type && f.type.startsWith('image/'));
    if(!files.length){ window.toast('no image files', true); return; }
    const first = photos.length;
    for(const f of files){
      // The original file is kept too: text reading needs full resolution,
      // and the stored canvas is shrunk to 1200 px.
      try{
        const img = await decode(f);
        photos.push({ name: f.name || 'pasted image', img, file: f, mode: looksLikeText(img) ? 'text' : 'photo' });
      }
      catch(e){ window.toast(e.message, true); }
    }
    if(photos.length > first) { cur = first; render(); }
  }

  // One photo to a packed 1-bit raster, Atkinson dithered. The preview
  // canvas is filled only when asked, so a print run does not repaint it.
  // crop: { x, y, w, h } as fractions of the rotated picture, or null.
  function pack(img, draw, crop, mode){
    const c    = crop || { x: 0, y: 0, w: 1, h: 1 };
    const rot  = +el('irot').value;
    const W    = targetWidth();
    const swap = (rot === 90 || rot === 270);
    const srcW = (swap ? img.height : img.width) * c.w;
    const srcH = (swap ? img.width  : img.height) * c.h;
    const rows = Math.max(1, Math.round(W * srcH / srcW));
    const rowBytes = W / 8;

    const cv = document.createElement('canvas');
    cv.width = W; cv.height = rows;
    const cx = cv.getContext('2d', { willReadFrequently: true });
    cx.fillStyle = '#fff'; cx.fillRect(0, 0, W, rows);
    cx.imageSmoothingQuality = 'high';
    cx.save();
    // The whole rotated picture at the crop's scale, shifted so the crop
    // lands on the canvas.
    const FW = W / c.w, FH = rows / c.h;
    cx.translate(FW / 2 - c.x * FW, FH / 2 - c.y * FH);
    cx.rotate(rot * Math.PI / 180);
    const dw = swap ? FH : FW, dh = swap ? FW : FH;
    cx.drawImage(img, -dw / 2, -dh / 2, dw, dh);
    cx.restore();
    const px = cx.getImageData(0, 0, W, rows).data;

    const bright = +el('ibright').value, contr = +el('icontr').value;
    const gamma  = +el('igamma').value / 100, cf = (100 + contr) / 100;
    const th = +el('ithresh').value, inv = el('iinv').checked;

    const n = W * rows, g = new Float32Array(n);
    for(let i = 0; i < n; i++){
      const o = i * 4, a = px[o + 3] / 255;
      let v = 0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2];
      v = 255 - a * (255 - v);
      v = (v + bright - 128) * cf + 128;
      v = v < 0 ? 0 : (v > 255 ? 255 : v);
      g[i] = 255 * Math.pow(v / 255, gamma);
    }

    const out = new Uint8Array(rowBytes * rows);
    const pv  = draw ? cx.createImageData(W, rows) : null;

    // TEXT & DRAWINGS: sharp black and white, no dither - dithering turns
    // the grey edges of small letters into speckle. Each pixel is judged
    // against its own neighbourhood (a box mean via an integral image), so a
    // phone photo of a printed page with a shadow across it still comes out
    // clean. Very dark is always black, so solid shapes stay solid. The
    // threshold slider shifts the sensitivity.
    if(mode === 'text'){
      const raw = new Float32Array(n);
      for(let i = 0; i < n; i++){
        const o = i * 4, a = px[o + 3] / 255;
        let v = 255 - a * (255 - (0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2]));
        v = (v + bright - 128) * cf + 128;
        raw[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
      }
      const S = new Float64Array((W + 1) * (rows + 1));
      for(let y = 0; y < rows; y++){
        let line = 0;
        for(let x = 0; x < W; x++){
          line += raw[y * W + x];
          S[(y + 1) * (W + 1) + x + 1] = S[y * (W + 1) + x + 1] + line;
        }
      }
      const r = Math.max(8, Math.round(W / 40)), shift = (th - 128) / 2;
      for(let y = 0; y < rows; y++){
        const y0 = Math.max(0, y - r), y1 = Math.min(rows, y + r + 1);
        for(let x = 0; x < W; x++){
          const x0 = Math.max(0, x - r), x1 = Math.min(W, x + r + 1);
          const mean = (S[y1 * (W + 1) + x1] - S[y0 * (W + 1) + x1] - S[y1 * (W + 1) + x0] + S[y0 * (W + 1) + x0])
                     / ((x1 - x0) * (y1 - y0));
          const v = raw[y * W + x];
          const black = v < 80 + shift || (v < mean - 20 + shift && v < 205 + shift);
          const on = inv ? !black : black;
          if(on) out[y * rowBytes + (x >> 3)] |= 0x80 >> (x & 7);
          if(pv){ const o = (y * W + x) * 4, c = on ? 0 : 255;
                  pv.data[o] = pv.data[o+1] = pv.data[o+2] = c; pv.data[o+3] = 255; }
        }
      }
    } else {
    // PHOTO - Atkinson: leaks 1/4 of the error, which clusters dots and
    // suits a printhead that cannot hold isolated ones.
    const add = (i, e) => { if(i < n) g[i] += e; };
    for(let y = 0; y < rows; y++){
      for(let x = 0; x < W; x++){
        const i = y * W + x, old = g[i], black = old < th;
        const on = inv ? !black : black;
        if(on) out[y * rowBytes + (x >> 3)] |= 0x80 >> (x & 7);
        if(pv){ const o = i * 4, c = on ? 0 : 255;
                pv.data[o] = pv.data[o+1] = pv.data[o+2] = c; pv.data[o+3] = 255; }
        const e = (old - (black ? 0 : 255)) / 8;
        if(x + 1 < W) add(i + 1, e);
        if(x + 2 < W) add(i + 2, e);
        if(y + 1 < rows){ if(x > 0) add(i + W - 1, e); add(i + W, e);
                          if(x + 1 < W) add(i + W + 1, e); }
        if(y + 2 < rows) add(i + 2 * W, e);
      }
    }
    }
    if(pv){
      const c = el('preview');
      c.width = W; c.height = rows; c.style.display = 'block';
      c.getContext('2d').putImageData(pv, 0, 0);
    }
    return { data: out, width: W, rows, coverage: coverageOf(out) };
  }

  function render(){
    const nav = el('inav'), btn = el('iprint');
    el('iocr').style.display = el('icrop').style.display = photos.length ? '' : 'none';
    if(photos.length){
      const q = photos[Math.min(cur, photos.length - 1)];
      el('icrop').textContent = cropping ? 'Cancel' : q.crop ? 'Uncrop' : 'Crop';
      el('iocr').textContent  = q.way ? 'Try another way' : 'Extract text';
    }
    if(!photos.length){
      packed = null; nav.style.display = 'none';
      el('preview').style.display = 'none';
      el('ipv').textContent = el('icov').textContent = '';
      btn.disabled = true; btn.textContent = 'Print photo';
      return;
    }
    if(cur >= photos.length) cur = photos.length - 1;
    const p = photos[cur];
    nav.style.display = photos.length ? 'flex' : 'none';
    el('iname').textContent = (photos.length > 1 ? (cur + 1) + ' of ' + photos.length + ' \u00b7 ' : '')
      + p.name;

    el('imode').value = p.mode || 'photo';
    const k = pack(p.img, true, cropping ? null : p.crop, p.mode);
    const bytes = k.data.length, nParts = splitParts(k).length;
    const rest = restMsFor(k.rows, k.coverage);
    packed = k;

    el('ipv').textContent = k.width + ' x ' + k.rows + ' dots \u00b7 '
      + (k.rows / 8).toFixed(0) + ' mm long \u00b7 ' + (bytes / 1024).toFixed(1) + ' kB'
      + (nParts > 1 ? ' \u00b7 prints in ' + nParts + ' parts' : '');
    el('icov').textContent = k.coverage + '% ink'
      + (rest ? ' \u00b7 ' + (rest / 1000).toFixed(1) + ' s rest after printing'
              : ' \u00b7 no rest needed')
      + (k.coverage > 65 ? '  \u2014 very dense; raise gamma or lower contrast '
                           + 'for a faster, cleaner print' : '');
    if(!running){
      btn.disabled = false;
      btn.textContent = photos.length > 1 ? 'Print ' + photos.length + ' photos'
                                          : 'Print photo';
    }
  }

  function queueRender(){ clearTimeout(timer); timer = setTimeout(render, 90); }

  // Upload one packed photo. A 503 means the unit has no room yet (the
  // previous photo is still in memory): wait and try again.
  // The unit holds one image in memory at a time (~95 KB), so a long one is
  // sent in parts, each after the last has printed. Cuts are made at a
  // white row near the limit when there is one, so text documents join
  // invisibly; a photo with no white rows joins with a faint line.
  function splitParts(k){
    const rb = k.width / 8, limit = Math.min(maxBytes, window.REMOTE ? 24 * 1024 : 64 * 1024);
    const maxRows = Math.max(32, Math.floor(limit / rb)), out = [];
    const white = y => { for(let i = y * rb; i < (y + 1) * rb; i++) if(k.data[i]) return false; return true; };
    let a = 0;
    while(a < k.rows){
      let b = Math.min(k.rows, a + maxRows);
      if(b < k.rows){
        let y = b; const stop = a + Math.floor(maxRows * 0.6);
        while(y > stop && !white(y)) y--;
        if(y > stop) b = y;
      }
      const data = k.data.slice(a * rb, b * rb);
      out.push({ data, width: k.width, rows: b - a, coverage: coverageOf(data) });
      a = b;
    }
    return out;
  }

  async function upload(k, cut, tail){
    const url = '/api/print/raster?width=' + k.width + '&rows=' + k.rows
              + '&coverage=' + k.coverage + '&cut=' + (cut ? 1 : 0) + '&tail=' + (tail ? 1 : 0);
    for(let attempt = 0; ; attempt++){
      const fd = new FormData();
      fd.append('data', new Blob([k.data], { type:'application/octet-stream' }), 'raster.bin');
      try{ return (await window.api(url, { method:'POST', body: fd })).jobId; }
      catch(e){
        // Retry only what time can fix: no memory yet, a full queue, or a
        // dropped request. Anything else is a real refusal.
        const busy = /out of memory|queue full|HTTP 5|fetch|network|load failed/i
                       .test(e.message || '');
        if(!busy || attempt >= 40 || cancel) throw e;
        await new Promise(r => setTimeout(r, 3000));
      }
    }
  }

  // The unit holds one photo in memory at a time, so the next is sent only
  // once this one has printed (or failed) and its memory is free again.
  async function waitDone(id){
    for(;;){
      await new Promise(r => setTimeout(r, 1500));
      try{
        const j = await window.api('/api/jobs?limit=25');
        const x = (j.jobs || []).find(q => q.id === id);
        if(x && (x.status === 'done' || x.status === 'failed')) return x.status;
      }catch(e){}
    }
  }

  async function printAll(){
    if(running){ cancel = true; el('iprog').textContent = 'Stopping after this photo\u2026'; return; }
    if(!photos.length) return;
    running = true; cancel = false;
    const btn = el('iprint');
    btn.textContent = 'Stop';
    btn.disabled = false;
    let done = 0, failed = 0, lastErr = '';
    for(let i = 0; i < photos.length && !cancel; i++){
      el('iprog').textContent = 'Sending ' + (i + 1) + ' of ' + photos.length + '\u2026';
      const ph = photos[i];
      const parts = splitParts(pack(ph.img, false, ph.crop, ph.mode));
      try{
        let ok = true;
        for(let j = 0; j < parts.length && ok && !cancel; j++){
          const last = j === parts.length - 1;
          // Only the end gets the cut, and the white tail - and the tail
          // only for photos (it fixes a density step a document doesn't show).
          const id = await upload(parts[j], last && el('icut').checked, last && ph.mode !== 'text');
          el('iprog').textContent = 'Printing ' + (i + 1) + ' of ' + photos.length
            + (parts.length > 1 ? ', part ' + (j + 1) + ' of ' + parts.length : '')
            + ' (job #' + id + ')\u2026';
          ok = (await waitDone(id)) === 'done';
        }
        ok ? done++ : failed++;
      }catch(e){ failed++; lastErr = e.message; window.toast(e.message, true); }
    }
    el('iprog').textContent = done + ' printed' + (failed ? ', ' + failed + ' failed' : '')
      + (cancel ? ' \u2014 stopped' : '') + (lastErr ? ' (' + lastErr + ')' : '');
    if(done && !failed) window.toast(done === 1 ? 'Printed \u2713' : done + ' photos printed \u2713');
    running = false;
    render();
  }

  // ------------------------------------------------------------------
  // Extract text. Nothing is recognised: the words are found by layout and
  // copied as pictures (see FI in WebUITemplate.h), so nothing is
  // downloaded and nothing can be misread. This hands the full-resolution
  // screenshot to the Text card's "From image" style.
  // ------------------------------------------------------------------
  async function fullRes(p){
    let src = p.img;
    if(p.file){
      try{ src = await createImageBitmap(p.file, { imageOrientation: 'from-image' }); }catch(e){}
    }
    // Same rotation and crop as the photo, at full resolution: wide enough
    // for small text, capped so a long scroll screenshot stays within the
    // browser's canvas limits.
    const c = p.crop || { x: 0, y: 0, w: 1, h: 1 };
    const rot = +el('irot').value, swap = (rot === 90 || rot === 270);
    const RW = swap ? src.height : src.width, RH = swap ? src.width : src.height;
    const k = Math.min(1, 2000 / (RW * c.w), 12000 / (RH * c.h));
    const cv = document.createElement('canvas');
    cv.width = Math.max(1, Math.round(RW * c.w * k)); cv.height = Math.max(1, Math.round(RH * c.h * k));
    const cx = cv.getContext('2d');
    cx.fillStyle = '#fff'; cx.fillRect(0, 0, cv.width, cv.height);
    const FW = RW * k, FH = RH * k;
    cx.translate(FW / 2 - c.x * FW, FH / 2 - c.y * FH);
    cx.rotate(rot * Math.PI / 180);
    cx.drawImage(src, -(swap ? FH : FW) / 2, -(swap ? FW : FH) / 2, swap ? FH : FW, swap ? FW : FH);
    return cv;
  }

  // Short lines, bullets or quantities: most of the lines means a list.
  function looksLikeList(t){
    const lines = t.split('\n').map(l => l.trim()).filter(Boolean);
    if(lines.length < 3) return false;
    const QTY = /^(\d|[\u00bc-\u00be\u2150-\u215e]|[-*\u2022\u25cf\u25aa\u2013])/;
    const hits = lines.filter(l => l.length <= 45 || QTY.test(l)).length;
    return hits / lines.length >= 0.6;
  }

  // Three ways to read a page; each press of the button tries the next.
  const WAYS = ['', 'whole lines', 'separate pieces, pictures left out',
                'main column only (drops prices, buttons, side bits)'];
  async function extractText(p){
    if(!window.fromImage) return;
    const btn = el('iocr'); btn.disabled = true;
    const way = p.way ? (p.way % 3) + 1 : 1;
    el('iprog').textContent = 'Finding the text\u2026';
    try{
      const n = window.fromImage.set(await fullRes(p), way);
      p.way = way;
      render();
      el('iprog').textContent = n
        ? 'Way ' + way + ' of 3: ' + WAYS[way] + '. ' + n + ' lines found. '
          + 'Messy? Tap "Try another way".'
        : '';
      if(!n) window.toast('no text found in that image', true);
      else document.getElementById('tcv').scrollIntoView({ behavior: 'smooth', block: 'center' });
    }catch(e){ window.toast('could not read that image', true); el('iprog').textContent = ''; }
    btn.disabled = false;
  }

  // ------------------------------------------------------------------
  // Crop: tap Crop, drag over the part you want. Used for printing the
  // photo and for Extract text alike. Uncrop (or Reset) restores it.
  // ------------------------------------------------------------------
  let cropping = false, cropBase = null, cropStart = null;
  function cropAt(e){
    const r = el('preview').getBoundingClientRect();
    return { x: Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)),
             y: Math.min(1, Math.max(0, (e.clientY - r.top) / r.height)) };
  }
  function cropDraw(a, b){
    const c = el('preview'), cx = c.getContext('2d');
    cx.putImageData(cropBase, 0, 0);
    if(!a) return;
    const x = Math.min(a.x, b.x) * c.width, y = Math.min(a.y, b.y) * c.height;
    const w = Math.abs(a.x - b.x) * c.width, h = Math.abs(a.y - b.y) * c.height;
    cx.fillStyle = 'rgba(0,0,0,0.45)';
    cx.fillRect(0, 0, c.width, y); cx.fillRect(0, y + h, c.width, c.height - y - h);
    cx.fillRect(0, y, x, h); cx.fillRect(x + w, y, c.width - x - w, h);
    cx.strokeStyle = '#1b7f5a'; cx.lineWidth = Math.max(2, c.width / 200);
    cx.strokeRect(x, y, w, h);
  }
  function cropEnd(){ cropping = false; cropStart = null; el('preview').style.touchAction = ''; }

  document.addEventListener('DOMContentLoaded', () => {
    const drop = el('drop');

    el('icrop').onclick = () => {
      if(!photos.length || running) return;
      const p = photos[cur];
      if(cropping){ cropEnd(); el('iprog').textContent = ''; render(); return; }
      if(p.crop){ p.crop = null; p.way = 0; render(); return; }
      cropping = true;
      render();                                      // the whole picture
      const c = el('preview');
      cropBase = c.getContext('2d').getImageData(0, 0, c.width, c.height);
      c.style.touchAction = 'none';                  // the drag must not scroll
      el('iprog').textContent = 'Drag over the part you want.';
    };
    const pv = el('preview');
    pv.addEventListener('pointerdown', e => {
      if(!cropping) return;
      cropStart = cropAt(e); pv.setPointerCapture(e.pointerId); e.preventDefault();
    });
    pv.addEventListener('pointermove', e => { if(cropping && cropStart) cropDraw(cropStart, cropAt(e)); });
    pv.addEventListener('pointerup', e => {
      if(!cropping || !cropStart) return;
      const a = cropStart, b = cropAt(e);
      const r = { x: Math.min(a.x, b.x), y: Math.min(a.y, b.y), w: Math.abs(a.x - b.x), h: Math.abs(a.y - b.y) };
      if(r.w < 0.03 || r.h < 0.03){ cropStart = null; cropDraw(null); return; }   // a tap, not a drag
      const p = photos[cur];
      p.crop = r; p.way = 0;
      cropEnd();
      el('iprog').textContent = 'Cropped. Print it, or tap Extract text.';
      render();
    });
    el('ifile').onchange = e => { addFiles(e.target.files); e.target.value = ''; };
    drop.onclick = () => el('ifile').click();
    ['dragenter','dragover'].forEach(ev => drop.addEventListener(ev, e => {
      e.preventDefault(); drop.classList.add('over'); }));
    ['dragleave','drop'].forEach(ev => drop.addEventListener(ev, e => {
      e.preventDefault(); drop.classList.remove('over'); }));
    drop.addEventListener('drop', e => {
      if(e.dataTransfer.files.length) addFiles(e.dataTransfer.files); });
    document.addEventListener('paste', e => {
      const f = [...((e.clipboardData && e.clipboardData.items) || [])]
        .filter(i => i.type.startsWith('image/')).map(i => i.getAsFile());
      // An image pasted while typing in the Text box must not also land
      // in it as junk.
      if(f.length){ e.preventDefault(); addFiles(f); }
    });

    el('iprev').onclick = () => { if(photos.length){ cur = (cur + photos.length - 1) % photos.length; render(); } };
    el('inext').onclick = () => { if(photos.length){ cur = (cur + 1) % photos.length; render(); } };
    el('idrop').onclick = () => { if(!running){ photos.splice(cur, 1); render(); } };

    ['iwidth','irot','iinv'].forEach(id => el(id).onchange = render);
    el('imode').onchange = () => { if(photos.length){ photos[cur].mode = el('imode').value; render(); } };
    [['ibright','vbright',v=>v], ['icontr','vcontr',v=>v],
     ['igamma','vgamma',v=>(v/100).toFixed(2)], ['ithresh','vthresh',v=>v]]
      .forEach(([id, out, fmt]) => {
        el(id).oninput = () => { el(out).textContent = fmt(+el(id).value); queueRender(); };
      });

    function applyDefaults(){
      el('ibright').value = DEFAULTS.brightness; el('vbright').textContent = String(DEFAULTS.brightness);
      el('icontr').value  = DEFAULTS.contrast;   el('vcontr').textContent  = String(DEFAULTS.contrast);
      el('igamma').value  = DEFAULTS.gamma;      el('vgamma').textContent  = (DEFAULTS.gamma / 100).toFixed(2);
      el('ithresh').value = DEFAULTS.threshold;  el('vthresh').textContent = String(DEFAULTS.threshold);
      el('iinv').checked = false; el('irot').value = '0'; el('iwidth').value = '1';
    }
    el('ireset').onclick = () => {
      applyDefaults();
      if(photos.length){ photos[cur].crop = null; photos[cur].way = 0; }
      if(cropping) cropEnd();
      render();
    };

    // The photo defaults arrive with the printer status (v0.22: /api/settings
    // is home-network only, so the website page could not read them).
    let defaultsIn = false;
    const takeDefaults = () => {
      const s = window.PRINTER.image;
      if(defaultsIn || !s) return;
      defaultsIn = true;
      if(typeof s.gamma      === 'number') DEFAULTS.gamma      = s.gamma;
      if(typeof s.threshold  === 'number') DEFAULTS.threshold  = s.threshold;
      if(typeof s.brightness === 'number') DEFAULTS.brightness = s.brightness;
      if(typeof s.contrast   === 'number') DEFAULTS.contrast   = s.contrast;
      if(!photos.length) applyDefaults();
    };

    el('iprint').onclick = printAll;

    // Levels (v0.24): below Expert the fine-tuning is hidden, so it goes
    // back to the printer's defaults rather than silently changing prints.
    window.addEventListener('levelchange', () => {
      if((window.LEVEL || 4) >= 4) return;
      const was = [el('ibright').value, el('icontr').value, el('igamma').value, el('ithresh').value, el('iinv').checked].join();
      el('ibright').value = DEFAULTS.brightness; el('vbright').textContent = String(DEFAULTS.brightness);
      el('icontr').value  = DEFAULTS.contrast;   el('vcontr').textContent  = String(DEFAULTS.contrast);
      el('igamma').value  = DEFAULTS.gamma;      el('vgamma').textContent  = (DEFAULTS.gamma / 100).toFixed(2);
      el('ithresh').value = DEFAULTS.threshold;  el('vthresh').textContent = String(DEFAULTS.threshold);
      el('iinv').checked = false;
      const now = [el('ibright').value, el('icontr').value, el('igamma').value, el('ithresh').value, el('iinv').checked].join();
      if(now !== was && photos.length && !running) render();
    });

    // Text pasted into an empty Text box that looks like a list becomes a
    // List - the same rule as Extract text, for text copied out of a
    // screenshot with the phone's or PC's own text recognition.
    const box = document.getElementById('text');
    box.addEventListener('paste', e => {
      const was = box.value.trim();
      if(!e.clipboardData || !e.clipboardData.getData('text')) return;
      setTimeout(() => {
        const st = document.getElementById('tstyle');
        if(!was && st && st.value === 'plain' && (window.LEVEL || 4) >= 3 && looksLikeList(box.value)){
          st.value = 'list'; st.dispatchEvent(new Event('change', { bubbles: true }));
        }
      }, 0);
    });


    el('iocr').onclick = () => { if(photos.length && !running) extractText(photos[cur]); };

    window.onPrinterInfo = () => {
      takeDefaults();
      if(window.PRINTER.maxImageBytes)
        maxBytes = Math.min(HARD_MAX, window.PRINTER.maxImageBytes);
      if(photos.length && !running) queueRender();
    };
  });
})();
</script>
)HTML";

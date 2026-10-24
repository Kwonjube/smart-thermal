// WebUITemplate.h - the Style menu on the Text card: List, Note, Love note,
// and themes (Parking fine; its form and drawing are in WebUITheme.h).
//
// Deliberately small. Plain is untouched: it still prints as printer text.
// The other styles are drawn by the browser onto a canvas exactly as they
// will print, turned into crisp 1-bit rows (text needs a threshold, not a
// dither) and sent through the image path with the white tail off - text is
// light, and the tail would only waste paper.
//
// How the typed text is read, the same for every style:
//   first line          the title
//   every other line    the body (List: one tick-box item per line)
//   a line ending ":"   a subheading (List: no box)
//   a blank line        a small gap
// Leading "-", "*", "[ ]" or "1." on a List line is dropped; the box
// replaces it.
//
// A long template is split into parts of at most ~48 KB, always at a white
// row, and sent one after another - the unit holds one image in memory at
// a time. Only the last part is cut.

#pragma once
#include <Arduino.h>

static const char WEB_UI_TEMPLATE[] PROGMEM = R"HTML(
<script>
(() => {
  const el = id => document.getElementById(id);
  const SANS  = 'Helvetica, Arial, sans-serif';
  const SERIF = 'Georgia, "Times New Roman", serif';
  const BASE  = { b1: 22, a1: 26, b2: 32, a2: 42, a3: 56 };   // px per size
  const PAD   = 18;
  const MAX_PART_BYTES = 48 * 1024;

  // ---- From image: find words by layout, never by reading them ----------
  // A screenshot's own pixels are copied, word by word, and re-wrapped to
  // the paper. Nothing is recognised, so nothing can be misread: "1½"
  // prints as the site drew it. Works for light and dark mode.
  const FI = (() => {
    const lum = (d, i) => 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];

    // Three ways to read a page, chosen by "Try another way":
    //   1 lines   whole-width rows of text (clean pages, the default)
    //   2 blocks  the page split into separate pieces first, pictures and
    //             buttons dropped (pages with pictures and columns)
    //   3 main    blocks, then only text on the page's main left edge
    //             (drops prices, captions and side panels)
    const MODES = ['lines', 'blocks', 'main'];

    function find(img, mode){
      mode = mode || 1;
      const W = img.width, H0 = img.height, d = img.data;
      const hist = new Uint32Array(256);
      for (let i = 0; i < W * H0 * 4; i += 16) hist[lum(d, i) | 0]++;
      let bg = 0; for (let v = 1; v < 256; v++) if (hist[v] > hist[bg]) bg = v;
      const dark = bg < 110;
      const ink = new Uint8Array(W * H0), mid = new Uint8Array(W * H0);
      for (let p = 0, i = 0; p < W * H0; p++, i += 4) {
        const v = lum(d, i), dv = dark ? v - bg : bg - v;
        if (dv > 70) ink[p] = 1; else if (dv > 20) mid[p] = 1;
      }

      // Rows of ink within a rectangle -> lines -> words.
      function linesIn(x0, x1, y0, y1, Hm, blk){
        const rowInk = new Uint32Array(y1 - y0);
        for (let y = y0; y < y1; y++) { let c = 0; const o = y * W; for (let x = x0; x < x1; x++) c += ink[o + x]; rowInk[y - y0] = c; }
        const bands = []; let y = y0;
        while (y < y1) {
          while (y < y1 && !rowInk[y - y0]) y++;
          if (y >= y1) break;
          const s = y; while (y < y1 && rowInk[y - y0]) y++;
          const last = bands[bands.length - 1];
          if (last && s - last.y1 <= 2) last.y1 = y; else bands.push({ y0: s, y1: y });
        }
        if (!Hm) {
          const hs = bands.map(b => b.y1 - b.y0).filter(h => h >= 6).sort((a, b) => a - b);
          Hm = hs.length ? hs[hs.length >> 1] : 20;
        }
        const lines = [];
        for (const b of bands) {
          const h = b.y1 - b.y0;
          if (h < 4 || h > Hm * 3) continue;                        // specks, pictures
          let area = 0; for (let yy = b.y0; yy < b.y1; yy++) area += rowInk[yy - y0];
          if (area / (h * (x1 - x0)) > 0.45) continue;               // solid bars
          const col = new Uint8Array(x1 - x0);
          for (let yy = b.y0; yy < b.y1; yy++) { const o = yy * W; for (let x = x0; x < x1; x++) if (ink[o + x]) col[x - x0] = 1; }
          const gapMax = Math.max(3, Math.round(h * 0.3)), words = [];
          let x = 0; const CW = x1 - x0;
          while (x < CW) {
            while (x < CW && !col[x]) x++;
            if (x >= CW) break;
            const s = x; let e = x;
            while (x < CW) {
              if (col[x]) { e = ++x; continue; }
              let g = x; while (g < CW && !col[g]) g++;
              if (g >= CW || g - x > gapMax) break;
              x = g;
            }
            words.push({ x0: s + x0, x1: e + x0 });
            x = e;
          }
          let bullet = false;
          const w0 = words[0];
          if (w0 && words.length > 1 && w0.x1 - w0.x0 <= Hm * 0.45) {
            let top = b.y1, bot = b.y0;
            for (let yy = b.y0; yy < b.y1; yy++) for (let xx = w0.x0; xx < w0.x1; xx++)
              if (ink[yy * W + xx]) { if (yy < top) top = yy; if (yy > bot) bot = yy; }
            if (bot - top + 1 <= h * 0.55) { bullet = true; words.shift(); }
          }
          if (!words.length) continue;
          let wsum = 0; for (const w of words) wsum += w.x1 - w.x0;
          // Not text: a lone mark narrower than a letter pair (a cursor, an
          // icon), or a slab of ink denser than any lettering (a picture's
          // edge).
          if (wsum < Hm * 0.6) continue;
          if (area / (wsum * h) > 0.6) continue;
          // A picture's edge: long pieces with no gap between letters. Real
          // words always have some empty columns inside them.
          const solidPiece = w => {
            if (w.x1 - w.x0 < h * 2) return false;
            for (let x = w.x0 + 1; x < w.x1 - 1; x++) if (!col[x - x0]) return false;
            return true;
          };
          for (let k = words.length - 1; k >= 0; k--) if (solidPiece(words[k])) words.splice(k, 1);
          if (!words.length) continue;
          wsum = 0; for (const w of words) wsum += w.x1 - w.x0;
          lines.push({ y0: b.y0, y1: b.y1, words, bullet, blk,
                       tall: h > Hm * 1.12, dens: wsum ? area / (wsum * h) : 0 });
        }
        return { lines, Hm };
      }

      // Pass 1 always: whole-width lines, which also give the text height.
      let { lines, Hm } = linesIn(0, W, 0, H0, 0, 0);

      if (mode >= 2) {
        // Blocks: smear ink on a coarse grid so letters, words and lines of
        // one piece join up, then take each connected piece on its own.
        const c = Math.max(2, Math.floor(Hm / 4));
        const GW = Math.ceil(W / c), GH = Math.ceil(H0 / c);
        const g = new Uint8Array(GW * GH);
        for (let y = 0; y < H0; y++) for (let x = 0; x < W; x++)
          if (ink[y * W + x]) g[((y / c) | 0) * GW + ((x / c) | 0)] = 1;
        const hGap = Math.ceil(Hm * 1.0 / c), vGap = Math.ceil(Hm * 0.8 / c);
        const sm = new Uint8Array(g);
        for (let y = 0; y < GH; y++) {                             // across
          let last = -1;
          for (let x = 0; x < GW; x++) if (g[y * GW + x]) {
            if (last >= 0 && x - last - 1 <= hGap) for (let k = last + 1; k < x; k++) sm[y * GW + k] = 1;
            last = x;
          }
        }
        const sm2 = new Uint8Array(sm);
        for (let x = 0; x < GW; x++) {                             // down
          let last = -1;
          for (let y = 0; y < GH; y++) if (sm[y * GW + x]) {
            if (last >= 0 && y - last - 1 <= vGap) for (let k = last + 1; k < y; k++) sm2[k * GW + x] = 1;
            last = y;
          }
        }
        const lab = new Int32Array(GW * GH), boxes = [];
        for (let i = 0; i < GW * GH; i++) {
          if (!sm2[i] || lab[i]) continue;
          const id = boxes.length + 1, st = [i]; lab[i] = id;
          let bx0 = GW, by0 = GH, bx1 = 0, by1 = 0;
          while (st.length) {
            const q = st.pop(), qx = q % GW, qy = (q / GW) | 0;
            if (qx < bx0) bx0 = qx; if (qx > bx1) bx1 = qx; if (qy < by0) by0 = qy; if (qy > by1) by1 = qy;
            for (const n of [q - 1, q + 1, q - GW, q + GW]) {
              if (n < 0 || n >= GW * GH || lab[n] || !sm2[n]) continue;
              if ((n === q - 1 && qx === 0) || (n === q + 1 && qx === GW - 1)) continue;
              lab[n] = id; st.push(n);
            }
          }
          boxes.push({ x0: bx0 * c, y0: by0 * c, x1: Math.min(W, (bx1 + 1) * c), y1: Math.min(H0, (by1 + 1) * c) });
        }
        // Pictures and buttons: mostly mid-tones, or solid ink.
        const text = boxes.filter(b => {
          let ni = 0, nm = 0, n = (b.x1 - b.x0) * (b.y1 - b.y0);
          for (let y = b.y0; y < b.y1; y++) for (let x = b.x0; x < b.x1; x++) { ni += ink[y * W + x]; nm += mid[y * W + x]; }
          return ni && nm < ni * 1.5 && ni / n < 0.5;
        });
        text.sort((a, b) => (a.y0 - b.y0) || (a.x0 - b.x0));
        lines = [];
        text.forEach((b, k) => lines.push(...linesIn(b.x0, b.x1, b.y0, b.y1, Hm, k + 1).lines));
        if (mode === 3 && lines.length) {
          // The main left edge: where the most lines start.
          // Weighted by how much text starts there, so a column of long
          // lines beats a column of short labels and prices.
          const tol = Math.max(3, Hm * 0.6);
          const wt = l => l.words.reduce((a, w) => a + w.x1 - w.x0, 0) * (l.y1 - l.y0);
          let best = 0, bestN = -1;
          for (const l of lines) {
            if (!l.words.length) continue;
            const s = l.words[0].x0;
            const n = lines.reduce((a, t) => a + (t.words.length && Math.abs(t.words[0].x0 - s) <= tol ? wt(t) : 0), 0);
            if (n > bestN) { bestN = n; best = s; }
          }
          lines = lines.filter(l => l.words.length && Math.abs(l.words[0].x0 - best) <= tol);
        }
      }

      // Headings: taller than body text, or short, bold lines.
      const ds = lines.map(l => l.dens).sort((a, b) => a - b);
      const dm = ds.length ? ds[ds.length >> 1] : 0;
      for (const l of lines)
        l.heading = !l.bullet && (l.tall || (l.words.length <= 4 && l.dens > dm * 1.3));
      return { lines, dark, H: Hm, mode, bg: dark ? 255 - bg : bg };
    }

    // Lines into blocks: headings, items (tick box) and plain text. Wrapped
    // lines are joined to the block above when the spacing says so.
    function blocks(res){
      const L = res.lines, out = [];
      const body = L.filter(l => !l.heading);
      const bulletMode = body.filter(l => l.bullet).length >= 3;
      const gaps = [];
      for (let i = 1; i < L.length; i++)
        if (!L[i].heading && !L[i - 1].heading) gaps.push(L[i].y0 - L[i - 1].y1);
      const gmin = gaps.length ? Math.min(...gaps) : 0;
      const bimodal = gaps.length && Math.max(...gaps) >= gmin * 1.5 + 2;
      for (let i = 0; i < L.length; i++) {
        const l = L[i], prev = out[out.length - 1];
        if (l.heading) { out.push({ kind: 'heading', lines: [l] }); continue; }
        const gap = i ? l.y0 - L[i - 1].y1 : 1e9;
        // A wrapped line starts where the line above started, or at its
        // hanging indent (after a number or bullet).
        const first = prev && prev.lines[0], tol = res.H * 0.6;
        const aligned = first && l.words.length && (
          Math.abs(l.words[0].x0 - first.words[0].x0) <= tol ||
          (first.words[1] && Math.abs(l.words[0].x0 - first.words[1].x0) <= tol));
        const joins = prev && prev.kind !== 'heading' && !l.bullet && aligned &&
          (i === 0 || L[i - 1].blk === l.blk) &&
          (bimodal ? gap < gmin * 1.25 + 1 : (bulletMode && prev.kind === 'item'));
        if (joins) { prev.lines.push(l); continue; }
        out.push({ kind: bulletMode ? (l.bullet ? 'item' : 'text') : 'item', lines: [l] });
      }
      return out;
    }

    // Placement of every word on the paper. Dropped blocks are skipped
    // unless keepDropped (the preview shows them faint).
    // Typed blocks (from Edit) are laid out with the page's own font:
    // opts.wrap(text, maxW) -> lines, opts.font for drawing.
    function layout(res, bl, W, targetH, pad, keepDropped, opts){
      const s = targetH / res.H, gap = Math.round(targetH * 0.35);
      const box = Math.round(targetH * 0.72), indent = pad + box + Math.round(targetH * 0.45);
      const words = [], boxes = [], spans = [], texts = [];
      let y = pad;
      bl.forEach((b, bi) => {
        if (b.dropped && !keepDropped) return;
        const y0 = y;
        if (b.kind === 'typed') {
          const th = Math.round(targetH * 0.8), lh = Math.round(th * 1.3);
          y += Math.round(targetH * 0.2);
          if (!b.text.trim()) { y += lh; spans.push({ bi, y0, y1: y }); return; }   // a gap
          const tick = /^[-*]\s/.test(b.text), t = tick ? b.text.replace(/^[-*]\s+/, '') : b.text;
          const left = tick ? indent : pad;
          if (tick) boxes.push({ x: pad, y: y + Math.round((th - box) / 2), s: box, dropped: b.dropped });
          for (const line of opts.wrap(t, W - pad - left, th)) {
            texts.push({ text: line, x: left, y, size: th, dropped: b.dropped });
            y += lh;
          }
          spans.push({ bi, y0, y1: y });
          return;
        }
        const left = b.kind === 'item' ? indent : pad;
        if (b.kind === 'heading') y += Math.round(targetH * 0.35);
        else y += Math.round(targetH * 0.2);
        const h = Math.round((b.lines[0].y1 - b.lines[0].y0) * s);
        if (b.kind === 'item') boxes.push({ x: pad, y: y + Math.round((h - box) / 2), s: box, dropped: b.dropped });
        let x = left;
        for (const l of b.lines) {
          const lh = Math.round((l.y1 - l.y0) * s);
          for (const w of l.words) {
            let dw = Math.round((w.x1 - w.x0) * s), dh = lh;
            const room = W - pad - left;
            if (dw > room) { dh = Math.round(lh * room / dw); dw = room; }
            if (x > left && x + dw > W - pad) { x = left; y += Math.round(h * 1.15); }
            words.push({ sx: w.x0, sy: l.y0, sw: w.x1 - w.x0, sh: l.y1 - l.y0, dx: x, dy: y, dw, dh, dropped: b.dropped });
            x += dw + gap;
          }
        }
        y += Math.round(h * 1.15);
        spans.push({ bi, y0, y1: y });
      });
      return { words, boxes, spans, texts, height: y + pad };
    }

    // Tapping a heading drops or restores its whole section.
    function toggle(bl, bi){
      const b = bl[bi], to = !b.dropped;
      b.dropped = to;
      if (b.kind === 'heading')
        for (let i = bi + 1; i < bl.length && bl[i].kind !== 'heading'; i++) bl[i].dropped = to;
    }
    return { find, blocks, layout, toggle, MODES };
  })();

  const styleOf = () => el('tstyle').value;

  // The extracted screenshot: its pixels (dark mode already flipped to
  // dark-on-light), the words found in it, and the blocks with any the
  // user has tapped out.
  let fiSrc = null, fiRes = null, fiBlocks = null, fiSpans = [], fiTop = 0;
  let fiEditing = false, fiMode = 1;
  let dateBand = null;                 // set by draw(): rows holding the date
  const today = () => new Date().toLocaleDateString(undefined,
      { weekday: 'long', day: 'numeric', month: 'long' });

  // Words to lines that fit; a word wider than a line is broken by letter.
  function wrap(cx, text, maxW){
    const out = []; let line = '';
    for (const word of text.split(/\s+/).filter(Boolean)) {
      const t = line ? line + ' ' + word : word;
      if (cx.measureText(t).width <= maxW) { line = t; continue; }
      if (line) out.push(line);
      if (cx.measureText(word).width <= maxW) { line = word; continue; }
      let part = '';
      for (const ch of word) {
        if (cx.measureText(part + ch).width > maxW) { out.push(part); part = ch; }
        else part += ch;
      }
      line = part;
    }
    if (line) out.push(line);
    return out.length ? out : [''];
  }

  function heart(cx, x, y, s){           // a small filled heart, centred
    cx.beginPath();
    cx.moveTo(x, y + s * 0.35);
    cx.bezierCurveTo(x - s * 0.9, y - s * 0.25, x - s * 0.35, y - s * 0.85, x, y - s * 0.35);
    cx.bezierCurveTo(x + s * 0.35, y - s * 0.85, x + s * 0.9, y - s * 0.25, x, y + s * 0.35);
    cx.fill();
  }

  // Draws the template; returns the height used. Works on any tall canvas.
  function draw(cx, W, lines, style, base, align){
    cx.fillStyle = '#fff'; cx.fillRect(0, 0, W, cx.canvas.height);
    cx.fillStyle = '#000'; cx.strokeStyle = '#000'; cx.textBaseline = 'top';
    let y = PAD;
    const title = (lines[0] || '').trim();
    const body  = lines.slice(1);
    const lh = Math.round(base * 1.3);
    const xFor = (w, a) => a === 'center' ? (W - w) / 2 : a === 'right' ? W - PAD - w : PAD;
    const text = (s, font, a, x0, maxW) => {
      cx.font = font;
      for (const l of wrap(cx, s, maxW)) {
        const w = cx.measureText(l).width;
        cx.fillText(l, a === 'left' ? x0 : xFor(w, a), y);
        y += lh;
      }
    };
    const hearts = () => {
      const s = Math.round(base * 0.62), n = 5, gap = s * 2.2;
      for (let i = 0; i < n; i++) heart(cx, W / 2 + (i - (n - 1) / 2) * gap, y + s, s);
      y += s * 2.6;
    };

    if (style === 'love') {
      hearts();
      if (title) text(title, 'italic bold ' + Math.round(base * 1.35) + 'px ' + SERIF,
                      'center', PAD, W - 2 * PAD);
      y += Math.round(base * 0.4);
      for (const raw of body) {
        const s = raw.trim();
        if (!s) { y += Math.round(lh / 2); continue; }
        text(s, 'italic ' + base + 'px ' + SERIF, 'center', PAD, W - 2 * PAD);
      }
      y += Math.round(base * 0.3);
      hearts();
      return Math.ceil(y + PAD);
    }

    // List and Note share a heading: title, date, rule.
    if (title) text(title, 'bold ' + Math.round(base * 1.25) + 'px ' + SANS,
                    'center', PAD, W - 2 * PAD);
    cx.font = Math.round(base * 0.6) + 'px ' + SANS;
    const d = today(), dw = cx.measureText(d).width, dy = y;
    cx.fillText(d, (W - dw) / 2, y); y += Math.round(base * 0.9);
    // Where the date sits, so a scheduled print can have the printer write
    // the day it actually prints instead (see WebUIFuture.h).
    if (!dateBand) dateBand = { y0: dy, y1: y, base };
    cx.fillRect(PAD, y, W - 2 * PAD, 3); y += Math.round(base * 0.6);

    if (style === 'note') {
      for (const raw of body) {
        const s = raw.trim();
        if (!s) { y += Math.round(lh / 2); continue; }
        const sub = s.endsWith(':');
        text(s, (sub ? 'bold ' : '') + base + 'px ' + SANS, align, PAD, W - 2 * PAD);
      }
      return Math.ceil(y + PAD);
    }

    // List: a box per item, hanging indent for wrapped lines.
    const box = Math.round(base * 0.72), gap = Math.round(base * 0.45);
    const tx = PAD + box + gap, tw = W - tx - PAD;
    for (const raw of body) {
      let s = raw.trim();
      if (!s) { y += Math.round(lh / 2); continue; }
      if (s.endsWith(':')) {                              // subheading
        y += Math.round(lh * 0.25);
        text(s, 'bold ' + base + 'px ' + SANS, 'left', PAD, W - 2 * PAD);
        continue;
      }
      s = s.replace(/^(\[\s?\]|[-*\u2022]|\d+[.)])\s*/, '');
      cx.lineWidth = 3;
      cx.strokeRect(PAD + 1.5, y + (base - box) / 2 + 1.5, box - 3, box - 3);
      text(s, base + 'px ' + SANS, 'left', tx, tw);
      y += Math.round(lh * 0.15);
    }
    return Math.ceil(y + PAD);
  }

  // From image: anything typed prints above as a Note-style heading, then
  // the extracted words. forPreview keeps tapped-out lines (drawn faint by
  // preview()) so their place stays put while choosing.
  function drawImageStyle(cx, W, base, forPreview){
    const typed = el('text').value;
    let y = 0;
    if (typed.trim()) y = draw(cx, W, typed.split('\n'), 'note', base, el('align').value) - PAD;
    if (!fiRes) return { h: y + PAD, L: null, top: y };
    const L = FI.layout(fiRes, fiBlocks, W, Math.round(base * 1.05), PAD, forPreview, {
      wrap: (t, maxW, size) => { cx.font = size + 'px ' + SANS; return wrap(cx, t, maxW); }
    });
    cx.imageSmoothingQuality = 'high';
    for (const p of L.words)
      cx.drawImage(fiSrc, p.sx, p.sy, p.sw, p.sh, p.dx, p.dy + y, p.dw, p.dh);
    cx.fillStyle = '#000'; cx.textBaseline = 'top';
    for (const t of L.texts) { cx.font = t.size + 'px ' + SANS; cx.fillText(t.text, t.x, t.y + y); }
    cx.lineWidth = 3; cx.strokeStyle = '#000';
    for (const b of L.boxes) cx.strokeRect(b.x + 1.5, b.y + y + 1.5, b.s - 3, b.s - 3);
    return { h: Math.min(cx.canvas.height, y + L.height), L, top: y };
  }

  // Render to 1 bit. Returns { canvas, rows, width, data } or null.
  function render(forPreview){
    dateBand = null;
    const raw = el('text').value, img = styleOf() === 'image', fine = styleOf() === 'fine';
    if (fine && !window.PF) return null;
    if (!fine && !raw.trim() && !(img && fiRes)) return null;
    const W = window.PRINTER.printableDots;
    const tall = document.createElement('canvas');
    tall.width = W; tall.height = img ? 16000 : 6000;
    const cx = tall.getContext('2d', { willReadFrequently: true });
    const base = BASE[el('size').value] || 32;
    let H;
    if (fine) {
      cx.fillStyle = '#fff'; cx.fillRect(0, 0, W, tall.height);
      H = Math.min(6000, window.PF.draw(cx, W, window.PF.values(), null));
    } else if (img) {
      cx.fillStyle = '#fff'; cx.fillRect(0, 0, W, tall.height);
      const r = drawImageStyle(cx, W, base, forPreview);
      H = r.h;
      if (forPreview && r.L) { fiSpans = r.L.spans; fiTop = r.top; }
    } else {
      H = Math.min(6000, draw(cx, W, raw.split('\n'), styleOf(), base, el('align').value));
    }

    const px = cx.getImageData(0, 0, W, H).data;
    const rb = W / 8, data = new Uint8Array(rb * H);
    for (let y = 0; y < H; y++)
      for (let x = 0; x < W; x++) {
        const o = (y * W + x) * 4;
        const v = 0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2];
        // Screenshot words: dark if clearly off the page background, which
        // keeps thin strokes and light-grey or coloured text.
        if (v < (img && fiRes ? fiRes.bg - 60 : 160)) data[y * rb + (x >> 3)] |= 0x80 >> (x & 7);
      }
    return { canvas: tall, rows: H, width: W, data };
  }

  function preview(){
    const on = styleOf() !== 'plain';
    el('tedit').style.display = styleOf() === 'image' && fiRes ? '' : 'none';
    el('tedit').textContent = fiEditing ? 'Done' : 'Edit';
    el('tpv').style.display = on ? 'none' : '';
    el('tcv').style.display = on ? 'block' : 'none';
    const ww = el('wwrap'); if (ww) ww.parentElement.style.visibility = on ? 'hidden' : '';
    // A theme is filled in from its form: no text box, no size or align.
    const fine = styleOf() === 'fine';
    el('text').style.display = fine ? 'none' : '';
    ['align', 'size'].forEach(id => el(id).parentElement.style.display = fine ? 'none' : '');
    if (window.PF) window.PF.show(fine);
    el('print').textContent = on ? 'Print ' + el('tstyle').selectedOptions[0].text.toLowerCase()
                                 : 'Print text';
    el('thint').textContent = !on ? '' :
      styleOf() === 'list'
        ? 'First line is the title. Each line after it gets a tick box; end a line with : for a heading.'
      : fine
        ? 'Open a section below to change its wording. The disclaimer always prints.'
      : styleOf() === 'image'
        ? (!fiRes ? 'Paste a screenshot in the photo card and tap Extract text.'
           : fiEditing ? 'Editing: tap a line to add your own text above it (leave it empty for a gap). Start with "- " for a tick box. Tap added text to change or remove it.'
           : 'Tap a line to leave it out; tap a heading to drop its whole section. Anything typed above prints as a title.')
        : 'First line is the title. Blank lines add space.';
    if (!on) return;
    const r = render(styleOf() === 'image'), c = el('tcv');
    if (!r) { c.width = 1; c.height = 1; el('tinfo').textContent = ''; return; }
    c.width = r.width; c.height = r.rows;
    const cx = c.getContext('2d');
    const img = cx.createImageData(r.width, r.rows), rb = r.width / 8;
    for (let y = 0; y < r.rows; y++)
      for (let x = 0; x < r.width; x++) {
        const on1 = r.data[y * rb + (x >> 3)] & (0x80 >> (x & 7)), o = (y * r.width + x) * 4;
        img.data[o] = img.data[o + 1] = img.data[o + 2] = on1 ? 0 : 255; img.data[o + 3] = 255;
      }
    cx.putImageData(img, 0, 0);
    let mm = r.rows / 8;
    if (styleOf() === 'image' && fiBlocks) {
      // Tapped-out lines: faded and struck through, and left off the paper.
      let gone = 0;
      for (const s of fiSpans) {
        if (!fiBlocks[s.bi].dropped) continue;
        gone += s.y1 - s.y0;
        cx.fillStyle = 'rgba(255,255,255,0.78)';
        cx.fillRect(0, fiTop + s.y0, r.width, s.y1 - s.y0);
        cx.fillStyle = '#c0392b';
        cx.fillRect(PAD, fiTop + Math.round((s.y0 + s.y1) / 2), r.width - 2 * PAD, 3);
      }
      mm = (r.rows - gone) / 8;
    }
    el('tinfo').textContent = mm.toFixed(0) + ' mm long';
  }

  // Called by the photo card's Extract text with a full-size canvas.
  window.fromImage = {
    set(canvas, mode){
      fiMode = mode || 1;
      const cx = canvas.getContext('2d', { willReadFrequently: true });
      const img = cx.getImageData(0, 0, canvas.width, canvas.height);
      const res = FI.find(img, mode);
      if (!res.lines.length) return 0;
      if (res.dark) {                          // copy dark-on-light words
        const d = img.data;
        for (let i = 0; i < d.length; i += 4) { d[i] = 255 - d[i]; d[i + 1] = 255 - d[i + 1]; d[i + 2] = 255 - d[i + 2]; }
        cx.putImageData(img, 0, 0);
      }
      fiSrc = canvas; fiRes = res; fiBlocks = FI.blocks(res); fiEditing = false;
      const st = el('tstyle');
      if (![...st.options].some(o => o.value === 'image')) {
        const o = document.createElement('option'); o.value = 'image'; o.textContent = 'From image';
        st.appendChild(o);
      }
      st.value = 'image';
      preview();
      return res.lines.length;
    },
    // What a draft or saved item keeps: the way used, which found lines
    // were tapped out, and the user's own added lines (by position among
    // the found ones). The picture itself is stored separately.
    has: () => !!fiRes,
    // The whole From-image state, taken and put back around a quick print.
    snap: () => ({ fiSrc, fiRes, fiBlocks: fiBlocks && fiBlocks.map(b => Object.assign({}, b)), fiMode, fiEditing }),
    unsnap(x){ ({ fiSrc, fiRes, fiBlocks, fiMode, fiEditing } = x); },
    canvas: () => fiSrc,
    state(){
      if (!fiBlocks) return null;
      const dropped = [], typed = [];
      let n = 0;
      for (const b of fiBlocks) {
        if (b.kind === 'typed') typed.push({ at: n, text: b.text });
        else { if (b.dropped) dropped.push(n); n++; }
      }
      return { mode: fiMode, found: n, dropped, typed };
    },
    // After set() on the stored picture. False if the picture no longer
    // reads the same way (then only the words are restored, not the edits).
    restore(st){
      if (!fiBlocks || !st) return false;
      const found = fiBlocks.filter(b => b.kind !== 'typed');
      if (found.length !== st.found) { preview(); return false; }
      for (const i of st.dropped || []) if (found[i]) found[i].dropped = true;
      const out = [];
      let n = 0;
      const typedAt = k => (st.typed || []).filter(t => t.at === k)
        .forEach(t => out.push({ kind: 'typed', text: t.text, lines: [] }));
      for (const b of found) { typedAt(n); out.push(b); n++; }
      typedAt(n);
      fiBlocks.length = 0; out.forEach(b => fiBlocks.push(b));
      preview();
      return true;
    }
  };

  // For drafts, saved and scheduled prints (WebUIFuture.h).
  window.TPL = { render, parts, preview, sendRaster, band: () => dateBand };

  // Split at white rows into parts the unit can hold one at a time.
  function parts(r){
    // v0.24.1: back to fixed 48 KB parts at home (the 0.23.1 cap at the
    // unit's free memory made more, shorter parts). Online stays at 24 KB.
    const cap = window.REMOTE ? 24 * 1024 : MAX_PART_BYTES;
    const rb = r.width / 8, maxRows = Math.max(32, Math.floor(cap / rb)), out = [];
    const white = y => { for (let i = y * rb; i < (y + 1) * rb; i++) if (r.data[i]) return false; return true; };
    let start = 0;
    while (start < r.rows) {
      let end = Math.min(r.rows, start + maxRows);
      if (end < r.rows) {
        let y = end; while (y > start + 32 && !white(y)) y--;
        end = y > start + 32 ? y : end;                   // no white row: cut anyway
      }
      out.push(r.data.slice(start * rb, end * rb));
      start = end;
    }
    return out;
  }

  async function waitDone(id){
    for (let i = 0; i < 400; i++) {
      await new Promise(res => setTimeout(res, window.REMOTE ? 2000 : 1000));
      try {
        const j = await window.api('/api/jobs?limit=25');
        const x = (j.jobs || []).find(q => q.id === id);
        if (x && (x.status === 'done' || x.status === 'failed')) return x.status;
      } catch (e) {}
    }
    return 'failed';
  }

  async function printTemplate(){
    const r = render(false);
    if (!r) { window.toast('nothing to print', true); return; }
    await sendRaster(r, el('cut').value === '1', el('print'));
  }

  // Sends a rendered slip in parts, each after the last has printed.
  async function sendRaster(r, cut, btn){
    const list = parts(r);
    if (btn) btn.disabled = true;
    try {
      for (let i = 0; i < list.length; i++) {
        const d = list[i], rows = d.length / (r.width / 8);
        let set = 0; for (const b of d) { let v = b; while (v) { set += v & 1; v >>= 1; } }
        const cov = Math.round(set * 100 / (d.length * 8));
        const fd = new FormData();
        fd.append('data', new Blob([d], { type: 'application/octet-stream' }), 'raster.bin');
        const last = i === list.length - 1;
        const url = '/api/print/raster?width=' + r.width + '&rows=' + rows
          + '&coverage=' + cov + '&tail=0&cut=' + (last && cut ? 1 : 0);
        let res, split = false;
        for (let a = 0; ; a++) {          // memory frees as the printer works
          try { res = await window.api(url, { method: 'POST', body: fd }); break; }
          catch (e) {
            const oom = /out of memory/i.test(e.message || '');
            if (!oom && !/queue full/i.test(e.message || '') || a >= 20) throw e;
            // Still no room after three tries: this part goes as two halves.
            if (oom && a >= 2 && rows >= 64) {
              const rb = r.width / 8, mid = Math.floor(rows / 2) * rb;
              list.splice(i, 1, d.slice(0, mid), d.slice(mid));
              split = true; break;
            }
            await new Promise(ok => setTimeout(ok, 3000));
          }
        }
        if (split) { i--; continue; }
        window.toast(list.length > 1 ? 'part ' + (i + 1) + ' of ' + list.length + ' queued'
                                     : 'queued as job #' + res.jobId);
        if (!last && (await waitDone(res.jobId)) !== 'done')
          throw new Error('part ' + (i + 1) + ' failed - stopped');
        if (last) window.confirmPrinted(res.jobId);
      }
      if (typeof loadJobs === 'function') loadJobs();
    } catch (e) { window.toast(e.message, true); }
    if (btn) btn.disabled = false;
  }

  window.tplPreview = preview;          // the theme form asks for redraws

  document.addEventListener('DOMContentLoaded', () => {
    // Runs after the main page's handler, so Plain keeps its own printing.
    const plainPrint = el('print').onclick;
    el('print').onclick = () => styleOf() === 'plain' ? plainPrint() : printTemplate();
    let t = null;
    const later = () => { clearTimeout(t); t = setTimeout(preview, 120); };
    el('text').addEventListener('input', later);
    ['tstyle', 'size', 'align'].forEach(id => el(id).addEventListener('change', preview));
    el('tedit').onclick = () => { fiEditing = !fiEditing; preview(); };
    el('tcv').addEventListener('click', e => {
      if (styleOf() !== 'image' || !fiBlocks) return;
      const c = el('tcv'), y = (e.offsetY * c.height / c.clientHeight) - fiTop;
      const s = fiSpans.find(s => y >= s.y0 && y < s.y1);
      if (!s) return;
      const b = fiBlocks[s.bi];
      if (fiEditing) {
        if (b.kind === 'typed') {
          const t = prompt('Change this text (empty removes it):', b.text);
          if (t === null) return;
          if (t.trim()) b.text = t; else fiBlocks.splice(s.bi, 1);
        } else {
          const t = prompt('Text to add above this line.\nLeave empty for a gap. Start with "- " for a tick box.', '');
          if (t === null) return;
          fiBlocks.splice(s.bi, 0, { kind: 'typed', text: t, lines: [] });
        }
      } else {
        FI.toggle(fiBlocks, s.bi);
      }
      preview();
    });
    preview();
  });
})();
</script>

<!--home-only-->
<script>
(() => {
  const el = id => document.getElementById(id);
  const THEMES = { love: 'Love note', classic: 'Classic', bold: 'Big & bold', type: 'Typewriter' };
  const DECOS = { none: 'None', hearts: 'Hearts', stars: 'Stars', sparkles: 'Sparkles', dots: 'Dots',
    flowers: 'Flowers', balloons: 'Balloons', confetti: 'Confetti', waves: 'Waves', zigzag: 'Zigzag',
    double: 'Double line', frame: 'Double frame', dotframe: 'Dotted frame' };
  const ticks = (box, all, on) => {
    box.textContent = '';
    for (const k of Object.keys(all)) {
      const l = document.createElement('label');
      l.style.cssText = 'flex:0 0 auto;display:flex;gap:6px;align-items:center;text-transform:none;letter-spacing:0;margin:2px 10px 2px 0;color:var(--ink)';
      const c = document.createElement('input'); c.type = 'checkbox'; c.value = k; c.style.width = 'auto';
      c.checked = !on || on.includes(k);
      l.appendChild(c); l.appendChild(document.createTextNode(all[k])); box.appendChild(l);
    }
  };
  const picked = box => [...box.querySelectorAll('input:checked')].map(c => c.value);
  async function load(){
    try {
      const c = await (await fetch('/api/kiosk', { cache: 'no-store' })).json();
      el('kTitle').value = c.title || ''; el('kSub').value = c.subtitle || '';
      el('kPrint').value = c.printTitle || ''; el('kFoot').value = c.footer || '';
      el('kDate').checked = !!c.date;
      ticks(el('kThemes'), THEMES, c.themes); ticks(el('kDecos'), DECOS, c.decos);
    } catch (e) {}
  }
  async function save(){
    const themes = picked(el('kThemes')), decos = picked(el('kDecos'));
    const body = { title: el('kTitle').value, subtitle: el('kSub').value, printTitle: el('kPrint').value,
      footer: el('kFoot').value, date: el('kDate').checked,
      themes: themes.length ? themes : ['classic'], decos: decos.length ? decos : ['none'],
      defTheme: themes[0] || 'classic', defDeco: decos.find(d => d !== 'none') || decos[0] || 'none' };
    const r = await fetch('/api/kiosk', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                          body: JSON.stringify(body) });
    if (window.toast) window.toast(r.ok ? 'kiosk saved' : 'could not save', !r.ok);
    return r.ok;
  }
  document.addEventListener('DOMContentLoaded', () => {
    if (!el('kSave')) return;
    load();
    el('kSave').onclick = save;
    el('kStart').onclick = async () => {
      if (!(await save())) return;
      try { localStorage.setItem('kiosk', '1'); } catch (e) {}
      location.href = '/kiosk';
    };
  });
})();
</script>
<!--/home-only-->
)HTML";

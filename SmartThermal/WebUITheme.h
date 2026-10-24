// WebUITheme.h - themes: ready-made layouts filled in from a short form.
//
// First theme: Parking fine, a joke infringement notice (from the owner's
// "Bunny Parking Patrol" example). Chosen in the Text card's Style menu;
// the text box is swapped for a form of collapsible sections, one per
// part of the notice, prefilled. "Save as my defaults" keeps the wording
// on the unit (GET/POST /api/theme?name=fine, NVS namespace "themes") so
// every phone sees it. Drawing, preview, 1-bit conversion, splitting into
// parts and printing are the Style menu's own (WebUITemplate.h).
//
// The disclaimer always prints: its wording can change, but if a line is
// left empty the default wording is used.

#pragma once
#include <Arduino.h>

static const char WEB_UI_THEME[] PROGMEM = R"HTML(
<script>
window.PF = (() => {
  const SANS = 'Helvetica, Arial, sans-serif';
  const MONO = '"Courier New", Courier, "Liberation Mono", monospace';
  const DEFAULTS = {
    issuer: 'BUNNY PARKING PATROL', dept: 'DEPARTMENT OF PETTY OFFENCES',
    title: 'PARKING INFRINGEMENT', subtitle: "You've been dobbed in.",
    caption: 'OFFICER FLOPSY / BADGE BUN-01', tagline: 'Hopping mad since 2019.',
    notice: 'BPP-000417', status: 'UNPAID',
    plate: 'CUD 4ME',
    vehicle: 'Little hatchback', colour: 'Suspiciously shiny', location: 'Out the front', suburb: 'Burrowville',
    offence: 'Parked with total confidence in a spot reserved for someone far more important, namely the officer.',
    offenceBold: 'NO EXCUSES ACCEPTED.',
    amount: '$250', amountNote: 'or the settlement below',
    settle: 'ONE COFFEE + ONE SLICE OF CAKE',
    settleText: 'Delivered in person, with a smile, to the issuing officer.',
    due: 'This weekend', lateFee: 'An extra biscuit',
    cheeky: 'You have the right to remain cheeky.',
    fine: 'This notice is issued under the Carrot Act, section 3(b).\nCake must be of a reasonable size. Crumbs are not cake.\nObjections may be lodged with the officer, who will ignore them.',
    disc1: 'ALL FLUFF. NO LEGAL AUTHORITY.', disc2: 'THIS IS A JOKE NOTICE. NO PAYMENT REQUIRED.',
    signoff: 'With love, the Bunny Parking Patrol'
  };

  function wrap(cx, s, maxW){
    const out = [];
    for (const para of String(s).split('\n')) {
      let line = '';
      for (const w of para.split(/\s+/).filter(Boolean)) {
        const t = line ? line + ' ' + w : w;
        if (cx.measureText(t).width <= maxW || !line) line = t; else { out.push(line); line = w; }
      }
      out.push(line);
    }
    return out;
  }

  // Draws onto cx (caller has painted it white); returns the height used.
  // pic: an image or canvas for the illustration slot, or null for none.
  function draw(cx, W, f, pic){
    const P = 22, IW = W - 2 * P;
    let y = P;
    cx.fillStyle = '#000'; cx.strokeStyle = '#000'; cx.textBaseline = 'top';
    const font = (px, w, fam) => (cx.font = (w || '') + ' ' + px + 'px ' + (fam || SANS));
    const lines = (s, px, w, fam, align, x0, maxW, lh) => {
      font(px, w, fam); x0 = x0 ?? P; maxW = maxW ?? IW; lh = lh ?? Math.round(px * 1.25);
      for (const l of wrap(cx, s, maxW)) {
        const tw = cx.measureText(l).width;
        cx.fillText(l, align === 'center' ? x0 + (maxW - tw) / 2 : align === 'right' ? x0 + maxW - tw : x0, y);
        y += lh;
      }
    };
    const fit = (s, px, w, fam, maxW) => {       // one line, shrunk to fit
      maxW = maxW ?? IW;
      font(px, w, fam); while (px > 14 && cx.measureText(s).width > maxW) font(--px, w, fam);
      cx.fillText(s, P + (IW - cx.measureText(s).width) / 2, y); y += Math.round(px * 1.15);
    };
    const dashed = (dash) => {
      cx.save(); cx.lineWidth = 3; cx.setLineDash(dash || [12, 8]);
      cx.beginPath(); cx.moveTo(P, y + 1.5); cx.lineTo(W - P, y + 1.5); cx.stroke(); cx.restore();
      y += 3;
    };
    const bar = (s) => {                         // white text on a black band
      y += 14; const h = 34; cx.fillRect(P, y, IW, h);
      cx.fillStyle = '#fff'; font(22, 'bold');
      cx.fillText(s, P + 12, y + 7); cx.fillStyle = '#000'; y += h + 12;
    };
    const field = (label, value) => {
      font(22); cx.fillText(label, P, y + 3);
      const lw = cx.measureText(label).width + 16;
      font(26, 'bold'); const vs = wrap(cx, value, IW - lw);
      for (const v of vs) { cx.fillText(v, W - P - cx.measureText(v).width, y); y += 32; }
      y += 4; cx.fillRect(P, y, IW, 1); y += 9;
    };
    const rrect = (x, yy, w, h, r) => {
      cx.beginPath(); cx.moveTo(x + r, yy); cx.arcTo(x + w, yy, x + w, yy + h, r);
      cx.arcTo(x + w, yy + h, x, yy + h, r); cx.arcTo(x, yy + h, x, yy, r); cx.arcTo(x, yy, x + w, yy, r); cx.closePath();
    };
    const has = s => s && String(s).trim();

    // 1 Issuer
    if (has(f.issuer)) fit(f.issuer, 38, 'bold');
    if (has(f.dept)) { y += 2; fit(f.dept, 20, 'bold'); }
    y += 8; dashed(); y += 18;
    // 2 Title
    if (has(f.title)) fit(f.title, 60, 'bold');
    if (has(f.subtitle)) { y += 4; lines(f.subtitle, 26, 'italic', SANS, 'center'); }
    y += 14;
    // 3 Illustration
    const ph = 300;
    if (pic) {
      const s = Math.min(IW / pic.width, ph / pic.height), w = pic.width * s, h = pic.height * s;
      cx.drawImage(pic, (W - w) / 2, y, w, h); y += h + 10;
    }
    if (has(f.caption)) fit(f.caption, 22, 'bold');
    if (has(f.tagline)) lines(f.tagline, 20, 'italic', SANS, 'center');
    y += 14; dashed([4, 6]); y += 14;
    // 4 Notice | Status
    const half = IW / 2, top = y;
    font(18, 'bold'); cx.fillText('NOTICE', P, y); cx.fillText('STATUS', P + half + 14, y);
    y += 24; font(28, 'bold');
    cx.fillText(f.notice || '', P, y); cx.fillText(f.status || '', P + half + 14, y);
    cx.fillRect(P + half, top, 2, 58); y += 46;
    // 5 Plate
    y += 6; const plH = 110; cx.lineWidth = 6; rrect(P + 40, y, IW - 80, plH, 22); cx.stroke();
    cx.lineWidth = 2; rrect(P + 50, y + 10, IW - 100, plH - 20, 14); cx.stroke();
    { let px = 76; font(px, 'bold', MONO); const s = (f.plate || '').toUpperCase();
      while (px > 20 && cx.measureText(s).width > IW - 140) font(--px, 'bold', MONO);
      cx.fillText(s, (W - cx.measureText(s).width) / 2, y + (plH - px) / 2 + 2); }
    y += plH + 22;
    // 6 Vehicle fields
    for (const [k, l] of [['vehicle', 'VEHICLE'], ['colour', 'COLOUR'], ['location', 'LOCATION'], ['suburb', 'SUBURB']])
      if (has(f[k])) field(l, f[k]);
    // 7 Offence
    bar('ALLEGED OFFENCE');
    if (has(f.offence)) lines(f.offence, 24);
    if (has(f.offenceBold)) { y += 4; lines(f.offenceBold, 26, 'bold'); }
    // 8 Penalty
    y += 18; const boxTop = y; y += 14;
    font(18, 'bold'); { const t = 'TOTAL PENALTY'; cx.fillText(t, (W - cx.measureText(t).width) / 2, y); } y += 26;
    fit(f.amount || '', 84, 'bold'); y += 2;
    if (has(f.amountNote)) lines(f.amountNote, 20, '', SANS, 'center');
    y += 10; cx.lineWidth = 5; cx.strokeRect(P + 2.5, boxTop, IW - 5, y - boxTop); y += 6;
    // 9 Settlement
    bar('ALTERNATIVE SETTLEMENT');
    if (has(f.settle)) lines(f.settle, 28, 'bold', SANS, 'center');
    if (has(f.settleText)) { y += 4; lines(f.settleText, 22, '', SANS, 'center'); }
    y += 10;
    if (has(f.due)) field('DUE', f.due);
    if (has(f.lateFee)) field('LATE FEE', f.lateFee);
    // 10 Cheeky line
    if (has(f.cheeky)) { y += 12; lines(f.cheeky, 28, 'bold italic', SANS, 'center'); }
    // 11 Fine print
    if (has(f.fine)) {
      bar('THE VERY, VERY FINE PRINT');
      const items = String(f.fine).split('\n').map(s => s.trim()).filter(Boolean);
      items.forEach((s, i) => {
        const n = (i + 1) + '. '; font(19, 'bold', MONO); const nw = cx.measureText(n).width;
        cx.fillText(n, P, y);
        lines(s, 19, 'bold', MONO, 'left', P + nw, IW - nw, 24); y += 6;
      });
    }
    // 12 Disclaimer - always printed
    y += 14; dashed(); y += 16;
    const d1 = has(f.disc1) ? f.disc1 : DEFAULTS.disc1, d2 = has(f.disc2) ? f.disc2 : DEFAULTS.disc2;
    lines(d1, 28, 'bold', SANS, 'center');
    lines(d2, 20, 'bold', SANS, 'center');
    if (has(f.signoff)) { y += 6; lines(f.signoff, 20, 'italic', SANS, 'center'); }
    return Math.ceil(y + P);
  }

  // ---- The form: one collapsible section per part, prefilled -----------
  const FIELDS = [
    ['Issuer',       [['issuer', 'Name'], ['dept', 'Line under it']]],
    ['Title',        [['title', 'Title'], ['subtitle', 'Subtitle']]],
    ['Picture',      [['caption', 'Caption'], ['tagline', 'Tagline']]],
    ['Notice',       [['notice', 'Notice number'], ['status', 'Status']]],
    ['Number plate', [['plate', 'Plate']]],
    ['Vehicle',      [['vehicle', 'Vehicle'], ['colour', 'Colour'], ['location', 'Location'], ['suburb', 'Suburb']]],
    ['Offence',      [['offence', 'What happened', 3], ['offenceBold', 'Bold line']]],
    ['Penalty',      [['amount', 'Amount'], ['amountNote', 'Line under it']]],
    ['Settlement',   [['settle', 'Bold line'], ['settleText', 'Details', 2], ['due', 'Due'], ['lateFee', 'Late fee']]],
    ['Cheeky line',  [['cheeky', 'Line']]],
    ['Fine print',   [['fine', 'One point per line, numbered for you', 4]]],
    ['Disclaimer (always printed)', [['disc1', 'Big line'], ['disc2', 'Second line'], ['signoff', 'Sign-off']]]
  ];
  let saved = Object.assign({}, DEFAULTS), cur = Object.assign({}, DEFAULTS);
  const el = id => document.getElementById(id);
  const changed = () => { clearTimeout(changed.t); changed.t = setTimeout(() => window.tplPreview && window.tplPreview(), 150); };

  function fill(){
    for (const [, fs] of FIELDS) for (const [k] of fs) { const i = el('pf_' + k); if (i) i.value = cur[k] || ''; }
    changed();
  }

  function build(){
    const box = document.createElement('div');
    box.id = 'tform'; box.style.cssText = 'display:none;margin-top:10px';
    for (const [title, fs] of FIELDS) {
      const d = document.createElement('details');
      d.style.cssText = 'border-top:1px solid var(--line);padding:8px 0';
      const s = document.createElement('summary'); s.textContent = title; d.appendChild(s);
      for (const [k, label, rows] of fs) {
        const l = document.createElement('label'); l.textContent = label; l.htmlFor = 'pf_' + k;
        const i = document.createElement(rows ? 'textarea' : 'input');
        i.id = 'pf_' + k; if (rows) { i.rows = rows; i.style.minHeight = '0'; }
        i.addEventListener('input', () => { cur[k] = i.value; changed(); });
        d.appendChild(l); d.appendChild(i);
      }
      box.appendChild(d);
    }
    const row = document.createElement('div'); row.className = 'row';
    row.style.cssText = 'margin-top:8px;border-top:1px solid var(--line);padding-top:10px';
    const b1 = document.createElement('button'); b1.className = 'ghost'; b1.textContent = 'Save as my defaults';
    const b2 = document.createElement('button'); b2.className = 'ghost'; b2.textContent = 'Undo my changes';
    b1.onclick = async () => {
      try {
        const r = await fetch('/api/theme?name=fine', { method: 'POST',
          headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(cur) });
        if (!r.ok) throw 0;
        saved = Object.assign({}, cur);
        window.toast && window.toast('saved on the printer');
      } catch (e) { window.toast && window.toast('could not save', true); }
    };
    b2.onclick = () => { cur = Object.assign({}, saved); fill(); };
    row.appendChild(b1); row.appendChild(b2); box.appendChild(row);
    const hint = el('thint').parentElement;              // just below the preview
    hint.parentElement.insertBefore(box, hint.nextSibling);
    fill();
  }

  async function load(){
    try {
      const j = await (await fetch('/api/theme?name=fine', { cache: 'no-store' })).json();
      for (const k of Object.keys(DEFAULTS)) if (typeof j[k] === 'string') saved[k] = j[k];
      cur = Object.assign({}, saved); fill();
    } catch (e) {}
  }

  document.addEventListener('DOMContentLoaded', () => { build(); load(); });

  return { DEFAULTS, draw,
           values: () => cur,
           set: v => { cur = Object.assign({}, DEFAULTS, v || {}); fill(); },
           show: on => { const f = el('tform'); if (f) f.style.display = on ? '' : 'none'; } };
})();
</script>
)HTML";

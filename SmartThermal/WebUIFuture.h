// WebUIFuture.h - drafts, saved prints and scheduled prints on the page
// (v0.19). The lists themselves live on the unit (WebApiItems.cpp).
//
//   Future box (top right)  drafts count (red when any) and scheduled count;
//                           opens the Future view (#future)
//   Saved drawer (bottom)   "Saved (4/20)", tap to open; tap an item to load
//                           it into the Text card, or Print to print it now
//   Text card               Save, Keep as draft, Schedule...
//
// An item keeps what the Text card shows (style, text, sizes, form wording,
// From-image edits) - never a drawing - so reloading it gives an editable
// card. A scheduled print also keeps the finished slip (drawn here, now),
// because no phone is open when it fires; its date line is left for the
// printer to write on the day.
//
// Drafts are shared, living lists. Opening one links it to the Text card,
// with a bar saying so. Since v0.23 (owner's choice) changes are saved only
// with "Update draft" - nothing saves by itself; the bar says "unsaved
// changes" until then, and leaving asks first. "Close" unlinks the card.
// Each visit that updates a draft counts as one edit (the tiny pencil
// number). The latest update wins; whoever was working on an older copy is
// told, and can load the other person's version.
// Print on a saved item or a draft prints it without touching the card.

#pragma once
#include <Arduino.h>

static const char WEB_UI_FUTURE[] PROGMEM = R"HTML(
<script>
(() => {
  const el = id => document.getElementById(id);
  const toast = (m, bad) => window.toast && window.toast(m, bad);
  const api = (p, o) => window.api(p, o);
  const post = (p, body) => api(p, { method: 'POST',
    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  const NAME = { d: 'Drafts', s: 'Saved', p: 'Scheduled' };
  const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
  const MAX_PIC = 350 * 1024;
  let counts = { d: 0, s: 0, p: 0, limit: { d: 10, s: 20, p: 8 }, pic: '' };
  let lists = { d: [], s: [], p: [] };
  let link = null;           // the draft open in the Text card: {id, rev, edits, sess}
  let quiet = false;         // true while the page itself changes the card
  let saveT = null, saving = false, dirty = false;
  const newSess = () => Math.random().toString(36).slice(2, 10);

  const when = t => {
    if (!t) return '';
    const d = new Date(t * 1000);
    return d.toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short' })
      + ', ' + d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
  };
  const styleName = v => { const o = el('tstyle').querySelector('option[value="' + v + '"]');
                           return o ? o.textContent : (v === 'image' ? 'From image' : v); };

  // ---- The Text card, captured and restored ------------------------------
  function capture(){
    const style = el('tstyle').value, text = el('text').value;
    if (style === 'fine' && window.PF) return { style, fine: Object.assign({}, window.PF.values()), cut: el('cut').value };
    if (style === 'image') {
      const img = window.fromImage && window.fromImage.state();
      if (!img) return null;
      return { style, text, size: el('size').value, align: el('align').value, cut: el('cut').value, img };
    }
    if (!text.trim()) return null;
    return { style, text, size: el('size').value, align: el('align').value,
             cut: el('cut').value, wwrap: el('wwrap').checked };
  }
  function titleOf(src){
    if (src.style === 'fine') return [src.fine.title, src.fine.plate].filter(Boolean).join(' \u00b7 ') || 'Parking fine';
    const first = (src.text || '').split('\n').map(s => s.trim()).find(Boolean);
    return (first || (src.style === 'image' ? 'From image' : 'Untitled')).slice(0, 60);
  }
  function cardBusy(){
    const st = el('tstyle').value;
    return !!el('text').value.trim() || (st === 'image' && window.fromImage && window.fromImage.has());
  }
  async function pictureCanvas(){
    const r = await fetch('/api/items/pic', { cache: 'no-store' });
    if (!r.ok) throw new Error('the stored picture is missing');
    const bmp = await createImageBitmap(await r.blob());
    const c = document.createElement('canvas'); c.width = bmp.width; c.height = bmp.height;
    c.getContext('2d').drawImage(bmp, 0, 0);
    return c;
  }
  // Loads an item into the Text card. Asks first if that would replace
  // something the user has typed.
  async function apply(it, askFirst){
    const s = it.src;
    if (!s) { toast('this one can\u2019t be opened - it was made from a picture', true); return false; }
    if (askFirst && cardBusy() && !confirm('Replace what\u2019s in the Text card?')) return false;
    quiet = true;
    try { return await fillCard(s); } finally { quiet = false; }
  }
  async function fillCard(s){
    let edits = true;
    if (s.style === 'image') {
      try { window.fromImage.set(await pictureCanvas(), s.img.mode); edits = window.fromImage.restore(s.img); }
      catch (e) { toast(e.message, true); return false; }
    }
    el('tstyle').value = s.style;
    el('text').value = s.text || '';
    if (s.size)  el('size').value = s.size;
    if (s.align) el('align').value = s.align;
    if (s.cut)   el('cut').value = s.cut;
    if ('wwrap' in s) el('wwrap').checked = !!s.wwrap;
    if (s.style === 'fine' && window.PF) window.PF.set(s.fine);
    ['tstyle', 'size', 'align', 'wwrap'].forEach(id => el(id).dispatchEvent(new Event('change')));
    el('text').dispatchEvent(new Event('input'));
    if (s.style === 'image' && window.fromImage) window.TPL.preview();
    if (!edits) toast('the picture reads differently now - your tapped-out lines were not kept', true);
    return true;
  }

  // A JPEG of the From-image picture, in grey (colour adds nothing to
  // extracted words and doubles the size). null if it won't fit.
  async function pictureBlob(){
    const src = window.fromImage.canvas(); if (!src) return null;
    const c = document.createElement('canvas'); c.width = src.width; c.height = src.height;
    const cx = c.getContext('2d'); cx.drawImage(src, 0, 0);
    const im = cx.getImageData(0, 0, c.width, c.height), d = im.data;
    for (let i = 0; i < d.length; i += 4) d[i] = d[i + 1] = d[i + 2] = 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
    cx.putImageData(im, 0, 0);
    for (const q of [0.88, 0.75, 0.6]) {
      const b = await new Promise(res => c.toBlob(res, 'image/jpeg', q));
      if (b && b.size <= MAX_PIC) return b;
    }
    return null;
  }
  // In pieces of at most 128 KB: through the website a request may carry
  // 160 KB at most. The unit keeps the file only once the last piece is in.
  async function upload(path, blob, name){
    const PIECE = 128 * 1024;
    let r = null;
    for (let off = 0; off < blob.size; off += PIECE) {
      const fd = new FormData(); fd.append('data', blob.slice(off, off + PIECE), name);
      r = await api(path + '&bytes=' + blob.size + '&off=' + off, { method: 'POST', body: fd });
    }
    return r;
  }

  // ---- Keep as draft / Save ----------------------------------------------
  async function keep(kind){
    const src = capture();
    if (!src) { toast('nothing to keep yet', true); return; }
    const pic = src.style === 'image';
    if (pic && counts.pic) {
      toast('Only one picture can be stored. Delete the item with a picture first (in '
            + NAME[counts.pic[0]] + ').', true);
      return;
    }
    let blob = null;
    if (pic && !(blob = await pictureBlob())) { toast('that picture is too big to store', true); return; }
    try {
      const sess = newSess();
      const r = await post('/api/items?kind=' + kind, { title: titleOf(src), style: src.style, src, pic, sess });
      if (blob) {
        try { await upload('/api/items/pic?owner=' + kind + r.id, blob, 'pic.jpg'); }
        catch (e) { await api('/api/items/delete?kind=' + kind + '&id=' + r.id, { method: 'POST' }).catch(() => {});
                    throw e; }
      }
      counts[kind]++; if (pic) counts.pic = kind + r.id; paint();
      if (kind === 'd' && !pic) setLink({ id: r.id, rev: r.rev || 1, edits: 0, sess, title: titleOf(src) });
      toast((kind === 'd' ? 'Kept as draft (' : 'Saved (') + counts[kind] + '/' + counts.limit[kind] + ')');
      if (kind === 's' && el('drawer').classList.contains('open')) load('s');
      if (document.body.classList.contains('fut')) load(kind);
    } catch (e) { toast(e.message, true); }
  }

  // ---- Lists ---------------------------------------------------------------
  function ruleText(it){
    const at = ' at ' + it.at;
    if (it.rep === 'once') {
      const [y, m, d] = (it.date || '').split('-').map(Number);
      return 'Once, ' + new Date(y, m - 1, d).toLocaleDateString(undefined,
        { weekday: 'short', day: 'numeric', month: 'short' }) + at;
    }
    if (it.rep === 'daily') return 'Every day' + at;
    if (it.rep === 'month') {
      const n = it.mday, sfx = (n % 10 === 1 && n !== 11) ? 'st' : (n % 10 === 2 && n !== 12) ? 'nd'
              : (n % 10 === 3 && n !== 13) ? 'rd' : 'th';
      return 'Every month on the ' + n + sfx + at;
    }
    const b = it.days & 127;
    if (b === 62) return 'Weekdays' + at;
    if (b === 65) return 'Weekends' + at;
    if (b === 127) return 'Every day' + at;
    return [1, 2, 3, 4, 5, 6, 0].filter(i => (b >> i) & 1).map(i => DAYS[i]).join(', ') + at;
  }
  function schedState(it){
    const now = Date.now() / 1000;
    if (!it.ready) return ['Not ready - its slip did not reach the printer. Delete it and schedule again.', true];
    if (counts.running === it.id) return ['Printing now', false];
    let s, bad = false;
    if (!it.next) { s = 'Did not print' + (it.why ? ' (' + it.why + ')' : '') + ' - tap Print now'; bad = true; }
    else if (it.next <= now) s = 'Due - waiting for the printer';
    else s = 'Next: ' + when(it.next);
    if (it.last && it.next) s += it.lastOk ? ' \u00b7 last printed ' + when(it.last)
                                           : ' \u00b7 last try failed' + (it.why ? ': ' + it.why : '');
    return [s, bad];
  }

  function row(kind, it){
    const r = document.createElement('div'); r.className = 'irow';
    const t = document.createElement('div'); t.className = 'it';
    const b = document.createElement('b'); b.textContent = it.title || 'Untitled';
    const sub = document.createElement('span');
    if (kind === 'p') { const [s, bad] = schedState(it); sub.textContent = ruleText(it) + ' \u2014 ' + s; if (bad) sub.className = 'bad'; }
    else sub.textContent = styleName(it.style) + (it.pic ? ' \u00b7 with picture' : '') + (it.t ? ' \u00b7 ' + when(it.t) : '');
    t.appendChild(b); t.appendChild(sub);
    t.onclick = () => open(it);
    r.appendChild(t);
    const btn = (label, fn, ghost) => { const x = document.createElement('button'); x.textContent = label;
      if (ghost) x.className = 'ghost'; x.onclick = fn; r.appendChild(x); };
    if (kind === 's') btn('Print', () => quickPrint(it));
    if (kind === 'd') { btn(link && link.id === it.id ? 'Open \u2713' : 'Open', () => open(it), true); btn('Print', () => quickPrint(it));
                        btn('Copy link', () => copyLink(it), true);
                        btn('QR', () => window.showQr(draftLink(it), '\u201c' + (it.title || 'Untitled') + '\u201d'), true); }
    if (kind === 'p') btn('Print now', () => runNow(it), true);
    btn('\u2715', () => remove(kind, it), true);
    if (kind !== 'd') return r;
    // A draft shows everything in it, with its edit count top right.
    const box = document.createElement('div'); box.className = 'dfull';
    const pen = document.createElement('span'); pen.className = 'pen'; pen.textContent = '\u270e ' + (it.edits || 0);
    pen.title = 'edited ' + (it.edits || 0) + ' time' + (it.edits === 1 ? '' : 's');
    box.appendChild(r); box.appendChild(pen);
    const body = document.createElement('div'); body.className = 'dtext';
    const lines = fullText(it);
    if (lines.length && lines[0][0] === it.title) lines.shift();     // the title is already above
    lines.forEach(([txt, cls]) => { const d = document.createElement('div'); d.textContent = txt; if (cls) d.className = cls; body.appendChild(d); });
    box.appendChild(body);
    return box;
  }
  // The lines of a draft as they would read on paper.
  function fullText(it){
    const s = it.src || {};
    if (s.style === 'fine') {
      const f = s.fine || {};
      return [['Plate ' + (f.plate || ''), ''], [f.offence || '', ''], ['Penalty ' + (f.amount || '') + ' or ' + (f.settle || ''), '']];
    }
    if (s.style === 'image') return [[s.text || '', 'b'], ['(words from a picture)', 'dim']];
    const lines = (s.text || '').split('\n'), out = [];
    lines.forEach((l, i) => {
      const t = l.trim();
      if (!t) { out.push(['\u00a0', 'gap']); return; }
      if (i === 0 && s.style !== 'plain') { out.push([t, 'b']); return; }
      if (s.style === 'list' && !t.endsWith(':')) out.push(['\u2610 ' + t.replace(/^(\[\s?\]|[-*\u2022]|\d+[.)])\s*/, ''), '']);
      else out.push([t, t.endsWith(':') ? 'b' : '']);
    });
    return out;
  }

  function fill(kind){
    const box = el(kind === 's' ? 'dbList' : kind === 'd' ? 'fdList' : 'fpList');
    if (!box) return;
    box.textContent = '';
    if (!lists[kind].length) {
      const e = document.createElement('div'); e.className = 'empty';
      e.textContent = kind === 's' ? 'Nothing saved yet. Tap Save on the Text card.'
                    : kind === 'd' ? 'No drafts. Tap Keep as draft on the Text card.'
                    : 'Nothing scheduled. Tap Schedule\u2026 on the Text card.';
      box.appendChild(e); return;
    }
    for (const it of lists[kind]) box.appendChild(row(kind, it));
  }
  async function load(kind){
    try { lists[kind] = await api('/api/items?kind=' + kind); counts[kind] = lists[kind].length; }
    catch (e) { lists[kind] = []; }
    fill(kind); paint();
  }

  async function open(it){
    if (link && link.id === it.id && dirty) { showFuture(false); return; }   // already open here
    if (!canLeave()) return;
    const draft = lists.d.some(x => x.id === it.id) && !it.pic;
    if (!draft && link) setLink(null);
    if (await apply(it, !draft && !link)) {
      if (draft) setLink({ id: it.id, rev: it.rev || 1, edits: it.edits || 0, sess: newSess(), title: it.title });
      showFuture(false);
      el('drawer').classList.remove('open');
      el('text').closest('.card').scrollIntoView({ behavior: 'smooth', block: 'start' });
    }
  }
  // Prints an item without changing the Text card: the card is put back
  // the moment the slip has been drawn, before anything is sent.
  function snapCard(){
    return { style: el('tstyle').value, text: el('text').value, size: el('size').value, align: el('align').value,
             cut: el('cut').value, wwrap: el('wwrap').checked,
             fine: window.PF ? Object.assign({}, window.PF.values()) : null,
             img: window.fromImage ? window.fromImage.snap() : null };
  }
  function unsnapCard(c){
    if (c.img) window.fromImage.unsnap(c.img);
    el('tstyle').value = c.style; el('text').value = c.text; el('size').value = c.size;
    el('align').value = c.align; el('cut').value = c.cut; el('wwrap').checked = c.wwrap;
    if (c.fine && window.PF) window.PF.set(c.fine);
    ['tstyle', 'size', 'align', 'wwrap'].forEach(id => el(id).dispatchEvent(new Event('change')));
    el('text').dispatchEvent(new Event('input'));
    window.TPL.preview();
  }
  async function quickPrint(it){
    if (!it.src) { toast('this one can\u2019t be printed from here', true); return; }
    const card = snapCard();
    let job = null;
    quiet = true;
    try {
      if (!(await fillCard(it.src))) return;
      const s = it.src, cut = (s.cut || '1') === '1';
      if (s.style === 'plain') {
        const sz = sizeSpec();
        job = { text: el('wwrap').checked ? wrappedForPrint() : el('text').value, align: el('align').value,
                size: sz.scale, font: sz.font, cut };
      } else {
        const r = window.TPL.render(false);
        if (r) job = { r, cut };
      }
    } catch (e) { toast(e.message, true); }
    finally { unsnapCard(card); quiet = false; }
    if (!job) { toast('nothing to print', true); return; }
    toast('printing \u201c' + (it.title || 'Untitled') + '\u201d');
    if (job.r) await window.TPL.sendRaster(job.r, job.cut, null);
    else {
      try { const r = await post('/api/print/text', job); toast('queued as job #' + r.jobId); window.confirmPrinted(r.jobId); }
      catch (e) { toast(e.message, true); }
    }
  }
  // A link straight to one draft (v0.22): the printer's own link (online one
  // if online printing is on) with #draft-<id>. Whoever opens it still needs
  // what the printer's link needs (approval, if extra secure is on).
  function draftLink(it){
    const base = window.REMOTE ? location.origin + window.BASE + '/'
               : (window.ONLINE_URL || location.origin + '/');
    return base + '#draft-' + it.id;
  }
  async function copyLink(it){
    const url = draftLink(it);
    let ok = false;
    try { await navigator.clipboard.writeText(url); ok = true; } catch (e) {}
    if (!ok) {                       // plain http (home network): the old way
      const ta = document.createElement('textarea'); ta.value = url; ta.style.cssText = 'position:fixed;left:-9999px';
      document.body.appendChild(ta); ta.select();
      try { ok = document.execCommand('copy'); } catch (e) {}
      ta.remove();
    }
    if (ok) toast('link to \u201c' + (it.title || 'Untitled') + '\u201d copied');
    else prompt('Copy this link:', url);
  }
  async function openFromHash(){
    const m = location.hash.match(/^#draft-(\d+)$/);
    if (!m) return;
    history.replaceState(null, '', location.pathname);
    await load('d');
    const it = lists.d.find(x => x.id === +m[1]);
    if (!it) { toast('that draft is no longer here', true); return; }
    await open(it);
  }

  async function runNow(it){
    try { await api('/api/items/run?id=' + it.id, { method: 'POST' }); toast('it will print as soon as the printer is free');
          setTimeout(() => load('p'), 1500); }
    catch (e) { toast(e.message, true); }
  }
  async function remove(kind, it){
    if (!confirm('Delete \u201c' + (it.title || 'Untitled') + '\u201d?')) return;
    try { await api('/api/items/delete?kind=' + kind + '&id=' + it.id, { method: 'POST' });
          if (counts.pic === kind + it.id) counts.pic = '';
          if (kind === 'd' && link && link.id === it.id) setLink(null);
          load(kind); }
    catch (e) { toast(e.message, true); }
  }

  // ---- Counts: Future box and Saved title ----------------------------------
  function paint(){
    const L = counts.limit || { d: 10, s: 20, p: 8 };
    el('futD').textContent = counts.d; el('futD').className = counts.d ? 'on' : '';
    el('futP').textContent = counts.p;
    const set = (id, k) => { const h = el(id); if (h) h.textContent = NAME[k] + ' (' + counts[k] + '/' + L[k] + ')'; };
    set('dhTitle', 's'); set('fdTitle', 'd'); set('fpTitle', 'p');
  }
  window.onStatus = s => {
    if (!s.items) return;
    const was = counts.running, ver = counts.ver;
    counts = s.items; paint();
    window.ONLINE_URL = (s.online && s.online.url) || '';
    window.LOCAL = s.local !== false;
    document.body.classList.toggle('remote', !window.LOCAL);
    const fut = document.body.classList.contains('fut'), changed = ver && ver !== counts.ver;
    if (fut && (changed || was || counts.running || counts.waiting)) { load('p'); if (changed) load('d'); }
    if (changed && el('drawer').classList.contains('open')) load('s');
  };

  // ---- The draft open in the Text card ---------------------------------
  function setLink(l){
    link = l; dirty = false; clearTimeout(saveT);
    const w = el('dWarn');
    if (w) w.style.display = 'none';
    paintLink();
    if (document.body.classList.contains('fut')) fill('d');
  }
  // The badge (top right), the bar over the text, and the button's name.
  function paintLink(){
    const b = el('dBadge'), bar = el('dBar');
    if (!b || !bar) return;
    b.style.display = bar.style.display = link ? '' : 'none';
    el('tdraft').textContent = link ? 'Update draft' : 'Keep as draft';
    el('tdraft').classList.toggle('ghost', !(link && dirty));
    if (!link) return;
    b.textContent = 'Draft \u270e ' + (link.edits || 0);
    b.title = 'Edited ' + (link.edits || 0) + ' times';
    el('dBarText').textContent = 'Draft \u201c' + (link.title || 'Untitled') + '\u201d \u2014 '
      + (dirty ? 'unsaved changes' : 'all changes saved');
    el('dBarText').className = dirty ? 'bad' : '';
  }
  // True if it is all right to leave the linked draft (asks if unsaved).
  function canLeave(){
    return !link || !dirty || confirm('Your changes to the draft \u201c' + (link.title || 'Untitled') + '\u201d are not saved. Discard them?');
  }
  function warn(msg, offer){
    const w = el('dWarn'); if (!w) return;
    w.textContent = msg + ' ';
    if (offer) {
      const a = document.createElement('button'); a.className = 'ghost small'; a.textContent = 'Load theirs';
      a.onclick = offer; w.appendChild(a);
    }
    w.style.display = 'block';
  }
  function edited(e){
    if (!link || quiet) return;
    if (e && e.target && e.target.closest && e.target.closest('#schedPanel, #dBar')) return;
    if (!dirty) { dirty = true; paintLink(); }
  }
  async function save(){
    if (!link) return;
    if (!dirty) { toast('no changes to save'); return; }
    if (saving) return;
    const src = capture();
    if (!src) return;                       // an emptied card is not saved over the list
    if (src.style === 'image') { toast('a draft can\u2019t become a picture - keep it as a new draft instead', true); setLink(null); return; }
    const l = link; saving = true; dirty = false;
    try {
      const r = await post('/api/items/update?kind=d&id=' + l.id, { src, title: titleOf(src), base: l.rev, sess: l.sess });
      if (link !== l) return;
      l.rev = r.rev; l.edits = r.edits; l.title = titleOf(src);
      paintLink();
      toast('Draft updated \u2713');
      if (r.conflict) warn('Someone else changed this draft while you had it open. Your version replaced theirs.', () => loadTheirs(l, true));
    } catch (e) {
      if (/deleted/.test(e.message)) { toast('this draft was deleted on another device', true); setLink(null); }
      else { dirty = true; paintLink(); toast('could not update the draft: ' + e.message, true); }
    } finally { saving = false; }
  }
  async function loadTheirs(l, prev){
    try {
      const d = await api('/api/items/one?kind=d&id=' + l.id);
      const src = prev ? (d.prev || d.src) : d.src;
      quiet = true; try { await fillCard(src); } finally { quiet = false; }
      l.rev = d.rev; l.edits = d.edits || 0; dirty = false;
      el('dWarn').style.display = 'none';
      paintLink();
      if (prev && d.prev) { dirty = true; save(); }    // their version becomes the latest again
    } catch (e) { toast(e.message, true); }
  }
  // Every few seconds: has someone else saved this draft?
  async function watch(){
    if (!link || saving || document.hidden) return;
    const l = link;
    try {
      const d = await api('/api/items/one?kind=d&id=' + l.id);
      if (link !== l || !d.rev || d.rev <= l.rev || d.by === l.sess) return;
      if (dirty) warn('Someone else updated this draft (\u270e ' + (d.edits || 0) + ') since you opened it. Updating now will replace their version.', () => loadTheirs(l, false));
      else { await loadTheirs(l, false); warn('Updated with changes from another device.'); setTimeout(() => { if (el('dWarn').textContent.startsWith('Updated')) el('dWarn').style.display = 'none'; }, 4000); }
    } catch (e) {
      if (/no such item/.test(e.message) && link === l) { toast('this draft was deleted on another device', true); setLink(null); }
    }
  }

  // ---- Future view ----------------------------------------------------------
  const ZONES = [['Perth', 'AWST-8'], ['Darwin', 'ACST-9:30'],
    ['Adelaide', 'ACST-9:30ACDT,M10.1.0,M4.1.0/3'], ['Brisbane', 'AEST-10'],
    ['Sydney, Melbourne, Canberra, Hobart', 'AEST-10AEDT,M10.1.0,M4.1.0/3'],
    ['Auckland', 'NZST-12NZDT,M9.5.0,M4.1.0/3'], ['London', 'GMT0BST,M3.5.0/1,M10.5.0'], ['UTC', 'UTC0']];
  async function loadClock(){
    try {
      const c = await api('/api/clock');
      el('fClock').textContent = c.ok ? 'Printer\u2019s clock: ' + when(c.now)
        : 'The printer doesn\u2019t know the time yet - it needs the internet. Scheduled prints wait until it does.';
      el('fClock').className = c.ok ? 'meta' : 'meta bad';
      const z = el('fTz');
      if (![...z.options].some(o => o.value === c.tz)) {
        const o = document.createElement('option'); o.value = c.tz; o.textContent = 'Other (' + c.tz + ')'; z.appendChild(o);
      }
      z.value = c.tz;
    } catch (e) {}
  }
  function showFuture(on){
    document.body.classList.toggle('fut', on);
    el('futbox').setAttribute('aria-pressed', String(!!on));
    if (on) { load('d'); load('p'); loadClock(); window.scrollTo(0, 0); }
    else if (location.hash === '#future') history.replaceState(null, '', location.pathname);
  }

  function build(){
    const fv = document.createElement('div'); fv.id = 'futView';
    fv.innerHTML =
      '<div class="card"><div class="hdr"><button class="ghost small" id="fBack" style="flex:0 0 auto">\u2039 Back</button>'
      + '<b style="font-size:15px;letter-spacing:.08em">FUTURE</b><span style="width:60px"></span></div>'
      + '<div class="meta" id="fClock"></div>'
      + '<div class="localonly"><label for="fTz">Time zone</label><select id="fTz"></select></div></div>'
      + '<div class="card"><label id="fdTitle">Drafts</label><div id="fdList"></div></div>'
      + '<div class="card"><label id="fpTitle">Scheduled</label><div id="fpList"></div>'
      + '<div class="meta">A print that falls due while the printer is off or out of paper comes out when it is back.</div></div>';
    const top = document.querySelector('.top');
    top.parentNode.insertBefore(fv, top.nextSibling);
    const z = el('fTz');
    for (const [n, v] of ZONES) { const o = document.createElement('option'); o.value = v; o.textContent = n; z.appendChild(o); }
    z.onchange = async () => {
      try { await post('/api/clock', { tz: z.value }); toast('time zone changed'); loadClock(); load('p'); }
      catch (e) { toast(e.message, true); }
    };
    el('fBack').onclick = () => showFuture(false);

    const dr = document.createElement('div'); dr.id = 'drawer';
    dr.innerHTML = '<div class="dh" id="dhTitle">Saved</div><div class="db" id="dbList"></div>';
    document.body.appendChild(dr);
    el('dhTitle').onclick = () => { const o = dr.classList.toggle('open'); if (o) load('s'); };

    const card = el('text').closest('.card');
    card.style.position = 'relative';
    const badge = document.createElement('span'); badge.id = 'dBadge'; badge.className = 'dbadge'; badge.style.display = 'none';
    card.appendChild(badge);
    const w = document.createElement('div'); w.id = 'dWarn'; w.className = 'alert info'; w.style.display = 'none';
    el('text').parentNode.insertBefore(w, el('text'));
    const bar = document.createElement('div'); bar.id = 'dBar'; bar.className = 'dbar'; bar.style.display = 'none';
    bar.innerHTML = '<span id="dBarText"></span><button class="ghost small" id="dClose" type="button">Close</button>';
    el('text').parentNode.insertBefore(bar, w);
    card.addEventListener('input', edited, true);
    card.addEventListener('change', edited, true);
    setInterval(watch, window.REMOTE ? 15000 : 4000);
    document.addEventListener('visibilitychange', () => { if (!document.hidden) watch(); });

    el('tsave').onclick  = () => keep('s');
    el('tdraft').onclick = () => link ? save() : keep('d');
    el('dClose').onclick = () => { if (canLeave()) setLink(null); };
    window.addEventListener('beforeunload', e => { if (link && dirty) { e.preventDefault(); e.returnValue = ''; } });
    el('tsched').onclick = toggleSchedule;
    window.addEventListener('hashchange', () => { if (/^#draft-/.test(location.hash)) openFromHash(); else showFuture(location.hash === '#future'); });
    // Future again while its page is open goes back to the main page.
    el('futbox').addEventListener('click', e => {
      if (document.body.classList.contains('fut')) { e.preventDefault(); showFuture(false); }
    });
    if (location.hash === '#future') showFuture(true);
    openFromHash();
    paint();
  }

  // ---- Schedule -------------------------------------------------------------
  const pad = n => String(n).padStart(2, '0');
  function nextLocal(f){
    // The browser's view of the first print, shown before confirming.
    const [hh, mm] = f.at.split(':').map(Number), now = new Date();
    if (f.rep === 'once') { const [y, m, d] = f.date.split('-').map(Number); return new Date(y, m - 1, d, hh, mm); }
    for (let off = 0; off < 400; off++) {
      const d = new Date(now.getFullYear(), now.getMonth(), now.getDate() + off, hh, mm);
      const last = new Date(d.getFullYear(), d.getMonth() + 1, 0).getDate();
      const ok = f.rep === 'daily' || (f.rep === 'days' && (f.days >> d.getDay()) & 1)
              || (f.rep === 'month' && d.getDate() === Math.min(f.mday, last));
      if (ok && d > now) return d;
    }
    return null;
  }
  function readForm(){
    const rep = el('sRep').value;
    const days = [...document.querySelectorAll('#sDays input:checked')].reduce((a, c) => a | (1 << +c.value), 0);
    return { rep, at: el('sAt').value, date: el('sDate').value, days, mday: +el('sMday').value };
  }
  function showForm(){
    const f = readForm();
    el('sDateW').style.display  = f.rep === 'once'  ? '' : 'none';
    el('sDaysW').style.display  = f.rep === 'days'  ? '' : 'none';
    el('sMdayW').style.display  = f.rep === 'month' ? '' : 'none';
    document.querySelectorAll('#sDays label').forEach(l => l.classList.toggle('on', l.firstChild.checked));
    const n = f.at && (f.rep !== 'once' || f.date) && !(f.rep === 'days' && !f.days) ? nextLocal(f) : null;
    el('sNext').textContent = !n ? 'Pick when it should print.'
      : n < new Date() ? 'That time has already passed.'
      : 'First print: ' + n.toLocaleDateString(undefined, { weekday: 'long', day: 'numeric', month: 'long' })
        + ' at ' + f.at + '.';
  }
  function toggleSchedule(){
    const p = el('schedPanel');
    if (p.firstChild) { p.textContent = ''; return; }
    const now = new Date(), nh = new Date(now.getTime() + 3600000);
    let mdays = ''; for (let i = 1; i <= 31; i++) mdays += '<option>' + i + '</option>';
    p.innerHTML =
      '<div style="border-top:1px solid var(--line);margin-top:12px;padding-top:4px">'
      + '<div class="row"><div><label for="sRep">Repeat</label><select id="sRep">'
      + '<option value="once">Once</option><option value="daily">Every day</option>'
      + '<option value="days">Chosen days</option><option value="month">Every month</option></select></div>'
      + '<div id="sDateW"><label for="sDate">Date</label><input type="date" id="sDate"></div>'
      + '<div><label for="sAt">Time</label><input type="time" id="sAt"></div></div>'
      + '<div id="sDaysW"><label>Days</label><div class="chips" id="sDays">'
      + [1, 2, 3, 4, 5, 6, 0].map(i => '<label><input type="checkbox" value="' + i + '"' + (i >= 1 && i <= 5 ? ' checked' : '') + '>' + DAYS[i] + '</label>').join('')
      + '</div></div>'
      + '<div id="sMdayW"><label for="sMday">Day of the month</label><select id="sMday">' + mdays + '</select></div>'
      + '<div class="meta" id="sNext"></div>'
      + '<div class="row" style="margin-top:10px"><button id="sGo">Schedule it</button>'
      + '<button class="ghost" id="sNo" style="flex:0 0 auto">Cancel</button></div></div>';
    el('sDate').value = nh.getFullYear() + '-' + pad(nh.getMonth() + 1) + '-' + pad(nh.getDate());
    el('sAt').value = pad(nh.getHours()) + ':00';
    el('sMday').value = String(now.getDate());
    p.querySelectorAll('select,input').forEach(i => i.addEventListener('change', showForm));
    el('sAt').addEventListener('input', showForm);
    el('sNo').onclick = () => { p.textContent = ''; };
    el('sGo').onclick = schedule;
    showForm();
  }

  // The slip as the printer will get it: parts of at most ~48 KB, with the
  // date line (List and Note) left out for the printer to write itself.
  function buildSlip(){
    const style = el('tstyle').value;
    if (style === 'plain') {
      if (!el('text').value.trim()) return null;
      const sz = sizeSpec();
      const text = el('wwrap').checked ? wrappedForPrint() : el('text').value;
      return { width: window.PRINTER.printableDots, seq: [{ x: text, z: sz.scale, f: sz.font === 'b' ? 1 : 0,
               a: ({ left: 0, center: 1, right: 2 })[el('align').value] || 0 }], bin: new Uint8Array(0) };
    }
    const r = window.TPL.render(false);
    if (!r) return null;
    const rb = r.width / 8, band = window.TPL.band(), seq = [], bins = [];
    const add = (y0, y1) => {
      if (y1 <= y0) return;
      for (const d of window.TPL.parts({ width: r.width, rows: y1 - y0, data: r.data.slice(y0 * rb, y1 * rb) })) {
        let set = 0; for (const b of d) { let v = b; while (v) { set += v & 1; v >>= 1; } }
        seq.push({ r: d.length / rb, c: Math.round(set * 100 / (d.length * 8)) });
        bins.push(d);
      }
    };
    if (band) { add(0, band.y0); seq.push({ d: 1, f: band.base <= 32 ? 1 : 0 }); add(band.y1, r.rows); }
    else add(0, r.rows);
    const bin = new Uint8Array(bins.reduce((a, d) => a + d.length, 0));
    let o = 0; for (const d of bins) { bin.set(d, o); o += d.length; }
    return { width: r.width, seq, bin };
  }

  async function schedule(){
    const f = readForm();
    if (!f.at || (f.rep === 'once' && !f.date)) { toast('pick a date and time', true); return; }
    if (f.rep === 'days' && !f.days) { toast('pick at least one day', true); return; }
    const n = nextLocal(f);
    if (!n || n < new Date()) { toast('that time has already passed', true); return; }
    const slip = buildSlip();
    if (!slip) { toast('nothing to print yet', true); return; }
    const src = capture();
    const body = Object.assign({ title: src ? titleOf(src) : 'From image', style: el('tstyle').value,
      src: src && src.style !== 'image' ? src : null, cut: el('cut').value === '1',
      width: slip.width, seq: slip.seq }, f);
    const go = el('sGo'); go.disabled = true;
    try {
      const r = await post('/api/items?kind=p', body);
      if (slip.bin.length) {
        try { await upload('/api/items/bin?id=' + r.id, new Blob([slip.bin]), 'slip.bin'); }
        catch (e) { await api('/api/items/delete?kind=p&id=' + r.id, { method: 'POST' }).catch(() => {}); throw e; }
      }
      el('schedPanel').textContent = '';
      counts.p++; paint();
      toast('Scheduled - first print ' + when(r.next));
    } catch (e) { toast(e.message, true); go.disabled = false; }
  }

  document.addEventListener('DOMContentLoaded', build);
  window.FUT = { capture, titleOf, buildSlip };     // for tests
})();
</script>
)HTML";

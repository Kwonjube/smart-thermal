// WebUI.h — Section 16.
//
// The page is split into three PROGMEM blocks, sent in order by
// WebApi::handleIndex(): HEAD, then WEB_UI_IMAGE (WebUIImage.h), then TAIL.
// Splitting keeps each block readable and lets the image pipeline live in
// its own file.
//
// The core script sits in HEAD and only defines helpers plus a
// DOMContentLoaded handler, so it can safely reference elements that
// appear in later blocks.

#pragma once
#include <Arduino.h>

static const char WEB_UI_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<script>
// Opened through the website (v0.21) the page lives under /p/<code>/, so
// every request the page makes to "/..." is sent to "/p/<code>/..." instead.
// On the printer's own network BASE is empty and nothing changes.
window.BASE = (location.pathname.match(/^\/p\/[a-z0-9]{6}(?=\/)/) || [''])[0];
// preview.html / demo.html (tools/build_pages.js) set SHOW_AS_ONLINE to
// show the page exactly as a visitor through the website sees it.
window.REMOTE = !!window.BASE || !!window.SHOW_AS_ONLINE;
if (window.BASE) {
  const f = window.fetch.bind(window);
  window.fetch = (u, o) => f(typeof u === 'string' && u[0] === '/' && u[1] !== '/' ? window.BASE + u : u, o);
}
// A screen started as the party kiosk goes straight back to it.
try { if (!window.REMOTE && localStorage.getItem('kiosk') === '1') location.replace('/kiosk'); } catch (e) {}
</script>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Thermal</title>
<style>
:root{--bg:#f4f4f2;--card:#fff;--ink:#1a1a18;--dim:#6b6b66;--line:#dcdcd6;
      --accent:#1f6f4a;--bad:#a8332a;--radius:10px}
@media(prefers-color-scheme:dark){
:root{--bg:#16161a;--card:#1f1f24;--ink:#ecece8;--dim:#9a9a94;--line:#33333a;
      --accent:#4fbf88;--bad:#e0705f}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--ink);
     font:15px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
     max-width:640px;margin-inline:auto}
h1{font-size:17px;letter-spacing:.14em;text-transform:uppercase;margin:0 0 4px}
.sub{color:var(--dim);font-size:12px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);
      padding:14px;margin-bottom:14px}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
.row>*{flex:1 1 auto}
label{display:block;font-size:11px;text-transform:uppercase;letter-spacing:.1em;
      color:var(--dim);margin:10px 0 4px}
textarea,input,select{width:100%;padding:9px;background:var(--bg);color:var(--ink);
      border:1px solid var(--line);border-radius:6px;font:inherit}
textarea{min-height:120px;resize:vertical}
input[type=range]{padding:0}
button{padding:11px 14px;background:var(--accent);color:#fff;border:0;
       border-radius:6px;font:inherit;font-weight:600;cursor:pointer}
button.ghost{background:transparent;color:var(--ink);border:1px solid var(--line)}
button:disabled{opacity:.5;cursor:default}
.dot{width:14px;height:14px;border-radius:50%;background:var(--bad);flex:0 0 auto;
  box-shadow:0 0 0 3px rgba(0,0,0,.05)}
.dot.green{background:#1f9d55}.dot.amber{background:#e0a100}.dot.red{background:#d33a2c}
.pico{width:30px;height:30px;color:var(--ink);flex:0 0 auto}
.stat{display:flex;align-items:center;gap:10px}
.alert{display:none;margin-top:10px;padding:11px 13px;border-radius:9px;font-size:15px;line-height:1.4;background:#fbe5e2;color:#5a1a12}
.alert.info{background:#eef2f7;color:#223}
.alert b{margin-right:6px}
.meta{color:var(--dim);font-size:12px;margin-top:8px;white-space:pre-line}
.jobs{font-size:12px;margin-top:6px}
.jobs table{width:100%;border-collapse:collapse}
.jobs td{padding:4px 6px 4px 0;border-bottom:1px solid var(--line);
     vertical-align:top;color:var(--dim)}
.jobs td.id{color:var(--ink);white-space:nowrap}
.jobs td.st{white-space:nowrap}
.jobs .ok{color:var(--accent)}
.jobs .no{color:var(--bad)}
.jobs .src{color:var(--ink)}
.hdr{display:flex;justify-content:space-between;align-items:center;gap:8px}
details summary{cursor:pointer;font-size:11px;text-transform:uppercase;
       letter-spacing:.1em;color:var(--dim)}
.toast{position:fixed;left:50%;bottom:72px;transform:translateX(-50%);
       background:var(--ink);color:var(--bg);padding:10px 16px;border-radius:6px;
       font-size:13px;opacity:0;transition:opacity .2s;pointer-events:none;
       z-index:10;max-width:90vw;text-align:center}
.toast.show{opacity:1}
.drop{border:2px dashed var(--line);border-radius:8px;padding:18px;
      text-align:center;color:var(--dim);font-size:13px;cursor:pointer}
.drop.over{border-color:var(--accent);color:var(--ink)}
#pvwrap{margin-top:12px;text-align:center;background:#fff;border:1px solid var(--line);
        border-radius:6px;padding:8px;overflow:auto}
#preview{image-rendering:pixelated;max-width:100%;height:auto;display:none}
#tpvwrap{margin-top:10px;overflow-x:auto;background:#fff;border-radius:6px;
        border:1px solid var(--line);padding:8px}
#tpv{font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
     color:#111;white-space:pre;display:inline-block;text-align:left;
     border-left:2px solid #c9c9c4;border-right:2px solid #c9c9c4;
     padding:0 6px;min-height:18px}
#tpv .split{background:#ffe9c7}
#tpv .empty{color:#bbb}
.chk{flex:0 0 auto;margin:0;text-transform:none;letter-spacing:0;color:var(--ink);
     font-size:13px;display:flex;align-items:center;gap:6px}
.chk input{width:auto}
.sliders label{display:flex;justify-content:space-between;margin-bottom:2px}
.sliders label span{color:var(--ink);text-transform:none;letter-spacing:0}
/* v0.19: Future box, Saved drawer, item rows */
.top{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:flex-start;gap:10px;margin-bottom:16px}
.top .sub{margin-bottom:0}
.futbox .fl{font-size:11px;font-weight:700;letter-spacing:.06em}
.futbox .fi{display:inline-flex;align-items:center;gap:3px}
.futbox svg{width:18px;height:18px}
.futbox b{font-size:13px;line-height:1}
.futbox b.on{color:#d33a2c}
button.small{padding:7px 10px;font-size:13px}
img.qr{width:132px;height:132px;image-rendering:pixelated;border:1px solid var(--line);border-radius:6px;background:#fff}
.qrbox{display:flex;gap:16px;flex-wrap:wrap;margin-top:10px}
.qrbox>div{text-align:center}
.qrpop{position:fixed;inset:0;background:rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center;z-index:50}
.qrpop>div{background:var(--card,#fff);color:var(--ink);padding:18px;border-radius:10px;text-align:center;max-width:90vw}
.qrpop img.qr{width:220px;height:220px}
/* v0.24: levels. lvN = needs level N or higher; body.L1..L4 = this device's level. */
body.L1 .lv2,body.L1 .lv3,body.L1 .lv4,body.L2 .lv3,body.L2 .lv4,body.L3 .lv4,
body.L1 #drawer,body.L1 #dBadge,body.L2 #dBadge{display:none!important}
.topR{display:flex;gap:8px;align-items:center;flex:0 0 auto;position:relative;margin-left:auto}
/* v0.24: Future and level are plain buttons, styled like the page's drop-downs */
.hbtn{display:flex;align-items:center;gap:8px;height:40px;padding:0 10px;background:var(--bg);color:var(--ink);
  border:1px solid var(--line);border-radius:6px;font:inherit;font-size:14px;font-weight:400;text-decoration:none;cursor:pointer}
.hbtn:hover,.hbtn[aria-expanded=true]{border-color:var(--dim)}
.hbtn[aria-pressed=true]{border-color:var(--ink)}
.hbtn .chev{width:12px;height:12px;color:var(--dim);margin-left:2px}
.lvbars{display:inline-flex;gap:2px;align-items:flex-end;height:12px}
.lvbars i{display:block;width:3px;background:var(--line);border-radius:1px}
.lvbars i:nth-child(1){height:4px}.lvbars i:nth-child(2){height:7px}
.lvbars i:nth-child(3){height:10px}.lvbars i:nth-child(4){height:12px}
.lvbars i.on{background:var(--ink)}
.lvmenu{position:absolute;right:0;top:calc(100% + 6px);z-index:20;width:min(270px,calc(100vw - 32px));
  background:var(--card);border:1px solid var(--line);border-radius:var(--radius);padding:6px;
  box-shadow:0 8px 24px rgba(0,0,0,.18)}
.lvmenu[hidden]{display:none}
.lvmenu button{display:flex;gap:10px;align-items:center;width:100%;text-align:left;background:transparent;
  color:var(--ink);border:0;border-radius:6px;padding:9px 10px;font-weight:400}
.lvmenu button:hover{background:var(--bg)}
.lvmenu button[aria-checked=true]{background:var(--bg);box-shadow:inset 3px 0 0 var(--accent)}
.lvmenu b{display:block;font-size:13px}
.lvmenu small{display:block;color:var(--dim);font-size:12px;line-height:1.35}
.lvmenu .meta{margin:4px 10px 4px}
#futView{display:none}
body.fut>.card{display:none}
body.fut #futView{display:block}
body.fut #futView .card{display:block}
.irow{display:flex;align-items:center;gap:8px;padding:9px 0;border-top:1px solid var(--line)}
.irow:first-child{border-top:0}
.irow .it{flex:1 1 auto;min-width:0;cursor:pointer}
.irow .it b{display:block;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.irow .it span{display:block;font-size:12px;color:var(--dim)}
.irow .it span.bad{color:var(--bad)}
.irow button{flex:0 0 auto;padding:6px 10px;font-size:12px}
.empty{color:var(--dim);font-size:13px;padding:6px 0}
.meta.bad{color:var(--bad)}
#drawer{position:fixed;left:0;right:0;bottom:0;z-index:5;background:var(--card);
  border-top:2px solid var(--ink);max-width:640px;margin:0 auto;
  border-radius:10px 10px 0 0;box-shadow:0 -4px 14px rgba(0,0,0,.12)}
#drawer .dh{display:flex;align-items:center;justify-content:center;gap:8px;padding:8px;
  cursor:pointer;font-weight:700;font-size:14px;letter-spacing:.04em;user-select:none}
#drawer .dh:before,#drawer .dh:after{content:"";flex:1 1 auto;border-top:2px solid var(--ink);margin:0 6px}
#drawer .db{display:none;max-height:48vh;overflow-y:auto;padding:0 14px 12px}
#drawer.open .db{display:block}
body{padding-bottom:64px}
.chips{display:flex;gap:5px;flex-wrap:wrap}
.chips label{margin:0;padding:6px 8px;border:1px solid var(--line);border-radius:6px;
  text-transform:none;letter-spacing:0;color:var(--ink);font-size:13px;cursor:pointer}
.chips input{display:none}
.chips label.on{background:var(--accent);border-color:var(--accent);color:#fff}
.dbadge{position:absolute;top:8px;right:12px;font-size:11px;color:var(--dim);letter-spacing:.04em}
.dbar{display:flex;align-items:center;gap:8px;margin:0 0 8px;padding:7px 10px;border:1px solid var(--line);border-radius:6px;font-size:13px}
.dbar span{flex:1 1 auto}.dbar span.bad{color:var(--bad);font-weight:600}.dbar button{flex:0 0 auto;padding:4px 10px}
.dfull{position:relative;border-top:1px solid var(--line);padding:12px 0 8px}
.dfull:first-child{border-top:0}
.dfull .irow{border-top:0;padding-top:2px}
.dfull .pen{position:absolute;top:2px;right:0;font-size:11px;color:var(--dim)}
.dtext{font-size:13px;line-height:1.45;padding:0 2px 2px;white-space:pre-wrap;word-break:break-word}
.dtext .b{font-weight:700}.dtext .dim{color:var(--dim)}.dtext .gap{height:6px;overflow:hidden}
#dWarn{margin:0 0 8px}
#dWarn button{margin-left:6px;padding:3px 8px}
body.remote .localonly{display:none}
</style>
<script>
// --- shared helpers, used by every panel -------------------------------
const $ = id => document.getElementById(id);
window.PRINTER = { printableDots: 576, charsPerLine: 48, paper: '80 mm' };

let toastTimer;
window.toast = function(msg, bad){
  const t = $('toast');
  t.textContent = msg;
  t.style.background = bad ? 'var(--bad)' : '';
  t.style.color = bad ? '#fff' : '';
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(()=>t.classList.remove('show'), 2600);
};

// QR codes (v0.26): drawn by the unit (/api/qr.svg) - through the website
// too, so the path carries the website prefix when online.
window.qrSrc = window.qrSrc || (t => (window.BASE || '') + '/api/qr.svg?t=' + encodeURIComponent(t));
// (the preview and demo provide their own, drawn without a printer)
window.showQr = function(url, title){
  const p = document.createElement('div');
  p.className = 'qrpop';
  p.innerHTML = '<div><div class="meta" style="margin:0 0 8px"></div><img class="qr" alt="QR code">'
    + '<div class="meta" style="overflow-wrap:anywhere;margin:8px 0"></div><button class="ghost small">Close</button></div>';
  p.querySelector('.meta').textContent = title || 'Scan to open';
  p.querySelector('img').src = window.qrSrc(url);
  p.querySelectorAll('.meta')[1].textContent = url;
  p.onclick = e => { if (e.target === p || e.target.tagName === 'BUTTON') p.remove(); };
  document.body.appendChild(p);
};

// An error says what refused it: the printer's own reason, or (from the
// website relay, which answers in plain text) the status and its words.
window.api = async function(path, opts){
  const r = await fetch(path, opts);
  const t = await r.text().catch(()=>'');
  let j = {}; try { j = t ? JSON.parse(t) : {}; } catch(e) {}
  if(!r.ok) throw new Error(j.error || ('HTTP ' + r.status + (t ? ': ' + t.trim().slice(0, 120) : '')));
  return j;
};

// Waits until a job has printed (or failed), so the page can say so - the
// way to know a print came out when you are not at home (v0.22).
window.waitJob = async function(id, limitMs){
  const t0 = Date.now(), step = window.REMOTE ? 2000 : 1000;
  while(Date.now() - t0 < (limitMs || 120000)){
    await new Promise(r => setTimeout(r, step));
    try{
      const x = await window.api('/api/jobs?id=' + id);
      if(x.status === 'done' || x.status === 'failed') return x;
    }catch(e){}
  }
  return { status: 'unknown' };
};
window.confirmPrinted = async function(id){
  const x = await window.waitJob(id);
  if(x.status === 'done') toast('Printed \u2713 (job #' + id + ')');
  else if(x.status === 'failed') toast('Did not print: ' + (x.error || 'the printer did not finish it'), true);
  else toast('Job #' + id + ' is still waiting - see Recent prints', true);
  if(typeof loadJobs === 'function') loadJobs();
};

const esc = s => String(s==null?'':s).replace(/[&<>]/g,
  c => ({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));

function shortTime(s){
  if(!s) return '\u2014';
  const m = s.match(/^(\d{4})-(\d{2})-(\d{2}) (\d{2}:\d{2})/);
  if(!m) return esc(s);
  const mon = ['Jan','Feb','Mar','Apr','May','Jun',
               'Jul','Aug','Sep','Oct','Nov','Dec'][+m[2]-1];
  return +m[3] + ' ' + mon + ' ' + m[4];
}

// --- print-width preview ------------------------------------------------
// The printer hard-wraps at the character cell, mid-word, with no hyphen.
// That is what the photo of "Hel|lo World" shows. So the preview wraps the
// same way, and "wrap at words" works by inserting real newlines before
// sending, rather than by hoping the printer behaves differently.

// Five sizes, smallest to largest, each a font and a scale: "b2" is Font
// B (9-dot cell) at 2x. Medium is the default.
function sizeSpec(){
  const v = $('size') ? $('size').value : 'b2';
  return { font: v[0], scale: +v[1] };
}
function printCols(){
  const f = sizeSpec();
  const cell = (f.font === 'b' ? 9 : 12) * f.scale;
  return Math.max(1, Math.floor(window.PRINTER.printableDots / cell));
}

function hardWrap(text, cols){
  const out = [];
  for(const line of text.split('\n')){
    if(line === ''){ out.push({ s:'', split:false }); continue; }
    for(let i = 0; i < line.length; i += cols){
      const piece = line.substr(i, cols);
      // A line is "split" if it was cut with more text following on the
      // same logical line — i.e. the break landed inside a word.
      const more  = (i + cols) < line.length;
      const mid   = more && /\S$/.test(piece) && /^\S/.test(line.substr(i + cols, 1));
      out.push({ s:piece, split:mid });
    }
  }
  return out;
}

function wordWrap(text, cols){
  const out = [];
  for(const para of text.split('\n')){
    if(!para.trim()){ out.push({ s:'', split:false }); continue; }
    let cur = '';
    for(const word of para.split(/\s+/)){
      if(!word) continue;
      if(word.length > cols){                  // longer than the paper
        if(cur){ out.push({ s:cur, split:false }); cur = ''; }
        for(let i = 0; i < word.length; i += cols){
          const piece = word.substr(i, cols);
          if(piece.length === cols) out.push({ s:piece, split:true });
          else cur = piece;
        }
        continue;
      }
      if(!cur) cur = word;
      else if(cur.length + 1 + word.length <= cols) cur += ' ' + word;
      else { out.push({ s:cur, split:false }); cur = word; }
    }
    if(cur) out.push({ s:cur, split:false });
  }
  return out;
}

// Exactly what gets sent when "wrap at words" is on.
function wrappedForPrint(){
  const cols = printCols();
  return wordWrap($('text').value, cols).map(l => l.s).join('\n');
}

function renderTextPreview(){
  const cols = printCols();
  const text = $('text').value;
  const lines = $('wwrap').checked ? wordWrap(text, cols) : hardWrap(text, cols);

  const pv = $('tpv');
  pv.style.width = cols + 'ch';
  pv.innerHTML = text === ''
    ? '<span class="empty">' + '·'.repeat(cols) + '</span>'
    : lines.map(l => {
        if(l.s === '') return ' ';
        // Shift each line as the printer will: it aligns every line
        // within the paper width by itself.
        const a = $('align').value, room = Math.max(0, cols - l.s.length);
        const lead = ' '.repeat(a === 'center' ? Math.floor(room / 2)
                                : a === 'right' ? room : 0);
        return lead + (l.split ? '<span class="split">' + esc(l.s) + '</span>' : esc(l.s));
      }).join('\n');

  const splits = lines.filter(l => l.split).length;
  $('tinfo').textContent = cols + ' cols · ' + lines.length + ' line'
    + (lines.length === 1 ? '' : 's') + ' · ' + window.PRINTER.paper
    + (splits ? '  — ' + splits + ' word broken' : '');
}

async function refresh(){
  try{
    const s = await api('/api/printer/status');
    window.PRINTER.printableDots = s.paper.printableDots;
    window.PRINTER.charsPerLine  = s.paper.charsPerLine;
    window.PRINTER.paper         = s.paper.name;
    window.PRINTER.maxImageBytes = s.system.maxImageBytes;
    window.PRINTER.rasterz       = !!s.system.rasterz;   // compressed images (v0.25)
    window.PRINTER.photoRoom     = s.system.photoRoom || 0;
    window.PRINTER.storePrint    = !!s.system.storePrint;   // print from storage (v0.26)
    if (s.image) window.PRINTER.image = s.image;
    if($('tpv')) renderTextPreview();
    if(window.onPrinterInfo) window.onPrinterInfo();
    if(window.onStatus) window.onStatus(s);      // Future box, Saved bar

    // A fault cooldown is not a fault in the UI's sense: the printer is
    // fine, it is simply not answering for ~30 s and nothing can be done
    // about that except wait. Say so, rather than showing "unreachable"
    // and inviting someone to power-cycle a healthy printer.
    const cooling = (s.pacing && s.pacing.coolingMs) || 0;
    const state = cooling
      ? ('printer cooling after a fault \u2014 ' + Math.ceil(cooling / 1000)
         + ' s, then it resumes by itself')
      : (s.ip ? (s.connected ? 'printer ready at ' : 'printer unreachable at ') + s.ip + ':' + s.port
              : (s.connected ? 'printer ready' : 'printer unreachable'));
    // Red: it needs someone (off, unplugged, not answering, paper, cover).
    // Amber: working on it (printing, resting, recovering, searching).
    const code = s.alert && s.alert.code;
    let level = 'green';
    if ((!s.connected && !cooling && !s.searching) || ['E03','E04','E05','E06'].includes(code)) level = 'red';
    else if (code || cooling || s.restingMs > 1000 || s.searching
             || (s.queue && s.queue.pending > 0)) level = 'amber';
    $('dot').className = 'dot ' + level;
    $('stat').title = state;
    // The borrower's line: an error code with what to do, or the rest
    // before the next print. Built with textContent, never as HTML.
    const al = $('alert');
    if (s.alert && s.alert.code) {
      al.className = 'alert'; al.textContent = '';
      const b = document.createElement('b'); b.textContent = s.alert.code;
      al.appendChild(b); al.appendChild(document.createTextNode(s.alert.message));
      al.style.display = 'block';
    } else if (s.restingMs > 1000) {
      al.className = 'alert info';
      al.textContent = 'Next print in ' + Math.ceil(s.restingMs / 1000)
        + ' s \u2014 letting the printer cool.';
      al.style.display = 'block';
    } else al.style.display = 'none';
    // Online, the home network's addresses are not sent (v0.23.2).
    $('sub').textContent = (s.system.ip ? s.system.hostname + '.local \u00b7 ' + s.system.ip
                                        : s.system.hostname + ' \u00b7 online')
      + ' \u00b7 v' + (s.system.version || '?');
    if ($('meta')) $('meta').textContent = state + '\n' +
      s.paper.name + ' \u00b7 ' + s.paper.printableDots + ' dots \u00b7 '
      + s.paper.charsPerLine + ' chars/line\n'
      + 'queue ' + s.queue.pending + ' pending, ' + s.queue.completed
      + ' done, ' + s.queue.failed + ' failed'
      + (s.queue.faults ? ', ' + s.queue.faults + ' thermal' : '')
      + ' \u00b7 heap ' + (s.system.freeHeap/1024).toFixed(0) + ' kB\n'
      + (s.system.clockSet ? s.system.localTime : 'clock not synced')
      + ' \u00b7 history ' + s.system.historyStored + '/100\n'
      + 'largest free block ' + (s.system.largestBlock/1024).toFixed(0)
      + ' kB \u00b7 max image ' + (s.system.maxImageBytes/1024).toFixed(0)
      + ' kB \u00b7 chunk ' + (s.system.chunkRows
          ? (s.system.chunkRows + ' rows') : 'auto')
      + (s.system.debug ? ' \u00b7 DEBUG ON' : '')
      + (s.pacing
          ? '\npacing: fixed table'
            + (s.pacing.coverage
                ? ' \u00b7 last ' + s.pacing.coverage + '% ink, '
                  + s.pacing.printMs + ' ms print, ' + s.pacing.gapMs + ' ms rest'
                : '')
          : '');
  }catch(e){
    $('stat').title = 'no response from the printer controller';
    $('dot').className = 'dot red';
  }
}

async function loadJobs(){
  try{
    const j = await api('/api/jobs?limit=25');
    if(!j.jobs || !j.jobs.length){ $('jobs').innerHTML = 'none yet'; }
    else {
      $('jobs').innerHTML = '<table>' + j.jobs.map(x => {
        const ev  = x.type === 'event';
        const cls = ev ? '' : (x.status==='done' ? 'ok'
                    : (x.status==='failed' ? 'no' : ''));
        const from = (x.ip && x.ip !== '0.0.0.0')
          ? esc(x.source) + ' \u00b7 ' + esc(x.ip) : esc(x.source);
        // The note column carries a boot message for events and a fault
        // reason for prints, so only the latter is coloured as an error.
        const note = x.error
          ? (ev ? esc(x.error) : '<span class="no">' + esc(x.error) + '</span>')
          : '';
        return '<tr><td class="id">#' + x.id + '</td>'
             + '<td class="st ' + cls + '">' + (ev ? '\u2014' : esc(x.status)) + '</td>'
             + '<td>' + esc(x.type) + '</td>'
             + '<td class="src">' + from + '</td>'
             + '<td>' + shortTime(x.localTime) + '</td>'
             + '<td>' + note + '</td>'
             + '</tr>';
      }).join('') + '</table>';
    }
    $('jobmeta').textContent = (j.stored||0) + ' of ' + (j.capacity||0)
      + ' stored \u00b7 ' + (j.pending||0) + ' pending';
  }catch(e){}
}

async function loadSettings(){
  try{
    const s = await api('/api/settings');
    $('paper').innerHTML = s.papers.map(p =>
      '<option value="'+p.index+'">'+esc(p.name)+' \u2014 '+p.printableDots+' dots</option>').join('');
    $('paper').value   = s.paperIndex;
    $('pip').value     = s.printerIp;
    $('pport').value   = s.printerPort;
    $('host').value    = s.hostname;
    $('autocut').value = s.autoCut ? '1' : '0';
    $('chunk').value   = String(s.chunkRows || 0);
    $('debug').value   = s.debug ? '1' : '0';
  }catch(e){}
}

// Preferences (v0.20): this browser's starting choices for the Text card.
const PREF_KEYS = { tstyle: 'Style', size: 'Size', align: 'Align', cut: 'Cut' };
function readPrefs(){ try { return JSON.parse(localStorage.getItem('prefs') || '{}'); } catch(e){ return {}; } }
function writePrefs(p){ try { localStorage.setItem('prefs', JSON.stringify(p)); } catch(e){} }
function setupPrefs(){
  const p = readPrefs(), row = $('prefRow');
  for (const [id, label] of Object.entries(PREF_KEYS)) {
    const src = $(id), sel = document.createElement('select');
    for (const o of src.options) sel.appendChild(new Option(o.text, o.value));
    if (p[id] && [...src.options].some(o => o.value === p[id])) src.value = p[id];
    sel.value = src.value;
    sel.onchange = () => { const q = readPrefs(); q[id] = sel.value; writePrefs(q);
                           src.value = sel.value; src.dispatchEvent(new Event('change')); toast('saved on this device'); };
    const w = document.createElement('div'); w.style.flex = '1 1 90px';
    if (id === 'tstyle') w.className = 'lv3';          // Style shows from Advanced up
    const l = document.createElement('label'); l.textContent = label;
    w.appendChild(l); w.appendChild(sel); row.appendChild(w);
  }
  if ('wwrap' in p) $('wwrap').checked = !!p.wwrap;
  $('pWrap').checked = $('wwrap').checked;
  $('pWrap').onchange = () => { const q = readPrefs(); q.wwrap = $('pWrap').checked; writePrefs(q);
                                $('wwrap').checked = q.wwrap; $('wwrap').dispatchEvent(new Event('change')); toast('saved on this device'); };
}

document.addEventListener('DOMContentLoaded', () => {
  setupPrefs();
  $('print').onclick = async () => {
    if(!$('text').value.trim()){ toast('nothing to print', true); return; }
    // Send the already-wrapped text so the printer never has to break a
    // word itself. Unchecked, the raw text goes over and the printer wraps.
    const text = $('wwrap').checked ? wrappedForPrint() : $('text').value;
    $('print').disabled = true;
    try{
      const r = await api('/api/print/text', {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({ text, align: $('align').value,
          size: sizeSpec().scale, font: sizeSpec().font,
          cut: $('cut').value === '1' })
      });
      toast('queued as job #' + r.jobId);
      window.confirmPrinted(r.jobId);
      loadJobs();
    }catch(e){ toast(e.message, true); }
    $('print').disabled = false;
  };

  $('feedcut').onclick = async () => {
    try{
      await api('/api/print/text', {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({ text:'', cut:true })
      });
      toast('cut queued'); loadJobs();
    }catch(e){ toast(e.message, true); }
  };

  $('clearjobs').onclick = async () => {
    if(!confirm('Clear the stored print history?')) return;
    try{ await api('/api/jobs/clear', {method:'POST'});
         toast('history cleared'); loadJobs(); }
    catch(e){ toast(e.message, true); }
  };

  if ($('save')) $('save').onclick = async () => {
    try{
      const r = await api('/api/settings', {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({
          paperIndex: +$('paper').value, printerIp: $('pip').value,
          printerPort: +$('pport').value, hostname: $('host').value,
          autoCut: $('autocut').value === '1',
          chunkRows: +$('chunk').value,
          debug: $('debug').value === '1' })
      });
      toast(r.rebootRequired ? 'saved \u2014 reboot needed' : 'saved');
      refresh();
    }catch(e){ toast(e.message, true); }
  };

  if ($('reboot')) $('reboot').onclick = async () => {
    try{ await api('/api/system/reboot', {method:'POST'}); }catch(e){}
    toast('rebooting');
  };

  $('text').oninput  = renderTextPreview;
  $('size').onchange = renderTextPreview;
  $('align').onchange = renderTextPreview;
  $('wwrap').onchange = renderTextPreview;
  renderTextPreview();

  refresh();
  if (!window.REMOTE) { loadSettings(); loadJobs(); }
  if (window.REMOTE) document.body.classList.add('remote');
  // Faster while something is wrong or waiting, so the message tracks it.
  // Through the website every request counts against its daily allowance,
  // so it checks less often there.
  (function tick(){ refresh().finally(() => setTimeout(tick,
      window.REMOTE ? ($('alert').style.display === 'block' ? 6000 : 15000)
                    : ($('alert').style.display === 'block' ? 2000 : 5000))); })();
  // Through the website the list is fetched only while it is open.
  if (!window.REMOTE) setInterval(loadJobs, 5000);
  else {
    $('jobsBox').addEventListener('toggle', () => { if ($('jobsBox').open) loadJobs(); });
    setInterval(() => { if ($('jobsBox').open) loadJobs(); }, 20000);
  }
});
</script>
</head>
<body>

<div class="top">
  <div><h1>Smart Thermal</h1>
  <div class="sub lv2" id="sub">connecting&hellip;</div></div>
  <div class="topR">
  <!-- Future: drafts (red when there are any) and scheduled prints. Its
       counts and the page it opens come from WebUIFuture.h. -->
  <a class="hbtn futbox lv3" id="futbox" href="#future" title="Drafts and scheduled prints" aria-pressed="false">
    <span class="fl">Future</span>
    <span class="fi"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
      stroke-linejoin="round"><path d="M6 2.5h8l4 4v15H6z"/><path d="M14 2.5v4h4M9 11h6M9 14.5h6M9 18h4"/></svg>
      <b id="futD">0</b></span>
    <span class="fi"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
      stroke-linejoin="round"><path d="M5 2.5h8l4 4v6"/><path d="M13 2.5v4h4M8 9.5h6M8 13h3"/><path d="M5 2.5v19h5"/>
      <circle cx="16.5" cy="17" r="5"/><path d="M16.5 14.3V17l1.8 1.3"/></svg>
      <b id="futP">0</b></span>
  </a>
  <button type="button" class="hbtn" id="lvBtn" aria-haspopup="true" aria-expanded="false"
    title="How much the page shows"><span class="lvbars" aria-hidden="true"><i></i><i></i><i></i><i></i></span><span id="lvName">Basic</span><svg class="chev" viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true"><path d="M2.5 4.5 6 8l3.5-3.5"/></svg></button>
  <div class="lvmenu" id="lvMenu" role="menu" hidden></div>
  </div>
</div>

<div class="card">
  <!-- The printer at a glance: green ready, amber busy or recovering, red
       needs attention. The words are in the tooltip and in Settings. -->
  <div class="stat" id="stat" title="checking printer">
    <svg class="pico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"
         stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
      <path d="M6 9V3h12v6"/><rect x="3" y="9" width="18" height="8" rx="2"/>
      <path d="M6 14h12v7H6z"/><path d="M17 12h.01"/></svg>
    <span class="dot" id="dot"></span>
  </div>
  <div class="alert" id="alert"></div>
  <div class="meta localonly lv3" id="onlineLine" style="display:none"></div>
  <div class="alert info" id="pausedNote" style="display:none">Printing is paused by the printer&rsquo;s owner. You can look around, but nothing will print for now.</div>
</div>

<div class="card">
  <label for="text">Text</label>
  <textarea id="text" placeholder="Type or paste text&hellip;"></textarea>

  <div class="row lv2" style="margin-top:8px">
    <label class="chk"><input type="checkbox" id="wwrap" checked> Wrap at words</label>
    <span class="meta" id="tinfo" style="text-align:right;margin:0"></span>
  </div>
  <div id="tpvwrap" class="lv2"><div id="tpv"></div>
    <canvas id="tcv" style="display:none;width:100%;image-rendering:pixelated"></canvas></div>
  <div class="row lv3" style="align-items:flex-start;margin-top:4px">
    <div class="meta" id="thint" style="flex:1 1 auto;margin:0"></div>
    <button class="ghost" id="tedit" style="flex:0 0 auto;display:none;padding:4px 12px;font-size:12px">Edit</button>
  </div>

  <div class="row lv2">
    <div class="lv3" style="flex:1 1 90px">
      <label for="tstyle">Style</label>
      <select id="tstyle"><option value="plain">Plain</option>
        <option value="list">List</option><option value="note">Note</option>
        <option value="love">Love note</option>
        <option value="fine">Parking fine</option></select>
    </div>
    <div style="flex:1 1 90px">
      <label for="align">Align</label>
      <select id="align"><option value="left">Left</option>
        <option value="center">Centre</option><option value="right">Right</option></select>
    </div>
    <div style="flex:1 1 70px">
      <label for="size">Size</label>
      <select id="size">
        <option value="b1">XS</option>
        <option value="a1">S</option>
        <option value="b2" selected>M</option>
        <option value="a2">L</option>
        <option value="a3">XL</option></select>
    </div>
    <div style="flex:1 1 70px">
      <label for="cut">Cut</label>
      <select id="cut"><option value="1">Yes</option><option value="0">No</option></select>
    </div>
  </div>
  <div class="row" style="margin-top:14px">
    <button id="print">Print text</button>
    <button class="ghost lv2" id="tsave" style="flex:0 0 auto">Save</button>
    <button class="ghost lv3" id="feedcut" style="flex:0 0 auto">Feed &amp; cut</button>
  </div>
  <div class="row lv3" style="margin-top:8px">
    <button class="ghost small" id="tdraft">Keep as draft</button>
    <button class="ghost small" id="tsched">Schedule&hellip;</button>
  </div>
  <div id="schedPanel" class="lv3"></div>
</div>
)HTML";

static const char WEB_UI_TAIL[] PROGMEM = R"HTML(
<div class="card lv2">
  <details id="jobsBox">
    <summary>Recent prints</summary>
    <div class="jobs" id="jobs">none yet</div>
    <div class="hdr" style="margin-top:10px">
      <span class="meta" id="jobmeta"></span>
      <button class="ghost localonly" id="clearjobs"
              style="flex:0 0 auto;padding:6px 10px;font-size:12px">Clear history</button>
    </div>
  </details>
</div>

<div class="card lv3" id="others" style="display:none">
  <label>Other printers</label>
  <div id="othersList"></div>
</div>

<div class="card lv2">
  <details>
    <summary>Preferences (this device)</summary>
    <div class="meta" style="margin-top:6px">How the Text card starts on this device. Kept in this
      browser only; other devices keep their own.</div>
    <div class="row" id="prefRow"></div>
    <label class="chk" style="margin-top:10px"><input type="checkbox" id="pWrap"> Wrap at words</label>
  </details>
</div>

<!--home-only: tools/build_pages.js leaves this out of the public demo-->
<div class="card localonly lv4">
  <details>
    <summary>Settings (this printer)</summary>
    <div class="meta" style="margin-top:6px">Shared by everyone, and only changeable on the printer's own network.</div>
    <div class="row">
      <div><label for="paper">Paper</label><select id="paper"></select></div>
      <div><label for="pip">Printer IP</label><input id="pip"></div>
      <div style="flex:0 0 100px"><label for="pport">Port</label><input id="pport"></div>
    </div>
    <div class="row">
      <div><label for="host">Hostname</label><input id="host"></div>
      <div style="flex:0 0 120px"><label for="autocut">Auto cut</label>
        <select id="autocut"><option value="1">On</option><option value="0">Off</option></select></div>
      <div style="flex:0 0 120px"><label for="debug">Debug</label>
        <select id="debug"><option value="0">Off</option>
          <option value="1">Serial trace</option></select></div>
      <div style="flex:0 0 130px"><label for="chunk">Chunk rows</label>
        <select id="chunk">
          <option value="0">Auto</option>
          <option value="32">32</option><option value="64">64</option>
          <option value="128">128</option><option value="256">256</option>
          <option value="512">512</option><option value="1024">1024</option>
        </select></div>
    </div>
    <div class="row" style="margin-top:14px">
      <button id="save">Save settings</button>
      <button class="ghost" id="reboot" style="flex:0 0 auto">Reboot</button>
    </div>
    <div class="meta" id="meta" style="margin-top:12px;white-space:pre-line"></div>
    <div class="meta" style="margin-top:12px">
      <a href="/log" target="_blank">Open the log</a> &mdash; recent activity, with a Copy button.
    </div>
    <details style="margin-top:16px" id="online">
      <summary>Online printing</summary>
      <div class="meta" id="oState" style="margin-top:8px"></div>
      <label class="chk" style="margin-top:10px"><input type="checkbox" id="oOn"> Allow online printing</label>
      <label class="chk" style="margin-top:6px"><input type="checkbox" id="oPause"> Pause (visitors can look, nothing prints)</label>
      <div id="oForm">
        <div class="meta" id="oName"></div>
        <label class="chk" style="margin-top:10px"><input type="checkbox" id="oVis"> Visible to other printers</label>
        <label class="chk" style="margin-top:6px"><input type="checkbox" id="oSec"> Extra secure: approve each device first</label>
      </div>
      <div class="row" style="margin-top:12px"><button id="oSave">Save online printing</button></div>
      <div id="oLink" style="display:none;margin-top:10px">
        <div class="meta">Link: <a id="oUrl" target="_blank" rel="noopener"></a></div>
        <div class="meta">Owner code: <b id="oCode"></b></div>
        <div class="meta" style="overflow-wrap:anywhere"><span id="oSecHelp">Devices ask first and a &ldquo;Knock knock!&rdquo; slip prints. </span>Owner page (approve devices, turn online printing off): <a id="oAdmin" target="_blank" rel="noopener"></a> with the owner code.</div>
        <div class="qrbox">
          <div><img class="qr" id="oQr" alt="QR code for the link"><div class="meta">Anyone with the link</div></div>
          <div><img class="qr" id="oQrA" alt="QR code for the owner page"><div class="meta">Owner page</div></div>
        </div>
        <div class="row" style="margin-top:8px">
          <button class="ghost small" id="oNewLink">New link</button>
          <button class="ghost small" id="oNewOwner">New owner code</button>
        </div>
        <div class="meta">The old one stops working, and a new slip prints.</div>
      </div>
    </details>
    <details style="margin-top:16px" id="kiosk">
      <summary>Kiosk (party screen)</summary>
      <label for="kTitle">Screen title</label><input id="kTitle" maxlength="120">
      <label for="kSub">Instructions under the title</label>
      <textarea id="kSub" rows="3" maxlength="300" style="min-height:70px"></textarea>
      <label for="kPrint">Title on every print</label>
      <input id="kPrint" maxlength="120" placeholder="e.g. Sam's 30th Birthday">
      <label for="kFoot">Small text at the bottom of the screen</label><input id="kFoot" maxlength="120">
      <label style="display:flex;gap:8px;align-items:center;text-transform:none;letter-spacing:0;color:var(--ink)">
        <input type="checkbox" id="kDate" style="width:auto"> Date on every print</label>
      <label>Styles guests can choose (first ticked one starts selected)</label><div id="kThemes" class="row"></div>
      <label>Decorations guests can choose</label><div id="kDecos" class="row"></div>
      <div class="row" style="margin-top:14px">
        <button id="kSave">Save kiosk</button>
        <button class="ghost" id="kStart" style="flex:0 0 auto">Start kiosk on this screen</button>
      </div>
      <div class="meta" style="margin-top:8px">The kiosk is remembered on the screen it was started on
        only. Its &#128274; button returns here. The camera needs a one-off browser setting on the
        kiosk computer.</div>
    </details>
    <details style="margin-top:16px" id="storage">
      <summary>Storage and reset</summary>
      <div class="meta" id="stInfo" style="margin-top:8px"></div>
      <label class="chk" style="margin-top:10px"><input type="checkbox" id="stPrint"> Print images from storage (testing)</label>
      <div class="meta">Images go to the printer's storage first and print from there as one job, up to about
        900 mm at home and 260 mm online. Sending takes a little longer. Off by default; needs free storage.</div>
      <div class="row" style="margin-top:12px">
        <button class="ghost" id="stClear" style="flex:0 0 auto">Clear storage</button>
        <button class="ghost" id="stReset" style="flex:0 0 auto;color:var(--bad);border-color:var(--bad)">Factory reset</button>
      </div>
      <div class="meta" style="margin-top:8px">Clear storage removes drafts, saved and scheduled prints, the stored
        picture, print history and the saved log. Settings stay. Factory reset erases everything, including Wi-Fi
        and the online link, and starts setup again; online, it joins as a new device that needs approving.</div>
    </details>
  </details>
</div>
<!--/home-only-->
<div class="toast" id="toast"></div>
<script>
// Levels (v0.24): how much of the page this device shows. Nothing is
// removed: higher levels only reveal more. Kept in this browser ('level');
// a new device starts on Basic, one already in use (it has preferences)
// on Expert. Anything hidden goes back to its plain default, so a hidden
// choice never changes a print.
(() => {
  const el = id => document.getElementById(id);
  const LEVELS = [
    ['Basic', 'Type and print.'],
    ['Standard', 'Size, alignment, saved favourites, recent prints.'],
    ['Advanced', 'Photos, styles, drafts and scheduling.'],
    ['Expert', 'Photo fine-tuning, printer details' + (window.REMOTE ? '.' : ' and settings.')]];
  // control id, level that shows it, its plain value
  const PLAIN = [['tstyle', 3, 'plain'], ['align', 2, 'left'], ['size', 2, 'b2'], ['cut', 2, '1'], ['wwrap', 2, true]];
  let level = 1;
  try {
    const v = +localStorage.getItem('level');
    level = v >= 1 && v <= 4 ? v : (localStorage.getItem('prefs') || localStorage.getItem('kiosk') ? 4 : 1);
  } catch (e) {}

  function plain(){
    for (const [id, need, v] of PLAIN) {
      const c = el(id); if (!c || level >= need) continue;
      if (typeof v === 'boolean') { if (c.checked !== v) { c.checked = v; c.dispatchEvent(new Event('change', { bubbles: true })); } }
      else if (c.value !== v) { c.value = v; c.dispatchEvent(new Event('change', { bubbles: true })); }
    }
  }
  function apply(save){
    document.body.classList.remove('L1', 'L2', 'L3', 'L4');
    document.body.classList.add('L' + level);
    window.LEVEL = level;
    el('lvName').textContent = LEVELS[level - 1][0];
    el('lvBtn').querySelectorAll('.lvbars i').forEach((b, i) => b.classList.toggle('on', i < level));
    el('lvMenu').querySelectorAll('button').forEach(b => b.setAttribute('aria-checked', String(+b.dataset.l === level)));
    if (save) { try { localStorage.setItem('level', String(level)); } catch (e) {} }
    plain();
    window.dispatchEvent(new Event('levelchange'));
  }
  const close = () => { el('lvMenu').hidden = true; el('lvBtn').setAttribute('aria-expanded', 'false'); };
  document.addEventListener('DOMContentLoaded', () => {
    const m = el('lvMenu');
    m.innerHTML = LEVELS.map((x, i) => '<button type="button" role="menuitemradio" data-l="' + (i + 1) + '">'
      + '<span class="lvbars" aria-hidden="true">' + [1, 2, 3, 4].map(n => '<i' + (n <= i + 1 ? ' class="on"' : '') + '></i>').join('') + '</span>'
      + '<span><b>' + x[0] + '</b><small>' + x[1] + '</small></span></button>').join('')
      + '<div class="meta">Remembered on this device. Each level adds to the one before.</div>';
    m.addEventListener('click', e => {
      const b = e.target.closest('button'); if (!b) return;
      level = +b.dataset.l; apply(true); close();
      if (window.toast) window.toast(LEVELS[level - 1][0] + ' \u2014 ' + LEVELS[level - 1][1]);
    });
    el('lvBtn').onclick = e => { e.stopPropagation(); const open = m.hidden; m.hidden = !open; el('lvBtn').setAttribute('aria-expanded', String(open)); };
    document.addEventListener('click', e => { if (!m.hidden && !e.target.closest('.topR')) close(); });
    document.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
    apply(false);
  });
})();
</script>
<script>
// Online printing (v0.21), visitor side: the list of other printers when
// the page is opened through the website, and the status line.
(() => {
  const el = id => document.getElementById(id);
  document.addEventListener('DOMContentLoaded', () => {
    if (window.REMOTE) {
      el('others').style.display = 'none';
      (async () => {
        try {
          const list = await (await fetch('/_r/printers', { cache: 'no-store' })).json();
          if (!Array.isArray(list) || !list.length) return;
          const box = el('othersList');
          for (const p of list) {
            const r = document.createElement('div'); r.className = 'irow';
            const t = document.createElement('div'); t.className = 'it';
            const b = document.createElement('b'); b.textContent = p.name;
            const s = document.createElement('span'); s.textContent = p.online ? (p.paused ? 'Online, paused' : 'Online') : 'Offline';
            t.appendChild(b); t.appendChild(s); r.appendChild(t);
            const a = document.createElement('a'); a.className = 'btn'; a.textContent = 'Open';
            a.href = location.origin + '/p/' + encodeURIComponent(p.code) + '/';
            a.style.cssText = 'padding:6px 10px;border:1px solid var(--line);border-radius:6px;color:var(--ink);text-decoration:none;font-size:12px';
            r.appendChild(a); box.appendChild(r);
          }
          el('others').style.display = '';
        } catch (e) {}
      })();
    }
  });
  // The one-line status under the printer light (home network only).
  const prev = window.onStatus;
  window.onStatus = s => {
    if (prev) prev(s);
    const o = s.online, line = el('onlineLine');
    if (!o || !line) return;
    el('pausedNote').style.display = window.REMOTE && o.paused ? 'block' : 'none';
    line.style.display = o.state === 'off' || window.REMOTE ? 'none' : '';
    line.textContent = 'Online printing: ' + o.text;
  };
})();
</script>
<!--home-only-->
<script>
// Online printing (v0.21), Settings section: home network only.
(() => {
  const el = id => document.getElementById(id);
  const paint = o => {
    el('oState').textContent = 'Online printing: ' + o.text
      + (o.state === 'pending' ? '. A slip prints once it is approved.' : '');
    el('oOn').checked = o.enabled;
    el('oPause').checked = !!o.paused; el('oPause').disabled = !o.enabled;
    el('oName').textContent = 'Shown online as \u201c' + o.name + '\u201d (the Hostname above).';
    el('oVis').checked = o.visible; el('oSec').checked = o.secure;
    el('oSecHelp').style.display = o.secure ? '' : 'none';
    el('oForm').style.opacity = o.enabled ? 1 : 0.5;
    el('oLink').style.display = o.url ? '' : 'none';
    if (o.url) {
      el('oUrl').textContent = o.url; el('oUrl').href = o.url;
      const a = o.url.replace(/\/$/, '') + '-a';
      el('oAdmin').textContent = a; el('oAdmin').href = a;
      el('oQr').src = window.qrSrc(o.url); el('oQrA').src = window.qrSrc(a);
    }
    el('oCode').textContent = o.ownerCode || '';
  };
  async function load(){ try { paint(await window.api('/api/online')); } catch (e) {} }
  const post = (p, b) => window.api(p, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(b || {}) });
  document.addEventListener('DOMContentLoaded', () => {
    if (window.REMOTE || !el('oOn')) return;
    el('online').addEventListener('toggle', () => { if (el('online').open) load(); });
    el('oOn').onchange = () => { el('oForm').style.opacity = el('oOn').checked ? 1 : 0.5; };
    // Pausing takes effect straight away, without pressing Save.
    el('oPause').onchange = async () => {
      try { paint(await post('/api/online', { paused: el('oPause').checked }));
            window.toast(el('oPause').checked ? 'online printing paused' : 'online printing resumed'); }
      catch (e) { window.toast(e.message, true); }
    };
    el('oSave').onclick = async () => {
      try {
        paint(await post('/api/online', { enabled: el('oOn').checked, visible: el('oVis').checked,
          secure: el('oSec').checked }));
        window.toast('saved');
        setTimeout(load, 3000);
      } catch (e) { window.toast(e.message, true); }
    };
    el('oNewLink').onclick = async () => {
      if (!confirm('Make a new link? The old one stops working.')) return;
      try { paint(await post('/api/online/newlink')); window.toast('asking for a new link'); setTimeout(load, 3000); }
      catch (e) { window.toast(e.message, true); }
    };
    el('oNewOwner').onclick = async () => {
      if (!confirm('Make a new owner code? The old one stops working.')) return;
      try { paint(await post('/api/online/newowner')); window.toast('new owner code - a slip will print'); setTimeout(load, 4000); }
      catch (e) { window.toast(e.message, true); }
    };
  });
})();

// Storage and reset (v0.26), Settings section: home network only.
(() => {
  const el = id => document.getElementById(id);
  const kb = n => Math.round(n / 1024) + ' KB';
  const post = (p, b) => window.api(p, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(b || {}) });
  const paint = s => {
    el('stPrint').checked = !!s.printFromStorage;
    if (window.PRINTER) window.PRINTER.storePrint = !!s.printFromStorage;   // takes effect now
    el('stInfo').textContent = 'Storage: ' + kb(s.used) + ' used of ' + kb(s.total) + ', ' + kb(s.free) + ' free.'
      + (s.free < 64 * 1024 ? ' Nearly full: Clear storage frees it.' : '');
  };
  const load = async () => { try { paint(await window.api('/api/system/storage')); } catch (e) {} };
  document.addEventListener('DOMContentLoaded', () => {
    if (window.REMOTE || !el('stPrint')) return;
    el('storage').addEventListener('toggle', () => { if (el('storage').open) load(); });
    el('stPrint').onchange = async () => {
      try { paint(await post('/api/system/storage', { printFromStorage: el('stPrint').checked }));
            window.toast(el('stPrint').checked ? 'printing from storage' : 'printing from memory'); }
      catch (e) { window.toast(e.message, true); }
    };
    el('stClear').onclick = async () => {
      if (!confirm('Clear storage?\n\nDrafts, saved and scheduled prints, the stored picture, print history and the saved log are deleted. Settings stay.\n\nThe printer restarts.')) return;
      try { await post('/api/system/storage/clear'); window.toast('clearing storage - the printer restarts'); }
      catch (e) { window.toast(e.message, true); }
    };
    el('stReset').onclick = async () => {
      if (!confirm('Factory reset?\n\nEverything is erased: Wi-Fi, settings, online printing, drafts, saved and scheduled prints, history and logs.')) return;
      if (!confirm('Are you sure? This cannot be undone.\n\nThe printer restarts into setup and prints a setup slip. Online, it joins as a new device.')) return;
      try { await post('/api/system/reset'); window.toast('resetting - the printer restarts into setup'); }
      catch (e) { window.toast(e.message, true); }
    };
  });
})();
</script>
<!--/home-only-->
</body>
</html>
)HTML";

#!/usr/bin/env node
// build_pages.js - builds two stand-alone pages from the SAME page source the
// printer serves (the R"HTML(...)HTML" blocks in WebUI*.h). Nothing here is a
// second copy of the interface: change the page, run this, and both follow.
//
//   preview.html  The page as it will look after flashing. Opens from a
//                 double-click (no server). "Home" / "Online" switch at the
//                 top shows it as the owner at home or a visitor online.
//   demo.html     The public demo: always the visitor's (online) view, so
//                 Settings, owner code and online controls are absent.
//
// Both use the pretend printer in tools/fakeunit.js: every request the page
// makes is answered in the browser, nothing leaves it, and "printed" output
// appears on a strip of virtual paper instead.
//
// Usage: node SmartThermal/tools/build_pages.js [outDir]   (default: dist/ at the repo root)
// Needs only Node (any version from the last few years). No packages.
const fs = require('fs'), path = require('path');
const SRC = path.join(__dirname, '..');

function raw(file, name) {
  const s = fs.readFileSync(path.join(SRC, file), 'utf8');
  const m = s.match(new RegExp(name + '\\[\\] PROGMEM = R"HTML\\(([\\s\\S]*?)\\)HTML"'));
  if (!m) throw new Error(`${name} not found in ${file}`);
  return m[1];
}
// Same blocks, same order, as WebApi::handleIndex() sends them.
function assemble() {
  return raw('WebUI.h', 'WEB_UI_HEAD') + raw('WebUITemplate.h', 'WEB_UI_TEMPLATE')
    + raw('WebUITheme.h', 'WEB_UI_THEME') + raw('WebUIFuture.h', 'WEB_UI_FUTURE')
    + raw('WebUIImage.h', 'WEB_UI_IMAGE') + raw('WebUI.h', 'WEB_UI_TAIL');
}
function version() {
  const m = fs.readFileSync(path.join(SRC, 'Config.h'), 'utf8').match(/FIRMWARE_VERSION\s+"([^"]+)"/);
  return m ? m[1] : '0.0.0';
}

// ---------------------------------------------------------------- the shim
// Runs before the page's own scripts. Answers fetch() from the pretend printer.
const SHIM = (cfg) => `<script>
${fs.readFileSync(path.join(__dirname, 'fakeunit.js'), 'utf8')}
</script>
<script>
(() => {
  const CFG = ${JSON.stringify(cfg)};
  let online = CFG.demo;
  if (!CFG.demo) { try { online = new URLSearchParams(location.search).get('view') === 'online'; } catch (e) {} }
  window.SHOW_AS_ONLINE = online;
  const papers = [];
  const unit = FakeUnit.createUnit({ version: CFG.version, hostname: 'print', remote: online, demo: CFG.demo, home: CFG.home,
    onPrint: (p) => { papers.push(p); window.dispatchEvent(new Event('fake-print')); } });
  window.__fakeUnit = unit;

  const real = window.fetch ? window.fetch.bind(window) : null;
  window.fetch = async (input, init) => {
    init = init || {};
    const href = typeof input === 'string' ? input : input.url;
    if (/^(blob:|data:)/.test(href) && real) return real(input, init);
    const u = new URL(href, 'http://unit.local');
    if (u.host !== 'unit.local') return new Response('{"error":"offline preview"}', { status: 503 });
    const headers = {};
    try { new Headers(init.headers || {}).forEach((v, k) => { headers[k] = v; }); } catch (e) {}
    let text = null, file = null;
    const b = init.body;
    if (typeof b === 'string') text = b;
    else if (b instanceof FormData) { for (const [, v] of b) if (v instanceof Blob) { file = new Uint8Array(await v.arrayBuffer()); break; } }
    else if (b instanceof Blob) file = new Uint8Array(await b.arrayBuffer());
    else if (b instanceof ArrayBuffer || ArrayBuffer.isView(b)) file = new Uint8Array(b.buffer || b);
    const query = {}; u.searchParams.forEach((v, k) => { query[k] = v; });
    await new Promise(r => setTimeout(r, 120 + (file ? Math.min(800, file.length / 200) : 0)));   // feels like a real unit
    const r = unit.handle({ method: (init.method || 'GET').toUpperCase(), path: u.pathname, query, headers, text, file });
    if (r.bytes) return new Response(new Blob([r.bytes], { type: r.type }), { status: r.status });
    return new Response(JSON.stringify(r.json), { status: r.status, headers: { 'Content-Type': 'application/json' } });
  };

  // Links and buttons that would leave the page (log, kiosk) stay put.
  const note = (m) => { if (window.toast) window.toast(m); };
  document.addEventListener('click', (e) => {
    const a = e.target.closest && e.target.closest('a[href^="/"]');
    if (a) { e.preventDefault(); note('That opens on the printer itself.'); return; }
    if (e.target.closest && e.target.closest('#kStart')) { e.preventDefault(); e.stopPropagation(); note('Kiosk mode opens on the printer itself.'); }
  }, true);

  // ------------------------------------------------ the bar and the paper
  document.addEventListener('DOMContentLoaded', () => {
    const css = document.createElement('style');
    css.textContent = \`
      #pvBar{position:sticky;top:0;z-index:9998;display:flex;gap:.6rem;align-items:center;flex-wrap:wrap;
        padding:.5rem .9rem;background:#2D2C4C;color:#E2E1EF;font:600 13px/1.3 system-ui,sans-serif}
      #pvBar a{color:#E2E1EF;padding:.2rem .7rem;border-radius:999px;border:1px solid rgba(226,225,239,.4);text-decoration:none}
      #pvBar a.on{background:#E98D78;color:#1E1D35;border-color:transparent}
      #pvBar span{opacity:.8;font-weight:400}
      #pvTray{position:fixed;right:12px;bottom:58px;z-index:9999;font:13px system-ui,sans-serif}
      #pvTray>button{padding:.55rem .9rem;border-radius:999px;border:0;background:#2D2C4C;color:#fff;font:600 13px system-ui;cursor:pointer;box-shadow:0 4px 14px rgba(0,0,0,.25)}
      #pvPaper{display:none;position:absolute;right:0;bottom:46px;width:min(360px,calc(100vw - 24px));max-height:70vh;overflow:auto;
        background:#d9d6cf;border-radius:12px;padding:12px;box-shadow:0 10px 30px rgba(0,0,0,.3)}
      #pvTray.open #pvPaper{display:block}
      .pvSlip{background:#fff;color:#000;margin:0 auto 12px;width:100%;box-shadow:0 1px 3px rgba(0,0,0,.2);padding:10px 5% 0;container-type:inline-size}
      .pvSlip canvas{display:block;margin:0;image-rendering:pixelated}
      .pvFeed{height:24px}
      .pvText{font-family:"Courier New",monospace;white-space:pre-wrap;word-break:break-all;line-height:1.25}
      .pvCut{border-top:2px dashed #999;margin-top:10px}
      #pvEmpty{color:#444;text-align:center;padding:1rem}\`;
    document.head.appendChild(css);

    const bar = document.createElement('div'); bar.id = 'pvBar';
    if (CFG.demo) bar.innerHTML = '<b>Demo</b><span>A pretend printer: nothing is printed. Your printouts appear on the paper (bottom right).</span>';
    else bar.innerHTML = '<b>Preview ' + CFG.version + '</b>'
      + '<a href="?view=home" class="' + (online ? '' : 'on') + '">Home view</a>'
      + '<a href="?view=online" class="' + (online ? 'on' : '') + '">Online view</a>'
      + '<span>Nothing is sent to the printer.</span>';
    // The bar's own links must work, so they are not "leave the page" links.
    bar.addEventListener('click', (e) => { const a = e.target.closest('a'); if (a) { e.stopPropagation(); location.search = a.getAttribute('href'); e.preventDefault(); } }, true);
    document.body.prepend(bar);

    const tray = document.createElement('div'); tray.id = 'pvTray';
    tray.innerHTML = '<div id="pvPaper"><div id="pvEmpty">Nothing printed yet.</div></div><button type="button">Paper (0)</button>';
    document.body.appendChild(tray);
    const btn = tray.querySelector('button'), paper = tray.querySelector('#pvPaper');
    btn.onclick = () => tray.classList.toggle('open');

    // Like the real paper: one strip, printed top to bottom. A print without
    // a cut carries straight on into the next one (so a photo sent in parts
    // reads as one picture); a cut ends the piece and the next starts afresh.
    let piece = null, pieces = 0;
    const add = (node, cut) => {
      if (!piece) { piece = document.createElement('div'); piece.className = 'pvSlip'; paper.appendChild(piece); pieces++; }
      piece.appendChild(node);
      if (cut) { piece.appendChild(Object.assign(document.createElement('div'), { className: 'pvCut' })); piece = null; }
    };
    const show = (p) => {
      if (p.kind === 'raster' && p.data && p.width) {
        const c = document.createElement('canvas'); c.width = p.width; c.height = p.rows;
        const g = c.getContext('2d'), img = g.createImageData(p.width, p.rows), rb = p.width / 8;
        for (let y = 0; y < p.rows; y++) for (let x = 0; x < p.width; x++) {
          const on = (p.data[y * rb + (x >> 3)] >> (7 - (x & 7))) & 1, i = (y * p.width + x) * 4;
          img.data[i] = img.data[i + 1] = img.data[i + 2] = on ? 0 : 255; img.data[i + 3] = 255;
        }
        g.putImageData(img, 0, 0);
        c.style.width = (100 * p.width / 576) + '%';            // narrower prints sit at the left, as on paper
        add(c, p.cut);
      } else {
        const j = p.job || {};
        if (!j.text) { add(Object.assign(document.createElement('div'), { className: 'pvFeed' }), true); return; }
        const t = document.createElement('div'); t.className = 'pvText'; t.textContent = j.text;
        const scale = +j.size || 1, fontB = j.font === 1 || j.font === 'B';
        t.style.fontSize = (scale * (fontB ? 2.29 : 3.05)) + 'cqw';  // 48 (A) or 64 (B) characters across
        t.style.textAlign = j.align === 'center' ? 'center' : j.align === 'right' ? 'right' : 'left';
        add(t, j.cut !== false);
      }
    };
    window.addEventListener('fake-print', () => {
      const e = paper.querySelector('#pvEmpty'); if (e) e.remove();
      show(papers[papers.length - 1]);
      btn.textContent = 'Paper (' + papers.length + ')';
      tray.classList.add('open');
      paper.scrollTop = paper.scrollHeight;
    });
  });
})();
</script>`;

// Sample home-network data for the preview only (made-up values).
const HOME_SAMPLE = { host: 'print.example.com', url: 'https://print.example.com/p/k7f3q9/', ownerCode: 'R8XK2M', mac: '24:6F:28:AA:BB:CC' };
// The public demo must not contain these. All of them are checked against the
// page; the pretend printer (fakeunit.js, also in the file) is checked for the
// first four only - it names API paths such as /api/online so it can answer
// "local only", but carries no addresses, links or codes.
const NOT_IN_DEMO = [/justifiedsystems/i, /example\.com/i, /R8XK2M/, /k7f3q9/, /\/api\/online/, /\/api\/settings"/, /owner code/i];

function build(cfg) {
  let page = assemble();
  if (cfg.demo) {
    // Settings, online-printing set-up and kiosk set-up are marked home-only
    // in the page source and left out of the demo entirely.
    page = page.replace(/<!--home-only[^>]*-->[\s\S]*?<!--\/home-only-->/g, '');
    if (/home-only/.test(page)) throw new Error('unbalanced home-only markers');
  } else cfg = Object.assign({ home: HOME_SAMPLE }, cfg);
  const at = page.indexOf('<meta charset="utf-8">');
  if (at < 0) throw new Error('page head not found');
  const cut = at + '<meta charset="utf-8">'.length;
  let out = page.slice(0, cut) + '\n' + SHIM(cfg) + page.slice(cut);
  if (cfg.demo) {
    // The page part: none of it. The pretend printer (shim): no sample data.
    for (const re of NOT_IN_DEMO) if (re.test(page)) throw new Error('demo page would contain ' + re);
    for (const re of NOT_IN_DEMO.slice(0, 4)) if (re.test(out)) throw new Error('demo.html would contain ' + re);
  }
  return out;
}

if (require.main === module) {
  const outDir = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'dist'));
  fs.mkdirSync(outDir, { recursive: true });
  const v = version();
  fs.writeFileSync(path.join(outDir, 'preview.html'), build({ version: v, demo: false }));
  fs.writeFileSync(path.join(outDir, 'demo.html'), build({ version: v, demo: true }));
  console.log(`built preview.html and demo.html (firmware ${v}) in ${outDir}`);
}

module.exports = { assemble, version, build };

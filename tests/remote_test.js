require('fs').mkdirSync(require('path').join(__dirname, 'out'), { recursive: true });
// End to end through the stand-in relay: page at /p/<code>/, served by the
// unit's relay session replaying to the mock unit.
const puppeteer = require('puppeteer-core');
const fs = require('fs');
const sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0; const check = (n, ok, x) => { if (!ok) fails++; console.log((ok ? 'ok   ' : 'FAIL ') + n + (x ? '  ' + x : '')); };
const codeOf = f => (fs.readFileSync('relayhost/' + f, 'utf8').match(/STATE on ([a-z0-9]{6})/) || [])[1];
const MOCK = 'http://localhost:8765';
(async () => {
  const A = codeOf(process.env.LOGA || 'unitA.log'), Bc = codeOf(process.env.LOGB || 'unitB.log');
  const BASE = (process.env.RELAY || 'http://localhost:8790') + '/p/' + A + '/';
  await fetch(MOCK + '/__reset');
  const br = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox', '--no-proxy-server'] });
  const p = await br.newPage(); const errs = []; p.on('pageerror', e => errs.push(e.message));
  p.on('console', m => { if (/Content Security Policy|Refused to/i.test(m.text())) errs.push('CSP: ' + m.text().slice(0, 160)); }); p.on('dialog', d => d.accept());
  await p.setViewport({ width: 390, height: 820, deviceScaleFactor: 2 });
  const t0 = Date.now(); await p.goto(BASE); await sleep(1500);
  check('page loads through the website', await p.evaluate(() => window.REMOTE === true && window.BASE.length === 9), (Date.now() - t0) + ' ms');
  check('status reached the unit (green light)', await p.$eval('#dot', e => e.className.includes('green')));
  check('Settings, Recent jobs and online line hidden', await p.evaluate(() =>
    [...document.querySelectorAll('.localonly')].every(e => getComputedStyle(e).display === 'none')));
  check('Preferences shown', await p.evaluate(() => !!document.querySelector('summary') && [...document.querySelectorAll('summary')].some(s => s.textContent.startsWith('Preferences') && s.offsetParent)));
  const others = await p.$$eval('#othersList .irow b', r => r.map(x => x.textContent));
  check('other visible printers listed', others.length === 1, others.join(','));
  check('other printer link points at its own code', await p.$eval('#othersList a', (a, c) => a.href.endsWith('/p/' + c + '/'), Bc));
  // Plain text print
  await p.$eval('#text', e => { e.value = 'Hello from afar'; e.dispatchEvent(new Event('input')); });
  await p.$eval('#print', e => e.click()); await sleep(1500);
  let st = await (await fetch(MOCK + '/__state')).json();
  check('text print arrives', st.jobs.some(j => j.type === 'text' && j.body.text.includes('Hello from afar')));
  check('labelled as online, who=guest', st.lastRelay && st.lastRelay.who === 'guest', JSON.stringify(st.lastRelay));
  // Long list: raster parts uploaded through the relay
  await p.select('#tstyle', 'list');
  const items = Array.from({ length: 70 }, (_, i) => 'Item number ' + (i + 1)).join('\n');
  await p.$eval('#text', (e, v) => { e.value = 'Big list\n' + v; e.dispatchEvent(new Event('input')); }, items);
  await sleep(500);
  const before = st.jobs.length;
  await p.$eval('#print', e => e.click());
  for (let i = 0; i < 40; i++) { await sleep(500); if (await p.$eval('#print', e => !e.disabled)) break; }
  st = await (await fetch(MOCK + '/__state')).json();
  const parts = st.jobs.slice(before);
  check('multi-part list uploaded through the website', parts.length >= 2 && parts.every(j => j.type === 'raster' && j.body > 1000),
        parts.map(j => j.body).join('+') + ' bytes');
  // Save + drawer + draft autosave all through the relay
  await p.$eval('#tsave', e => e.click()); await sleep(800);
  await p.$eval('#tdraft', e => e.click()); await sleep(800);
  await p.$eval('#text', e => { e.value = 'Big list\nOnly this now'; e.dispatchEvent(new Event('input', { bubbles: true })); });
  await sleep(2200);
  st = await (await fetch(MOCK + '/__state')).json();
  check('saved + draft stored via website', st.items.s.length === 1 && st.items.d.length === 1);
  check('draft autosaves via website', st.items.d[0].src.text === 'Big list\nOnly this now', st.items.d[0].rev + ' revs');
  // Local-only refused through the relay, even if asked directly
  const r1 = await p.evaluate(async () => (await fetch('/api/settings', { method: 'POST', body: '{}' })).status);
  const r2 = await p.evaluate(async () => (await fetch('/log')).status);
  check('Settings and log refused through the website', r1 === 403 && r2 === 403, r1 + ' ' + r2);
  await p.screenshot({ path: require('path').join(__dirname, 'out', 'remote.png') });
  check('no page errors', !errs.length, errs.join(' / '));
  console.log(fails ? '\n' + fails + ' FAILED' : '\nall passed');
  await br.close();
})().catch(e => { console.error(e); process.exit(1); });

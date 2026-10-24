// preview.html (home + online views) and demo.html open from file://, print
// text and a photo to the virtual paper, and show the right features.
const puppeteer = require('puppeteer-core'); const sleep = ms => new Promise(r => setTimeout(r, ms));
require('fs').mkdirSync(require('path').join(__dirname, 'out'), { recursive: true });
const OUT = require('path').join(__dirname, '..', 'dist') + '/';  // node SmartThermal/tools/build_pages.js
let fails = 0; const check = (ok, m) => { console.log((ok ? 'ok   ' : 'FAIL ') + m); if (!ok) fails++; };
(async () => {
  const b = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox', '--allow-file-access-from-files'] });
  for (const [name, file, online] of [['preview home', 'preview.html', false], ['preview online', 'preview.html?view=online', true], ['demo', 'demo.html', true]]) {
    const p = await b.newPage(); const errs = []; const net = []; await p.evaluateOnNewDocument(() => { try { localStorage.setItem('level', '4'); } catch (e) {} });
    p.on('pageerror', e => errs.push(e.message)); p.on('requestfailed', r => net.push(r.url()));
    p.on('request', r => { if (!/^(file|data|blob):/.test(r.url())) net.push(r.url()); });
    await p.setViewport({ width: 420, height: 900 });
    await p.goto(require('url').pathToFileURL(OUT).href + file); await sleep(1500);
    const vis = id => p.$eval('#' + id, e => !!(e.offsetWidth || e.offsetHeight)).catch(() => false);
    check(await p.$eval('#pvBar', e => !!e), name + ': bar shown');
    check((await vis('oOn')) === !online, name + ': online settings ' + (online ? 'hidden' : 'shown'));
    await p.$eval('#text', e => { e.value = 'Milk\nEggs\nBread'; e.dispatchEvent(new Event('input')); });
    await p.$eval('#print', e => e.click()); await sleep(1500);
    const slips = await p.$$eval('.pvSlip', s => s.length);
    check(slips >= 1, name + ': text printed to virtual paper (' + slips + ')');
    const inp = await p.$('#ifile'); await inp.uploadFile('photo.jpg'); await sleep(2500);
    await p.$eval('#iprint', e => e.click());
    for (let i = 0; i < 30; i++) { await sleep(1000); const t = await p.$eval('#iprog', e => e.textContent); if (/printed|failed/.test(t)) break; }
    check(/printed/.test(await p.$eval('#iprog', e => e.textContent)), name + ': photo printed');
    check(await p.$$eval('.pvSlip canvas', c => c.length) >= 1, name + ': photo on paper');
    if (name === 'demo') { const h = await p.content(); check(!/R8XK2M|k7f3q9/.test(await p.evaluate(() => document.body.innerText)), 'demo: no owner code or link shown'); }
    check(!net.length, name + ': no network requests' + (net.length ? ' ' + net.slice(0, 3) : ''));
    check(!errs.length, name + ': no page errors' + (errs.length ? ' ' + errs[0] : ''));
    await p.screenshot({ path: require('path').join(__dirname, 'out', 'pv_' + name.replace(' ', '_') + '.png'), fullPage: false });
    await p.close();
  }
  await b.close(); console.log(fails ? fails + ' failed' : 'all passed'); process.exit(fails ? 1 : 0);
})();

// v0.24 levels: what each level shows, plain fallback for hidden choices,
// remembered per device, fresh device starts on Basic.
const puppeteer = require('puppeteer-core'); const sleep = ms => new Promise(r => setTimeout(r, ms));
require('fs').mkdirSync(require('path').join(__dirname, 'out'), { recursive: true });
const OUT = require('path').join(__dirname, '..', 'dist') + '/';  // node SmartThermal/tools/build_pages.js
let fails = 0; const check = (ok, m) => { console.log((ok ? 'ok   ' : 'FAIL ') + m); if (!ok) fails++; };
const SHOW = {   // element -> first level that shows it (home view)
  text: 1, print: 1, dot: 1, lvBtn: 1,
  size: 2, align: 2, cut: 2, wwrap: 2, tsave: 2, jobsBox: 2, sub: 2, drawer: 2,
  tstyle: 3, futbox: 3, ifile: 0, drop: 3, tdraft: 3, tsched: 3, feedcut: 3, iwidth: 3,
  ibright: 4, iinv: 4, paper: 0, save: 4 };
(async () => {
  const b = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox'] });
  const p = await b.newPage(); const errs = []; p.on('pageerror', e => errs.push(e.message));
  await p.setViewport({ width: 420, height: 900 });
  await p.goto(require('url').pathToFileURL(OUT).href + 'preview.html?view=home'); await p.evaluate(() => localStorage.clear()); await p.reload(); await sleep(1200);
  check(await p.$eval('#lvName', e => e.textContent) === 'Basic', 'fresh device starts on Basic');
  const vis = id => p.$eval('#' + id, e => { let n = e; while (n) { if (getComputedStyle(n).display === 'none') return false; n = n.parentElement; } return true; }).catch(() => null);
  for (let L = 1; L <= 4; L++) {
    await p.$eval('#lvBtn', e => e.click()); await sleep(100);
    await p.$eval('#lvMenu button[data-l="' + L + '"]', e => e.click()); await sleep(300);
    const bad = [];
    for (const [id, need] of Object.entries(SHOW)) { if (!need) continue; const v = await vis(id); if (v !== (L >= need)) bad.push(id + '=' + v); }
    check(!bad.length, 'level ' + L + ': right things shown' + (bad.length ? ' (' + bad.join(', ') + ')' : ''));
  }
  // At Expert pick a style and size, then drop to Basic: both go back to plain.
  await p.select('#tstyle', 'list'); await p.select('#size', 'a3'); await p.select('#align', 'center');
  await p.$eval('#ibright', e => { e.value = 40; e.dispatchEvent(new Event('input')); });
  await p.$eval('#lvBtn', e => e.click()); await p.$eval('#lvMenu button[data-l="1"]', e => e.click()); await sleep(300);
  check(await p.$eval('#tstyle', e => e.value) === 'plain' && await p.$eval('#size', e => e.value) === 'b2' && await p.$eval('#align', e => e.value) === 'left', 'Basic: hidden choices back to plain');
  check(await p.$eval('#ibright', e => e.value) === '0', 'below Expert: photo fine-tuning back to defaults');
  await p.reload(); await sleep(1000);
  check(await p.$eval('#lvName', e => e.textContent) === 'Basic', 'level remembered after reload');
  // Basic print: plain text
  await p.$eval('#text', e => { e.value = 'Milk\nEggs'; e.dispatchEvent(new Event('input')); });
  await p.$eval('#print', e => e.click()); await sleep(1200);
  const job = await p.evaluate(() => { const j = window.__fakeUnit.state.jobs; return j[j.length - 1]; });
  check(job && job.type === 'text' && job.body.text.includes('Milk'), 'Basic prints plain text');
  // Paste of a list-like text does not switch to List on Basic
  await p.$eval('#text', e => { e.value = ''; });
  await p.evaluate(() => { const dt = new DataTransfer(); dt.setData('text', '- a\n- b\n- c'); const t = document.getElementById('text'); t.value = '- a\n- b\n- c'; t.dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true })); });
  await sleep(200);
  check(await p.$eval('#tstyle', e => e.value) === 'plain', 'Basic: pasted list stays plain');
  // existing device (has prefs) starts on Expert
  await p.evaluate(() => { localStorage.clear(); localStorage.setItem('prefs', '{}'); }); await p.reload(); await sleep(1000);
  check(await p.$eval('#lvName', e => e.textContent) === 'Expert', 'device already in use starts on Expert');
  // online view: Expert has no Settings
  await p.goto(require('url').pathToFileURL(OUT).href + 'preview.html?view=online'); await sleep(1200);
  check(await vis('save') === false && await vis('ibright') === true, 'online Expert: fine-tuning yes, Settings no');
  for (const L of [1, 2, 3, 4]) {
    await p.$eval('#lvBtn', e => e.click()); await p.$eval('#lvMenu button[data-l="' + L + '"]', e => e.click()); await sleep(200);
    await p.screenshot({ path: require('path').join(__dirname, 'out', 'lv' + L + '.png'), fullPage: true });
  }
  await p.goto(require('url').pathToFileURL(OUT).href + 'preview.html?view=home'); await sleep(1000);
  await p.$eval('#lvBtn', e => e.click()); await sleep(200);
  await p.screenshot({ path: require('path').join(__dirname, 'out', 'lvmenu.png') });
  check(!errs.length, 'no page errors' + (errs.length ? ' ' + errs[0] : ''));
  await b.close(); console.log(fails ? fails + ' failed' : 'all passed'); process.exit(fails ? 1 : 0);
})();

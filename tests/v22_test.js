// v0.22 through the real relay code: history, confirmations, draft links,
// clearer errors, no home-only calls from the online page.
const puppeteer = require('puppeteer-core'); const fs = require('fs');
const sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0; const check = (n, ok, x) => { if (!ok) fails++; console.log((ok ? 'ok   ' : 'FAIL ') + n + (x ? '  ' + x : '')); };
const MOCK = 'http://localhost:8765', R = 'http://localhost:8787';
(async () => {
  const code = fs.readFileSync('relayhost/v22A.log', 'utf8').match(/STATE on ([a-z0-9]{6})/)[1];
  await fetch(MOCK + '/__reset');
  const br = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox', '--no-proxy-server'] });
  const ctx = br.defaultBrowserContext(); await ctx.overridePermissions(R, ['clipboard-read', 'clipboard-write']);
  const p = await br.newPage(); const errs = [], reqs = [];
  p.on('pageerror', e => errs.push(e.message)); p.on('dialog', d => d.accept());
  p.on('console', m => { if (/Content Security Policy|Refused to/i.test(m.text())) errs.push('CSP: ' + m.text().slice(0, 160)); });
  p.on('request', r => reqs.push(r.url().replace(/^.*\/p\/[a-z0-9]{6}/, '')));
  await p.setViewport({ width: 390, height: 820, deviceScaleFactor: 2 });
  await p.goto(R + '/p/' + code + '/'); await sleep(1500);
  await p.evaluate(() => { const t = window.toast; window.toast = (m, b) => { (window.__t = window.__t || []).push((b ? '!' : '') + m); t(m, b); }; });
  check('no home-only requests from the online page', !reqs.some(u => /api\/settings|\/log/.test(u)), reqs.filter(u => /settings|log/.test(u)).join(','));
  // Print + confirmation
  await p.$eval('#text', e => { e.value = 'Milk please'; e.dispatchEvent(new Event('input')); });
  await p.$eval('#print', e => e.click()); await sleep(4500);
  const ts = await p.evaluate(() => window.__t || []);
  check('print confirmed on the page', ts.some(t => /^Printed ✓ \(job #\d+\)/.test(t)), ts.join(' | '));
  // History card online
  check('Recent prints card shown online', await p.evaluate(() => getComputedStyle(document.getElementById('jobsBox').closest('.card')).display !== 'none'));
  await p.evaluate(() => { const d = document.getElementById('jobsBox'); d.open = true; d.dispatchEvent(new Event('toggle')); }); await sleep(2500);
  const row = await p.$eval('#jobs', e => e.textContent);
  check('history lists the print, labelled online, no address', /done/.test(row) && /online/.test(row) && !/\d+\.\d+\.\d+\.\d+/.test(row), row.slice(0, 80));
  check('Clear history hidden online', await p.$eval('#clearjobs', e => getComputedStyle(e).display === 'none'));
  // Draft link
  await p.select('#tstyle', 'list');
  await p.$eval('#text', e => { e.value = 'Shared shop\nBread\nEggs'; e.dispatchEvent(new Event('input')); });
  await p.$eval('#tdraft', e => e.click()); await sleep(1200);
  await p.goto(R + '/p/' + code + '/#future'); await sleep(2000);
  await p.evaluate(() => [...document.querySelectorAll('#fdList button')].find(b => b.textContent === 'Copy link').click()); await sleep(800);
  const link = await p.evaluate(() => navigator.clipboard.readText());
  check('Copy link gives the printer link + #draft-N', new RegExp('/p/' + code + '/#draft-\\d+$').test(link), link);
  const q = await br.newPage(); await q.goto(link); await sleep(2500);
  check('opening the link opens that draft, linked', await q.evaluate(() => document.getElementById('text').value.startsWith('Shared shop') && document.getElementById('dBadge').style.display !== 'none'),
        await q.$eval('#dBadge', e => e.textContent));
  // Clearer error text: a relay refusal reads as status + words
  const msg = await p.evaluate(async () => { try { await window.api('/api/print/raw', { method: 'POST', body: 'x' }); } catch (e) { return e.message; } });
  check('relay refusals show their words', /^HTTP 403: Forbidden/.test(msg), msg);
  check('no page or CSP errors', !errs.filter(e => !/Failed to load resource/.test(e)).length, errs.join(' / '));
  await br.close();
  const log = fs.readFileSync('relayhost/v22A.log', 'utf8');
  check('unit log has a line per POST', /NOTE POST \/api\/print\/text -> 200 \(\d+ KB in, \d+ ms\)/.test(log), (log.match(/NOTE (GET|POST).*/g) || []).slice(0, 4).join(' | '));
  console.log(fails ? '\n' + fails + ' FAILED' : '\nall passed');
})().catch(e => { console.error(e); process.exit(1); });

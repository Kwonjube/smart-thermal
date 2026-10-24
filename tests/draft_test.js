// Drafts, v0.23: saved only with "Update draft" (owner's choice C).
const puppeteer = require('puppeteer-core');
const B = 'http://localhost:8765', sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0; const check = (n, ok, x) => { if (!ok) fails++; console.log((ok ? 'ok   ' : 'FAIL ') + n + (x ? '  ' + x : '')); };
const state = async () => (await fetch(B + '/__state')).json();
const type = (p, v) => p.$eval('#text', (e, v) => { e.value = v; e.dispatchEvent(new Event('input', { bubbles: true })); }, v);
(async () => {
  await fetch(B + '/__reset');
  const br = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox'] });
  const mk = async () => { const p = await br.newPage(); await p.evaluateOnNewDocument(() => { try { localStorage.setItem('level', '4'); } catch (e) {} }); p.dialogs = []; p.on('dialog', d => { p.dialogs.push(d.message()); d.accept(); });
    const errs = []; p.on('pageerror', e => errs.push(e.message)); p.errs = errs;
    await p.setViewport({ width: 390, height: 820, deviceScaleFactor: 2 }); await p.goto(B + '/'); await sleep(900); return p; };
  const A = await mk();
  await A.select('#tstyle', 'list'); await type(A, 'Groceries\nMilk\nBread'); await sleep(200);
  await A.$eval('#tdraft', e => e.click()); await sleep(600);
  let s = await state(), d = s.items.d[0];
  check('Keep as draft creates it and links the card', d && d.rev === 1 && await A.$eval('#dBar', e => e.style.display !== 'none'));
  check('button becomes "Update draft"', await A.$eval('#tdraft', e => e.textContent) === 'Update draft');
  check('bar says all changes saved', /all changes saved/.test(await A.$eval('#dBarText', e => e.textContent)));
  await type(A, 'Groceries\nMilk\nBread\nEggs'); await sleep(2500);
  s = await state(); d = s.items.d[0];
  check('typing does NOT save by itself', d.rev === 1 && !d.src.text.includes('Eggs'));
  check('bar says unsaved changes', /unsaved changes/.test(await A.$eval('#dBarText', e => e.textContent)));
  await A.$eval('#tdraft', e => e.click()); await sleep(700);
  s = await state(); d = s.items.d[0];
  check('Update draft saves into the same draft', s.items.d.length === 1 && d.rev === 2 && d.src.text.endsWith('Eggs'));
  check('bar back to all changes saved', /all changes saved/.test(await A.$eval('#dBarText', e => e.textContent)));
  // Second device
  const Bp = await mk();
  await Bp.goto(B + '/#future'); await sleep(1000);
  await Bp.$eval('#fdList .irow button', e => e.click()); await sleep(700);
  await type(Bp, 'Groceries\nMilk\nBread\nEggs\nTea'); await Bp.$eval('#tdraft', e => e.click()); await sleep(700);
  s = await state(); d = s.items.d[0];
  check('other device: one edit for its visit', d.edits === 1 && d.rev === 3, 'edits ' + d.edits);
  await A.bringToFront(); await sleep(4600);
  check('unchanged device picks up the update', await A.$eval('#text', e => e.value.endsWith('Tea')));
  // Unsaved + someone else updates -> warning, nothing lost
  await type(A, 'Groceries\nMilk\nBread\nEggs\nTea\nButter');
  await fetch(B + '/__edit?id=' + d.id, { method: 'POST', body: JSON.stringify({ src: Object.assign({}, d.src, { text: d.src.text + '\nOnions' }), title: 'Groceries' }) });
  await sleep(4600);
  const w = await A.$eval('#dWarn', e => e.style.display !== 'none' ? e.textContent : '');
  check('unsaved + someone else updated: warned, my text kept', /Updating now will replace/.test(w) && await A.$eval('#text', e => e.value.endsWith('Butter')), w.slice(0, 60));
  // Close with unsaved changes asks first
  A.dialogs.length = 0;
  await A.$eval('#dClose', e => e.click()); await sleep(300);
  check('Close asks before discarding', A.dialogs.some(m => /not saved/.test(m)) && await A.$eval('#dBar', e => e.style.display === 'none'));
  // Quick print still leaves the card alone
  await fetch(B + '/__reset');
  await A.goto(B + '/'); await sleep(800);
  await fetch(B + '/api/items?kind=d', { method: 'POST', body: JSON.stringify({ title: 'Hardware', style: 'list', sess: 'x', src: { style: 'list', text: 'Hardware\nScrews', size: 'b2', align: 'left', cut: '1' } }) });
  await A.goto(B + '/#future'); await sleep(900); await type(A, 'my words');
  await A.evaluate(() => [...document.querySelectorAll('#fdList .irow button')].find(b => b.textContent === 'Print').click()); await sleep(1500);
  s = await state();
  check('draft Print prints and leaves the card', s.jobs.length === 1 && await A.$eval('#text', e => e.value) === 'my words');
  check('no page errors', !A.errs.length && !Bp.errs.length, A.errs.concat(Bp.errs).join(' / '));
  console.log(fails ? '\n' + fails + ' FAILED' : '\nall passed');
  await br.close();
})().catch(e => { console.error(e); process.exit(1); });

require('fs').mkdirSync(require('path').join(__dirname, 'out'), { recursive: true });
const puppeteer = require('puppeteer-core');
const BASE = 'http://localhost:8765';
const sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0; const check = (name, ok, extra) => { if (!ok) fails++; console.log((ok ? 'ok   ' : 'FAIL ') + name + (extra ? '  ' + extra : '')); };
const state = async () => (await fetch(BASE + '/__state')).json();
(async () => {
  await fetch(BASE + '/__reset');
  const b = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox'] });
  const p = await b.newPage(); await p.evaluateOnNewDocument(() => { try { localStorage.setItem('level', '4'); } catch (e) {} });
  const errs = []; p.on('pageerror', e => errs.push(e.message)); p.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  p.on('dialog', d => d.accept());
  const toasts = []; 
  await p.setViewport({ width: 400, height: 860, deviceScaleFactor: 2 });
  await p.goto(BASE + '/'); await sleep(800);
  await p.evaluate(() => { const t = window.toast; window.toast = (m, bad) => { (window.__t = window.__t || []).push((bad ? '!' : '') + m); t(m, bad); }; });
  const lastToast = () => p.evaluate(() => (window.__t || []).slice(-1)[0]);
  await p.screenshot({ path: require('path').join(__dirname, 'out', 'shot_main.png') });

  // List -> draft, save
  await p.select('#tstyle', 'list');
  await p.$eval('#text', e => { e.value = 'Groceries\nMilk\nBread\nEggs'; e.dispatchEvent(new Event('input')); });
  await sleep(300);
  await p.$eval('#tdraft', e => e.click()); await sleep(400);
  let s = await state();
  check('draft stored', s.items.d.length === 1 && s.items.d[0].src.text.startsWith('Groceries') && s.items.d[0].title === 'Groceries', await lastToast());
  check('future box shows 1 draft in red', await p.$eval('#futD', e => e.textContent === '1' && e.className === 'on'));
  await p.$eval('#dClose', e => e.click()); await p.$eval('#tdraft', e => e.click()); await sleep(300);
  s = await state(); check('Close, then Keep as draft makes a new draft', s.items.d.length === 2);
  await p.$eval('#tsave', e => e.click()); await sleep(400);
  s = await state(); check('saved stored', s.items.s.length === 1, await lastToast());
  check('drawer title', await p.$eval('#dhTitle', e => e.textContent) === 'Saved (1/20)');

  // Parking fine save
  await p.select('#tstyle', 'fine'); await sleep(300);
  await p.$eval('#tsave', e => e.click()); await sleep(400);
  s = await state(); check('fine saved with form values', s.items.s[1] && s.items.s[1].src.fine.plate === 'CUD 4ME', s.items.s[1] && s.items.s[1].title);

  // Drawer open, load list item into card
  await p.select('#tstyle', 'plain');
  await p.$eval('#text', e => { e.value = ''; e.dispatchEvent(new Event('input')); });
  await p.$eval('#dhTitle', e => e.click()); await sleep(500);
  const rows = await p.$$eval('#dbList .irow b', r => r.map(x => x.textContent));
  check('drawer lists newest first', rows.length === 2 && rows[1] === 'Groceries', rows.join(' | '));
  await p.screenshot({ path: require('path').join(__dirname, 'out', 'shot_drawer.png') });
  await p.evaluate(() => [...document.querySelectorAll('#dbList .irow')].find(r => r.textContent.includes('Groceries')).querySelector('.it').click());
  await sleep(500);
  check('tap loads into Text card', await p.evaluate(() => document.getElementById('tstyle').value === 'list' && document.getElementById('text').value.startsWith('Groceries')));
  check('drawer closes after loading', await p.$eval('#drawer', e => !e.classList.contains('open')));

  // Quick print
  await p.$eval('#dhTitle', e => e.click()); await sleep(400);
  const before = (await state()).jobs.length;
  await p.evaluate(() => [...document.querySelectorAll('#dbList .irow')].find(r => r.textContent.includes('Groceries')).querySelector('button').click());
  await sleep(1500);
  s = await state(); check('quick print sends the slip', s.jobs.length > before, (s.jobs.length - before) + ' part(s)');

  // Schedule a List on weekdays
  await p.$eval('#tsched', e => e.click()); await sleep(200);
  await p.select('#sRep', 'days'); await p.$eval('#sAt', e => { e.value = '07:00'; e.dispatchEvent(new Event('change')); });
  await sleep(200);
  const nextTxt = await p.$eval('#sNext', e => e.textContent);
  check('schedule panel shows first print', /First print: \w+day/.test(nextTxt), nextTxt);
  await p.screenshot({ path: require('path').join(__dirname, 'out', 'shot_sched.png'), clip: await p.$eval('#tcv', e => { const r = e.closest('.card').getBoundingClientRect(); return { x: r.x, y: r.y + window.scrollY + r.height - 470, width: r.width, height: 470 }; }) }).catch(()=>{});
  const card = await p.$('#schedPanel'); await card.evaluate(e => e.closest('.card').scrollIntoView({block:'end'})); await sleep(100);
  await p.screenshot({ path: require('path').join(__dirname, 'out', 'shot_sched.png') });
  await p.$eval('#sGo', e => e.click()); await sleep(800);
  s = await state(); const it = s.items.p[0];
  check('scheduled item stored', !!it, await lastToast());
  if (it) {
    const dateStep = it.seq.findIndex(e => e.d);
    check('date left for the printer', dateStep > 0 && it.seq[dateStep].f === 1, JSON.stringify(it.seq));
    check('slip uploaded and ready', it.ready && s.bins[it.id] === it.bytes, it.bytes + ' bytes');
    check('rule kept', it.rep === 'days' && it.days === 62 && it.at === '07:00');
  }
  // Plain schedule
  await p.select('#tstyle', 'plain');
  await p.$eval('#text', e => { e.value = 'Bins out tonight!'; e.dispatchEvent(new Event('input')); });
  await p.$eval('#tsched', e => e.click()); await sleep(200);
  await p.select('#sRep', 'daily'); await p.$eval('#sAt', e => { e.value = '18:30'; e.dispatchEvent(new Event('change')); });
  await p.$eval('#sGo', e => e.click()); await sleep(600);
  s = await state(); const pl = s.items.p[1];
  check('plain schedule is printer text', pl && pl.seq.length === 1 && pl.seq[0].x === 'Bins out tonight!' && pl.ready, pl && JSON.stringify(pl.seq));
  check('future box shows 2 scheduled', await p.$eval('#futP', e => e.textContent) === '2');

  // Future view
  await p.$eval('#futbox', e => e.click()); await sleep(800);
  check('future view hides main cards', await p.evaluate(() => getComputedStyle(document.getElementById('text').closest('.card')).display === 'none'
       && getComputedStyle(document.getElementById('futView')).display === 'block'));
  const fp = await p.$$eval('#fpList .irow .it span', r => r.map(x => x.textContent));
  check('scheduled rows describe the rule', fp.some(t => t.startsWith('Weekdays at 07:00')) && fp.some(t => t.startsWith('Every day at 18:30')), fp.join(' | '));
  check('drafts listed', (await p.$$('#fdList .irow')).length === 2);
  await p.screenshot({ path: require('path').join(__dirname, 'out', 'shot_future.png'), fullPage: true });
  // Open a draft from Future
  await p.evaluate(() => document.querySelector('#fdList .irow button').click()); await sleep(600);
  check('open draft returns to main with it loaded', await p.evaluate(() => !document.body.classList.contains('fut') && document.getElementById('text').value.startsWith('Groceries')));

  // From image: synthetic screenshot, keep as draft, restore with edits
  await p.evaluate(() => {
    const c = document.createElement('canvas'); c.width = 700; c.height = 520; const x = c.getContext('2d');
    x.fillStyle = '#fff'; x.fillRect(0, 0, 700, 520); x.fillStyle = '#111'; x.font = 'bold 34px Arial'; x.fillText('Pancakes', 30, 50);
    x.font = '24px Arial'; ['2 cups flour', '1 egg', '1 1/2 cups milk', '2 tbsp sugar', 'Pinch of salt'].forEach((t, i) => x.fillText('• ' + t, 30, 110 + i * 44));
    x.font = 'bold 30px Arial'; x.fillText('Method', 30, 360); x.font = '24px Arial'; x.fillText('Whisk everything together.', 30, 410); x.fillText('Fry in a hot pan.', 30, 454);
    window.fromImage.set(c, 1);
  });
  await sleep(300);
  // tap out the second found line
  const st0 = await p.evaluate(() => window.fromImage.state());
  await p.evaluate(() => { const s = window.fromImage.state(); });
  await p.evaluate(() => { const c = document.getElementById('tcv'); });
  await p.$eval('#text', e => { e.value = 'Sunday breakfast'; e.dispatchEvent(new Event('input')); }); await sleep(300);
  // drop found line index 2 via the internal toggle, by simulating a click on its span
  const dropped = await p.evaluate(() => {
    const c = document.getElementById('tcv'); const r = c.getBoundingClientRect();
    // click about 40% down the canvas
    c.dispatchEvent(new MouseEvent('click', { bubbles: true, clientX: r.x + 50, clientY: r.y + r.height * 0.42 }));
    return window.fromImage.state();
  });
  await sleep(200);
  const st1 = await p.evaluate(() => window.fromImage.state());
  await p.evaluate(() => { const c = document.getElementById('dClose'); if (c && c.offsetParent) c.click(); });
  await p.$eval('#tdraft', e => e.click()); await sleep(1500);
  s = await state(); const im = s.items.d.find(i => i.style === 'image');
  check('image draft stored with picture', !!im && s.pic === 'd' + im.id && s.picBytes > 1000, s.picBytes + ' bytes jpeg; state ' + JSON.stringify(st1));
  await p.$eval('#tsave', e => e.click()); await sleep(800);
  check('second picture refused', /Only one picture/.test(await lastToast()), await lastToast());
  // clear and restore
  await p.select('#tstyle', 'plain'); await p.$eval('#text', e => { e.value = ''; e.dispatchEvent(new Event('input')); });
  await p.goto(BASE + '/#future'); await sleep(900);
  await p.evaluate(() => { const t = window.toast; window.toast = (m, bad) => { (window.__t = window.__t || []).push((bad ? '!' : '') + m); t(m, bad); }; });
  await p.evaluate(() => [...document.querySelectorAll('#fdList .irow')].find(r => r.textContent.includes('Sunday')).querySelector('button').click());
  await sleep(1500);
  const st2 = await p.evaluate(() => window.fromImage.state());
  check('image draft restored after reload, edits kept', JSON.stringify(st2) === JSON.stringify(st1) && await p.$eval('#tstyle', e => e.value) === 'image', JSON.stringify(st2) + ' ' + (await lastToast() || ''));

  // Limits
  for (let i = 0; i < 9; i++) await fetch(BASE + '/api/items?kind=d', { method: 'POST', body: JSON.stringify({ title: 'x' + i, style: 'plain', src: { style: 'plain', text: 'x' } }) });
  await p.$eval('#text', e => { e.value = 'one more'; e.dispatchEvent(new Event('input')); });
  await p.select('#tstyle', 'plain');
  await p.$eval('#tdraft', e => e.click()); await sleep(400);
  check('draft limit message', /full \(10 of 10\)/.test(await lastToast()), await lastToast());
  // Delete
  await p.goto(BASE + '/#future'); await sleep(900);
  const n0 = (await p.$$('#fpList .irow')).length;
  await p.evaluate(() => { const bs = document.querySelectorAll('#fpList .irow')[0].querySelectorAll('button'); bs[bs.length - 1].click(); });
  await sleep(600);
  check('delete removes it', (await p.$$('#fpList .irow')).length === n0 - 1);
  await p.screenshot({ path: require('path').join(__dirname, 'out', 'shot_future2.png'), fullPage: true });

  errs.splice(0, errs.length, ...errs.filter(e => !e.startsWith('Failed to load resource'))); check('no page errors', errs.length === 0, errs.join(' / '));
  console.log(fails ? '\n' + fails + ' FAILED' : '\nall passed');
  await b.close();
})().catch(e => { console.error(e); process.exit(1); });

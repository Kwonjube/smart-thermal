// v0.24.1: a styled print refused 3 times for memory is sent as two halves.
const puppeteer = require('puppeteer-core'); const sleep = ms => new Promise(r => setTimeout(r, ms));
(async () => {
  const U = 'http://localhost:8765';
  await fetch(U + '/__reset'); await fetch(U + '/__oom?n=3');
  const b = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox', '--no-proxy-server'] });
  const p = await b.newPage(); await p.evaluateOnNewDocument(() => { try { localStorage.setItem('level', '4'); } catch (e) {} });
  await p.goto(U + '/'); await sleep(1500);
  await p.select('#tstyle', 'list');
  await p.$eval('#text', e => { e.value = 'Milk\nEggs\nBread\nButter'; e.dispatchEvent(new Event('input')); });
  await sleep(500); await p.$eval('#print', e => e.click()); await sleep(16000);
  const st = await (await fetch(U + '/__state')).json();
  const r = st.jobs.filter(j => j.type === 'raster');
  console.log('raster jobs:', r.length, r.map(j => j.q.rows).join(','), 'oom left', st.oom);
  const ok = r.length === 2 && st.oom === 0;
  console.log(ok ? 'PASS' : 'FAIL'); await b.close(); process.exit(ok ? 0 : 1);
})();

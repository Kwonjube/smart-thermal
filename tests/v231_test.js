// v0.23.1: online photo parts are <= 24 KB, an "out of memory" refusal is
// shown with its reason and retried, and the print completes.
const puppeteer = require('puppeteer-core'); const sleep = ms => new Promise(r => setTimeout(r, ms));
(async () => {
  const U = 'http://localhost:8765';
  await fetch(U + '/__reset'); await fetch(U + '/__oom?n=2');
  const b = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox', '--no-proxy-server'] });
  const p = await b.newPage(); const posts = []; await p.evaluateOnNewDocument(() => { try { localStorage.setItem('level', '4'); } catch (e) {} });
  p.on('request', r => { if (/print\/raster/.test(r.url())) posts.push((r.postData() || '').length); });
  p.on('response', async r => { if (/print\/raster/.test(r.url())) posts.push(r.status() + ' ' + (await r.text()).slice(0, 60)); });
  await p.goto(U + '/'); await sleep(1500);
  await p.evaluate(() => { window.REMOTE = true; });
  const inp = await p.$('#ifile'); await inp.uploadFile('photo.jpg'); await sleep(2500);
  await p.$eval('#iprint', e => e.click());
  for (let i = 0; i < 40; i++) { await sleep(1000); const t = await p.$eval('#iprog', e => e.textContent); if (/printed|failed/.test(t)) break; }
  const prog = await p.$eval('#iprog', e => e.textContent);
  const st = await (await fetch(U + '/__state')).json();
  const sizes = st.jobs.map(j => j.body);
  console.log('responses:', posts.filter(x => typeof x === 'string').join(' | '));
  console.log('part sizes:', sizes.join(', '));
  console.log('progress:', prog);
  const ok = /1 printed/.test(prog) && !/failed/.test(prog) && sizes.length > 0 && sizes.every(n => n <= 24 * 1024 + 400)
    && posts.some(x => typeof x === 'string' && /out of memory/.test(x));
  console.log(ok ? 'PASS' : 'FAIL'); await b.close(); process.exit(ok ? 0 : 1);
})();

// Extra checks against the REAL relay code (wrangler dev on :8787, with a
// test-only approve hook), beyond remote_test.js.
const puppeteer = require('puppeteer-core');
const fs = require('fs'), { spawn, execFileSync } = require('child_process');
const sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0; const check = (n, ok, x) => { if (!ok) fails++; console.log((ok ? 'ok   ' : 'FAIL ') + n + (x ? '  ' + x : '')); };
const R = 'http://localhost:8787', MOCK = 'http://localhost:8765';
const list = async () => (await fetch(R + '/__test/list')).json();
const unit = (key, secs, env = {}) => new Promise(res => {
  const p = spawn('./host_relay', ['8787', '8765', String(secs), key.replace(/^relayhost\//, '')], { cwd: 'relayhost', env: { ...process.env, ...env } });
  let out = ''; p.stdout.on('data', d => out += d); p.on('close', () => res(out)); p.out = () => out; unit.last = p;
});
const codeIn = s => (s.match(/STATE on ([a-z0-9]{6})/g) || []).map(x => x.slice(9));
(async () => {
  const A = codeIn(fs.readFileSync('relayhost/liveA.log', 'utf8'))[0];
  // 1. Reconnect with the same key: same code again (so no new welcome slip).
  const again = await unit('relayhost/liveA.bin', 3);
  check('reconnect gets the same code', codeIn(again)[0] === A, again.replace(/\n/g, ' | '));
  await sleep(2500);                                   // let the long-running A take its place back... (it was replaced)
  // 2. Bad signature: closed 1008, firmware sees REJECTED.
  const bad = await unit('relayhost/bad.bin', 5, { BADSIG: '1' });
  check('bad signature -> 1008 REJECTED', /END REJECTED close=1008/.test(bad), bad.trim().split('\n').pop());
  // 3. Too many waiting joins from one IP: 4th gets 1013 BUSY.
  for (const f of ['j1', 'j2', 'j3', 'j4']) try { fs.unlinkSync('relayhost/' + f + '.bin'); } catch (e) {}
  const js = ['j1', 'j2', 'j3'].map(f => unit('relayhost/' + f + '.bin', 4));
  await sleep(1500);
  const busy = await unit('relayhost/j4.bin', 4);
  check('4th waiting join from one IP -> 1013 BUSY', /END BUSY close=1013/.test(busy), busy.trim().split('\n').pop());
  await Promise.all(js);
  for (const p of (await list()).filter(p => p.status === 'pending')) await fetch(R + '/__test/forget?id=' + p.id);
  // 4. New link: new code, old one 404.
  try { fs.unlinkSync('relayhost/nc.bin'); } catch (e) {}
  const ncRun = unit('relayhost/nc.bin', 8, { NEWCODE: '1', NAME: 'Newcode' });
  await sleep(1200); await fetch(R + '/__test/approve');
  const nc = await ncRun, codes = codeIn(nc);
  check('new link: relay sends a second code', codes.length === 2 && codes[0] !== codes[1], codes.join(' -> '));
  const old = await fetch(R + '/p/' + codes[0] + '/', { redirect: 'manual' });
  check('old code is 404', old.status === 404, String(old.status));
  // 5. Extra secure: visitor asks, printer gets the knock notice.
  try { fs.unlinkSync('relayhost/sec.bin'); } catch (e) {}
  const secRun = unit('relayhost/sec.bin', 10, { SECURE: '1', NAME: 'Secure one' });
  await sleep(1200); await fetch(R + '/__test/approve'); await sleep(1000);
  const sc = codeIn(unit.last.out())[0];
  const page = await fetch(R + '/p/' + sc + '/'); const txt = await page.text();
  check('secure printer: visitor gets "Ask to print here"', /Ask to print here/.test(txt));
  // A real form post (Node's fetch is refused by the relay's page-only rule, as it should be).
  const ask = { status: execFileSync('curl', ['-s', '--noproxy', '*', '-o', '/dev/null', '-w', '%{http_code}', '-X', 'POST',
    '-H', 'content-type: application/x-www-form-urlencoded', '-d', 'name=Alex', R + '/p/' + sc + '/_r/ask']).toString() };
  await sleep(1000);
  check('ask -> knock notice reaches the printer', /NOTICE device-request Alex/.test(unit.last.out()), ask.status + ' ' + (unit.last.out().match(/NOTICE.*/) || [''])[0]);
  await secRun;
  // 6. Big uploads in pieces through the relay (picture and long scheduled slip).
  await fetch(MOCK + '/__reset');
  unit('relayhost/liveA.bin', 300, { NAME: 'Kitchen' }); await sleep(1500);   // A back online for the page
  const br = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox', '--no-proxy-server'] });
  const p = await br.newPage(); const errs = []; p.on('pageerror', e => errs.push(e.message)); p.on('dialog', d => d.accept());
  p.on('console', m => { if (/Content Security Policy|Refused to/i.test(m.text())) errs.push('CSP: ' + m.text().slice(0, 160)); });
  await p.goto(R + '/p/' + A + '/'); await sleep(1500);
  await p.evaluate(() => {
    const c = document.createElement('canvas'); c.width = 1200; c.height = 1500; const x = c.getContext('2d');
    x.fillStyle = '#fff'; x.fillRect(0, 0, 1200, 1500);
    const im = x.getImageData(0, 0, 1200, 1500); for (let i = 0; i < im.data.length; i += 4) { const v = 225 + Math.random() * 30; im.data[i] = im.data[i + 1] = im.data[i + 2] = v; }
    x.putImageData(im, 0, 0); x.fillStyle = '#000'; x.font = 'bold 60px Arial'; x.fillText('Shopping', 60, 120); x.font = '40px Arial';
    for (let i = 0; i < 20; i++) x.fillText('• Thing ' + (i + 1), 60, 220 + i * 70);
    window.fromImage.set(c, 1);
  });
  await sleep(500);
  await p.$eval('#tdraft', e => e.click()); await sleep(6000);
  let st = await (await fetch(MOCK + '/__state')).json();
  check('picture > 128 KB uploaded in pieces through the relay', st.picBytes > 131072 && st.pieces >= 2, st.picBytes + ' bytes, ' + st.pieces + ' pieces');
  await p.select('#tstyle', 'list');
  await p.$eval('#text', (e, v) => { e.value = v; e.dispatchEvent(new Event('input')); }, 'Long list\n' + Array.from({ length: 160 }, (_, i) => 'Line ' + (i + 1)).join('\n'));
  await p.$eval('#tsched', e => e.click()); await sleep(200);
  await p.select('#sRep', 'daily'); await p.$eval('#sAt', e => { e.value = '23:59'; e.dispatchEvent(new Event('change')); });
  await p.$eval('#sGo', e => e.click()); await sleep(6000);
  st = await (await fetch(MOCK + '/__state')).json();
  const it = st.items.p[0];
  check('scheduled slip > 160 KB stored in pieces and ready', it && it.ready && it.bytes > 163840 && st.bins[it.id] === it.bytes, it && (it.bytes + ' bytes'));
  check('no page or CSP errors', !errs.length, errs.join(' / '));
  await br.close();
  console.log(fails ? '\n' + fails + ' FAILED' : '\nall passed');
  process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });

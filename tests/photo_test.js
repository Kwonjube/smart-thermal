const puppeteer = require('puppeteer-core'); const sleep = ms => new Promise(r => setTimeout(r, ms));
(async () => {
  const code = require('fs').readFileSync('relayhost/liveA2.log', 'utf8').match(/STATE on ([a-z0-9]{6})/)[1];
  const b = await puppeteer.launch({ executablePath: require('./chrome'), args: ['--no-sandbox', '--no-proxy-server'] });
  const p = await b.newPage(); const log = [];
  p.on('response', r => { if (r.url().includes('/api/')) log.push(r.request().method() + ' ' + r.status() + ' ' + r.url().replace(/^.*\/p\/[a-z0-9]{6}/, '')); });
  p.on('console', m => log.push('console: ' + m.text().slice(0, 150)));
  await p.goto('http://localhost:8787/p/' + code + '/'); await sleep(1500);
  const inp = await p.$('#ifile'); await inp.uploadFile('photo.jpg'); await sleep(2500);
  await p.$eval('#iprint', e => e.click()); await sleep(12000);
  console.log(log.filter(l => !/printer\/status/.test(l)).join('\n'));
  console.log('progress:', await p.$eval('#iprog', e => e.textContent));
  await b.close();
})();

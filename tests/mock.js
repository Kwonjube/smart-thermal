// Mock unit: serves the real page (assembled from the firmware headers) and
// answers the API with the shared pretend printer (SmartThermal/tools/fakeunit.js),
// the same one built into preview.html and demo.html.
const http = require('http'), url = require('url');
const SRC = require('path').join(__dirname, '..', 'SmartThermal') + '/';
const { assemble, version } = require(SRC + 'tools/build_pages.js');
const { createUnit } = require(SRC + 'tools/fakeunit.js');
const unit = createUnit({ testHooks: true, version: version(), hostname: 'print',
  home: { host: 'print.justifiedsystems.com.au', url: 'https://print.justifiedsystems.com.au/p/k7f3q9/', ownerCode: 'R8XK2M', mac: '24:6F:28:AA:BB:CC' } });

function multipart(buf, ct) {
  const b = '--' + ct.split('boundary=')[1]; const s = buf.toString('latin1');
  const i = s.indexOf('\r\n\r\n') + 4, j = s.indexOf('\r\n' + b, i);
  return new Uint8Array(Buffer.from(s.slice(i, j), 'latin1'));
}

http.createServer((req, res) => {
  const u = url.parse(req.url, true); let body = [];
  req.on('data', c => body.push(c)); req.on('end', () => {
    body = Buffer.concat(body);
    const st = unit.state;
    if (req.headers['x-via-relay']) st.lastRelay = { ua: req.headers['user-agent'], who: req.headers['x-via-relay'], path: req.url };
    if (u.pathname === '/') { res.writeHead(200, { 'Content-Type': 'text/html' }); return res.end(assemble()); }
    const ct = String(req.headers['content-type'] || '');
    const multi = ct.includes('multipart');
    const r = unit.handle({ method: req.method, path: u.pathname, query: u.query, headers: req.headers,
      text: !multi && body.length ? body.toString() : null, file: multi ? multipart(body, ct) : null });
    // Tests read job sizes as the whole request body, as before.
    if (multi && /\/api\/print\/raster/.test(u.pathname) && r.status === 200) unit.state.jobs[unit.state.jobs.length - 1].body = body.length;
    if (r.bytes) { res.writeHead(r.status, { 'Content-Type': r.type }); return res.end(Buffer.from(r.bytes)); }
    res.writeHead(r.status, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(r.json));
  });
}).listen(8765, () => console.log('mock on 8765'));

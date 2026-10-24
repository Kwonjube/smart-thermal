// relay.js - a STAND-IN for the website's printer relay, used to test the
// unit's side of protocol v1 (see PROTOCOL.md). It is a reference for the
// message flow only: the real relay is a Cloudflare Worker + Durable Object,
// with storage, the admin page behind Cloudflare Access, extra-secure device
// approval and rate limits, none of which are here.
//
//   node relay.js            listens on :8790
//   AUTO=1 node relay.js     approves every new printer after 0.5 s
//
// Visitors:  http://localhost:8790/p/<code>/...
// Test admin: GET /admin/printers, /admin/approve?id=, /admin/decline?id=,
//             /admin/notice?id=&name=, /admin/kick?id=
const http = require('http');
const { WebSocketServer } = require('ws');
const { webcrypto: { subtle }, randomBytes, createHash } = require('crypto');

const PORT = +(process.env.PORT || 8790);
const AUTO = process.env.AUTO === '1';
const ALPHA = '23456789abcdefghjkmnpqrstuvwxyz';
const b64u = b => Buffer.from(b).toString('base64url');
const newCode = () => { let s = ''; const r = randomBytes(6); for (const x of r) s += ALPHA[x % ALPHA.length]; return s; };
const perth = () => new Date().toLocaleString('en-AU', { timeZone: 'Australia/Perth' });

// id -> { id, key, info, state, code, ip, at, ws, queue, cur, prints }
const printers = new Map();
const byCode = id => [...printers.values()].find(p => p.code === id);
const log = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

function sendState(p, msg){
  if (!p.ws) return;
  const s = { t: 'state', s: p.state };
  if (p.state === 'on') { s.code = p.code; s.url = `http://localhost:${PORT}/p/${p.code}/`; }
  if (msg) s.msg = msg;
  p.ws.send(JSON.stringify(s));
  if (p.state === 'declined' || p.state === 'off') setTimeout(() => p.ws && p.ws.close(1000, p.state), 200);
}

async function verify(hello, nonce){
  const pub = Buffer.from(hello.key || '', 'base64url'), sig = Buffer.from(hello.sig || '', 'base64url');
  if (pub.length !== 65 || sig.length !== 64) return false;
  const key = await subtle.importKey('raw', pub, { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
  return subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, key, sig, Buffer.from('smartthermal-relay-v1:' + nonce));
}

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x');
  if (u.pathname.startsWith('/admin/')) {
    const p = printers.get(u.searchParams.get('id'));
    const out = o => { res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify(o, null, 1)); };
    if (u.pathname === '/admin/printers')
      return out([...printers.values()].map(p => ({ id: p.id, state: p.state, code: p.code, online: !!p.ws, info: p.info,
        ip: p.ip, requested: p.at, prints: p.prints })));
    if (!p) { res.writeHead(404); return res.end('no such printer'); }
    if (u.pathname === '/admin/approve') { p.state = 'on'; p.code = p.code || newCode(); sendState(p); return out({ ok: 1, code: p.code }); }
    if (u.pathname === '/admin/decline') { p.state = 'declined'; sendState(p); return out({ ok: 1 }); }
    if (u.pathname === '/admin/notice') { p.ws && p.ws.send(JSON.stringify({ t: 'notice', k: 'device-request', name: u.searchParams.get('name') || 'Someone' })); return out({ ok: 1 }); }
    if (u.pathname === '/admin/kick') { p.ws && p.ws.terminate(); return out({ ok: 1 }); }
  }
  const m = u.pathname.match(/^\/p\/([a-z0-9]{6})(\/.*)?$/);
  if (!m) { res.writeHead(404); return res.end('not found'); }
  const p = byCode(m[1]);
  if (!p || p.state !== 'on') { res.writeHead(404, { 'content-type': 'text/plain' }); return res.end('No printer at this link.'); }
  if (!m[2]) { res.writeHead(301, { location: `/p/${m[1]}/` }); return res.end(); }
  if (m[2] === '/_r/printers') {
    const list = [...printers.values()].filter(q => q !== p && q.state === 'on' && q.info.visible)
      .map(q => ({ name: q.info.name, code: q.code, online: !!q.ws }));
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
    return res.end(JSON.stringify(list));
  }
  if (!p.ws) { res.writeHead(503, { 'content-type': 'text/plain' }); return res.end('This printer is offline.'); }
  // Collect the body (max 160 KB), then queue: one request at a time per printer.
  const chunks = []; let size = 0;
  for await (const c of req) { size += c.length; if (size > 160 * 1024) { res.writeHead(413); return res.end('too large'); } chunks.push(c); }
  const body = Buffer.concat(chunks);
  p.queue.push({ req, res, path: m[2] + u.search, body });
  pump(p);
});

let nextId = 1;
function pump(p){
  if (p.cur || !p.queue.length || !p.ws) return;
  const job = p.cur = p.queue.shift();
  job.id = nextId++;
  job.timer = setTimeout(() => finish(p, 504, 'printer did not answer'), 90000);
  if (/^\/api\/print\//.test(job.path) && job.req.method === 'POST') p.prints++;
  p.ws.send(JSON.stringify({ t: 'req', id: job.id, m: job.req.method, p: job.path,
    ct: job.req.headers['content-type'] || '', len: job.body.length, who: 'guest',
    ua: job.req.headers['user-agent'] || '' }));
  for (let o = 0; o < job.body.length; o += 8192) p.ws.send(job.body.subarray(o, o + 8192));
}
function finish(p, status, msg){
  const job = p.cur; if (!job) return;
  clearTimeout(job.timer);
  if (!job.res.headersSent) { job.res.writeHead(status, { 'content-type': 'text/plain' }); job.res.end(msg); }
  else job.res.end();
  p.cur = null; pump(p);
}

const wss = new WebSocketServer({ server, path: '/relay/connect' });
wss.on('connection', (ws, req) => {
  const nonce = b64u(randomBytes(32));
  const ip = req.socket.remoteAddress;
  let p = null;
  ws.send(JSON.stringify({ t: 'challenge', n: nonce }));
  ws.on('message', async (data, isBinary) => {
    if (isBinary) {                                   // reply body
      if (p && p.cur && p.cur.started) p.cur.res.write(data);
      return;
    }
    const s = data.toString();
    if (s === 'ping') return ws.send('pong');
    let msg; try { msg = JSON.parse(s); } catch (e) { return; }
    if (msg.t === 'hello') {
      if (!(await verify(msg, nonce))) { log('bad signature - closing'); return ws.close(1008, 'bad signature'); }
      const id = b64u(createHash('sha256').update(Buffer.from(msg.key, 'base64url')).digest()).slice(0, 16);
      p = printers.get(id);
      const info = { name: msg.name, owner: msg.owner, mac: msg.mac, fw: msg.fw, visible: !!msg.visible,
                     secure: !!msg.secure, ownerHash: msg.ownerHash };
      if (!p) {
        p = { id, key: msg.key, state: 'pending', code: '', queue: [], cur: null, prints: 0, ip, at: perth(), info };
        printers.set(id, p);
        log('join request', id, info.name, 'from', info.owner, info.mac, ip);
        if (AUTO) setTimeout(() => { p.state = 'on'; p.code = newCode(); sendState(p); }, 500);
      }
      p.info = info;
      if (p.ws && p.ws !== ws) p.ws.terminate();
      p.ws = ws;
      log('hello', id, p.state);
      sendState(p);
      return;
    }
    if (!p) return;
    if (msg.t === 'newcode' && p.state === 'on') { p.code = newCode(); log('new code', p.code); return sendState(p); }
    if (msg.t === 'res' && p.cur && msg.id === p.cur.id) {
      const h = { 'content-type': msg.ct || 'application/octet-stream' };
      if (msg.cc) h['cache-control'] = msg.cc;
      p.cur.res.writeHead(msg.s || 502, h);
      p.cur.started = true;
      return;
    }
    if (msg.t === 'end' && p.cur && msg.id === p.cur.id) return finish(p, 502, msg.err || '');
  });
  ws.on('close', () => {
    if (p && p.ws === ws) { p.ws = null; if (p.cur) finish(p, 502, 'printer went offline'); p.queue.splice(0).forEach(j => { j.res.writeHead(503); j.res.end('offline'); }); log('closed', p.id); }
  });
});
server.listen(PORT, () => log('stand-in relay on', PORT, AUTO ? '(auto-approve)' : ''));

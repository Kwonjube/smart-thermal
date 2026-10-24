// Post-deployment check for the print relay. No dependencies (Node 22+).
//
//   node scripts/check-live.mjs
//   node scripts/check-live.mjs <your-workers-subdomain>   (optional: also checks workers.dev)
//
// Every check runs WITHOUT logging in. It confirms that the admin host is fully behind
// Cloudflare Access, that the print host has no admin, that the relay answers printers,
// and that TLS 1.2 works.

import tls from 'node:tls';

const PRINT = 'print.justifiedsystems.com.au';
const ADMIN = 'admin.justifiedsystems.com.au';
const TEAM = process.env.ACCESS_TEAM_DOMAIN || 'cloudflareaccess.com';   // any Access team, unless set
const sub = process.argv[2];
let pass = 0, fail = 0, notes = 0;
const ok = (c, m) => { c ? pass++ : fail++; console.log(c ? '  PASS' : '  FAIL', m); };
const note = (m) => { notes++; console.log('  NOTE', m); };
const go = (host, path, o = {}) => fetch(`https://${host}${path}`, { redirect: 'manual', ...o });
const toAccess = (r) => [301, 302, 303, 307].includes(r.status) && (r.headers.get('location') || '').includes(TEAM);
const form = { method: 'POST', body: 'action=approve&id=x&csrf=x', headers: { 'content-type': 'application/x-www-form-urlencoded' } };

console.log(`Admin: https://${ADMIN}   Printers: https://${PRINT}\n`);

console.log('Admin host is entirely behind Cloudflare Access');
for (const [label, path, opts] of [
  ['GET /', '/', {}],
  ['POST /action', '/action', form],
  ['POST /action with fake token and headers', '/action', { ...form, headers: { ...form.headers, 'cf-access-jwt-assertion': 'e30.e30.e30', 'x-relay-admin': 'x', 'x-relay-admin-host': '1', cookie: '__Host-relay_admin=AAAAAAAAAAAAAAAAAAAAAA' } }],
  ['GET /logout', '/logout', {}],
  ['GET /anything-else', '/anything-else', {}],
]) {
  const r = await go(ADMIN, path, opts);
  if (toAccess(r)) ok(true, `${label}: sent to Cloudflare Access login`);
  else if (r.status === 403 || r.status === 404) { ok(true, `${label}: refused by the Worker (${r.status})`); note(`Access did not intercept ${label}. Check the Access application covers the whole admin host (no path).`); }
  else ok(false, `${label}: unexpected ${r.status} ${r.headers.get('location') || ''}`);
}

console.log('\nPrint host has no admin and is not behind Access');
for (const path of ['/admin', '/admin/action', '/action', '/']) {
  const r = await go(PRINT, path, path.includes('action') ? form : {});
  if (toAccess(r)) { ok(false, `${PRINT}${path} is still behind Access. Remove the old print host entry from the Access application.`); continue; }
  ok(r.status === 404, `${PRINT}${path}: 404`);
}
const pz = await go(PRINT, '/p/zzzzzz/');
ok(pz.status === 404, 'unknown printer code: 404');
ok((pz.headers.get('strict-transport-security') || '').includes('max-age'), 'security headers present');

console.log('\nNo other way in');
if (sub) {
  try {
    const r = await fetch(`https://print-relay.${sub}.workers.dev/action`, { ...form, redirect: 'manual' });
    ok(r.status === 404 || r.status >= 500, `workers.dev address serves nothing (${r.status})`);
  } catch { ok(true, 'workers.dev address does not resolve'); }
} else note('Pass your workers.dev subdomain as an argument to also check that address (Workers & Pages overview, right-hand side).');

console.log('\nPrinter connection');
ok((await go(PRINT, '/relay/connect')).status === 426, '/relay/connect refuses plain HTTP (426)');
await new Promise((resolve) => {
  const ws = new WebSocket(`wss://${PRINT}/relay/connect`);
  const t = setTimeout(() => { ok(false, 'no challenge within 10 s'); try { ws.close(); } catch {} resolve(); }, 10000);
  ws.onmessage = (e) => {
    clearTimeout(t);
    let m = {}; try { m = JSON.parse(e.data); } catch {}
    ok(m.t === 'challenge' && typeof m.n === 'string' && m.n.length === 43, 'WebSocket opens and sends a 43-character challenge');
    ws.close(); resolve();
  };
  ws.onerror = () => { clearTimeout(t); ok(false, 'WebSocket failed to open'); resolve(); };
});

console.log('\nTLS');
for (const host of [PRINT, ADMIN]) {
  await new Promise((resolve) => {
    const s = tls.connect({ host, port: 443, servername: host, minVersion: 'TLSv1.2', maxVersion: 'TLSv1.2' }, () => {
      ok(s.getProtocol() === 'TLSv1.2', `${host}: TLS 1.2 accepted${host === PRINT ? ' (needed by the ESP32 firmware)' : ''}`); s.end(); resolve();
    });
    s.on('error', (e) => { ok(false, `${host}: TLS 1.2 refused: ${e.message}`); resolve(); });
  });
}

console.log(`\n${pass} passed, ${fail} failed${notes ? `, ${notes} note(s)` : ''}`);
process.exit(fail ? 1 : 0);

// Smart Thermal print relay — print.justifiedsystems.com.au
// Cloudflare Worker + one SQLite-backed Durable Object. No libraries, no build step.
// Implements "Smart Thermal online printing: spec for the website", protocol v1.
//
// Never stores or logs what is printed: request bodies are not kept. The admin activity
// log (0.23.1) keeps times, printer names and events: connects, drops and their reasons,
// admin actions, and FAILED requests as method + path (never the query or body) with the
// error text. Nothing that identifies a visitor. 30 days or 3000 entries, whichever is less.

import { DurableObject } from 'cloudflare:workers';

// ---------------------------------------------------------------- limits
const ALPHABET = '23456789abcdefghjkmnpqrstuvwxyz';
const MAX_BODY = 160 * 1024;          // visitor request body
const MAX_QUEUE = 10;                 // queued visitor requests per printer
const REQ_TIMEOUT = 90_000;           // per request
const FRAME = 8192;                   // binary frame size to the printer
const MISS_LIMIT = 20;                // unknown /p/ codes per IP ...
const MISS_WINDOW = 10 * 60_000;      // ... per 10 minutes
const OWNER_TRIES = 5;                // owner-code attempts per printer ...
const OWNER_WINDOW = 60 * 60_000;     // ... per hour
const ASK_LIMIT = 3;                  // "ask to print" requests per printer per hour
const MAX_PENDING_DEVICES = 10;       // waiting device requests per printer
const MAX_PENDING_JOINS = 20;         // waiting join requests in total
const MAX_JOINS_PER_IP = 3;           // waiting join requests per IP
const HELLO_TIMEOUT = 30_000;         // sockets must say hello within this time
const MAX_SOCKETS = 200;
const LOG_KEEP = 3000;                // activity log entries kept ...
const LOG_DAYS = 30;                  // ... and for at most this long
const SLOW_MS = 30_000;               // answers slower than this are logged
const LOG_MERGE_MS = 60_000;          // the same event again within a minute: counted on one line
const LOG_PER_MIN = 20;               // new lines per printer (or for the relay itself) per minute

// Paths the printer refuses anyway; refused here first (spec section 5).
const BLOCKED_PREFIXES = ['/api/settings', '/api/system', '/api/print/raw', '/api/jobs/clear', '/api/online', '/log', '/setup'];
const BLOCKED_POST = ['/api/kiosk', '/api/clock'];

// ---------------------------------------------------------------- headers
const SEC_COMMON = {
  'x-content-type-options': 'nosniff',
  'strict-transport-security': 'max-age=31536000; includeSubDomains',
  'x-robots-tag': 'noindex, nofollow',
  'x-frame-options': 'DENY',
};
// Printer pages (need the printer's scripts). Spec section 2.
const PRINTER_CSP = "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
// Pages the relay renders itself (ask page, owner page, admin): no scripts at all.
const OWN_CSP = "default-src 'none'; style-src 'unsafe-inline'; img-src data:; form-action 'self'; base-uri 'none'; frame-ancestors 'none'";

function printerHeaders() {
  return new Headers({ ...SEC_COMMON, 'content-security-policy': PRINTER_CSP, 'referrer-policy': 'no-referrer', 'cache-control': 'no-store' });
}
function ownHeaders(type = 'text/html; charset=utf-8') {
  return new Headers({
    // 'same-origin', not 'no-referrer': with no-referrer, browsers send "Origin: null" on form
    // posts and the relay's own origin check would refuse every button press.
    ...SEC_COMMON, 'content-type': type, 'content-security-policy': OWN_CSP, 'referrer-policy': 'same-origin',
    'cache-control': 'no-store',
    // Cuts the window handle if a printer page opens this page, so its scripts cannot drive it.
    'cross-origin-opener-policy': 'same-origin',
  });
}
const plain = (status, msg, extra = {}) => {
  const h = ownHeaders('text/plain; charset=utf-8');
  for (const [k, v] of Object.entries(extra)) h.set(k, v);
  return new Response(msg + '\n', { status, headers: h });
};
const html = (body, status = 200, extra = {}) => {
  const h = ownHeaders();
  for (const [k, v] of Object.entries(extra)) h.set(k, v);
  return new Response(body, { status, headers: h });
};
const redirect = (status, location, extra = {}) => {
  const h = ownHeaders('text/plain; charset=utf-8');
  h.set('location', location);
  for (const [k, v] of Object.entries(extra)) h.append(k, v);
  return new Response(null, { status, headers: h });
};

// ---------------------------------------------------------------- small helpers
// Short device description for the knock-knock slip and the owner page (0.26), e.g.
// "iPhone, Safari". Only the kind of device and browser; nothing that identifies a person.
function deviceKind(ua) {
  ua = String(ua || '');
  const os = /iPad/.test(ua) ? 'iPad' : /iPhone/.test(ua) ? 'iPhone'
    : /Android/.test(ua) ? (/Mobile/.test(ua) ? 'Android phone' : 'Android tablet')
    : /CrOS/.test(ua) ? 'Chromebook' : /Windows/.test(ua) ? 'Windows PC'
    : /Macintosh|Mac OS X/.test(ua) ? 'Mac' : /Linux/.test(ua) ? 'Linux PC' : '';
  const br = /Edg[A]?\//.test(ua) ? 'Edge' : /OPR\/|Opera/.test(ua) ? 'Opera' : /SamsungBrowser/.test(ua) ? 'Samsung Internet'
    : /FxiOS|Firefox\//.test(ua) ? 'Firefox' : /CriOS|Chrome\//.test(ua) ? 'Chrome' : /Safari\//.test(ua) ? 'Safari' : '';
  return [os, br].filter(Boolean).join(', ').slice(0, 47) || 'Unknown device';
}
// The owner page may show the printer's own QR code (/p/<code>/api/qr.svg), so its
// images may come from this origin. Still no scripts.
const OWNER_CSP = OWN_CSP.replace("img-src data:", "img-src 'self' data:");
const enc = (s) => new TextEncoder().encode(s);
function b64url(bytes) {
  let s = '';
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
function b64urlToBytes(s) {
  if (typeof s !== 'string' || !/^[A-Za-z0-9_-]*$/.test(s)) throw new Error('bad base64url');
  s = s.replace(/-/g, '+').replace(/_/g, '/');
  while (s.length % 4) s += '=';
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}
async function sha256hex(s) {
  const d = new Uint8Array(await crypto.subtle.digest('SHA-256', enc(s)));
  return [...d].map((b) => b.toString(16).padStart(2, '0')).join('');
}
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const clean = (s, max) => (typeof s === 'string' ? s.replace(/[\u0000-\u001f\u007f]/g, '').trim().slice(0, max) : '');
const perth = (ts) => (ts ? new Date(ts).toLocaleString('en-AU', { timeZone: 'Australia/Perth' }) : '—');
const perthDay = () => new Date().toLocaleDateString('en-CA', { timeZone: 'Australia/Perth' });
function cookie(request, name) {
  for (const part of (request.headers.get('cookie') || '').split(';')) {
    const i = part.indexOf('=');
    if (i > 0 && part.slice(0, i).trim() === name) return part.slice(i + 1).trim();
  }
  return null;
}
const deviceCookie = (pid, token, maxAge = 31536000) =>
  `d_${pid}=${token}; Path=/p/; Max-Age=${maxAge}; HttpOnly; Secure; SameSite=Strict`;
const randomToken = () => b64url(crypto.getRandomValues(new Uint8Array(16))); // 128 bits

// Browsers mark how a request was made. Pages the relay renders itself only answer real,
// top-level page loads and same-origin form posts: not fetch(), not iframes. Scripts on a
// printer page therefore cannot read or drive them. Forms also carry a CSRF token (below).
function navigationOnly(request) {
  const mode = request.headers.get('sec-fetch-mode');
  if (mode && mode !== 'navigate') return false;
  const dest = request.headers.get('sec-fetch-dest');
  if (dest && dest !== 'document') return false;
  if (request.method === 'POST') {
    const site = request.headers.get('sec-fetch-site');
    if (site && site !== 'same-origin') return false;
    const origin = request.headers.get('origin');
    if (origin && origin !== new URL(request.url).origin) return false;
  }
  return true;
}

function isBlocked(path, method) {
  if (BLOCKED_PREFIXES.some((b) => path.startsWith(b))) return true;
  if (method === 'POST' && BLOCKED_POST.some((b) => path === b || path.startsWith(b + '/'))) return true;
  return false;
}

async function readLimited(stream, limit) {
  if (!stream) return new ArrayBuffer(0);
  const reader = stream.getReader();
  const chunks = [];
  let n = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    n += value.byteLength;
    if (n > limit) { try { await reader.cancel(); } catch {} return null; }
    chunks.push(value);
  }
  const out = new Uint8Array(n);
  let o = 0;
  for (const c of chunks) { out.set(c, o); o += c.byteLength; }
  return out.buffer;
}

// ---------------------------------------------------------------- Cloudflare Access check
// Defence in depth: the admin host is behind Cloudflare Access, and the Worker also verifies
// the Access token itself. If anything is missing or unreachable, admin is refused (fails closed).
let certCache = { team: '', at: 0, keys: [] };
let lastForcedRefresh = 0;
async function accessKeys(team, force) {
  const cached = certCache.team === team ? certCache.keys : [];
  if (!force && cached.length && Date.now() - certCache.at < 3600_000) return cached;
  if (force) {
    // An unknown key id forces a refresh, at most every 5 minutes (stops refresh floods).
    if (Date.now() - lastForcedRefresh < 300_000) return cached;
    lastForcedRefresh = Date.now();
  }
  let j;
  try {
    const r = await fetch(`https://${team}/cdn-cgi/access/certs`);
    if (!r.ok) return [];
    j = await r.json();
  } catch { return []; } // keys unreachable: nothing verifies, so admin is refused
  certCache = { team, at: Date.now(), keys: Array.isArray(j.keys) ? j.keys : [] };
  return certCache.keys;
}
async function verifyAccess(request, env) {
  const team = String(env.ACCESS_TEAM_DOMAIN || '').trim().replace(/^https?:\/\//, '').replace(/\/+$/, '');
  const aud = String(env.ACCESS_AUD || '').trim();
  if (!team || !aud) return null;
  const jwt = request.headers.get('cf-access-jwt-assertion');
  if (!jwt) return null;
  const parts = jwt.split('.');
  if (parts.length !== 3) return null;
  let header, payload;
  try {
    header = JSON.parse(new TextDecoder().decode(b64urlToBytes(parts[0])));
    payload = JSON.parse(new TextDecoder().decode(b64urlToBytes(parts[1])));
  } catch { return null; }
  if (header.alg !== 'RS256') return null;
  let jwk = (await accessKeys(team, false)).find((k) => k.kid === header.kid);
  if (!jwk) jwk = (await accessKeys(team, true)).find((k) => k.kid === header.kid);
  if (!jwk) return null;
  try {
    const key = await crypto.subtle.importKey('jwk', { kty: 'RSA', n: jwk.n, e: jwk.e, alg: 'RS256', ext: true },
      { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, false, ['verify']);
    const ok = await crypto.subtle.verify('RSASSA-PKCS1-v1_5', key, b64urlToBytes(parts[2]), enc(parts[0] + '.' + parts[1]));
    if (!ok) return null;
  } catch { return null; }
  const now = Date.now() / 1000;
  if (typeof payload.exp !== 'number' || payload.exp < now) return null;
  if (typeof payload.nbf === 'number' && payload.nbf > now + 60) return null;
  if (payload.iss !== `https://${team}`) return null;
  const auds = Array.isArray(payload.aud) ? payload.aud : [payload.aud];
  if (!auds.includes(aud)) return null;
  // The email comes only from the signed token payload, never from a browser-supplied header.
  const allowed = String(env.ADMIN_EMAIL || '').trim().toLowerCase();
  const email = typeof payload.email === 'string' ? payload.email.trim().toLowerCase() : '';
  if (!allowed || email !== allowed) return null;
  return { email, iat: typeof payload.iat === 'number' ? payload.iat : 0 };
}

// ---------------------------------------------------------------- Worker entry
// Two hostnames, two origins:
//   print.justifiedsystems.com.au  printers and visitors (untrusted printer content)
//   admin.justifiedsystems.com.au  the site owner's admin (Cloudflare Access)
// Nothing admin-related is served on the print host, and nothing printer-generated on the admin host.
const hostOf = (origin, fallback) => { try { return new URL(origin).hostname; } catch { return fallback; } };
const ADMIN_PATHS = ['/', '/action', '/logout', '/log.txt'];

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;
    const host = url.hostname;
    const headers = new Headers(request.headers);
    for (const h of ['x-relay-admin', 'x-relay-admin-host', 'x-relay-cf']) headers.delete(h); // never trust these from outside

    if (path === '/robots.txt') return plain(200, 'User-agent: *\nDisallow: /');

    if (host === hostOf(env.ADMIN_ORIGIN, 'admin.justifiedsystems.com.au')) {
      if (!ADMIN_PATHS.includes(path)) return plain(404, 'Not found');
      headers.set('x-relay-admin-host', '1');
      if (path !== '/logout') {
        let who = null;
        try { who = await verifyAccess(request, env); } catch { who = null; }
        if (!who) return plain(403, 'Forbidden');
        headers.set('x-relay-admin', encodeURIComponent(JSON.stringify(who)));
      }
    } else if (host === hostOf(env.PUBLIC_ORIGIN, 'print.justifiedsystems.com.au')) {
      if (path !== '/relay/connect' && !path.startsWith('/p/')) return plain(404, 'Not found');
    } else {
      return plain(404, 'Not found'); // any other hostname (e.g. workers.dev) serves nothing
    }

    const cf = request.cf || {};
    headers.set('x-relay-cf', encodeURIComponent(JSON.stringify({
      city: cf.city || '', region: cf.region || '', country: cf.country || '', org: cf.asOrganization || '',
    })));
    const stub = env.RELAY.get(env.RELAY.idFromName('relay'));
    return stub.fetch(new Request(request, { headers }));
  },
};

// ---------------------------------------------------------------- Durable Object
// One object holds every printer: their sockets (hibernation API), queues, state,
// codes, counters and device approvals. Plenty for dozens of printers.
export class Relay extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.rt = new Map(); // printer id -> { active, queue, seq } (in memory only)
    this.sql.exec(`
      CREATE TABLE IF NOT EXISTS printers(
        id TEXT PRIMARY KEY, pubkey TEXT NOT NULL, status TEXT NOT NULL, code TEXT UNIQUE,
        name TEXT, owner TEXT, mac TEXT, fw TEXT, visible INTEGER DEFAULT 0, secure INTEGER DEFAULT 0,
        owner_hash TEXT, ip TEXT, place TEXT, org TEXT, requested_at INTEGER, last_seen INTEGER,
        prints_total INTEGER DEFAULT 0, prints_day INTEGER DEFAULT 0, day TEXT,
        tries_start INTEGER DEFAULT 0, tries INTEGER DEFAULT 0, asks_start INTEGER DEFAULT 0, asks INTEGER DEFAULT 0);
      CREATE TABLE IF NOT EXISTS devices(
        printer_id TEXT NOT NULL, token_hash TEXT NOT NULL, name TEXT, role TEXT NOT NULL,
        owner_hash TEXT, created INTEGER, place TEXT, PRIMARY KEY(printer_id, token_hash));
      CREATE TABLE IF NOT EXISTS misses(ip TEXT PRIMARY KEY, start INTEGER, n INTEGER);
      CREATE TABLE IF NOT EXISTS events(
        id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL, pid TEXT, kind TEXT NOT NULL, msg TEXT NOT NULL);
      CREATE INDEX IF NOT EXISTS events_pid ON events(pid, id);
      CREATE TABLE IF NOT EXISTS admin_sessions(
        id_hash TEXT PRIMARY KEY, email TEXT NOT NULL, created INTEGER NOT NULL, last INTEGER NOT NULL, csrf_hash TEXT);
    `);
    try { this.sql.exec('ALTER TABLE devices ADD COLUMN csrf_hash TEXT'); } catch {} // one-time owner form token
    try { this.sql.exec('ALTER TABLE printers ADD COLUMN paused INTEGER DEFAULT 0'); } catch {} // firmware 0.23: owner paused online printing
    try { this.sql.exec('ALTER TABLE devices ADD COLUMN kind TEXT'); } catch {}             // 0.26: device type, e.g. "iPhone, Safari"
    try { this.sql.exec('ALTER TABLE events ADD COLUMN n INTEGER DEFAULT 1'); } catch {}    // 0.24: repeats counted
    try { this.sql.exec('ALTER TABLE events ADD COLUMN last INTEGER'); } catch {}            // 0.24: time of the latest repeat
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }

  one(q, ...args) { return this.sql.exec(q, ...args).toArray()[0]; }

  // ------------------------------------------------------------ activity log (0.23.1)
  // kind: 'info' | 'warn' | 'error'. Never throws: logging must not break a request.
  // Flooding guard (0.24): anyone with a link can cause failed requests, so
  //  - the same event again within a minute adds to a count on its line ("x200"),
  //    keyed without the timings so identical requests match;
  //  - each printer (and the relay itself) gets LOG_PER_MIN new lines a minute;
  //    beyond that one line counts what was not shown.
  // Counting updates a line instead of adding one, so it cannot push history out.
  log(pid, kind, msg, key = msg) {
    try {
      const now = Date.now(), b = pid || '-';
      this.logState = this.logState || new Map();
      let st = this.logState.get(b);
      if (!st) { st = { last: new Map(), start: now, n: 0, over: null }; this.logState.set(b, st); }
      const prev = st.last.get(key);
      if (prev && now - prev.at < LOG_MERGE_MS
          && this.sql.exec('UPDATE events SET n = n + 1, last = ? WHERE id = ?', now, prev.id).rowsWritten) return;
      if (now - st.start >= 60_000) { st.start = now; st.n = 0; st.over = null; }
      // Connects, admin actions and the like are rare and need a key or Access; only
      // what a visitor or stranger can trigger counts towards the limit.
      if (kind !== 'info' && st.n >= LOG_PER_MIN) {
        if (st.over && this.sql.exec('UPDATE events SET n = n + 1, last = ? WHERE id = ?', now, st.over).rowsWritten) return;
        this.sql.exec('INSERT INTO events(at, pid, kind, msg, n, last) VALUES (?, ?, ?, ?, 1, ?)', now, pid || null, 'warn',
          `More events this minute not shown (too many at once)`, now);
        st.over = this.one('SELECT last_insert_rowid() AS id').id;
        return;
      }
      if (kind !== 'info') st.n++;
      this.sql.exec('INSERT INTO events(at, pid, kind, msg, n, last) VALUES (?, ?, ?, ?, 1, ?)', now, pid || null, kind, String(msg).slice(0, 300), now);
      const id = this.one('SELECT last_insert_rowid() AS id').id;
      st.last.set(key, { id, at: now });
      if (st.last.size > 200) st.last.delete(st.last.keys().next().value);
      this.logN = (this.logN || 0) + 1;
      if (this.logN % 50 === 1) {
        this.sql.exec('DELETE FROM events WHERE at < ?', Date.now() - LOG_DAYS * 86_400_000);
        this.sql.exec('DELETE FROM events WHERE id <= (SELECT MAX(id) FROM events) - ?', LOG_KEEP);
      }
    } catch {}
  }
  // A failed visitor request: method + path only (no query, no body).
  logReq(pid, job, status, why) {
    const kb = job.body ? Math.round(job.body.byteLength / 1024) : 0;
    const secs = ((Date.now() - (job.t0 || Date.now())) / 1000).toFixed(1);
    const what = `${job.m} ${job.path} -> ${status}: ${why || 'no reason given'}`;
    this.log(pid, status >= 500 ? 'error' : 'warn', `${what} (${kb} KB in, ${secs} s, ${job.who})`, `${what} ${job.who}`);
  }

  // One-time CSRF tokens. Each page render issues a fresh random token; only its hash is
  // stored. A form post must present it, and it is consumed on use (or replaced by the
  // next render), so a token works once and only from the latest page.
  async issueCsrf(table, where, ...args) {
    const token = randomToken();
    this.sql.exec(`UPDATE ${table} SET csrf_hash = ? WHERE ${where}`, await sha256hex(token), ...args);
    return token;
  }
  async consumeCsrf(stored, given, table, where, ...args) {
    if (!stored || typeof given !== 'string' || !/^[A-Za-z0-9_-]{22}$/.test(given)) return false;
    if ((await sha256hex(given)) !== stored) return false;
    this.sql.exec(`UPDATE ${table} SET csrf_hash = NULL WHERE ${where}`, ...args);
    return true;
  }
  all(q, ...args) { return this.sql.exec(q, ...args).toArray(); }
  link(code) { return `${String(this.env.PUBLIC_ORIGIN || 'https://print.justifiedsystems.com.au').replace(/\/+$/, '')}/p/${code}/`; }
  cfInfo(request) {
    try { return JSON.parse(decodeURIComponent(request.headers.get('x-relay-cf') || '')); } catch { return {}; }
  }
  rtFor(pid) {
    let r = this.rt.get(pid);
    if (!r) { r = { active: null, queue: [], seq: 0 }; this.rt.set(pid, r); }
    return r;
  }
  socketFor(pid, exclude) {
    for (const ws of this.ctx.getWebSockets()) {
      if (ws === exclude || ws.readyState !== 1) continue;
      const a = ws.deserializeAttachment() || {};
      if (a.pid === pid) return ws;
    }
    return null;
  }
  newCode() {
    for (;;) {
      let c = '';
      while (c.length < 6) {
        for (const x of crypto.getRandomValues(new Uint8Array(16))) {
          if (x < 248 && c.length < 6) c += ALPHABET[x % 31]; // 248 = 8*31, no bias
        }
      }
      if (!this.one('SELECT 1 AS x FROM printers WHERE code = ?', c)) return c;
    }
  }

  async fetch(request) {
    const res = await this.route(request);
    // Drop any request body a refusal did not read.
    if (request.body && !request.bodyUsed) request.body.cancel().catch(() => {});
    return res;
  }

  async route(request) {
    const url = new URL(request.url);
    try {
      if (request.headers.get('x-relay-admin-host') === '1') return await this.admin(request, url);
      if (url.pathname === '/relay/connect') return await this.connect(request);
      if (url.pathname.startsWith('/p/')) return await this.visitor(request, url);
      return plain(404, 'Not found');
    } catch (e) {
      return plain(500, 'Something went wrong.');
    }
  }

  // ------------------------------------------------------------ printer connection
  async connect(request) {
    if ((request.headers.get('upgrade') || '').toLowerCase() !== 'websocket') return plain(426, 'Expected a WebSocket.');
    if (this.ctx.getWebSockets().length >= MAX_SOCKETS) return plain(503, 'Busy.');
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    const cf = this.cfInfo(request);
    const nonce = b64url(crypto.getRandomValues(new Uint8Array(32)));
    server.serializeAttachment({
      nonce, at: Date.now(), ip: (request.headers.get('cf-connecting-ip') || '').slice(0, 64),
      place: [cf.city, cf.region, cf.country].filter(Boolean).join(', ').slice(0, 120), org: String(cf.org || '').slice(0, 120),
    });
    server.send(JSON.stringify({ t: 'challenge', n: nonce }));
    if ((await this.ctx.storage.getAlarm()) == null) await this.ctx.storage.setAlarm(Date.now() + HELLO_TIMEOUT + 1000);
    return new Response(null, { status: 101, webSocket: client });
  }

  async alarm() {
    const now = Date.now();
    let waiting = false;
    for (const ws of this.ctx.getWebSockets()) {
      const a = ws.deserializeAttachment() || {};
      if (a.pid) continue;
      if (now - (a.at || 0) > HELLO_TIMEOUT) { try { ws.close(1008, 'No hello'); } catch {} }
      else waiting = true;
    }
    this.sql.exec('DELETE FROM misses WHERE start < ?', now - MISS_WINDOW);
    if (waiting) await this.ctx.storage.setAlarm(now + HELLO_TIMEOUT);
  }

  async webSocketMessage(ws, msg) {
    const a = ws.deserializeAttachment() || {};
    if (typeof msg !== 'string') {
      if (a.pid) this.onBinary(a.pid, ws, msg);
      else { try { ws.close(1008, 'Expected hello'); } catch {} }
      return;
    }
    if (msg === 'ping') { ws.send('pong'); return; }
    let m;
    try { m = JSON.parse(msg); } catch { m = null; }
    if (!a.pid) {
      if (m && m.t === 'hello') return this.onHello(ws, a, m);
      try { ws.close(1008, 'Expected hello'); } catch {}
      return;
    }
    if (!m || typeof m !== 'object') return;
    if (m.t === 'res') this.onRes(a.pid, ws, m);
    else if (m.t === 'end') this.onEnd(a.pid, ws, m);
    else if (m.t === 'newcode') this.onNewCode(a.pid);
  }

  async webSocketClose(ws, code, reason) { this.onSocketGone(ws, `code ${code}${reason ? ' ' + clean(reason, 80) : ''}`); }
  async webSocketError(ws, err) { this.onSocketGone(ws, `error ${clean(String(err && err.message || err || ''), 80)}`); }
  onSocketGone(ws, why = '') {
    try { ws.close(1000, 'Bye'); } catch {}
    const a = ws.deserializeAttachment() || {};
    if (!a.pid) return;
    if (!a.gone) {
      const mins = a.at ? ` after ${fmtDur(Date.now() - a.at)}` : '';
      this.log(a.pid, 'warn', `Disconnected${mins} (${why || 'no reason'})`);
    }
    this.sql.exec('UPDATE printers SET last_seen = ? WHERE id = ?', Date.now(), a.pid);
    this.dropSocket(a.pid, ws);
  }

  async onHello(ws, a, h) {
    const fail = (why) => {
      this.log(null, 'warn', `A printer's hello was refused: ${why}${a.place ? ' (' + a.place + ')' : ''}`);
      try { ws.close(1008, why); } catch {}
    };
    if (h.v !== 1) return fail('Unsupported version');
    let keyBytes, sig;
    try { keyBytes = b64urlToBytes(h.key); sig = b64urlToBytes(h.sig); } catch { return fail('Bad hello'); }
    if (keyBytes.length !== 65 || keyBytes[0] !== 4 || sig.length !== 64) return fail('Bad hello');
    let ok = false;
    try {
      const key = await crypto.subtle.importKey('raw', keyBytes, { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
      ok = await crypto.subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, key, sig, enc('smartthermal-relay-v1:' + a.nonce));
    } catch {}
    if (!ok) return fail('Bad signature');
    if (ws.readyState !== 1) return;

    const pid = b64url(new Uint8Array(await crypto.subtle.digest('SHA-256', keyBytes))).slice(0, 16);
    const info = {
      name: clean(h.name, 60) || 'Printer',
      owner: clean(h.owner, 60),
      mac: typeof h.mac === 'string' && /^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/.test(h.mac) ? h.mac.toUpperCase() : '',
      fw: clean(h.fw, 20),
      visible: h.visible === true ? 1 : 0,
      secure: h.secure === true ? 1 : 0,
      paused: h.paused === true ? 1 : 0, // firmware 0.23+: the owner paused online printing at home
      ownerHash: typeof h.ownerHash === 'string' && /^[0-9a-fA-F]{64}$/.test(h.ownerHash) ? h.ownerHash.toLowerCase() : '',
    };
    const now = Date.now();
    let p = this.one('SELECT * FROM printers WHERE id = ?', pid);
    if (!p) {
      // A new key: record a join request (limited so strangers cannot flood the list).
      const c = this.one("SELECT COUNT(*) AS n, COALESCE(SUM(ip = ?), 0) AS byip FROM printers WHERE status = 'pending'", a.ip);
      if (c.n >= MAX_PENDING_JOINS || c.byip >= MAX_JOINS_PER_IP) {
        this.log(null, 'warn', `Join request refused: too many waiting (${clean(h.name, 60) || 'Printer'}${a.place ? ', ' + a.place : ''})`);
        try { ws.close(1013, 'Try again later'); } catch {} return;
      }
      this.sql.exec(`INSERT OR IGNORE INTO printers(id, pubkey, status, name, owner, mac, fw, visible, secure, owner_hash,
        ip, place, org, requested_at, last_seen, paused) VALUES (?, ?, 'pending', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
        pid, h.key, info.name, info.owner, info.mac, info.fw, info.visible, info.secure, info.ownerHash,
        a.ip, a.place, a.org, now, now, info.paused);
    } else {
      if (p.owner_hash !== info.ownerHash) {
        // New owner code: sign out existing owner devices.
        this.sql.exec("DELETE FROM devices WHERE printer_id = ? AND role = 'owner'", pid);
      }
      this.sql.exec(`UPDATE printers SET name = ?, owner = ?, mac = ?, fw = ?, visible = ?, secure = ?, owner_hash = ?,
        paused = ?, last_seen = ? WHERE id = ?`, info.name, info.owner, info.mac, info.fw, info.visible, info.secure, info.ownerHash,
        info.paused, now, pid);
      if (p.status === 'pending') this.sql.exec('UPDATE printers SET ip = ?, place = ?, org = ? WHERE id = ?', a.ip, a.place, a.org, pid);
    }
    const isNew = !p;
    p = this.one('SELECT * FROM printers WHERE id = ?', pid);

    // Firmware 0.23.1+ says why its last connection ended, so a drop shows from both sides.
    const up = Number.isInteger(h.up) && h.up >= 0 ? h.up : null;
    const heap = Number.isInteger(h.heap) && h.heap >= 0 ? h.heap : null;
    const drops = Number.isInteger(h.drops) && h.drops >= 0 ? h.drops : null;
    const facts = [`fw ${info.fw || '?'}`];
    if (up !== null) facts.push(`up ${fmtDur(up * 1000)}`);
    if (heap !== null) facts.push(`largest free block ${Math.round(heap / 1024)} KB`);
    if (drops) facts.push(`${drops} drop${drops === 1 ? '' : 's'} since start`);
    const last = clean(h.last, 120);
    if (isNew) this.log(pid, 'info', `Join request (${a.place || 'unknown place'}; ${facts.join(', ')})`);
    else this.log(pid, 'info', `Connected (${facts.join(', ')})${last ? `. Printer's last drop: ${last}` : ''}`);

    ws.serializeAttachment({ pid, at: now });
    this.sendState(ws, p);
    // A reconnect replaces any older connection from the same printer.
    for (const other of this.ctx.getWebSockets()) {
      if (other === ws) continue;
      if ((other.deserializeAttachment() || {}).pid === pid) {
        this.log(pid, 'info', 'Older connection replaced by the new one');
        try { other.close(1000, 'Replaced'); } catch {}
        this.dropSocket(pid, other);
      }
    }
  }

  stateMsg(p) {
    if (p.status === 'on') return JSON.stringify({ t: 'state', s: 'on', code: p.code, url: this.link(p.code) });
    return JSON.stringify({ t: 'state', s: p.status });
  }
  sendState(ws, p) {
    try {
      ws.send(this.stateMsg(p));
      if (p.status === 'declined' || p.status === 'off') ws.close(1000, p.status);
    } catch {}
  }

  onNewCode(pid) {
    const p = this.one('SELECT * FROM printers WHERE id = ?', pid);
    if (!p || p.status !== 'on') return;
    this.sql.exec('UPDATE printers SET code = ? WHERE id = ?', this.newCode(), pid); // old code is gone at once (404)
    this.enqueueMsg(pid, this.stateMsg(this.one('SELECT * FROM printers WHERE id = ?', pid)));
  }

  // ------------------------------------------------------------ per-printer queue
  enqueueMsg(pid, data) {
    if (!this.socketFor(pid)) return;
    this.rtFor(pid).queue.push({ kind: 'msg', data });
    this.pump(pid);
  }

  pump(pid) {
    const r = this.rtFor(pid);
    while (!r.active && r.queue.length) {
      const job = r.queue.shift();
      const ws = this.socketFor(pid);
      if (!ws) { this.failJob(job, 502, 'The printer went offline.'); continue; }
      if (job.kind === 'msg') { try { ws.send(job.data); } catch {} continue; }
      clearTimeout(job.waitTimer);
      r.active = job;
      job.ws = ws;
      try {
        const len = job.body.byteLength;
        ws.send(JSON.stringify({ t: 'req', id: job.id, m: job.m, p: job.p, ct: job.ct, len, who: job.who, ua: job.ua }));
        for (let i = 0; i < len; i += FRAME) ws.send(job.body.slice(i, i + FRAME));
      } catch {
        r.active = null;
        this.failJob(job, 502, 'The printer went offline.');
        continue;
      }
      job.timer = setTimeout(() => this.timeout(pid, job), REQ_TIMEOUT);
    }
  }

  failJob(job, status, msg) {
    if (job.kind !== 'req' || job.done) return;
    job.done = true;
    if (job.pid) this.logReq(job.pid, job, status, msg);
    clearTimeout(job.timer); clearTimeout(job.waitTimer);
    if (!job.res) job.resolve(plain(status, msg));
    else job.writer.abort(new Error(msg)).catch(() => {});
  }

  timeout(pid, job) {
    const r = this.rtFor(pid);
    if (r.active !== job) return;
    r.active = null;
    this.failJob(job, 504, 'The printer took too long to answer.');
    // Start clean: late frames from this request must not leak into the next one.
    try { job.ws.serializeAttachment({ ...(job.ws.deserializeAttachment() || {}), gone: true }); } catch {}
    this.log(pid, 'warn', 'Connection closed by the relay after the timeout (the printer reconnects)');
    try { job.ws.close(1011, 'Request timed out'); } catch {}
    this.dropSocket(pid, job.ws);
  }

  dropSocket(pid, ws) {
    const r = this.rt.get(pid);
    if (!r) return;
    if (r.active && r.active.ws === ws) {
      const j = r.active;
      r.active = null;
      this.failJob(j, 502, 'The printer went offline.');
    }
    if (this.socketFor(pid, ws)) { this.pump(pid); return; }
    const q = r.queue;
    r.queue = [];
    for (const j of q) this.failJob(j, 502, 'The printer went offline.');
  }

  onRes(pid, ws, m) {
    const job = this.rtFor(pid).active;
    if (!job || job.ws !== ws || m.id !== job.id || job.res) return;
    const status = Number.isInteger(m.s) && m.s >= 200 && m.s <= 599 ? m.s : 502;
    const h = printerHeaders();
    try { if (typeof m.ct === 'string' && m.ct) h.set('content-type', m.ct.slice(0, 200)); } catch {}
    try { if (typeof m.cc === 'string' && m.cc) h.set('cache-control', m.cc.slice(0, 200)); } catch {}
    const ts = new TransformStream();
    job.writer = ts.writable.getWriter();
    job.res = true;
    job.status = status;
    const noBody = status === 204 || status === 205 || status === 304;
    job.resolve(new Response(noBody ? null : ts.readable, { status, headers: h }));
  }

  onBinary(pid, ws, data) {
    const job = this.rtFor(pid).active;
    if (!job || job.ws !== ws || !job.res || job.status === 204 || job.status === 205 || job.status === 304) return;
    if (job.status >= 400 && (job.err || '').length < 300) {
      try { job.err = (job.err || '') + new TextDecoder().decode(new Uint8Array(data).slice(0, 300)); } catch {}
    }
    job.writer.write(new Uint8Array(data)).catch(() => {}); // visitor may have left; keep draining
  }

  onEnd(pid, ws, m) {
    const r = this.rtFor(pid);
    const job = r.active;
    if (!job || job.ws !== ws || m.id !== job.id) return;
    r.active = null;
    clearTimeout(job.timer);
    if (!job.res) { this.failJob(job, 502, 'The printer sent no answer.'); }
    else {
      job.done = true;
      job.writer.close().catch(() => {});
      if (job.status >= 400) this.logReq(pid, job, job.status, errText(job.err));
      else if (Date.now() - job.t0 > SLOW_MS) this.logReq(pid, job, job.status, 'OK but slow');
      if (job.countPrint && job.status < 300) {
        const today = perthDay();
        this.sql.exec(`UPDATE printers SET prints_total = prints_total + 1,
          prints_day = CASE WHEN day = ? THEN prints_day + 1 ELSE 1 END, day = ? WHERE id = ?`, today, today, pid);
      }
    }
    this.pump(pid);
  }

  // ------------------------------------------------------------ visitors
  limited(ip) {
    const row = this.one('SELECT start, n FROM misses WHERE ip = ?', ip);
    return !!row && Date.now() - row.start < MISS_WINDOW && row.n > MISS_LIMIT;
  }
  miss(ip) {
    const now = Date.now();
    const row = this.one('SELECT start, n FROM misses WHERE ip = ?', ip);
    if (!row || now - row.start >= MISS_WINDOW) this.sql.exec('INSERT OR REPLACE INTO misses(ip, start, n) VALUES (?, ?, 1)', ip, now);
    else {
      this.sql.exec('UPDATE misses SET n = n + 1 WHERE ip = ?', ip);
      if (row.n + 1 === MISS_LIMIT + 1) this.log(null, 'warn', `Link guessing: an address tried ${MISS_LIMIT + 1} unknown links and is blocked for 10 minutes`);
    }
  }

  async device(request, p) {
    const tok = cookie(request, 'd_' + p.id);
    if (!tok || !/^[A-Za-z0-9_-]{22}$/.test(tok)) return null;
    const d = this.one('SELECT * FROM devices WHERE printer_id = ? AND token_hash = ?', p.id, await sha256hex(tok));
    if (!d) return null;
    if (d.role === 'owner' && d.owner_hash !== p.owner_hash) return null; // owner code changed
    return d;
  }

  async visitor(request, url) {
    const ip = request.headers.get('cf-connecting-ip') || 'unknown';
    // While an IP is over the guessing limit, every /p/ request is refused, so hits and
    // misses look the same.
    if (this.limited(ip)) return plain(429, 'Too many requests. Try again later.', { 'retry-after': '600' });

    const m = url.pathname.match(/^\/p\/([0-9A-Za-z]{6})(-a)?(\/.*)?$/);
    const code = m && m[1].toLowerCase();
    const p = code && [...code].every((c) => ALPHABET.includes(c))
      ? this.one("SELECT * FROM printers WHERE code = ? AND status = 'on'", code) : null;
    if (!p) { this.miss(ip); return plain(404, 'Not found'); }

    if (m[2]) {
      if (m[3] && m[3] !== '/') return plain(404, 'Not found');
      return this.ownerPage(request, url, p, code);
    }
    const rest = m[3];
    if (!rest) return redirect(301, `/p/${code}/${url.search}`);

    const dev = await this.device(request, p);
    const approved = !!dev && (dev.role === 'guest' || dev.role === 'owner');

    if (rest === '/_r/ask') {
      if (!p.secure) return plain(404, 'Not found');
      if (request.method !== 'POST') return redirect(303, `/p/${code}/`);
      return this.ask(request, p, dev, code);
    }
    if (p.secure && !approved) {
      if (rest === '/' && request.method === 'GET') return this.askPage(p, dev, code);
      return plain(403, 'This printer needs its owner to approve your device first.');
    }
    if (rest === '/_r/printers') {
      if (request.method !== 'GET') return plain(405, 'Method not allowed', { allow: 'GET' });
      const list = this.all("SELECT id, name, code, paused FROM printers WHERE status = 'on' AND visible = 1 AND id != ? ORDER BY name", p.id)
        .map((x) => ({ name: x.name, code: x.code, online: !!this.socketFor(x.id), paused: !!x.paused }));
      const h = printerHeaders();
      h.set('content-type', 'application/json');
      return new Response(JSON.stringify(list), { headers: h });
    }
    if (rest.startsWith('/_r/')) return plain(404, 'Not found');

    return this.forward(request, url, p, rest, dev && dev.role === 'owner' ? 'owner' : 'guest');
  }

  async forward(request, url, p, rest, who) {
    const method = request.method;
    if (method !== 'GET' && method !== 'POST') return plain(405, 'Method not allowed', { allow: 'GET, POST' });
    let norm;
    try { norm = decodeURIComponent(rest); } catch { return plain(400, 'Bad request'); }
    norm = norm.replace(/\/{2,}/g, '/').toLowerCase();
    if (norm.includes('..') || norm.includes('\\')) return plain(400, 'Bad request');
    const refuse = (status, msg, extra) => {
      this.logReq(p.id, { m: method, path: rest.slice(0, 80), who, body: null }, status, msg);
      return plain(status, msg, extra);
    };
    if (isBlocked(norm, method)) return refuse(403, 'Forbidden');
    // Paused by the owner at home (firmware 0.23+): say so here instead of
    // sending the print down to the printer, which would refuse it anyway.
    if (p.paused && method === 'POST' && (norm.startsWith('/api/print/') || norm === '/api/items/run')) {
      return refuse(503, "Printing is paused by the printer's owner.", { 'retry-after': '600' });
    }
    if (!this.socketFor(p.id)) return refuse(503, 'This printer is offline right now. Try again later.', { 'retry-after': '30' });

    let body = new ArrayBuffer(0);
    if (method === 'POST') {
      const cl = Number(request.headers.get('content-length') || 0);
      if (cl > MAX_BODY) return refuse(413, 'Too large. The limit is 160 KB.');
      body = await readLimited(request.body, MAX_BODY);
      if (body === null) return refuse(413, 'Too large. The limit is 160 KB.');
    }
    const r = this.rtFor(p.id);
    if (r.queue.filter((j) => j.kind === 'req').length >= MAX_QUEUE) return refuse(503, 'The printer is busy. Try again shortly.', { 'retry-after': '5' });

    return new Promise((resolve) => {
      const job = {
        pid: p.id, kind: 'req', id: ++r.seq, m: method, p: rest + url.search, path: rest.slice(0, 80), t0: Date.now(),
        ct: (request.headers.get('content-type') || '').slice(0, 200), body, who,
        ua: (request.headers.get('user-agent') || '').slice(0, 200),
        countPrint: method === 'POST' && norm.startsWith('/api/print/'), resolve,
      };
      job.waitTimer = setTimeout(() => {
        const i = r.queue.indexOf(job);
        if (i >= 0) { r.queue.splice(i, 1); this.failJob(job, 504, 'The printer took too long to answer.'); }
      }, REQ_TIMEOUT);
      r.queue.push(job);
      this.pump(p.id);
    });
  }

  // ------------------------------------------------------------ extra secure printers
  askPage(p, dev, code, note = '') {
    let body;
    if (dev && dev.role === 'pending') {
      body = `<h1>Request sent</h1><p>The printer's owner needs to approve this device. Reload this page once they have.</p>`;
    } else if (dev && dev.role === 'declined') {
      body = `<h1>Not approved</h1><p>The owner of this printer did not approve this device.</p>`;
    } else {
      body = `<h1>Ask to print here</h1>
<p><strong>${esc(p.name)}</strong> only prints for devices its owner has approved. Send a request and they will get a note on the printer.</p>
${note ? `<p class="note">${esc(note)}</p>` : ''}
<form method="post" action="/p/${code}/_r/ask">
  <label for="name">Your name</label>
  <input id="name" name="name" maxlength="40" required autocomplete="name">
  <button type="submit">Ask to print</button>
</form>
<p class="muted small"><strong>Information collected:</strong> the name you enter, the type of device
(for example \u201ciPhone, Safari\u201d) and an approximate location from your internet connection
(city, region, country). They are printed on a slip for the printer's owner and shown on their
owner page, so they can decide. They are kept until the owner removes this device.</p>`;
    }
    return html(shell(p.name, body));
  }

  async ask(request, p, dev, code) {
    if (!navigationOnly(request)) return plain(403, 'Forbidden');
    if (dev) return redirect(303, `/p/${code}/`);
    if (Number(request.headers.get('content-length') || 0) > 2048) return plain(413, 'Too large.');
    let name = '';
    try { name = clean((await request.formData()).get('name'), 40); } catch {}
    if (!name) return redirect(303, `/p/${code}/`);

    const now = Date.now();
    let { asks_start: start, asks } = p;
    if (now - start > OWNER_WINDOW) { start = now; asks = 0; }
    const waiting = this.one("SELECT COUNT(*) AS n FROM devices WHERE printer_id = ? AND role = 'pending'", p.id).n;
    if (asks >= ASK_LIMIT || waiting >= MAX_PENDING_DEVICES) {
      return html(shell(p.name, '<h1>Please try later</h1><p>This printer has had a lot of requests. Try again in an hour.</p>'), 429);
    }
    this.sql.exec('UPDATE printers SET asks_start = ?, asks = ? WHERE id = ?', start, asks + 1, p.id);

    const token = randomToken();
    const cf = this.cfInfo(request);
    const place = [cf.city, cf.region, cf.country].filter(Boolean).join(', ').slice(0, 120);
    const kind = deviceKind(request.headers.get('user-agent'));
    this.sql.exec("INSERT INTO devices(printer_id, token_hash, name, role, created, place, kind) VALUES (?, ?, ?, 'pending', ?, ?, ?)",
      p.id, await sha256hex(token), name, now, place, kind);
    // "Knock knock!" slip. dev/loc (0.26) are ignored by older firmware.
    this.enqueueMsg(p.id, JSON.stringify({ t: 'notice', k: 'device-request', name, dev: kind, loc: place.slice(0, 63) }));
    this.log(p.id, 'info', 'A device asked to print (extra secure)');
    return redirect(303, `/p/${code}/`, { 'set-cookie': deviceCookie(p.id, token) });
  }

  async ownerPage(request, url, p, code) {
    if (!navigationOnly(request)) return plain(403, 'Forbidden');
    const self = `/p/${code}-a`;
    const dev = await this.device(request, p);
    const isOwner = !!dev && dev.role === 'owner';

    if (request.method === 'POST') {
      if (Number(request.headers.get('content-length') || 0) > 2048) return plain(413, 'Too large.');
      let f;
      try { f = await request.formData(); } catch { return redirect(303, self); }
      const action = String(f.get('action') || '');

      if (action === 'login') {
        const now = Date.now();
        let { tries_start: start, tries } = p;
        if (now - start > OWNER_WINDOW) { start = now; tries = 0; }
        if (tries >= OWNER_TRIES) { this.log(p.id, 'warn', 'Owner page locked: too many wrong codes'); return redirect(303, `${self}?e=locked`); }
        const given = clean(f.get('code'), 20).toUpperCase();
        const ok = !!p.owner_hash && !!given && (await sha256hex('smartthermal-owner-v1:' + given)) === p.owner_hash;
        if (!ok) {
          this.sql.exec('UPDATE printers SET tries_start = ?, tries = ? WHERE id = ?', start, tries + 1, p.id);
          this.log(p.id, 'warn', `Wrong owner code on the owner page (${tries + 1} of ${OWNER_TRIES} this hour)`);
          return redirect(303, `${self}?e=wrong`);
        }
        this.sql.exec('UPDATE printers SET tries = 0 WHERE id = ?', p.id);
        if (dev) {
          this.sql.exec("UPDATE devices SET role = 'owner', owner_hash = ? WHERE printer_id = ? AND token_hash = ?", p.owner_hash, p.id, dev.token_hash);
          return redirect(303, self);
        }
        const token = randomToken();
        this.sql.exec("INSERT INTO devices(printer_id, token_hash, name, role, owner_hash, created) VALUES (?, ?, 'Owner device', 'owner', ?, ?)",
          p.id, await sha256hex(token), p.owner_hash, now);
        return redirect(303, self, { 'set-cookie': deviceCookie(p.id, token) });
      }

      if (!isOwner) return redirect(303, self);
      if (!(await this.consumeCsrf(dev.csrf_hash, f.get('csrf'), 'devices', 'printer_id = ? AND token_hash = ?', p.id, dev.token_hash))) {
        return plain(403, 'This page has expired. Go back, reload it and try again.');
      }
      const id = String(f.get('id') || '');
      const target = /^[0-9a-f]{16}$/.test(id) ? this.one('SELECT * FROM devices WHERE printer_id = ? AND substr(token_hash, 1, 16) = ?', p.id, id) : null;
      if (action === 'off') {
        // Turn off (0.26): the printer saves online printing off and disconnects; it stays
        // off until switched back on in its Settings at home. Paused here as well, so
        // visitors are told at once; the next hello sets the real state again.
        if (!this.socketFor(p.id)) return redirect(303, `${self}?e=offline`);
        this.enqueueMsg(p.id, JSON.stringify({ t: 'owner', a: 'off' }));
        this.sql.exec('UPDATE printers SET paused = 1 WHERE id = ?', p.id);
        this.log(p.id, 'info', 'Online printing turned off from the owner page');
        return redirect(303, `${self}?m=off`);
      }
      if (action === 'signout') {
        this.sql.exec('DELETE FROM devices WHERE printer_id = ? AND token_hash = ?', p.id, dev.token_hash);
        return redirect(303, self, { 'set-cookie': deviceCookie(p.id, '', 0) });
      }
      if (target) {
        if (action === 'approve' && target.role === 'pending') this.sql.exec("UPDATE devices SET role = 'guest' WHERE printer_id = ? AND token_hash = ?", p.id, target.token_hash);
        if (action === 'decline' && target.role === 'pending') this.sql.exec("UPDATE devices SET role = 'declined' WHERE printer_id = ? AND token_hash = ?", p.id, target.token_hash);
        if (action === 'revoke') this.sql.exec('DELETE FROM devices WHERE printer_id = ? AND token_hash = ?', p.id, target.token_hash);
      }
      return redirect(303, self);
    }

    if (request.method !== 'GET') return plain(405, 'Method not allowed', { allow: 'GET, POST' });

    if (!isOwner) {
      const e = url.searchParams.get('e');
      const note = e === 'wrong' ? 'That code did not match. Check the welcome slip.' : e === 'locked' ? 'Too many tries. Wait an hour and try again.' : '';
      return html(shell(p.name, `<h1>Printer owner</h1>
<p>Enter the owner code from <strong>${esc(p.name)}</strong>'s welcome slip.</p>
${note ? `<p class="note">${esc(note)}</p>` : ''}
<form method="post" action="${self}">
  <input type="hidden" name="action" value="login">
  <label for="code">Owner code</label>
  <input id="code" name="code" maxlength="20" required autocomplete="off" autocapitalize="characters" spellcheck="false">
  <button type="submit">Continue</button>
</form>`));
    }

    const csrf = await this.issueCsrf('devices', 'printer_id = ? AND token_hash = ?', p.id, dev.token_hash);
    const online = !!this.socketFor(p.id);
    if (url.searchParams.get('confirm') === 'off') {
      return html(shell(p.name, `<h1>Turn off online printing?</h1>
<p>Nobody will be able to print to <strong>${esc(p.name)}</strong> over the internet, including you,
until it is switched back on in the printer's Settings on the home network.</p>
<form method="post" action="${self}" class="inline"><input type="hidden" name="csrf" value="${csrf}"><input type="hidden" name="action" value="off"><button>Turn off</button></form>
<form method="get" action="${self}" class="inline"><button class="ghost">Cancel</button></form>`));
    }
    const msg = url.searchParams.get('m'), err = url.searchParams.get('e');
    const note = msg === 'off' ? "Online printing is turned off. Switch it back on in the printer's Settings at home."
      : err === 'offline' ? 'The printer is offline right now, so it could not be turned off. Try again once it is online.' : '';
    const devs = this.all('SELECT * FROM devices WHERE printer_id = ? ORDER BY created DESC', p.id);
    const link = this.link(code);
    const btn = (id, action, label, ghost) => `<form method="post" action="${self}" class="inline"><input type="hidden" name="csrf" value="${csrf}"><input type="hidden" name="action" value="${action}"><input type="hidden" name="id" value="${esc(id)}"><button${ghost ? ' class="ghost"' : ''}>${label}</button></form>`;
    const row = (d, actions) => `<li><div><strong>${esc(d.name)}</strong>${d.token_hash === dev.token_hash ? ' (this device)' : ''}<br><span class="muted">${esc(perth(d.created))}${d.kind ? ' · ' + esc(d.kind) : ''}${d.place ? ' · ' + esc(d.place) : ''}</span></div><div class="acts">${actions}</div></li>`;
    const pending = devs.filter((d) => d.role === 'pending');
    const approvedDevs = devs.filter((d) => d.role === 'guest' || (d.role === 'owner' && d.owner_hash === p.owner_hash));
    const body = `<h1>${esc(p.name)}</h1>
${note ? `<p class="note">${esc(note)}</p>` : ''}
<p class="muted">${p.secure ? 'Extra secure: only approved devices can print.' : 'Anyone with the link can print. Approvals apply to extra secure printers only.'}</p>
<h2>Printer link</h2>
${online && !p.paused ? `<p><img class="qr" src="/p/${code}/api/qr.svg?t=${encodeURIComponent(link)}" width="168" height="168" alt="QR code for the printer link"></p>` : ''}
<p><code>${esc(link)}</code><br><span class="muted">${p.secure ? 'Devices that scan it ask first; approve them below.' : 'Anyone with the link can print.'}</span></p>
<h2>Requests</h2>
${pending.length ? `<ul class="list">${pending.map((d) => row(d, btn(d.token_hash.slice(0, 16), 'approve', 'Approve') + btn(d.token_hash.slice(0, 16), 'decline', 'Decline', true))).join('')}</ul>` : '<p class="muted">No requests waiting.</p>'}
<h2>Approved devices</h2>
${approvedDevs.length ? `<ul class="list">${approvedDevs.map((d) => row(d, (d.role === 'owner' ? '<span class="pill">Owner</span>' : '') + btn(d.token_hash.slice(0, 16), 'revoke', 'Revoke', true))).join('')}</ul>` : '<p class="muted">None yet.</p>'}
<h2>Online printing</h2>
${p.paused ? '<p class="muted">Paused or turned off at the printer.</p>' : `<p class="muted">Turning it off stops all online printing until it is switched back on in the printer's Settings at home.</p>
<form method="get" action="${self}"><input type="hidden" name="confirm" value="off"><button>Turn off online printing</button></form>`}
<form method="post" action="${self}"><input type="hidden" name="csrf" value="${csrf}"><input type="hidden" name="action" value="signout"><button class="ghost">Sign out this device</button></form>`;
    return html(shell(p.name, body), 200, { 'content-security-policy': OWNER_CSP });
  }

  // Admin page section: newest 150 events, optionally for one printer or errors only.
  activity(url, rows) {
    const names = new Map(rows.map((x) => [x.id, x.name]));
    const pid = url.searchParams.get('log');
    const only = /^[A-Za-z0-9_-]{16}$/.test(pid || '') && names.has(pid) ? pid : null;
    const errs = url.searchParams.get('k') === 'problems';
    const where = [], args = [];
    if (only) { where.push('pid = ?'); args.push(only); }
    if (errs) where.push("kind != 'info'");
    const ev = this.all(`SELECT * FROM events ${where.length ? 'WHERE ' + where.join(' AND ') : ''} ORDER BY id DESC LIMIT 150`, ...args);
    const total = this.one('SELECT COUNT(*) AS n FROM events').n;
    const q = (o) => { const u = new URLSearchParams(); if (o.log) u.set('log', o.log); if (o.k) u.set('k', o.k); const s = u.toString(); return s ? `/?${s}#log` : '/#log'; };
    const chip = (label, href, on) => `<a class="pill${on ? ' ok' : ''}" href="${esc(href)}">${esc(label)}</a>`;
    const filters = [chip('Everything', q({ log: only }), !errs), chip('Problems only', q({ log: only, k: 'problems' }), errs),
      chip('All printers', q({ k: errs ? 'problems' : '' }), !only),
      ...rows.map((x) => chip(x.name, q({ log: x.id, k: errs ? 'problems' : '' }), only === x.id))].join(' ');
    const mark = { info: '', warn: '<span class="pill">Warning</span> ', error: '<span class="pill err">Error</span> ' };
    const items = ev.map((e) => `<tr><td>${esc(perth(e.at))}</td><td>${e.pid ? esc(names.get(e.pid) || 'Removed printer') : '<span class="muted">Relay</span>'}</td><td>${mark[e.kind] || ''}${esc(e.msg)}${times(e)}</td></tr>`).join('');
    return `<h2 id="log">Activity log</h2>
<p class="muted">${total} entr${total === 1 ? 'y' : 'ies'} kept. Showing the newest ${ev.length}. <a href="/log.txt">Download all (text)</a></p>
<p class="filters">${filters}</p>
${items ? `<div class="scroll"><table class="log"><thead><tr><th>When</th><th>Printer</th><th>What happened</th></tr></thead><tbody>${items}</tbody></table></div>` : '<p class="muted">Nothing logged yet.</p>'}`;
  }

  // ------------------------------------------------------------ site owner admin
  // Admin session, on top of Cloudflare Access:
  //  - A relay session starts only straight after a real Access sign-in (token issued in the
  //    last 2 minutes). A token Access renewed in the background, or an old one, cannot start one.
  //  - It ends after 5 minutes without a request (ADMIN_IDLE_SECONDS) or 8 hours in total.
  //  - Ending it also ends the Access session: the browser is sent to Access's logout, which
  //    revokes the session for all applications. Signing in again means a full Access login.
  //  - Open admin pages send themselves to /logout after 5 minutes (no script needed).
  adminCfg() {
    return {
      idle: Math.max(1, Number(this.env.ADMIN_IDLE_SECONDS) || 300) * 1000,
      max: 8 * 3600_000,
      fresh: 120, // seconds: how recent the Access sign-in must be to start a session
      origin: String(this.env.ADMIN_ORIGIN || 'https://admin.justifiedsystems.com.au').replace(/\/+$/, ''),
    };
  }
  endAdminSession(cfg) {
    return redirect(303, `${cfg.origin}/cdn-cgi/access/logout`, {
      'set-cookie': '__Host-relay_admin=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Lax',
    });
  }

  async admin(request, url) {
    const cfg = this.adminCfg();
    const now = Date.now();
    const sid = cookie(request, '__Host-relay_admin');
    const sidHash = sid && /^[A-Za-z0-9_-]{22}$/.test(sid) ? await sha256hex(sid) : null;
    this.sql.exec('DELETE FROM admin_sessions WHERE last < ? OR created < ?', now - cfg.idle, now - cfg.max);

    if (url.pathname === '/logout') {
      if (sidHash) this.sql.exec('DELETE FROM admin_sessions WHERE id_hash = ?', sidHash);
      return this.endAdminSession(cfg);
    }
    if (!navigationOnly(request)) return plain(403, 'Forbidden');
    let who = null;
    try { who = JSON.parse(decodeURIComponent(request.headers.get('x-relay-admin') || '')); } catch {}
    if (!who || !who.email) return plain(403, 'Forbidden');

    let sess = sidHash ? this.one('SELECT * FROM admin_sessions WHERE id_hash = ?', sidHash) : null;
    if (sess && sess.email !== who.email) { this.sql.exec('DELETE FROM admin_sessions WHERE id_hash = ?', sidHash); sess = null; }
    let setCookie = null;
    if (sess) {
      this.sql.exec('UPDATE admin_sessions SET last = ? WHERE id_hash = ?', now, sidHash);
    } else {
      // No live session: only a fresh, interactive Access sign-in may start one.
      const freshLogin = typeof who.iat === 'number' && who.iat > 0 && now / 1000 - who.iat <= cfg.fresh && who.iat <= now / 1000 + 60;
      if (!freshLogin || request.method !== 'GET' || url.pathname !== '/') return this.endAdminSession(cfg);
      const token = randomToken();
      this.sql.exec('INSERT INTO admin_sessions(id_hash, email, created, last) VALUES (?, ?, ?, ?)', await sha256hex(token), who.email, now, now);
      sess = this.one('SELECT * FROM admin_sessions WHERE id_hash = ?', await sha256hex(token));
      setCookie = `__Host-relay_admin=${token}; Path=/; Secure; HttpOnly; SameSite=Lax`; // browser-session cookie
    }
    const where = 'id_hash = ?';

    if (request.method === 'POST' && url.pathname === '/action') {
      if (Number(request.headers.get('content-length') || 0) > 2048) return plain(413, 'Too large.');
      let f;
      try { f = await request.formData(); } catch { return redirect(303, '/'); }
      if (!(await this.consumeCsrf(sess.csrf_hash, f.get('csrf'), 'admin_sessions', where, sess.id_hash))) {
        return plain(403, 'This page has expired. Go back, reload it and try again.');
      }
      const id = String(f.get('id') || '');
      const action = String(f.get('action') || '');
      const p = this.one('SELECT * FROM printers WHERE id = ?', id);
      if (!p) return redirect(303, '/');
      const ws = this.socketFor(id);
      const did = { approve: 'Approved', decline: 'Declined', off: 'Switched off', on: 'Switched on', newcode: 'New link made', forget: 'Forgotten' }[action];
      if (did) this.log(id, 'info', `${did} by the admin`);

      if ((action === 'approve' && (p.status === 'pending' || p.status === 'declined')) || (action === 'on' && p.status === 'off')) {
        this.sql.exec("UPDATE printers SET status = 'on', code = COALESCE(code, ?) WHERE id = ?", this.newCode(), id);
        if (ws) this.enqueueMsg(id, this.stateMsg(this.one('SELECT * FROM printers WHERE id = ?', id)));
      } else if (action === 'decline' && p.status === 'pending') {
        this.sql.exec("UPDATE printers SET status = 'declined' WHERE id = ?", id);
        if (ws) this.sendState(ws, { status: 'declined' });
      } else if (action === 'off' && p.status === 'on') {
        this.sql.exec("UPDATE printers SET status = 'off' WHERE id = ?", id);
        if (ws) { this.sendState(ws, { status: 'off' }); this.dropSocket(id, ws); }
      } else if (action === 'newcode' && p.status === 'on') {
        this.sql.exec('UPDATE printers SET code = ? WHERE id = ?', this.newCode(), id);
        if (ws) this.enqueueMsg(id, this.stateMsg(this.one('SELECT * FROM printers WHERE id = ?', id)));
      } else if (action === 'forget' && p.status !== 'on') {
        if (ws) { try { ws.close(1000, 'Removed'); } catch {} this.dropSocket(id, ws); }
        this.sql.exec('DELETE FROM devices WHERE printer_id = ?', id);
        this.sql.exec('DELETE FROM printers WHERE id = ?', id);
      }
      return redirect(303, '/');
    }
    if (request.method === 'GET' && url.pathname === '/log.txt') {
      const names = new Map(this.all('SELECT id, name FROM printers').map((x) => [x.id, x.name]));
      const lines = this.all('SELECT * FROM events ORDER BY id DESC').map((e) =>
        `${perth(e.at)}\t${e.kind}\t${e.pid ? (names.get(e.pid) || e.pid) : '-'}\t${e.msg}${times(e)}`);
      const h = ownHeaders('text/plain; charset=utf-8');
      h.set('content-disposition', `attachment; filename="relay-log-${perthDay()}.txt"`);
      if (setCookie) h.set('set-cookie', setCookie);
      return new Response(`Print relay activity log (Perth time, newest first)\n\n${lines.join('\n')}\n`, { headers: h });
    }
    if (request.method !== 'GET' || url.pathname !== '/') return plain(404, 'Not found');

    const today = perthDay();
    const rows = this.all('SELECT * FROM printers ORDER BY status, name');
    const csrf = await this.issueCsrf('admin_sessions', where, sess.id_hash);
    const btn = (id, action, label, ghost) => `<form method="post" action="/action" class="inline"><input type="hidden" name="csrf" value="${csrf}"><input type="hidden" name="id" value="${esc(id)}"><input type="hidden" name="action" value="${action}"><button${ghost ? ' class="ghost"' : ''}>${label}</button></form>`;
    const yn = (v) => (v ? 'Yes' : 'No');
    const facts = (pairs) => `<dl>${pairs.map(([k, v]) => `<div><dt>${k}</dt><dd>${v}</dd></div>`).join('')}</dl>`;

    const joins = rows.filter((p) => p.status === 'pending').map((p) => `<li class="card">
<h3>${esc(p.name)}</h3>
${facts([['MAC', esc(p.mac || '—')], ['External IP', esc(p.ip || '—')],
  ['Place', esc([p.place, p.org].filter(Boolean).join(' · ') || '—')], ['Requested', esc(perth(p.requested_at))],
  ['Firmware', esc(p.fw || '—')], ['Visible', yn(p.visible)], ['Extra secure', yn(p.secure)], ['Waiting now', yn(this.socketFor(p.id))]])}
<div class="acts">${btn(p.id, 'approve', 'Approve')}${btn(p.id, 'decline', 'Decline', true)}</div></li>`).join('');

    const printers = rows.filter((p) => p.status !== 'pending').map((p) => {
      const online = !!this.socketFor(p.id);
      const state = { on: 'On', off: 'Switched off', declined: 'Declined' }[p.status] || esc(p.status);
      const acts = p.status === 'on'
        ? btn(p.id, 'newcode', 'New link', true) + btn(p.id, 'off', 'Switch off', true)
        : (p.status === 'off' ? btn(p.id, 'on', 'Switch on') : btn(p.id, 'approve', 'Approve')) + btn(p.id, 'forget', 'Forget', true);
      return `<li class="card">
<h3>${esc(p.name)} <span class="pill ${online ? 'ok' : ''}">${online ? 'Online' : 'Offline'}</span></h3>
${facts([['State', state], ['Last seen', esc(perth(p.last_seen))],
  ['Prints today', p.day === today ? p.prints_day : 0], ['Prints total', p.prints_total], ['Paused by owner', yn(p.paused)], ['Visible', yn(p.visible)],
  ['Extra secure', yn(p.secure)], ['Link', p.status === 'on' && p.code ? `<code>${esc(this.link(p.code))}</code>` : '—']])}
<div class="acts">${acts}</div></li>`;
    }).join('');

    const idleMin = Math.round(cfg.idle / 60000);
    return html(shell('Print relay admin', `<h1>Print relay</h1>
<p class="muted">Times are Perth time. Printed content is never stored; the activity log keeps events and errors for ${LOG_DAYS} days. You are signed out after ${idleMin} minute${idleMin === 1 ? '' : 's'} without activity.</p>
<form method="get" action="/logout"><button class="ghost">Sign out now</button></form>
<h2>Join requests</h2>
${joins ? `<ul class="cards">${joins}</ul>` : '<p class="muted">No join requests.</p>'}
<h2>Printers</h2>
${printers ? `<ul class="cards">${printers}</ul>` : '<p class="muted">No printers yet.</p>'}
${this.activity(url, rows)}`, true, Math.ceil(cfg.idle / 1000)),
      200, setCookie ? { 'set-cookie': setCookie } : {});
  }
}

// ---------------------------------------------------------------- log helpers
// The printer's error answer is {"error":"..."}; anything else is shown as text.
function errText(s) {
  if (!s) return '';
  try { const j = JSON.parse(s); if (j && typeof j.error === 'string') return j.error ? clean(j.error, 160) : '(empty error message)'; } catch {}
  return clean(s.replace(/\s+/g, ' '), 160);
}
// " (x200, last 10:42:07 pm)" for a counted line.
function times(e) {
  if (!(e.n > 1)) return '';
  const last = e.last ? new Date(e.last).toLocaleTimeString('en-AU', { timeZone: 'Australia/Perth' }) : '';
  return ` (\u00d7${e.n}${last ? ', last ' + last : ''})`;
}
function fmtDur(ms) {
  const m = Math.round(ms / 60000);
  if (m < 1) return `${Math.round(ms / 1000)} s`;
  if (m < 120) return `${m} min`;
  const h = Math.round(m / 6) / 10;
  return h < 48 ? `${h} h` : `${Math.round(h / 2.4) / 10} days`;
}

// ---------------------------------------------------------------- page shell
// Justified Systems colours, system fonts (nothing external), light and dark.
function shell(title, body, wide = false, logoutAfter = 0) {
  return `<!doctype html>
<html lang="en-AU">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex, nofollow">
${logoutAfter ? `<meta http-equiv="refresh" content="${logoutAfter};url=/logout">` : ''}
<title>${esc(title)}</title>
<style>
:root{--bg:#E2E1EF;--card:#F3F2F9;--text:#2D2C4C;--muted:#5F5A7A;--line:rgba(45,44,76,.15);--coral:#E98D78;color-scheme:light dark}
@media (prefers-color-scheme:dark){:root{--bg:#1E1D35;--card:#2A2946;--text:#E2E1EF;--muted:#B4B0CC;--line:rgba(226,225,239,.15)}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:400 1rem/1.6 ui-rounded,system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:${wide ? '60rem' : '30rem'};margin:0 auto;padding:2.5rem 1rem 4rem}
.bar{display:flex;gap:.3rem;height:.4rem;width:9rem;margin-bottom:1.5rem}.bar span{border-radius:9px}
.bar span:nth-child(1){flex:5;background:#E98D78}.bar span:nth-child(2){flex:2.5;background:#946F92}.bar span:nth-child(3){flex:2.5;background:#9598C6}
h1{font-size:1.8rem;line-height:1.2;margin:0 0 .75rem}h2{font-size:1.2rem;margin:2rem 0 .75rem}h3{font-size:1.1rem;margin:0 0 .75rem}
.muted{color:var(--muted)}.small{font-size:.85rem}img.qr{background:#fff;padding:8px;border-radius:.75rem}.note{background:var(--card);border-left:4px solid var(--coral);padding:.75rem 1rem;border-radius:.5rem}
label{display:block;font-weight:600;margin:1.25rem 0 .4rem}
input{width:100%;min-height:3rem;padding:0 1rem;border-radius:999px;border:2px solid var(--line);background:var(--card);color:var(--text);font:inherit}
button{min-height:2.75rem;padding:0 1.25rem;border-radius:999px;border:2px solid transparent;background:var(--coral);color:#1E1D35;font:600 1rem/1 system-ui,sans-serif;cursor:pointer;margin-top:1rem}
button.ghost{background:transparent;border-color:currentColor;color:var(--text)}
:focus-visible{outline:3px solid currentColor;outline-offset:3px}
form.inline{display:inline}form.inline button{margin:0 .5rem .5rem 0}
.list,.cards{list-style:none;padding:0;margin:0;display:grid;gap:.75rem}
.list li{background:var(--card);border-radius:1rem;padding:1rem;display:flex;flex-wrap:wrap;gap:.5rem;justify-content:space-between;align-items:center}
.cards{grid-template-columns:repeat(auto-fill,minmax(min(100%,22rem),1fr))}.card{background:var(--card);border-radius:1.25rem;padding:1.25rem}
dl{display:grid;grid-template-columns:repeat(auto-fill,minmax(9rem,1fr));gap:.5rem 1rem;margin:0 0 1rem}dt{font-size:.8rem;color:var(--muted)}dd{margin:0;font-weight:600;word-break:break-word}
code{font-size:.85rem;word-break:break-all}
.pill.err{background:#E98D78;color:#1E1D35;border-color:transparent}
a.pill{color:var(--text);text-decoration:none;margin:0 .25rem .4rem 0}
.scroll{overflow-x:auto}table.log{width:100%;border-collapse:collapse;font-size:.9rem;background:var(--card);border-radius:1rem}
table.log th,table.log td{text-align:left;padding:.5rem .75rem;border-bottom:1px solid var(--line);vertical-align:top}
table.log td:first-child,table.log td:nth-child(2){white-space:nowrap}table.log td:last-child{word-break:break-word}
.pill{display:inline-block;font-size:.8rem;font-weight:600;padding:.1rem .6rem;border-radius:999px;border:1px solid var(--line);vertical-align:middle}.pill.ok{background:#9598C6;color:#1E1D35;border-color:transparent}
</style>
</head>
<body><main><div class="bar" aria-hidden="true"><span></span><span></span><span></span></div>
${body}
</main></body>
</html>`;
}

// fakeunit.js - a pretend printer: answers the page's /api requests with
// believable data and prints nothing. ONE copy, used by:
//   - the test server (test/mock.js),
//   - preview.html and demo.html, built by tools/build_pages.js.
// Plain JavaScript, no libraries; runs in Node and in the browser.
//
// handle(req) takes { method, path, query, headers, text, file } where text
// is a JSON body (string) and file the bytes of an uploaded file, and returns
// { status, json } or { status, bytes, type }.
(function (root) {
  const LIM = { d: 10, s: 20, p: 8 };

  function createUnit(opt) {
    opt = opt || {};
    const fresh = () => ({ items: { d: [], s: [], p: [] }, pic: '', picData: null, nextId: 1, jobs: [], log: [], bins: {}, ver: 1,
      remote: !!opt.remote });
    let st = fresh();
    const now = () => Math.floor(Date.now() / 1000);
    const ok = (status, json) => ({ status, json });
    const clock = () => {
      const d = new Date(Date.now() + 8 * 3600e3);                    // Perth
      return d.toISOString().slice(0, 16).replace('T', ' ');
    };

    function handle(r) {
      const p = r.path, q = r.query || {}, h = r.headers || {};
      const remote = st.remote || !!h['x-via-relay'];
      st.log.push(r.method + ' ' + p);
      if (st.log.length > 200) st.log.shift();

      // Test-only controls (the Node test server); never in the built pages.
      if (opt.testHooks) {
        if (p === '/__state') return ok(200, st);
        if (p === '/__reset') { st = fresh(); return ok(200, {}); }
        if (p === '/__oom') { st.oom = +q.n || 0; return ok(200, {}); }
        if (p === '/__remote') { st.remote = q.on === '1'; return ok(200, {}); }
        if (p === '/__edit') { const it = st.items.d.find(i => i.id === +q.id); const b = JSON.parse(r.text); if (b.prevSrc) it.prev = it.src; it.src = b.src; it.title = b.title; it.rev++; it.by = 'other'; it.edits++; st.ver++; return ok(200, it); }
      }

      // Online, the unit leaves out the home network's addresses (as the firmware does).
      if (p === '/api/printer/status') return ok(200, { connected: true, ip: remote ? undefined : '192.168.1.50', port: remote ? undefined : 9100, alert: { code: '', message: '' }, restingMs: 0,
        paper: { name: '80 mm', printableDots: 576, charsPerLine: 48 }, queue: { pending: 0, completed: st.jobs.length, failed: 0, faults: 0 },
        system: { version: opt.version || '0.0.0', hostname: opt.hostname || 'print', ip: remote ? undefined : '192.168.1.40', freeHeap: 160000, largestBlock: 110000,
          maxImageBytes: 98000, clockSet: true, localTime: clock(), historyStored: st.jobs.length },
        local: !remote,
        online: opt.demo ? { state: 'on', text: 'On' }
          : { state: 'on', text: 'On', url: remote || !opt.home ? undefined : opt.home.url },
        image: { gamma: 60, threshold: 128, brightness: 0, contrast: 0 },
        items: { ver: st.ver, d: st.items.d.length, s: st.items.s.length, p: st.items.p.length, limit: LIM, pic: st.pic, free: 700000, reserve: 65536, running: 0, waiting: false } });

      if (p === '/api/jobs' && q.id) { const j = st.jobs.find(x => x.id === +q.id); return j ? ok(200, { id: j.id, status: 'done', type: j.type, source: remote ? 'online' : 'web' }) : ok(404, { error: 'unknown job' }); }
      if (p === '/api/jobs/clear') { if (remote) return ok(403, { error: 'local only' }); st.jobs = []; return ok(200, { ok: true }); }
      if (p === '/api/jobs') return ok(200, { jobs: st.jobs.slice().reverse().map(j => Object.assign({ id: j.id, status: 'done', type: j.type, localTime: j.at },
        remote ? { source: /^online/.test(j.src) ? 'online' : 'home' } : { source: j.src, ip: '192.168.1.20' })), stored: st.jobs.length, capacity: 100, pending: 0 });

      if (p === '/api/print/raster' && st.oom > 0) { st.oom--; return ok(400, { error: 'out of memory (41000 free)' }); }
      if (p === '/api/print/text' || p === '/api/print/raster') {
        const id = st.jobs.length + 1, text = p.endsWith('text');
        const job = { id, src: remote ? 'online (browser)' : 'web (browser)', type: text ? 'text' : 'raster', q, at: clock(),
          body: text ? JSON.parse(r.text || '{}') : (r.file ? r.file.length : 0) };
        st.jobs.push(job);
        if (opt.onPrint) opt.onPrint(text ? { kind: 'text', job: job.body } : { kind: 'raster', width: +q.width, rows: +q.rows, cut: q.cut === '1', data: r.file });
        return ok(200, { jobId: id });
      }
      if (p.startsWith('/api/print/')) return ok(403, { error: 'not available here' });

      if (p.startsWith('/api/online')) {
        // Home-network sample data comes from the caller (tests, preview);
        // the public demo is built without it.
        if (remote || !opt.home) return ok(403, { error: 'local only' });
        st.online = st.online || Object.assign({ enabled: true, name: 'kitchen', visible: true, secure: true, paused: false, state: 'on', text: 'On' }, opt.home);
        if (p === '/api/online' && r.method === 'POST') Object.assign(st.online, JSON.parse(r.text || '{}'));
        return ok(200, st.online);
      }
      if (p === '/api/settings' && remote) return ok(403, { error: 'local only' });
      if (p === '/api/settings') {
        if (r.method === 'POST') return ok(200, { ok: true });
        return ok(200, { papers: [{ index: 0, name: '80 mm', printableDots: 576 }, { index: 1, name: '58 mm', printableDots: 384 }], paperIndex: 0,
          printerIp: '192.168.1.50', printerPort: 9100, hostname: opt.hostname || 'print', image: { gamma: 60, threshold: 128, brightness: 0, contrast: 0 } });
      }
      if (p === '/api/theme' || p === '/api/kiosk') return ok(200, {});
      if (p === '/api/clock') { if (r.method === 'POST') { if (remote) return ok(403, { error: 'local only' }); st.tz = JSON.parse(r.text).tz; } return ok(200, { ok: true, now: now(), tz: st.tz || 'AWST-8', local: clock() }); }

      if (p === '/api/items' && r.method === 'GET') return ok(200, [...st.items[q.kind]].reverse());
      if (p === '/api/items' && r.method === 'POST') {
        const k = q.kind, doc = JSON.parse(r.text);
        if (st.items[k].length >= LIM[k]) return ok(409, { error: 'full (' + LIM[k] + ' of ' + LIM[k] + ') - delete one first' });
        if (doc.pic && st.pic) return ok(409, { error: 'only one picture can be stored - delete the item that has one first' });
        doc.id = st.nextId++; doc.t = now(); st.ver++;
        if (k === 'd') { doc.rev = 1; doc.edits = 0; doc.by = doc.sess; doc.seen = [doc.sess]; delete doc.sess; }
        if (k === 'p') { doc.bytes = (doc.seq || []).reduce((a, e) => a + (e.r || 0) * doc.width / 8, 0); doc.ready = doc.bytes === 0; doc.next = now() + 3600; doc.tries = 0; doc.last = 0; }
        st.items[k].push(doc); return ok(200, { id: doc.id, next: doc.next || 0, rev: doc.rev || 0 });
      }
      if (p === '/api/items/one') { const it = st.items[q.kind].find(i => i.id === +q.id); return it ? ok(200, it) : ok(404, { error: 'no such item' }); }
      if (p === '/api/items/update') {
        const it = st.items.d.find(i => i.id === +q.id); if (!it) return ok(404, { error: 'this draft was deleted' });
        const b = JSON.parse(r.text); const conflict = b.base !== it.rev && it.by !== b.sess;
        if (!it.seen.includes(b.sess)) { it.seen.push(b.sess); it.edits++; }
        if (conflict) it.prev = it.src; else delete it.prev;
        it.src = b.src; it.title = b.title; it.style = b.src.style; it.rev++; it.by = b.sess; st.ver++;
        return ok(200, { rev: it.rev, edits: it.edits, conflict });
      }
      if (p === '/api/items/delete') { st.ver++; const k = q.kind, id = +q.id; st.items[k] = st.items[k].filter(i => i.id !== id); if (st.pic === k + id) { st.pic = ''; st.picData = null; } return ok(200, { ok: true }); }

      // Pieces: bytes = whole file, off = where this piece starts (as on the unit).
      const piece = (key, d) => {
        const off = +(q.off || 0); st.up = st.up || {};
        if (off === 0) st.up[key] = new Uint8Array(0);
        if (!st.up[key] || st.up[key].length !== off) return { err: 'pieces out of order - start again' };
        const a = new Uint8Array(st.up[key].length + d.length); a.set(st.up[key]); a.set(d, st.up[key].length);
        st.up[key] = a; st.pieces = (st.pieces || 0) + 1;
        return a.length < +q.bytes ? { next: a.length } : { done: a };
      };
      if (p === '/api/items/bin') {
        const it = st.items.p.find(i => i.id === +q.id);
        if (!it || +q.bytes !== it.bytes) return ok(400, { error: 'size does not match' });
        const x = piece('bin' + it.id, r.file || new Uint8Array(0));
        if (x.err) return ok(400, { error: x.err }); if (x.next) return ok(200, { ok: true, next: x.next });
        it.ready = true; st.bins[it.id] = x.done.length; return ok(200, { ok: true });
      }
      if (p === '/api/items/pic' && r.method === 'POST') {
        if (st.pic && st.pic !== q.owner) return ok(400, { error: 'only one picture can be stored' });
        const x = piece('pic', r.file || new Uint8Array(0));
        if (x.err) return ok(400, { error: x.err }); if (x.next) return ok(200, { ok: true, next: x.next });
        st.pic = q.owner; st.picData = x.done; st.picBytes = x.done.length; return ok(200, { ok: true });
      }
      if (p === '/api/items/pic') return st.picData ? { status: 200, bytes: st.picData, type: 'image/jpeg' } : ok(404, { error: 'no picture' });
      if (p === '/api/items/run') return ok(200, { ok: true });
      return ok(404, { error: 'not found' });
    }

    return { handle, get state() { return st; } };
  }

  if (typeof module !== 'undefined' && module.exports) module.exports = { createUnit };
  else root.FakeUnit = { createUnit };
})(typeof window !== 'undefined' ? window : globalThis);

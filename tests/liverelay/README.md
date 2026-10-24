# Testing against the real relay, locally

Runs the firmware's relay session against the REAL relay code (`relay/src/index.js`)
on this machine, with no Cloudflare account and no Access sign-in.

1. Copy `relay/` to a scratch folder, apply the test hook and use the test config:
   ```
   cp -r relay /tmp/relay-test && cd /tmp/relay-test
   patch -p1 < <repo>/tests/liverelay/test-hook.patch
   cp <repo>/tests/liverelay/wrangler.test.jsonc wrangler.jsonc
   npm install wrangler@4 && npx wrangler dev --local --port 8787
   ```
   The hook adds `/__test/approve|decline|off|forget|list|events`, only when
   `TEST_HOOKS=1` (set in the test config, never in the real one).
2. `node tests/mock.js` - the unit's web server stand-in on :8765.
3. Build `tests/relayhost/host_relay` (see its `build.sh`), then connect test printers:
   `tests/relayhost/host_relay 8787 8765 <seconds> <keyfile>`
   (env `NAME`, `SECURE=1`, `HIDDEN=1`, `NEWCODE=1`, `BADSIG=1`, `PAUSED=1`, `LAST=...`).
4. `curl localhost:8787/__test/approve`, then
   `RELAY=http://localhost:8787 LOGA=<log> LOGB=<log> node tests/remote_test.js`.

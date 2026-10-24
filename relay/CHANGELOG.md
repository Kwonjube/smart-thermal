# Print relay changelog

Newest first. The relay is one file, `src/index.js`; deploy with `npx wrangler deploy`.

## Print relay 0.24: activity log flood guard

From the website review: anyone with a printer link could add one log line per failed
request. 200 blocked requests made 200 lines in about a second, so the 3000-line history
could be pushed out.

### Change (src/index.js, log())
- **Repeats are merged.** The same event for the same printer within a minute adds to
  a count on its existing line instead of adding a new line. Shown as
  "GET /api/settings -> 403: Forbidden (... ) (x200, last 9:46:04 am)". Timings are left
  out of the match, so identical requests merge.
- **Per-minute cap.** Each printer, and the relay itself (for strangers), gets 20 new
  warning/error lines a minute. Beyond that, ONE line "More events this minute not
  shown (too many at once)" counts the rest (x N).
- Counting updates an existing line and never adds one, so a flood cannot push the
  history out.
- Rare events that need a printer key or admin sign-in (connects, admin actions) are
  not capped.
- Two columns are added automatically (`n`, `last`). No migration step.

### Test (wrangler dev, real code)
- 200 x GET /api/settings -> 1 line (x200).
- Then 50 different blocked paths in the same minute -> 18 lines + 1 "not shown" line (x32).

### Deploy
Replace src/index.js and run `npx wrangler deploy`. Includes everything from 0.23.1.

## Print relay 0.23.1 — admin activity log

Owner's request: "the admin page could save logs of things happening on the
site ... if there's an error the relay would log that and the issue."

### What it does
- New table `events` (created automatically; no migration step).
- Admin page: **Activity log** section at the bottom — newest 150 entries,
  filters (Everything / Problems only / per printer), and **Download all
  (text)** at `/log.txt` on the admin address (added to ADMIN_PATHS).
- Kept 30 days or 3000 entries, whichever is less (LOG_DAYS, LOG_KEEP).

### What is logged
- Printer connected / disconnected (with the close code and how long it was up).
- Firmware 0.23.1+: on reconnect the printer reports why its last link ended,
  its uptime, largest free memory block and drop count ("Printer's last drop: ...").
- Join requests, refused hellos, "too many waiting" refusals.
- Admin actions (approve, decline, switch on/off, new link, forget).
- Every FAILED visitor request: method + path (no query, no body), status, the
  printer's error text, KB sent, seconds, guest/owner. Covers relay refusals
  (blocked, paused, offline, too large, busy) and timeouts (504).
- Answers slower than 30 s (logged as "OK but slow").
- Extra secure: "A device asked to print" (no name), wrong owner codes, lock-outs.
- Link guessing: an address crossing the limit (no IP stored).

### What is NOT logged
Printed content, request bodies, query strings, visitor IPs, visitor names,
cookies. The header comment and the admin page wording say so.

### Test (local, wrangler dev)
Printer harness with a mock unit failing one photo upload:
  POST /api/print/raster -> 400: out of memory (41000 free) (0 KB in, 0.0 s, guest)
  GET /api/settings -> 403: Forbidden
  Disconnected after 5 s (code 1006 ...)
  Connected (fw ..., up 2.5 h, largest free block 50 KB, 3 drops since start).
    Printer's last drop: connection ended: relay silent for 2 minutes

### Deploy
Replace src/index.js and `npx wrangler deploy`. No config change.

## Relay change for firmware 0.23 (21 Oct 2026, from the ESP32 chat)

**Feature:** the printer's owner can **pause online printing** from the
printer's Settings at home. The printer then reconnects and sends
`"paused": true` in its `hello`.

**Changes in `src/index.js`**:

1. **New column** `printers.paused`, added once with a guarded `ALTER TABLE`
   alongside the existing `csrf_hash` one.
2. **`hello.paused`** is stored on every hello: true counts, anything else
   counts as false. Printers on older firmware send nothing, so they read as
   not paused.
3. **`forward()`:** when the printer is paused, `POST /api/print/*` and
   `POST /api/items/run` are answered by the relay with
   **503 "Printing is paused by the printer's owner."** (`Retry-After: 600`),
   and are not forwarded. Everything else still reaches the printer: the
   page, history, drafts and saved items. The printer refuses paused prints
   itself as well.
4. **`/_r/printers`** entries gain `"paused": true|false`. The printer page
   shows "Online, paused".
5. **Admin:** each printer card shows **Paused by owner: Yes/No**.

Nothing else changes: no new routes, headers or storage beyond the one
column.

**Tested here:** this file was run locally with `wrangler dev`, with a
test-only approve hook not included in this copy. A paused printer's page
loaded (200), a print was refused at the relay (503 with the message above),
and saving an item still went through (200). `node --check` passes.

**Deploy:** `npx wrangler deploy` as usual. The column is added on first
use.


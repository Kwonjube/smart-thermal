# Smart Thermal — Handover

**State on 24 Oct 2026:**
- **Firmware:** v0.24.2. v0.24.0 is on the owner's printer and working. 0.24.1 and 0.24.2 have not been flashed yet.
- **Relay:** 0.24, delivered. The owner deploys it; the website chat reviewed it and said "good to deploy".
- **Demo:** live at `justifiedsystems.com.au/lab/smart-thermal/demo/` (0.24.0 build).

- **Repository:** `smart-thermal` on GitHub, published with GitHub Desktop; the owner is new to git (owner's PC: `C:\Git\smart-thermal`). First commit 24 Oct ("Smart Thermal 0.24.2", 122 files, includes the owner's `partitions.csv`). MIT licence (Justified Systems). The README has a clickable demo screenshot (`docs/images/demo.png`).
- **Relay:** the first deploy from the repo moves `ADMIN_EMAIL`, `ACCESS_TEAM_DOMAIN` and `ACCESS_AUD` to Worker secrets (`cd relay`, `npx wrangler deploy`, then three `npx wrangler secret put`). Check this was done before any other relay work.

To start a new chat, give it this file and the repository.

---

## 1. The owner and how to work

- **Brief, in the owner's words:** the purpose is *to quickly print clear physical lists*. Keep designs simple and easy to follow; be cautious about changing the UI or drifting from that purpose.
- **Replies:** brief, concise, executive style. The owner asks for detail when needed. Ask when a choice is genuinely the owner's; otherwise take the sensible default and say so.
- **Work in small, tested steps.** Every UI change goes to the owner as `dist/preview.html` first. The owner flashes only after approving it. Paper tests come last.
- **Owner:** in Perth (AWST, UTC+8). Printer name "printalotl".
- **Owner's visual taste, learned the hard way:** plain controls that match the page. Tabs were tried and rejected ("ugly"). The level and Future buttons are styled like the page's drop-downs. Avoid designs that read as "an accommodation for my words".

**Division of work**
- **This project:** the firmware, the printer's web page (the same page visitors see online), the preview/demo generator, and the relay code (`relay/`).
- **The website chat** (a separate chat; Cloudflare Pages): the main site, hosting the demo, and Pages headers. It reviews relay changes before the owner deploys.
- **The owner:** flashes the ESP32, deploys the relay (`npx wrangler deploy`), and passes `dist/demo.html` to the website chat, renaming it to `index.html`.

**Workflow**
1. Change the code in `SmartThermal/` (page = `WebUI*.h`).
2. `node SmartThermal/tools/build_pages.js` builds `dist/preview.html` and `dist/demo.html`.
3. The owner reviews `preview.html`. It opens by double-click, with a Home/Online view switch and virtual paper.
4. Compile and run the tests (section 12).
5. The owner flashes the firmware and gives `demo.html` to the website chat. Deploy the relay only if `relay/` changed.

---

## 2. Open items, in order

1. **Flash 0.24.2.** Then check:
   - The "Welcome to the internet" slip prints once, with a QR code.
   - `/log` shows `[slip] welcome online queued` / `printed`. If memory is short, it shows `not printed: <why>` instead, never a loop.
   - The Settings wording is right ("device", shorter text).
2. **Photo seams with online on.** The owner asked to "revert those chunks". Styled-image parts are back to 48 KB at home. Photo-card parts are memory-bound: about 32 KB with online on (largest block 38–54 KB), about 64 KB with it off. Ask the owner to compare the same photo with online on and off.
3. **Print photos from storage (agreed as next, not started).** The plan:
   - Upload the whole picture into LittleFS in pieces of 128 KB or less, reusing the streaming `/items/pic`-style upload. It writes 1.4 KB at a time, so RAM is not an issue.
   - Then add one queue job that streams the file to the printer in chunk-row reads.
   - The result: the same path for home and online, no RAM ceiling and no seams. The limit becomes storage, about 350 KB (about 600 mm).
   - Keep the coverage and pacing figures per job, and the white tail at the end only.
4. **Online drops.** The relay log shows "Printer's last drop: link closed without a goodbye" every 1–5 hours. Each reconnects within seconds. There was also one 504 (a status request took 90 s, so the relay closed the link). Watch only; no fix planned yet.
5. **Printer stops answering after long idle (parked by the owner).** The owner believes the printer is at fault. As a test, the keep-awake now sends a phantom print (one white raster row, no cut) after 8 hours with no print.
6. **`partitions.csv`.** Done: the owner's custom table (579 bytes) is committed in `SmartThermal/`. The Arduino IDE uses it instead of the board's scheme. Its contents were not reviewed here; check that it gives the app at least 1.7 MB.
7. **Lend-out switch-over:** `BUILD_TRAINER 0`, `BUILD_BENCH 0`, then the acceptance tests (section 9). It saves only about 12 KB of flash.
8. **Ideas for the owner to prioritise:** listed in section 14.

---

## 3. Rules that must keep holding

- The firmware must **never link to, print or call** the relay's admin host (`admin.justifiedsystems.com.au`).
- **Settings are local-only.** Requests carrying `X-Via-Relay` (all relay traffic) get 403 on settings, system, logs, raw print, history clear, kiosk set-up, clock POST and `/api/online`. The relay also refuses those paths itself.
- The **owner code** appears only on paper and in home-network Settings. The relay stores only SHA-256("smartthermal-owner-v1:"+code).
- Online visitors never receive home-network addresses (printer IP and port, unit IP, RSSI).
- **Demo:** anything not for the public goes between `<!--home-only-->` and `<!--/home-only-->`. Never edit `demo.html` by hand.
- **Levels:** a control hidden by the device's level reverts to its plain value, so a hidden choice never changes a print.
- **A slip never prints in part.** Make everything that can fail (the QR code) before queuing anything.
- **The record of work owed is never destroyed until it is done** (see invariant 9).
- **No extra libraries** beyond ArduinoJson 7 and Ethernet. No downloads in the page (OCR was rejected).
- Keep "Erase All Flash Before Sketch Upload" **disabled**. Use the **Huge APP** partition scheme, or the owner's `partitions.csv`.
- Nothing secret goes in the repository. The relay's `ADMIN_EMAIL`, `ACCESS_TEAM_DOMAIN` and `ACCESS_AUD` are Worker secrets.

---

## 4. Repository

| Path | What |
|---|---|
| `SmartThermal/` | Arduino sketch: firmware and page. Folder name must match the `.ino`. |
| `SmartThermal/tools/build_pages.js` | Assembles the page exactly as `WebApi::handleIndex()` sends it, then adds the pretend printer. Writes `dist/preview.html` and `dist/demo.html`. The demo strips home-only blocks and fails on forbidden strings. |
| `SmartThermal/tools/fakeunit.js` | The pretend printer, shared by the preview, the demo and `tests/mock.js` |
| `relay/` | Cloudflare Worker: `src/index.js`, `wrangler.jsonc`, `README.md` (set-up, secrets, go-live checks), `CHANGELOG.md`, `scripts/check-live.mjs` |
| `tests/` | `mock.js` + page tests; `host/` scheduler harness; `relayhost/` relay-session harness; `relay/relay.js` stand-in relay; `liverelay/` test hook for the real relay |
| `docs/` | This file, `PROTOCOL.md` (relay protocol v1, original spec), `LIVE-RELAY-BRIEF.md` (the website chat's brief of the live relay), `DEMO-HOSTING.md` |

---

## 5. Hardware and build

| Item | Value |
|---|---|
| Printer | POS-8370 / 80-VI-UL, firmware V1.3, 576 dots / 72 mm / 203 DPI, cutter `GS V B 0` |
| Printer power | 24 V 1.5 A (36 W), possibly marginal |
| Controller | ESP32-WROOM-32 (D0WD-V3), 4 MB flash, **no PSRAM** |
| Link | W5500 Lite over SPI, cabled direct to the printer: CS 5, RST 4, SCK 18, MISO 19, MOSI 23 |
| BOOT | GPIO 0. Hold 5 s **while running** to forget Wi-Fi and return to setup. Held at power-up, it enters flashing mode. |
| Printer IP | Discovered automatically. Bench `192.168.11.100`, factory `192.168.1.100`. |
| Build | ESP32 core 3.3.12, ArduinoJson 7.4, Ethernet 2.x, Huge APP (3 MB app / 1 MB LittleFS). Firmware about 1.61 MB, RAM globals about 81 KB. |
| NVS namespaces | `thermal` (settings), `unit` (hotspot password, announced address, time zone), `online` (key, code, url, owner code, welcome key, config), `themes`, `kiosk`, `thermprof`, `trainer` |
| Build switches (`Config.h`) | `PRINTER_LINK_W5500 1`, `BUILD_TRAINER 1`, `BUILD_BENCH 1`, `KEEPAWAKE_HOURS 8`, `RELAY_HOST`/`RELAY_PATH`, `TIMEZONE "AWST-8"` |

USB upload errors such as "Serial data stream stopped" come from the cable or port, not the firmware. Lower the upload speed to 115200, try another cable or port, close the Serial Monitor, and hold BOOT while "Connecting…" shows.

---

## 6. The web page

**Composition.** `handleIndex()` sends, in order: `WEB_UI_HEAD` (WebUI.h), `WEB_UI_TEMPLATE`, `WEB_UI_THEME`, `WEB_UI_FUTURE`, `WEB_UI_IMAGE`, `WEB_UI_TAIL` (WebUI.h). There are separate pages for the kiosk (`KioskPage.h`), setup (`SetupPage.h`, hotspot mode) and the log (`LogPage.h`).

**Through the website.** `window.BASE` = `/p/<code>`, and `fetch` is patched to prefix it; `window.REMOTE` is true. Status polls every 15 s, draft watch every 15 s, and the gap between parts is 2 s. The preview/demo set `window.SHOW_AS_ONLINE`.

**Levels (v0.24, approved: "balancing is perfect").**
- **The control:** a button at the far right of the header (bars + name + chevron) opens Basic / Standard / Advanced / Expert.
- **Mechanism:** elements carry `lv2` / `lv3` / `lv4`, and `body.L1..L4` hides them (CSS in WebUI.h).
- **Storage:** per device, in `localStorage.level`. A new device starts on Basic; a device that already has `prefs` or `kiosk` starts on Expert.

| Level | Shows |
|---|---|
| Basic | Title, printer light, text box, Print text |
| Standard | + align, size, cut, wrap, text preview, Save + Saved drawer, Recent prints, Preferences, header subtitle |
| Advanced | + Style menu (themes, From image), photos (width, type, rotate, crop, extract), drafts, Schedule, Feed & cut, Future button, other printers, online status line |
| Expert | + photo fine-tuning (brightness, contrast, gamma, threshold, invert), photo info lines, Settings (home only; includes kiosk and online set-up) |

- **Reverting:** hidden text choices revert to plain (style plain, left, M, cut on, wrap on). Hidden photo fine-tuning reverts to the printer's defaults (`levelchange` event).
- **Auto-List:** pasting a list switches the style to List only from Advanced up.
- **Header:** Future (left), then Level (far right), as plain `.hbtn` buttons. Pressing Future again returns to the main page. On a phone the buttons wrap under the title.

**Settings vs Preferences.** Settings (this printer) are home-network only. Preferences (this device) are the starting Style/Size/Align/Cut/Wrap, kept in `localStorage.prefs`.

**Drafts (owner's option C).** Drafts save only with "Update draft". A bar shows "all changes saved" or "unsaved changes" (red), with Close. Leaving with unsaved changes asks first. Other people's updates load automatically only when there are no unsaved changes; otherwise a warning appears with "Load theirs". "Copy link" copies the printer link + `#draft-<id>`.

**Saved / Future.** Limits are drafts 10, saved 20, scheduled 8. Only one stored picture, across all items. Scheduling runs on the unit (`Scheduler`):
- a print that was due while the printer was away prints when it is back;
- a print missed while the unit was off prints once;
- a failed print retries twice, 5 minutes apart.

**Image parts.**
- **Photo card:** parts are min(`maxImageBytes`, 64 KB) at home and 24 KB online. `maxImageBytes` = largest block − 12 KB at home, or − 24 KB when the request comes via the relay.
- **Styled and From-image prints:** 48 KB parts at home, 24 KB online.
- **Retries:** both retry on "out of memory" or "queue full". A styled part refused for memory 3 times is sent as two halves.
- **Order:** each part is sent after the previous one has printed.

**Printing feedback.** Every print ends with "Printed ✓ (job #N)" or "Did not print: <reason>". Errors show the server's own words ("HTTP 400: <reason>").

---

## 7. Online printing

**Relay.** One Cloudflare Worker plus one SQLite Durable Object (hibernation API).
- **Addresses:** `print.justifiedsystems.com.au` is for printers and visitors. `admin.justifiedsystems.com.au` is the admin, behind Access, with the token verified in the Worker. DNS is on Cloudflare; the domain is registered with VentraIP. The domain has no email.
- **Limits:** request body 160 KB; 90 s per request (a timeout gives 504 and closes the socket, code 1011).
- **Blocked paths:** `/api/settings`, `/api/system`, `/api/print/raw`, `/api/jobs/clear`, `/api/online`, `/log`, `/setup`, plus POST to `/api/kiosk` and `/api/clock`.
- **Headers:** the relay applies its own CSP to printer pages and drops Location, Set-Cookie, Content-Disposition and ETag.

**Activity log (relay 0.23.1/0.24).**
- **Connects and drops:** logged with reasons from both sides; the printer reports why its last link ended.
- **Other events:** admin actions; failed requests (method + path + status + the printer's error text + KB in + seconds); slow answers (over 30 s).
- **Flood guard:** repeats within 60 s merge into one line ("×N"); new warning lines are capped at 20 per printer per minute, then one counted "not shown" line.
- **Retention:** 30 days or 3,000 lines, whichever is less. Download at `/log.txt` on the admin host.

**Protocol v1** (`docs/PROTOCOL.md`; the live relay's details are in `docs/LIVE-RELAY-BRIEF.md`).
- **Messages:** challenge → hello (signed) → state `pending | on | declined | off` (+ code, url). Then req (+ binary body) → res + binary + end. Also notice `device-request`, newcode, and text `ping`/`pong` every 45 s; 120 s of silence means reconnect.
- **Close codes:** 1008 = hello rejected (wait 5 minutes); 1013 = too many join requests (wait at least 10 minutes). Backoff runs from 2 s to 5 minutes, +25 % jitter; after a decline, retry after 6 hours.
- **Hello fields:** `v`, `fw`, `key` (P-256 public), `sig` (raw r‖s over "smartthermal-relay-v1:"+nonce, base64url), `mac`, `name` (= hostname), `visible`, `secure`, `paused`, `ownerHash`, `up`, `heap`, `drops`, `last` (why the previous link ended).
- **On the unit (`RelaySession`):** each request is replayed over HTTP/1.0 to 127.0.0.1:80 with `X-Via-Relay: guest|owner`. Paths are blocked on the unit too, and header values are cleaned. One line is logged per non-routine request.
- **`Online` task:** 16 KB stack, core 0. It waits for Wi-Fi, the clock and a free block of at least 48 KB. TLS uses the core's CA bundle and holds about 40–50 KB while connected.
- **Status wording:** Off / Waiting for the internet / Connecting / Waiting for approval / On / Not approved / Can't reach <host>, retrying.
- **Pause:** the Settings tick box (applies at once). The unit refuses online prints, items/run and new schedules with 503, the visitor page shows a notice, and the relay refuses those prints before forwarding.
- **Welcome slip:** reprints when the link, owner code or extra secure changes (welcome key `code:ownercode:S|-`).
- **Extra secure:** devices ask at `/p/<code>`, a "KNOCK KNOCK!" slip prints, and the owner approves at `/p/<code>-a` with the owner code.

---

## 8. Slips (`Slips.cpp`)

| Slip | When | Contents |
|---|---|---|
| SETUP | No saved network | Wi-Fi QR for the hotspot `SmartThermal-XXXX`, typed fallback, 3 steps |
| ADDRESSES | Joined a network (once per address) | Name, "is ready to print", QR to the **IP** (`.local` fails on many Androids), both addresses, HOW TO USE (no troubleshooting since 0.24.1) |
| SETUP_FAILED | Setup page's network refused | Network, problem, what to do |
| ONLINE | Approved | WELCOME TO THE INTERNET!, QR + link, who can print, visibility, OWNER CODE, approval address if extra secure |
| KNOCK | A device asks (extra secure) | Who, the printer, approval address |

**Desk rules.**
- A slip is submitted only into a reachable, idle printer, then confirmed from the job history; a failed print is retried.
- **v0.24.1:** the QR code is made first, in one buffer handed to `submitRasterOwned`, so nothing is queued if it fails. Every failure is logged. After 3 failed tries the slip prints without the QR code.

---

## 9. Firmware map

| Area | What | Where |
|---|---|---|
| Discovery | Saved address, then 7 factory defaults, after a link reset | `Discovery.cpp`, `PrintQueue::discover()` |
| Setup | Hotspot (MAC suffix), password generated once in NVS `unit`, captive portal, network tested before saving | `Unit.cpp`, `SetupPortal.cpp`, `SetupPage.h` |
| Queue | One printer task owns the printer. Jobs wait for a missing printer (30 min); paper and cover are checked before sending; one reprint after a hard fault; pacing from a fixed table | `PrintQueue.cpp` |
| Errors | E01–E06 on the page, wording in one file | `Alerts.h` |
| Log | Serial + 16 KB RAM ring (survives resets) + saved to flash `/trace.log` (rolls to `/trace.old` at 96 KB; every 30 s or 4 KB). `/log` with Copy and "Saved log"; `/log.txt?saved=1`. A pulse line every 30 s. | `Log.cpp`, `LogPage.h` |
| Keep-awake | Phantom print (1 white row, no cut) after 8 h without a print | `SmartThermal.ino` `serviceKeepAwake()` |
| Items / scheduler | LittleFS `/k`: one JSON per item, `p<id>.bin` slip, `pic.jpg` + owner; rule maths in `SchedRule.h` (pure, PC-tested) | `Items.*`, `Scheduler.*`, `WebApiItems.cpp` |
| Online | Key, config, task, relay session, welcome and knock slips | `Online.*`, `RelaySession.*`, `OnlineKey.*`, `B64.h` |
| Kiosk | `/kiosk` party screen; set-up in Settings (NVS `kiosk`) | `KioskPage.h` |
| Themes | Parking fine form and drawing; wording at `/api/theme?name=fine` | `WebUITheme.h` |
| Extract text | No recognition by design: words found by layout and copied as pixels; three ways; crop | `WebUITemplate.h` (`FI`), `WebUIImage.h` |
| Bench / trainer | Behind `BUILD_BENCH` / `BUILD_TRAINER` | `BandTest.cpp`, `Trainer.cpp` |

### Architecture

```
Browser (decode, resize, gamma, Atkinson, pack, count coverage;
         templates drawn on a canvas and thresholded)
    |  HTTP over Wi-Fi (or the relay, replayed on 127.0.0.1)
WebApi  ->  PrintQueue  ->  Printer  ->  EscPos  ->  Transport
                 |            (pacing)                  |
              JobStore                        W5500 cable -> POS-8370
```

### Invariants — do not break

1. `Printer` is owned solely by the print-queue task. Outside code reaches it only through `PrintQueue::runOnPrinterTask()`.
2. Only `EscPos.cpp` contains ESC/POS bytes.
3. Never call `Transport::write()` directly; use `writeAll()`.
4. Reachability is probed only by the printer task, only when idle, never during a fault cooldown.
5. Only the printer task touches the W5500 over SPI, after `begin()`. `describe()` and `linkState()` return cached text and are safe anywhere.
6. Settings touching the printer or transport go through `queue.requestReconfig()`.
7. `PrinterProfile` is the mechanism; `PaperProfile` is the loaded roll. The thermal ceiling is a fixed table in `PrintQueue::limitFor()`.
8. Every raster job is paced. A job without a coverage figure gets counted.
9. **The record of work owed is never destroyed until the work is done**, or until the printer that was owed it has restarted (link lost).
10. Settings belong to `loop()`. The printer task hands changes over; it never writes Settings itself.

### Acceptance tests (lend-out)

1. Ten dense photos in a row: no gibberish; any fault recovers by itself.
2. Cover open for about 20 s mid-photo, then closed: the print finishes (E01).
3. Paper out mid-photo, then reloaded: the print finishes (E01).
4. Printer off, a job sent, printer on: it prints (E06, then resumes).
5. BOOT held 5 s: the setup slip prints; set up from a phone; the welcome slip prints.

---

## 10. The printer, as understood

**Pacing.** `gap_ms = basis_ms × (coverage × 100 / limit − 1)` (floor 0), with `basis_ms = rows / 748 × 1000`. The limit is a fixed table in `PrintQueue::limitFor()`: 1100 at 30 % coverage, 1000 at 50 %, 850 at 66 %, 700 at 80 %, 600 at 95 %, linear between. The browser sends the exact `&coverage=`. Real mechanics: 1292 rows/s (161.5 mm/s) plus 559 ms of fixed label, feed and cut.

**Two failure modes.**
- **Soft pause:** the printer stops for 30–35 s, beeps, and resumes; the connection survives.
- **Hard fault:** it drops TCP, prints its buffer, then returns in about 35 s or wedges until power-cycled.

**What wedges it:** stopping with a job mid-transfer, or while a connection is being worked during the stop. Jobs smaller than the printer's ~18–20 KB receive buffer are safe. Holding the connection through a stop works.

**W5500 stuck-link rule.** After the printer drops off (fault, paper-out, power cycle), the W5500 may fail to reconnect even to a healthy printer until the W5500 is reset. A reset made too early does not stick, so `maybeProbe()` resets it every 4 failed probes for the whole outage.

**Delivery and banding.**
- **Throughput:** the W5500 manages 100–320 KB/s while the printer has room. The printer's intake once full is about 90 KB/s, and a full-width raster needs about 93–120 KB/s. So a full-width photo runs slightly short of data, and the end prints lighter, with a line at the switch.
- **Fix — the white tail:** photos wider than 72 % get white rows appended (`26 − 0.137 × length_mm` + 4 mm, at least 23 mm).
- **Other marks:** faint lines every ~31 mm are the platen roller (cosmetic). The printer ignores speed and density commands.

**ESC v.** It is answered when parsed, which is ahead of the paper. Cut + ESC v is a reliable print-done clock (73 KB slip: 1356–1358 ms).

**Error codes.**

| Code | Shown | Behaviour |
|---|---|---|
| — | "Next print in 12 s — letting the printer cool." | Pacing, not an error |
| E01 | The printer has paused (paper, cover or cooling) | No progress for 3 s; connection held up to 10 min |
| E02 | Too hot, recovering, print retried in about 3 minutes | Hard fault: cooldown, resync, 3-minute image hold, one reprint |
| E03 | Not responding: switch it off, wait 5 s, on, wait 1 min (may take 2–3 goes) | Unreachable or stalled for 3 min |
| E04 | Out of paper | DLE EOT before a job; job held |
| E05 | Cover open | DLE EOT before a job; job held |
| E06 | Can't see the printer (power or cable) | Cable link down |

**Decisions taken.**
- **Printing:** density stays Dark, gamma 0.60. Atkinson dithering only (Floyd–Steinberg faulted). No thermal-limit setting.
- **Network:** the printer's IP never needs changing (discovery finds it). Setup only ever starts from BOOT. The unit's name becomes its mDNS name.
- **Templates:** drawn in the browser and printed as images.
- **Image size:** the ceiling is memory, not the printer.
- **Online:** anyone with a link may print (the owner's choice); Extra secure adds device approval; Pause covers the rest. No PIN on the kiosk (the owner's choice).

---

## 11. REST API (unit)

| Method | Path | Notes |
|---|---|---|
| GET | `/api/printer/status` | `alert`, `restingMs`, `paper`, `queue`, `pacing`, `system` (incl. `maxImageBytes`, `largestBlock`), `local`, `online{state,text,paused,url(home only)}`, `image` defaults, `items` counts. No IPs or RSSI via the relay. |
| GET/POST | `/api/settings` | Home only |
| POST | `/api/print/text` | `text`, `size`, `font`, `align`, `cut` |
| POST | `/api/print/raster` | Multipart; `width rows coverage cut tail` |
| POST | `/api/print/raw` | Home only |
| GET | `/api/jobs` | `?id=N` or `?limit=N`. Cleaned via the relay (source online/home/schedule, no IP). |
| POST | `/api/jobs/clear`, `/api/system/reboot`, `/api/system/reset` | Home only |
| GET/POST | `/api/items?kind=d\|s\|p`, `/api/items/one`, `/api/items/update`, `/api/items/delete`, `/api/items/run` | Drafts, saved, scheduled. `update` takes `{src,title,base,sess}` and replies with `conflict`/`prev`. |
| POST | `/api/items/bin?id=&bytes=&off=`, `/api/items/pic?owner=&bytes=&off=` | Streamed to flash, pieces of 128 KB or less |
| GET | `/api/items/pic` | The stored picture |
| GET/POST | `/api/clock` | POST home only (`{"tz":"POSIX TZ"}`) |
| GET/POST | `/api/theme?name=fine`, `/api/kiosk` | POST to kiosk is home only |
| GET/POST | `/api/online`, POST `/api/online/newlink`, `/api/online/newowner` | Home only |
| GET | `/log`, `/log.txt[?saved=1]`, `/kiosk` | `/log` home only |
| — | Setup mode | `/`, `/setup/networks`, `/setup/status`, `/setup/join`, `/setup/printer` on 192.168.4.1 |

Serial menu: `W` forget Wi-Fi, `A` reprint the welcome slip, `M` memory ladder. Bench (`BUILD_BENCH`): `J K E U H V`.

---

## 12. Building and testing

**Compile** (owner's PC): Arduino IDE 2, ESP32 Dev Module, Huge APP. **In the sandbox:**
- The Arduino download hosts are blocked; GitHub is not.
- Install arduino-cli from GitHub releases, with the board index at `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`.
- Stub `package_index.json` / `library_index.json`, plus empty tool folders for the built-in discoveries and `arduino:dfu-util`.
- Run `core install esp32:esp32@3.3.12 --skip-post-install`, and clone ArduinoJson v7.4.2 and Ethernet from GitHub.
- Compile with `--fqbn esp32:esp32:esp32:PartitionScheme=huge_app`.

**Tests** (`tests/README.md`):
- **Page tests:** `node tests/mock.js`, then `page_test`, `draft_test`, `v231_test` (online out-of-memory retry), `v241_test` (styled part split on memory). Other tests set level 4 first.
- **Preview, demo and levels:** build `dist/`, then `pages_test` and `levels_test`.
- **Scheduler and storage:** `tests/host/build.sh` (copies the real firmware files each time).
- **Relay link:** `tests/relayhost/build.sh` (needs ArduinoJson and mbedTLS). The real relay runs under `wrangler dev` with `tests/liverelay/test-hook.patch` (`TEST_HOOKS=1` only).
- **Gotchas:** puppeteer-core finds Chrome via `tests/chrome.js` (`CHROME` overrides). In the sandbox, `pkill -f` can kill the calling shell: use the `.pid_*` files.

---

## 13. Bugs and their lessons

| Bug | Lesson |
|---|---|
| `WiFiClient::write()` short writes corrupted rasters | A byte count that must match a declared length tolerates no partial write |
| Socket closed before the buffer drained (half images) | A successful write means *accepted*, never *delivered* |
| Shortfall cleared before reconnecting; in-flight bytes not counted; debt paid into a restarted printer | Keep the whole debt until it is paid; in-flight bytes are owed; a printer that restarts owes nothing |
| Status polling stole the printer's only socket | The printer accepts one TCP connection |
| Retry after a hard fault at about 70 s re-faulted | The head stays hot for minutes: hold images for 3 minutes |
| Ack sent before the cut measured arrival, not printing | A queued command is only a clock if something ahead of it must wait |
| A stale comment described reverted code | A stale comment is a bug that fires in the next session |
| **0.23.0:** online photo gave `HTTP 400: {"error":""}` — the reason was cleared before it was sent | Copy a message before resetting the state that holds it |
| **0.24.0:** "Welcome to the internet" printed every 30 s — the heading was queued, then the QR code failed for memory, silently, and was retried | Make everything that can fail before queuing anything, and log every refusal |
| **0.23.1:** capping styled parts at free memory made more seams | Memory-driven sizes belong to the photo card; keep the tested fixed sizes elsewhere and adapt only on failure |

**Dead ends — do not revisit without new evidence:**
- **Printer link:** a persistent TCP connection across jobs; delays inside an image (a pause makes a white line); W5500 socket exhaustion.
- **Printer controls and faults:** software density or speed control; cutter duty limits; our own status traffic causing faults.
- **Image handling:** raster stripe joins causing lines; a running heat account; narrowing photos to avoid the step.
- **OCR:** Tesseract was rejected by the owner (a 1.9 MB download).

**Gotchas.**
- **The print debt:** it is voided when the cable link drops (unplugging the cable with the printer on can garble the next photo), and it lives in RAM, so a reflash mid-fault loses it.
- **Memory:** `maxImageBytes` drops while a photo is held, and with online on.
- **Serial garbage:** comes from the USB monitor; `/log` is clean.
- **Banding checks:** judge banding from ruled test panels, never from photos.

---

## 14. Ideas (for the owner to prioritise)

- **Templates:** Recipe, Reminder, Voucher, Photo strip, speech palm cards. A "make your own" designer inside the Style menu (never a free-form canvas). Stored fonts (2–3 WOFF2).
- **Extract text:** tune on more real screenshots.
- **Kiosk:** a dry run on the MacBook; more decorations.
- **Printing:** print from storage (open item 3); profiles for another printer or roll.
- **Security:** an optional PIN for Settings on the home network.
- **Integrations:** Home Assistant or a PC through the REST API.

---

## 15. Version history

| Version | Date | What |
|---|---|---|
| 0.9–0.17 | Sep–Oct | Printer understood: pacing, recovery, banding tail, discovery, setup by slip, E-codes, log, photo card, Extract text, kiosk (all tested on hardware) |
| 0.18 | 18 Oct | Parking fine theme |
| 0.19 | 18 Oct | Future: drafts, saved, scheduled prints (on the unit) |
| 0.20 | 19 Oct | Shared drafts; Settings (printer, home only) vs Preferences (device) |
| 0.21.0–0.21.2 | 20 Oct | Online printing through the relay; identity key; welcome/knock slips; matched to the live relay |
| 0.22 | 21 Oct | Troubleshooting build: request logs, log saved to flash, recent prints online, draft links |
| 0.23.0 | 21 Oct | Drafts option C (manual save); Pause online printing |
| 0.23.1 | 21 Oct | Empty 400 error fixed; online parts 24 KB; phantom-print keep-awake; hello reports drops (relay Activity log) |
| 0.23.2 | 22 Oct | Preview and public demo generated from the page source; LAN addresses hidden online |
| 0.24.0 | 22 Oct | Levels; Future/Level header buttons; demo strips home-only code |
| 0.24.1 | 22 Oct | Welcome-slip loop fixed (QR first, logged, fallback); "device" wording; HOW TO USE rewritten; styled parts back to 48 KB |
| 0.24.2 | 24 Oct | Repository ready to go public: neutral kiosk placeholder, portable tests, relay identifiers moved to secrets, MIT licence, README demo screenshot |
| Relay 0.23 / 0.23.1 / 0.24 | 21–22 Oct | Pause; admin Activity log; flood guard |

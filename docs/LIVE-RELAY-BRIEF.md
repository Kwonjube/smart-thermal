# Smart Thermal firmware: brief for connecting to the live relay

From the website/relay chat, 20 Oct 2026. The relay is **deployed and live**. This brief is the source of truth wherever it differs from the original *online-printing-spec-for-website.md* or the `relay.js` stand-in. The relay code is attached as `print-relay.zip`; `src/index.js` is authoritative.

**Goal:** connect the first physical printer, have it appear under Join requests, get approved, and print the "Welcome to the internet!" slip. Then update the printer's existing web pages so they work through the relay (section 5).

Please answer for the owner: does the current firmware already support this, and how is online printing turned on on the device?

---

## 1. Addresses (final)

| Address | Use |
|---|---|
| `wss://print.justifiedsystems.com.au/relay/connect` | Printers connect here |
| `https://print.justifiedsystems.com.au/p/<code>/` | A printer's page for visitors. `/p/<code>` (no slash) redirects here |
| `https://print.justifiedsystems.com.au/p/<code>/_r/printers` | Visible printers list (answered by the relay) |
| `https://print.justifiedsystems.com.au/p/<code>-a` | The printer owner's page: owner code and device approvals |
| `https://admin.justifiedsystems.com.au` | Site owner's admin. **Changed:** it is no longer `print…/admin`. The firmware must never link to it, print it or call it |

The printer only needs the first address. The `url` field in the relay's `on` message always holds the exact visitor link.

## 2. TLS (required to connect at all)

- **Certificate type:** the domain is on Cloudflare's free plan, so the edge certificate is **ECDSA**. mbedTLS must allow ECDSA certificates (P-256/P-384) and ECDHE-ECDSA cipher suites under TLS 1.2.
- **Do not pin one root.** Cloudflare issues from **Let's Encrypt, Google Trust Services or SSL.com** and may switch between them at renewal. Use the **ESP-IDF certificate bundle** (`esp_crt_bundle_attach`, or `setCACertBundle` on Arduino). Otherwise, include at least ISRG Root X1/X2, GTS Root R1/R4 and the SSL.com/Certum roots.
- **SNI and time:** send SNI `print.justifiedsystems.com.au`, and set the clock (SNTP) before connecting so certificate dates validate.

## 3. Connection protocol v1, exactly as implemented

### Handshake
1. Open the WebSocket. The relay immediately sends `{"t":"challenge","n":"<43 chars>"}`.
2. Send `hello` **within 30 s**. If not, the relay closes with 1008 "No hello". Any other first message also gets 1008.
3. The relay validates the `hello` strictly:

| Field | Rule |
|---|---|
| `v` | The number `1` |
| `key` | base64url, **no `=` padding**, of the 65-byte uncompressed P-256 public key (first byte `0x04`) |
| `sig` | base64url, **no padding**, of the 64-byte raw `r‖s` ECDSA-SHA256 signature over UTF-8 `smartthermal-relay-v1:` + the challenge string exactly as received. DER signatures are rejected |
| `mac` | `AA:BB:CC:DD:EE:FF` format. Anything else is stored blank |
| `name`, `owner` | Up to 60 characters. Control characters are stripped. A blank name becomes "Printer" |
| `fw` | Up to 20 characters |
| `visible`, `secure` | JSON booleans. Anything other than `true` counts as false |
| `ownerHash` | 64 hex characters: SHA-256 of `smartthermal-owner-v1:` + the owner code **upper-cased**. If it is missing or invalid, the owner page can never sign in |

4. **Failures:**
   - Bad key or signature: close **1008**.
   - Too many waiting join requests (more than 3 per IP or 20 in total): close **1013**. Back off for at least 10 minutes.

### State messages
- `{"t":"state","s":"pending"}`: the socket stays open. Approval arrives on the same socket as `on`.
- `{"t":"state","s":"on","code":"ab12cd","url":"https://print.justifiedsystems.com.au/p/ab12cd/"}`: **this is sent on every hello of an approved printer, not just the first.** Print the welcome slip only when the code differs from the stored one (or none is stored), or the reconnects will print a slip each time.
- `{"t":"state","s":"declined"}` and `{"t":"state","s":"off"}` are followed by a close (1000). Retry after 6 hours, or straight away after a settings change or restart. When the owner switches the printer on again in admin, it takes effect at the next hello.
- `{"t":"newcode"}` from the printer: the relay answers with a new `on` message. The old code is a 404 immediately.

### Notices
`{"t":"notice","k":"device-request","name":"Alex"}` is for extra secure printers and should print the "Knock knock!" slip. `name` is visitor-supplied, at most 40 characters, with control characters stripped. Treat it as **plain text only**.

### Visitor requests
- **Request:** `{"t":"req","id":N,"m":"GET|POST","p":"/path?query","ct":"…","len":N,"who":"guest|owner","ua":"…"}`. Then `len` bytes arrive as **binary** frames of at most 8 KB. A GET has `len` 0 and no frames.
- **Paths:** `p` is the path with the `/p/<code>` prefix removed. The page root is `/`.
- **Ids:** only one request is in flight at a time. `id` counts up per connection and restarts after a relay restart, so only match it against the request in flight.
- **Reply:** `{"t":"res","id":N,"s":200,"ct":"…","cc":"…"}`, then the body as **binary** frames only (text frames are ignored), then `{"t":"end","id":N}`.
  - `s` must be an integer from 200 to 599. Anything else becomes 502.
  - 204, 205 and 304 carry no body.
- **Timeout: 90 s from when a request is sent.** If there is no `end` by then, the visitor gets 504 and the relay **closes the socket (1011)** so late data cannot leak into the next request. For anything slow, such as a long print, reply quickly (for example 202 plus a job id) and let the page poll.
- **What the relay refuses before forwarding:**
  - other methods: 405
  - bodies over **160 KB (163,840 bytes)**: 413
  - more than 10 queued requests: 503
- **Print counting:** `POST /api/print/*` is counted as a print in admin only if the printer answers with a status below 300.

### Keep-alive
The printer sends the text frame `ping` every 45 s. The relay answers with the **text frame** `pong`, not a WebSocket-level pong. Cloudflare drops idle WebSockets after about 100 s, so keep the 45 s ping. Reconnect after 120 s with nothing heard.

### Reconnect policy (recommended)
- Use exponential backoff with jitter, starting at 2 s and capping at 5 minutes.
- After 1013, wait at least 10 minutes.
- After 1008, log it and back off to the cap. It means a key or signature bug, and retrying fast will not help.
- A factory reset creates a new key, so the printer appears as a **new join request** and needs approving again.

## 4. Paths the relay blocks (so the page must not use them)

Before blocking, the relay percent-decodes and lower-cases the path and collapses `//`. Paths containing `..` or `\` get a 400. These return **403**:
- anything starting with `/api/settings`, `/api/system`, `/api/print/raw`, `/api/jobs/clear`, `/api/online`, `/log` or `/setup`
- `POST` to `/api/kiosk` or `/api/clock`

**Watch out:** these are prefixes. `/logo.png`, `/login` and `/setup.js` are blocked too. Rename any page asset that starts with `log` or `setup`.

## 5. Changes needed to the printer's existing web pages (online portal)

The relay serves the printer's own pages with **its own** headers, and passes through only `content-type` and `cache-control` from the printer.

**The relay sets this CSP:**
```
default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline';
img-src 'self' data: blob:; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'
```
It also sets `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, `X-Frame-Options: DENY` and `Cache-Control: no-store` (unless the printer sends `cc`).

Please review the pages against this list:
1. **No HTML form submissions** (`form-action 'none'`). Send everything with `fetch` or XHR from script and call `preventDefault()` on any `<form>`.
2. **No `eval`, `new Function` or string `setTimeout`.** `unsafe-eval` is not allowed.
3. **No external resources.** That means no CDN scripts, Google Fonts or remote images. Everything comes from the printer itself; `data:` and `blob:` images are fine, which covers photo previews.
4. **Correct `content-type` on every file**, because nosniff is on: JS as `application/javascript`, CSS as `text/css`, JSON as `application/json` and so on.
5. **Headers that are dropped:** `Location`, `Set-Cookie`, `Content-Disposition` and `ETag`.
   - Do redirects in the client, not with 3xx.
   - Don't rely on cookies set by the printer.
   - Don't rely on download filenames.
6. **Polling only.** There is no WebSocket or server-sent events for visitors through the relay, and no response may stay open longer than 90 s.
7. **Relative URLs everywhere.** The same page must work both locally at `http://<printer-ip>/` and online at `/p/<code>/`. Use relative URLs (for example `api/printer/status`, with no leading slash), or work out the base from `location.pathname` (`/^\/p\/[a-z0-9]{6}\//`). Never hard-code `/api/...` from the site root.
8. **Escape everything untrusted.** All printers' pages share one origin, so an injection in one page can act on visitors of another. Escape names from `_r/printers`, visitor input and shared-list content, using `textContent` rather than `innerHTML`. Links to other printers are `/p/<code>/` (same origin).
9. **Owner/guest:** the `who` field is the relay's only signal about the visitor (`owner` means an owner-approved device). Settings-type actions are refused anyway (section 4).
10. **Welcome slip contents:** the visitor link (`url` from `on`), the owner code, and, for extra secure printers, the owner page `https://print.justifiedsystems.com.au/p/<code>-a`. Do not print the admin address.
11. **Status polling:** keep it at 15 s or slower when opened online. The free plan allows about 100,000 relay requests a day across all printers.

## 6. First-printer test (owner + firmware)

1. Flash the firmware, connect it to Wi-Fi and turn on online printing.
2. In `https://admin.justifiedsystems.com.au`, the printer appears under **Join requests** with its name, owner, MAC, IP, place and Perth time.
3. Approve it. Within seconds, the "Welcome to the internet!" slip prints with the link and owner code.
4. Restart the printer. It reconnects and must **not** print a second slip, because the code is unchanged.
5. On a phone using **mobile data**, open the link. The page loads and its styles and scripts work (check the browser console for CSP errors). Print a short list, a long list and a photo. **Prints today** goes up in admin.
6. Ask the printer for a new link. The old link returns 404, and a new slip prints.
7. Unplug the printer and open the link: it is reported offline. Plug it back in: it reconnects within about 2 minutes.
8. Extra secure mode: turn on `secure`. A new phone sees "Ask to print here", and a "Knock knock!" slip prints. Approve the device at `/p/<code>-a` with the owner code, and the phone can then print.
9. Run the firmware test suite against the live relay (`wss://print.justifiedsystems.com.au/relay/connect`).

**Testing tips**
- Each factory reset leaves a waiting join request. Decline or **Forget** old ones in admin, because more than 3 waiting from the home IP will get new ones refused with 1013.
- More than 20 wrong `/p/` codes from one IP in 10 minutes locks that IP out of all printer links for 10 minutes.
- The old `relay.js` stand-in differs from the live relay. It has no hello timeout, no strict validation, no CSP or header stripping, and no repeated `on`. Test against the live relay.

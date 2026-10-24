# Smart Thermal online printing: spec for the website

> **Superseded where they differ** by the website chat's brief
> (`tests/liverelay/LIVE-RELAY-BRIEF.md`, 20 Oct) and the live relay code
> (`print-relay/src/index.js`). Admin is at `admin.justifiedsystems.com.au`.

For the website developer. From the Smart Thermal firmware (v0.21.0).
The printer side is built and tested against a stand-in relay (`relay.js`,
included). This document is everything the website side must do. Questions go
back through the owner.

## 1. The idea in one paragraph

Each printer (an ESP32 unit on someone's home Wi-Fi) connects **out** to the
website and keeps that connection open, so no home router needs setting up.
The website relays visitors' requests down that connection to the printer's
own web page, one at a time. A printer's first connection is a **join
request** that the site owner approves on an admin page; until then nothing
reaches the printer. Printers prove who they are with a key they make
themselves; the website stores only the public half.

## 2. Addresses

| Address | What |
|---|---|
| `wss://print.justifiedsystems.com.au/relay/connect` | Printers connect here (WebSocket). |
| `https://print.justifiedsystems.com.au/p/<code>/` | A printer's page for visitors. `<code>` is 6 characters from `23456789abcdefghjkmnpqrstuvwxyz`. `/p/<code>` redirects (301) to the version with the slash. |
| `https://print.justifiedsystems.com.au/p/<code>/_r/printers` | Answered by the website itself: other printers that chose "visible" (section 6). |
| `https://print.justifiedsystems.com.au/p/<code>-a` | The printer owner's page (extra-secure approvals, section 7). |
| `https://print.justifiedsystems.com.au/admin` | The site owner's admin page, behind Cloudflare Access (section 8). |

Everything under `/p/<code>/` except `/_r/...` is forwarded to the printer with
the `/p/<code>` prefix removed (`/p/ab12cd/api/printer/status?x=1` reaches
the printer as `/api/printer/status?x=1`). The printer's page already sends
all its requests under its own `/p/<code>/` prefix.

`print.justifiedsystems.com.au` is a Cloudflare Worker custom domain of its
own, separate from the main site (Cloudflare Pages). The main site's headers
and strict CSP are not involved, and the printer pages share nothing with it.

Requirements on the hosting:
- **TLS 1.2 must stay enabled** (minimum TLS 1.2, not 1.3-only). The ESP32
  firmware speaks TLS 1.2. Certificates are checked against the usual public
  roots, so the normal Cloudflare edge certificate is fine.
- WebSockets on (Cloudflare default).
- **Responses under `/p/*` carry these headers** (the printer page needs
  scripts, so not the main site's strict CSP). Suggested:
  `Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`
  plus `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`,
  `Cache-Control: no-store` (unless the printer's answer says otherwise).
  The `/p/<code>/` page itself is HTML from the printer; pass its
  `content-type` through.

## 3. Connection and identity (protocol v1)

WebSocket, text frames are JSON (except `ping`/`pong`), binary frames carry
request and response bodies.

1. Printer opens `wss://…/relay/connect`.
2. Website sends `{"t":"challenge","n":"<43 chars: 32 random bytes, base64url>"}`.
3. Printer answers:

```json
{"t":"hello","v":1,"fw":"0.21.0",
 "key":"<base64url of the 65-byte uncompressed P-256 public key>",
 "sig":"<base64url of the 64-byte raw r||s ECDSA signature>",
 "mac":"24:6F:28:AA:BB:CC","name":"Kitchen printer","owner":"Alex",
 "visible":true,"secure":false,
 "ownerHash":"<hex SHA-256 of 'smartthermal-owner-v1:' + owner code>"}
```

4. Website verifies the signature over the UTF-8 text
   `smartthermal-relay-v1:` + the challenge, with WebCrypto (no library):

```js
const key = await crypto.subtle.importKey('raw', b64urlToBytes(hello.key),
  { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
const ok = await crypto.subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, key,
  b64urlToBytes(hello.sig), new TextEncoder().encode('smartthermal-relay-v1:' + nonce));
```

   Bad signature: close the socket (code 1008). The printer's id can be
   `base64url(SHA-256(key bytes)).slice(0, 16)`. Store the public key only.
5. Website answers with the printer's state (below). An approved printer
   connecting again just sends a fresh hello; update the stored name, owner,
   visible, secure and ownerHash from it.

A **new key** means a new printer (a factory reset makes a new key), so it
needs approving again.

**When a new key says hello, record a join request** with: printer name,
owner (the person's name), MAC, firmware, visible, secure, the connecting IP
(`CF-Connecting-IP`), where it is from (`request.cf.city`, `region`,
`country`, `asOrganization`), and the time in Perth
(`new Date().toLocaleString('en-AU', { timeZone: 'Australia/Perth' })`).

## 4. States the website sends

```json
{"t":"state","s":"pending"}
{"t":"state","s":"on","code":"ab12cd","url":"https://print.justifiedsystems.com.au/p/ab12cd/"}
{"t":"state","s":"declined"}      then close the socket
{"t":"state","s":"off"}           switched off by the site owner; then close
```

- **pending**: keep the socket open (hibernation makes this free). The moment
  the request is approved, send `on`. The printer then prints a cheerful
  "Welcome to the internet!" slip with the link and owner code.
- **on**: send on every hello of an approved printer, with its current code
  and full URL (the printer prints and shows exactly this URL).
- **declined / off**: the printer shows "Not approved" and asks again after
  6 hours, or straight away if its owner changes its settings or restarts it.
  Re-approving a declined printer takes effect at its next hello.

The printer may ask for a new link: `{"t":"newcode"}`. Make a new unique
code, retire the old one at once (404), and answer with a new `on` state.

## 5. Visitor requests

One request at a time per printer; queue the rest (max ~10, then 503).

Website to printer:

```json
{"t":"req","id":42,"m":"POST","p":"/api/print/raster?width=576&rows=600&coverage=12&tail=0&cut=0",
 "ct":"multipart/form-data; boundary=...","len":48123,"who":"guest","ua":"Mozilla/5.0 ..."}
```

then the body as binary frames totalling exactly `len` bytes (8 KB frames
are good). No other message may be sent to that printer until its `end`.

- `m`: GET or POST only. `p`: path and query, starting with `/`.
- `len`: refuse bodies over **160 KB** with 413 before forwarding.
- `who`: `guest`, or `owner` for a device approved with the owner code
  (section 7).

Printer to website:

```json
{"t":"res","id":42,"s":200,"ct":"application/json","cc":"no-store"}
```

then zero or more binary frames (the body, in order), then
`{"t":"end","id":42}`. Stream the binary frames straight into the visitor's
response. If the socket closes mid-request, end the response (502 if nothing
was sent yet). Time out a request after 90 s (504).

The printer itself refuses with 403, whatever the website sends: `/api/settings`,
`/api/system/*`, `/api/print/raw`, `/api/jobs/clear`, `/api/online*`, `/log*`,
`/setup*`, and POST to `/api/kiosk` or `/api/clock`. Please refuse these at the
website too, before forwarding.

Keep-alive: the printer sends the text `ping` every 45 s. Answer `pong`
(with Durable Objects: `ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping','pong'))`
answers without waking the object). The printer reconnects after 120 s
without hearing anything.

## 6. Visible printers

`GET /p/<code>/_r/printers` (answered by the website, for any approved
printer's page) returns the other approved printers whose owners ticked
"visible":

```json
[{"name":"Kitchen printer","code":"ab12cd","online":true}]
```

Exclude the asking printer. Printers that are not visible are reachable only
by their direct link.

## 7. Extra secure printers (`"secure": true`)

For these the website forwards **nothing** (not even the page) until the
visitor's device is approved by the printer's owner.

- **Unapproved visitor** at `/p/<code>/...`: the website serves its own small
  "Ask to print here" page: a name box and a button. On submit, it creates a
  device request and sets a device cookie named for the printer (e.g.
  `d_ab12cd`): 128 random bits, `HttpOnly; Secure; SameSite=Strict; Path=/p/`
  (so the same cookie also works on the owner page `/p/<code>-a`). Store only
  SHA-256 of the token.
  It then tells the printer, which prints a "Knock knock!" slip:
  `{"t":"notice","k":"device-request","name":"Alex"}`.
  Only the device that asked gets access, nothing else.
- **Owner page** `/p/<code>-a`: asks for the **owner code** (6 characters,
  printed on the owner's welcome slip, letters and digits, compare
  upper-cased). Check `SHA-256('smartthermal-owner-v1:' + code)` against the
  printer's `ownerHash`. Allow 5 tries per hour per printer. A correct code
  marks that browser as an owner device (same cookie scheme) and shows:
  device requests (name, time in Perth, place from the IP) with Approve,
  Decline, and a list of approved devices with Revoke. Owner devices may print
  too (`"who":"owner"`).
- When the printer's `ownerHash` changes (its owner made a new owner code),
  sign out existing owner devices.

## 8. The site owner's admin page (`/admin`, Cloudflare Access only)

- **Join requests**: printer name, owner name, MAC, external IP, place
  (city, region, country, network), requested at (Perth time), firmware,
  visible/secure. Approve / Decline.
- **Printers**: name, owner, state, online or offline (socket open now),
  last seen, prints today and in total (count `POST /api/print/*`), visible,
  secure, link. Actions: switch off, switch on, new link.
- Never store or log what is printed: no bodies, no paths beyond counting.

## 9. Limits and safety

- Bodies 160 KB, queue 10 per printer, request timeout 90 s.
- 6-character codes can be guessed by brute force, so rate-limit misses:
  e.g. more than 20 unknown `/p/` codes from one IP in 10 minutes gets 429.
- Free plan: ~100,000 requests a day. A printer page opened from outside
  checks status every 15 s; printing a long list makes one request per part
  plus a check per second until each part prints.

## 10. Suggested shape (Cloudflare, no libraries)

- One Worker. A Durable Object per printer (SQLite-backed, free plan), named
  by printer id: holds the printer's socket (hibernation API), its queue,
  state, code, info, counters and device approvals.
- A small index from code to printer id (another Durable Object or D1).
- The admin page: plain HTML from the Worker, behind Cloudflare Access.

## 11. Testing against the firmware without hardware

`relay.js` is a stand-in relay (Node, `npm install ws`), with the same
messages as above but none of the storage, admin, extra-secure or limits.
`node relay.js` on :8790 (`AUTO=1` approves everything). The firmware's PC
harness connects to it in plain TCP, and the test suite drives the real
printer page through it. The real Worker should behave the same on the wire.

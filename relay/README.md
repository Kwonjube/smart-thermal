# Print relay

This is the Cloudflare Worker that lets Smart Thermal printers be reached from outside the home. It implements *Smart Thermal online printing: spec for the website*, protocol v1. It is separate from the website and shares nothing with it.

| Address | What | Who |
|---|---|---|
| `https://print.justifiedsystems.com.au` | Printers connect here, visitors use printer links and owners approve devices | Public (untrusted printer content) |
| `https://admin.justifiedsystems.com.au` | Your admin: join requests and printers | You only, through Cloudflare Access |

The two addresses are separate origins, so nothing a printer page does can read or drive the admin.

- **Code:** one file, `src/index.js`. No libraries, no build step and nothing to patch.
- **State:** one SQLite-backed Durable Object (free plan). It holds printer connections (hibernating), queues, state, codes, counters, device approvals and admin sessions.
- **Privacy:** printed content is never stored or logged. Request logs are off. The admin Activity log keeps events and failed requests (method + path, no query or body, nothing about visitors) for 30 days, with repeats merged and a per-minute cap (see CHANGELOG.md, 0.23.1 and 0.24).

## One-time Cloudflare steps

Do these once, before or right after the first deploy. Nothing needs repeating afterwards.

**1. Move the Access application to the admin host.** Go to **Zero Trust**, then **Access**, then **Applications**, and edit your existing application:
- Change the domain from `print.justifiedsystems.com.au` with path `admin` to `admin.justifiedsystems.com.au` with **no path**, so the whole host is protected.
- Remove the old `print.justifiedsystems.com.au/admin` entry. The print host must not be behind Access.
- Leave the policy as it is (allow only your own email).

Editing keeps the same **AUD tag**, so `wrangler.jsonc` needs no change. If you create a new application instead, copy its AUD tag into `ACCESS_AUD`.

**2. Make the session lengths equal** so Access cannot quietly sign you back in:
- **Application session duration:** 1 hour. Set it on the same application.
- **Global session timeout:** 1 hour. Set it under **Zero Trust**, then **Settings**, then **Authentication**.

Access issues a new application token without asking while its global session is still valid. With both at 1 hour, they end together, so after the relay signs you out, signing back in always needs a real login.

**3. DNS and certificates:** nothing to do. `wrangler deploy` creates both custom domains, `print.` and `admin.`, with their DNS records and certificates.

## Configuration

Public settings are in `wrangler.jsonc`. Three values that identify the admin are kept out of
this public repository as **Worker secrets**. Set them once; they survive every deploy:

```
npx wrangler secret put ADMIN_EMAIL          # the one email allowed into admin (must match the Access policy)
npx wrangler secret put ACCESS_TEAM_DOMAIN   # <team>.cloudflareaccess.com
npx wrangler secret put ACCESS_AUD           # the Access application's AUD tag
```

None of them is a password, but there is no reason to publish them. If any is missing, the
admin page refuses everyone (it fails closed); printers and visitors are unaffected.

**Moving an existing deployment to secrets (once):** a secret cannot have the same name as a
`vars` entry, so deploy this `wrangler.jsonc` first (the admin page refuses everyone for the
minute in between), then run the three `secret put` commands above. Printers stay connected.

## Deploy

1. Install Node.js LTS from nodejs.org.
2. In a terminal opened in this `print-relay` folder, run:
   ```
   npx wrangler login
   npx wrangler deploy
   ```
3. In the dashboard, go to **Workers & Pages**, open `print-relay`, then **Settings**, then **Domains & Routes**. Confirm that only the `print.` and `admin.` custom domains are listed, and that **workers.dev** and **Preview URLs** are off. The Worker also refuses any other hostname.

**Updating later:** run `npx wrangler deploy` again. Printers reconnect by themselves.

## Using the admin

- Open `https://admin.justifiedsystems.com.au` and sign in with Cloudflare.
- You are signed out automatically after **5 minutes without activity**. Activity means loading the page or pressing a button. Simply reading an open page for 5 minutes counts as idle.
- **Sign out now** does the same thing straight away.
- Signing out ends the relay's admin session **and** your Cloudflare Access session, so getting back in means signing in again. There is nothing to remember to do.

## Go-live checks

### A. Automated, no login (run after every deploy)
```
node scripts/check-live.mjs <your-workers.dev-subdomain>
```
This checks that:
- every admin address (`/`, `/action` with fake tokens and headers, `/logout`, others) goes to the Access login
- the print host has no admin and is not behind Access
- workers.dev serves nothing and unknown codes return 404
- the printer WebSocket opens and sends its challenge
- TLS 1.2 works on both hosts

### B. Your login
1. Open `https://admin.justifiedsystems.com.au/cdn-cgi/access/get-identity` after signing in, and check that `"email"` is the address you set as `ADMIN_EMAIL`.
2. Open `https://admin.justifiedsystems.com.au`. You should see the **Print relay** page. A plain "Forbidden" means the email does not match `ADMIN_EMAIL`.
3. Leave the page open for 5 minutes. It should sign you out to Cloudflare's logged-out page, and opening the admin again should ask you to sign in.

### C. Real firmware (needed before calling it production-ready)
| # | Do | Expect |
|---|---|---|
| 1 | Turn on online printing on a printer | It appears under **Join requests**, with name, owner, MAC, IP, place and Perth time |
| 2 | Approve it | It prints "Welcome to the internet!" with its link and owner code |
| 3 | Open the link on a phone using mobile data, not home Wi-Fi | The printer page loads; status updates every 15 s |
| 4 | Print a short list, a long list and a photo | All print; **Prints today** goes up |
| 5 | Unplug the printer, then open the link | "Offline" message. Plug it back in: it reconnects within about 2 minutes |
| 6 | Ask the printer for a new link | The old link returns 404 and the new slip works |
| 7 | **Switch off**, then **Switch on** in admin | The printer shows "Not approved", then works again after a restart |
| 8 | Extra secure printer: open its link on a new phone and ask to print | A "Knock knock!" slip prints. Approve the device at `/p/<code>-a` using the owner code; the phone can now print |
| 9 | Run the firmware's test suite against `wss://print.justifiedsystems.com.au/relay/connect` | All pass |

## Addresses in detail

| Host and path | Purpose |
|---|---|
| print `/relay/connect` | Printers connect here (WebSocket) |
| print `/p/<code>/…` | A printer's page for visitors (forwarded to the printer) |
| print `/p/<code>/_r/printers` | Other visible printers (answered by the relay) |
| print `/p/<code>-a` | The printer owner's page (owner code, device approvals) |
| admin `/` | Admin page |
| admin `/action` | Admin buttons (POST) |
| admin `/logout` | Sign out (also ends the Access session) |
| admin `/log.txt` | Activity log download |

Everything else returns 404, and so does any other hostname.

## Security

### Admin sign-in (Cloudflare Access + Worker)
Cloudflare Access protects the whole admin host. The Worker then checks the Access token itself:
- **Signature:** RS256 only, against your team's published keys. `none` and other algorithms are refused.
- **Issuer:** must be `https://<ACCESS_TEAM_DOMAIN>`.
- **Audience:** must be the AUD tag.
- **Expiry:** expired tokens, and tokens not yet valid, are refused.
- **Email:** the email *inside the signed token* must equal `ADMIN_EMAIL`. Nothing the browser sends is trusted for identity, and a token with no email is refused.

The check fails closed: if the signing keys cannot be fetched, or a setting is empty, admin access is refused. Key refreshes are limited to one every 5 minutes.

### Admin session and inactivity sign-out
- **Starting a session:** only a **fresh** Access sign-in can start one, meaning an Access token issued in the last 2 minutes. An old Access token cannot start or revive a session.
- **Ending a session:** sessions end after **5 minutes idle**, or 8 hours in total. This is enforced on the server, so it also works if you just close the tab. Open pages also send themselves to `/logout` after 5 minutes, with no script needed.
- **What ending does:** it deletes the relay session and sends the browser to Access's logout, which revokes the Access session for all applications.
- **Session cookie:** `__Host-relay_admin` is HttpOnly, Secure and SameSite=Lax, and the browser drops it when it closes. The `__Host-` prefix stops any other subdomain, including `print.`, from setting or overwriting it.

### Forms (CSRF)
- Each admin page load issues a fresh random token; only its hash is stored, tied to your session. A button press must present it, and it is **used up** on the first use. A newer page load also replaces it, so a token works once, only from the latest page, and only within a live session.
- Owner pages (`/p/<code>-a`) use the same one-time scheme, tied to the owner's device.
- Admin requests must also be top-level, same-origin page loads or form posts: `fetch`, frames and other sites are refused.
- If you open the admin in two tabs, only the newest tab's buttons work. Reload the other one.

### Untrusted printer content
- **Separate origins.** Printer pages live only on `print.`. They cannot read admin pages, are blocked by their own CSP (`connect-src 'self'`, `form-action 'none'`, `frame-ancestors 'none'`), and form posts from `print.` to `admin.` are refused as cross-origin.
- **Escaping.** All printer and visitor text is escaped. The relay's own pages allow no scripts at all.
- **Referrer policy.** The relay's own pages use `same-origin`. This keeps the referrer private to other sites, while letting browsers send the real `Origin` on form posts, which the origin check needs.
- **Access cookie.** Your Access cookie now belongs to `admin.` only. Printer pages can no longer read your identity through Cloudflare's `/cdn-cgi/access/get-identity`.

### Visitors and printers
- **Guessing:** an IP that tries more than 20 unknown codes in 10 minutes is refused on every `/p/` link, so its guesses reveal nothing.
- **Join requests:** at most 3 waiting per IP, and 20 in total.
- **"Ask to print":** at most 3 per printer per hour, and 10 waiting at once.
- **Timeouts:** a request that times out resets the printer's connection, so late data cannot reach the next visitor.
- **Printer identity:** each printer proves itself with a P-256 signature over a fresh challenge. Only public keys are stored.

## Tested

All of these ran on the relay code as of 0.23 (the Activity log of 0.23.1/0.24 was tested separately, see CHANGELOG.md).
- **Functional:** 94 checks, run locally (`wrangler dev`, both hostnames) against simulated printers that sign with real P-256 keys. They cover:
  - joining, approval, forwarding, limits and extra secure printers
  - the separate admin origin
  - one-time tokens: used once, refused on reuse, and refused from an older page
  - admin sessions: an old token cannot start one, idle sign-out, ending the Access session, "Sign out now", and a dead cookie cannot be revived
- **Access token:** 14 checks.
- **Unauthenticated attacks:** 23 attempts at admin actions with the production settings and no test bypass. They used forged and unsigned tokens, spoofed internal headers and session cookies, path tricks, the old `print/admin` address, the print host and other hostnames. All were refused, and the waiting printer was never approved.
- **Real-browser click-through:** 14 checks in Chromium using only the headers the browser sends itself:
  - admin Approve, New link and Sign out now
  - a visitor's "Ask to print"
  - the owner's sign-in and Approve
  - the approved visitor reaching the printer page

  An earlier version passed all the other suites but failed here, because the browser sent `Origin: null`. It is fixed in this version.
- **Browser attack:** Chromium, signed in with an open admin session. A malicious printer page tried six ways to approve a printer: fetch, fetch POST, an iframe, a popup, and forms injected into a same-origin frame and into a popup. All failed.

It has not yet been tested on Cloudflare itself or with real printers. Use Go-live checks A, B and C.

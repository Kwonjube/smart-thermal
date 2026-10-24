# Security

## Model

**On the home network.** Anyone on the same Wi-Fi as the printer can use its page and API.
That includes **Settings**, which shows the online link and owner code. This is deliberate:
the printer is a shared household device, like the printer itself. Put it on a network you
trust, or a guest network for visitors.

**From outside: the relay (optional).**
- **The printer dials out.** The unit opens one outgoing, TLS-protected WebSocket to the relay.
  Nothing on the home router is opened.
- **Printer identity.** Each unit makes its own P-256 key on first use and keeps the private
  half in flash. Every connection signs a fresh challenge, and the relay stores only public keys.
- **Approval.** A new printer appears as a join request, and nothing reaches it until the
  relay's owner approves it.
- **Visitors.** Anyone with a printer's 6-character link can print to it. This is by design: the
  link is the invitation, and its owner can replace it at any time. "Extra secure" printers
  also require the owner to approve each device, using the owner code printed on the welcome
  slip.
- **Local-only rules.** Requests that arrive through the relay are marked by the unit and
  refused for Settings, system, logs, raw printing and similar paths. The relay refuses them
  as well.
- **Privacy.** The relay never stores printed content or request bodies. Its admin activity
  log keeps times, events and failed requests (method and path only) for 30 days, with
  nothing identifying visitors.
- **Admin.** The relay's admin host is behind Cloudflare Access. The Worker verifies the
  Access token itself (signature, issuer, audience, expiry and email). The admin's email and
  Access identifiers are Worker secrets, not in this repository.

**What is not in this repository.** There are no Wi-Fi passwords, owner codes, identity keys
or API tokens here. Each unit creates its own, and they never leave it except as described
above. The relay's admin identifiers are set with `wrangler secret put`.

**No over-the-air updates.** Firmware changes only over USB.

## Known limits

- The page and API on the home network have no password (see the model above).
- The setup hotspot's password is printed on the setup slip. Anyone holding that slip can set
  the printer up while it is in setup mode, which happens only after BOOT is held for 5 s, or
  when no network is saved.
- A printer link, once shared, can be passed on. Use "New link" to replace it, or Extra
  secure to require device approval.

## Reporting a problem

Please do not open a public issue for a security problem. Use GitHub's **Report a
vulnerability** (Security tab) on this repository, with steps to reproduce. You will get a
reply.

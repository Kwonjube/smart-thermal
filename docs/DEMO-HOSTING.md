# Public demo hosting

The demo is the printer's real web page with a built-in pretend printer. It is generated
from this repository and never edited by hand.

## Build

```
node SmartThermal/tools/build_pages.js        # writes dist/demo.html and dist/preview.html
```

The build:
- leaves out everything between `<!--home-only-->` and `<!--/home-only-->` in the page source
  (Settings, online-printing set-up and kiosk set-up);
- refuses to finish if the demo page would contain the site's domain, sample addresses,
  links or codes, `/api/online`, or the words "owner code". The pretend printer still names
  a few API paths, only so that it can answer "local only". It holds no values.

## Hosting (Cloudflare Pages, the main site)

- **Address:** `https://justifiedsystems.com.au/lab/smart-thermal/demo/`. Future demos follow
  `/lab/<project>/demo/`.
- **File:** upload `dist/demo.html` as `lab/smart-thermal/demo/index.html`.
- **Headers** for `/lab/smart-thermal/demo/*`. This policy replaces the site-wide one rather
  than being added to it; two policies stacked would block the demo's script.

```
Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob:; connect-src blob: data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'
X-Content-Type-Options: nosniff
Referrer-Policy: no-referrer
X-Robots-Tag: noindex
```

`connect-src` allows only `blob:` and `data:`, so even an altered file could not contact a
server.

- **Camera:** the demo never uses the camera; photos come from the file picker.
- **Updates:** rebuild and replace the file with every firmware release. Nothing else changes.

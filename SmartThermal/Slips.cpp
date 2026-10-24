#include "Log.h"
#include "Slips.h"
#include "Unit.h"
#include "MonoBitmap.h"
#include "stqr.h"
#include "Online.h"

static const JobSource SLIP = { "unit", IPAddress(0, 0, 0, 0) };

static const uint8_t LEFT = 0, CENTRE = 1;
static const char* const NAMES[] = { "setup", "addresses", "setup failed", "welcome online", "knock" };
static const char* g_why = "";      // why the last slip could not be queued, for the log

void SlipDesk::begin(PrintQueue& queue, JobStore& store, uint16_t widthDots) {
  _q = &queue;
  _store = &store;
  _width = widthDots;
}

// ----------------------------------------------------------------------
// Wanting
// ----------------------------------------------------------------------

// A slip already on its way is left alone: wanting it again must not
// print it twice.
void SlipDesk::wantSetup() {
  if (_state[SETUP] != SUBMITTED) _state[SETUP] = WANTED;
}

void SlipDesk::wantAddresses(IPAddress ip, const char* host) {
  if (_state[ADDRESSES] == SUBMITTED) return;
  _ip = ip;
  strlcpy(_host, host, sizeof(_host));
  _state[ADDRESSES] = WANTED;
}

void SlipDesk::wantSetupFailed(const char* ssid, const char* why) {
  if (_state[SETUP_FAILED] == SUBMITTED) return;
  strlcpy(_ssid, ssid, sizeof(_ssid));
  strlcpy(_why, why, sizeof(_why));
  _state[SETUP_FAILED] = WANTED;
}

void SlipDesk::wantOnline(const char* name, const char* url, const char* ownerCode,
                          bool secure, bool visible, const char* key) {
  if (_state[ONLINE] == SUBMITTED) return;
  strlcpy(_oName, name, sizeof(_oName)); strlcpy(_oUrl, url, sizeof(_oUrl));
  strlcpy(_oCode, ownerCode, sizeof(_oCode)); strlcpy(_oKey, key, sizeof(_oKey));
  _oSec = secure; _oVis = visible;
  _state[ONLINE] = WANTED;
}

void SlipDesk::wantKnock(const char* who, const char* printer, const char* url) {
  if (_state[KNOCK] == SUBMITTED) return;
  strlcpy(_kWho, who, sizeof(_kWho)); strlcpy(_kName, printer, sizeof(_kName));
  strlcpy(_kUrl, url, sizeof(_kUrl));
  _state[KNOCK] = WANTED;
}

// ----------------------------------------------------------------------
// Submitting and confirming
// ----------------------------------------------------------------------

void SlipDesk::loop() {
  if (!_q) return;
  const uint32_t now = millis();

  for (uint8_t i = 0; i < KINDS; i++) {
    const Kind k = static_cast<Kind>(i);

    if (_state[k] == SUBMITTED) {
      StoredJob a, b;
      if (!_store->get(_first[k], a) || !_store->get(_last[k], b)) continue;
      const uint8_t done   = static_cast<uint8_t>(JobStatus::Done);
      const uint8_t failed = static_cast<uint8_t>(JobStatus::Failed);
      if (a.status == done && b.status == done) {
        confirmed(k);
      } else if (a.status == failed || b.status == failed) {
        Log.printf("[slip] %s failed to print - retrying\n", NAMES[i]);
        _state[k]   = WANTED;
        _retryAt[k] = now + RETRY_MS;
      }
      continue;
    }

    if (_state[k] != WANTED) continue;
    if (_retryAt[k] && (int32_t)(now - _retryAt[k]) < 0) continue;
    // Only into an idle, reachable printer: a slip queued behind an
    // unreachable printer would just fail and cost a retry.
    if (!_q->printerReachable() || _q->pending() || _q->busy()) continue;

    if (submit(k)) _state[k] = SUBMITTED;
    else           _retryAt[k] = now + RETRY_MS;
    return;                                  // one slip at a time
  }
}

bool SlipDesk::submit(Kind k) {
  uint32_t first = 0, last = 0;
  bool ok = false;
  g_why = "the printer queue refused it";
  _noQr = _fails[k] >= QR_TRIES;
  switch (k) {
    case SETUP:        ok = printSetup(first, last);         break;
    case ADDRESSES:    ok = printAddresses(first, last);     break;
    case SETUP_FAILED: ok = printFailed(first, last);        break;
    case ONLINE:       ok = printOnline(first, last);        break;
    case KNOCK:        ok = printKnock(first, last);         break;
    default: break;
  }
  if (!ok) {
    if (_fails[k] < 255) _fails[k]++;
    Log.printf("[slip] %s not printed: %s - trying again in %lu s%s\n", NAMES[k], g_why,
               (unsigned long)(RETRY_MS / 1000), _fails[k] >= QR_TRIES ? ", without the QR code" : "");
    return false;
  }
  _fails[k] = 0;
  _first[k] = first;
  _last[k]  = last;
  Log.printf("[slip] %s queued as #%lu-#%lu%s\n", NAMES[k],
                (unsigned long)first, (unsigned long)last, _noQr ? " (no QR code: not enough memory)" : "");
  return true;
}

void SlipDesk::confirmed(Kind k) {
  _state[k] = IDLE;
  _retryAt[k] = 0;
  if (k == ADDRESSES) Unit::markAnnounced(_ip, _host);
  if (k == ONLINE)    Online::welcomed(_oKey);
  Log.printf("[slip] %s printed\n", NAMES[k]);
}

// ----------------------------------------------------------------------
// The slips themselves
// ----------------------------------------------------------------------

// Standard Wi-Fi QR payload. Every phone camera since iOS 11 and Android
// 10 offers to join from it. ; , : \ and " in the fields must be escaped.
static void wifiPayload(char* out, size_t len, const char* ssid, const char* pass) {
  auto esc = [](const char* in, char* o, size_t cap) {
    size_t n = 0;
    for (; *in && n + 2 < cap; in++) {
      if (strchr(";,:\\\"", *in)) o[n++] = '\\';
      o[n++] = *in;
    }
    o[n] = 0;
  };
  char s[72], p[72];
  esc(ssid, s, sizeof(s));
  esc(pass, p, sizeof(p));
  snprintf(out, len, "WIFI:T:WPA;S:%s;P:%s;;", s, p);
}

// The QR code as a raster, centred, 8 dots per module (1 mm on this
// printer) with the standard four-module quiet zone.
//
// v0.24.1: the code is made BEFORE any part of a slip is queued, in one
// buffer the queue then owns (no second copy). Before, a QR code that did
// not fit in memory failed AFTER the heading had printed, and the retry
// printed the heading again every 30 s - the "welcome to the internet" loop.
struct Qr {
  uint8_t* buf = nullptr; size_t len = 0; uint16_t w = 0, h = 0;
  ~Qr() { free(buf); }
};

static bool makeQr(uint16_t width, const char* text, Qr& out) {
  static uint8_t qr[stqr_BUFFER_LEN_FOR_VERSION(10)];
  static uint8_t tmp[stqr_BUFFER_LEN_FOR_VERSION(10)];
  if (!stqr_encodeText(text, tmp, qr, stqr_Ecc_MEDIUM, 1, 10, stqr_Mask_AUTO, true)) {
    g_why = "the QR code could not be made"; return false;
  }
  const int size = stqr_getSize(qr);
  const uint16_t scale = 8, quiet = 4;
  const uint16_t side = (size + 2 * quiet) * scale;
  if (side > width) { g_why = "the QR code is wider than the paper"; return false; }

  const uint16_t rowBytes = width / 8;
  out.len = (size_t)rowBytes * side;
  out.buf = (uint8_t*)calloc(out.len, 1);
  if (!out.buf) { g_why = "not enough memory for the QR code"; return false; }
  out.w = width; out.h = side;
  const uint16_t x0 = (width - side) / 2 + quiet * scale;
  const uint16_t y0 = quiet * scale;
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++) {
      if (!stqr_getModule(qr, x, y)) continue;
      for (uint16_t dy = 0; dy < scale; dy++) {
        uint8_t* row = out.buf + (size_t)(y0 + y * scale + dy) * rowBytes;
        for (uint16_t dx = 0; dx < scale; dx++) {
          const uint16_t px = x0 + x * scale + dx;
          row[px >> 3] |= 0x80 >> (px & 7);
        }
      }
    }
  return true;
}

// No white tail: it would open a gap between the code and the text under
// it, and a density step in a QR code is harmless.
static bool submitQr(PrintQueue& q, Qr& code) {
  if (!code.buf) return true;                         // printing without it
  uint8_t* b = code.buf; code.buf = nullptr;          // the queue owns it now
  return q.submitRasterOwned(b, code.len, code.w, code.h, SLIP, false, 0, 0, nullptr, 0, false) != 0;
}

bool SlipDesk::printSetup(uint32_t& first, uint32_t& last) {
  char payload[128];
  wifiPayload(payload, sizeof(payload), Unit::apSsid(), Unit::apPass());

  Qr code;
  if (!_noQr && !makeQr(_width, payload, code)) return false;

  first = _q->submitText("PRINTER SETUP\n", SLIP, CENTRE, 2, true, false);
  if (!first) return false;
  if (!_q->submitText(code.buf ? "\n1. Point your phone camera at this\n"
                                 "   code and join the Wi-Fi it offers.\n"
                               : "\n1. Join this Wi-Fi:\n",
                      SLIP, LEFT, 1, false, false)) return false;
  if (!submitQr(*_q, code)) return false;

  String t;
  t += "   Or join it by hand:\n";
  t += "   Wi-Fi     "; t += Unit::apSsid(); t += "\n";
  t += "   Password  "; t += Unit::apPass(); t += "\n\n";
  t += "2. The setup page opens by itself.\n";
  t += "   If not, open http://192.168.4.1\n\n";
  t += "3. Choose your Wi-Fi, enter its\n";
  t += "   password and name this printer.\n";
  t += "   A slip with its address follows.\n";
  last = _q->submitText(t, SLIP, LEFT, 1, false, true);
  return last != 0;
}

// The welcome slip, printed once per address. Name, a QR code to the
// page, both addresses, then how to use it (v0.24.1: rewritten for the
// finished page - no troubleshooting; the page explains any problem).
//
// The QR code carries the IP address, not the .local name: .local does not
// resolve on every phone (Android in particular), and the IP always does.
// If the router hands out a new address, the slip is printed again.
bool SlipDesk::printAddresses(uint32_t& first, uint32_t& last) {
  const String url = "http://" + _ip.toString();
  Qr code;
  if (!_noQr && !makeQr(_width, url.c_str(), code)) return false;

  first = _q->submitText(String(_host) + "\n", SLIP, CENTRE, 2, true, false);
  if (!first) return false;
  if (!_q->submitText("is ready to print\n", SLIP, CENTRE, 1, false, false))
    return false;
  if (!submitQr(*_q, code)) return false;

  String a;
  a += code.len ? "Scan with your camera, or open:\n" : "\nOpen:\n";
  a += "http://"; a += _host; a += ".local\n";
  a += url; a += "\n";
  if (!_q->submitText(a, SLIP, CENTRE, 1, false, false)) return false;

  String t;
  t += "------------------------------------------------\n";
  t += "HOW TO USE\n\n";
  t += "Be on the same Wi-Fi as the printer,\n";
  t += "then open the address above.\n\n";
  t += "Type in the box and tap Print text.\n\n";
  t += "For photos, styles, saved lists and\n";
  t += "more, tap Basic at the top right and\n";
  t += "choose a level.\n\n";
  t += "Moving to new Wi-Fi? Hold BOOT on the\n";
  t += "small board for 5 seconds.\n";
  last = _q->submitText(t, SLIP, LEFT, 1, false, true);
  return last != 0;
}

bool SlipDesk::printFailed(uint32_t& first, uint32_t& last) {
  first = _q->submitText("CAN'T JOIN WI-FI\n", SLIP, CENTRE, 2, true, false);
  if (!first) return false;

  String t;
  t += "\nNetwork  "; t += _ssid; t += "\n";
  t += "Problem  ";   t += _why;  t += "\n\n";
  t += "Check the password on the setup\n";
  t += "page and try again. If the page has\n";
  t += "closed, rejoin Wi-Fi ";
  t += Unit::apSsid(); t += "\n";
  last = _q->submitText(t, SLIP, LEFT, 1, false, true);
  return last != 0;
}

// Approved for online printing. Cheerful, and the only place (with the
// home-network Settings) the owner code is shown.
bool SlipDesk::printOnline(uint32_t& first, uint32_t& last) {
  Qr code;
  if (!_noQr && !makeQr(_width, _oUrl, code)) return false;

  first = _q->submitText("WELCOME TO\nTHE INTERNET!\n", SLIP, CENTRE, 2, true, false);
  if (!first) return false;
  String a;
  a += "\n"; a += _oName; a += " is online.\n";
  a += code.buf ? "Scan to print from anywhere:\n" : "Print to it from anywhere:\n";
  if (!_q->submitText(a, SLIP, CENTRE, 1, false, false)) return false;
  if (!submitQr(*_q, code)) return false;
  String u = _oUrl; u += "\n";
  if (!_q->submitText(u, SLIP, CENTRE, 1, false, false)) return false;

  String admin = _oUrl;
  if (admin.endsWith("/")) admin.remove(admin.length() - 1);
  admin += "-a";
  String t;
  t += "------------------------------------------------\n";
  t += _oSec ? "Anyone with the link can ask to print.\n"
               "You approve each device first.\n"
             : "Anyone with the link can print here.\n";
  t += _oVis ? "Other printers can see this one.\n"
             : "Only people with the link can find it.\n";
  t += "\nOWNER CODE  "; t += _oCode; t += "\n";
  t += "Keep this slip.\n";
  if (_oSec) {
    t += "\nApprove devices at\n"; t += admin; t += "\n";
    t += "with the owner code.\n";
  }
  last = _q->submitText(t, SLIP, LEFT, 1, false, true);
  return last != 0;
}

// Someone asked to print here (extra secure is on).
bool SlipDesk::printKnock(uint32_t& first, uint32_t& last) {
  first = _q->submitText("KNOCK KNOCK!\n", SLIP, CENTRE, 2, true, false);
  if (!first) return false;
  String t;
  t += "\n"; t += _kWho; t += "\nwould like to print to\n"; t += _kName; t += ".\n\n";
  t += "Approve or decline at:\n";
  String admin = _kUrl;
  if (admin.endsWith("/")) admin.remove(admin.length() - 1);
  admin += "-a";
  t += admin; t += "\n";
  last = _q->submitText(t, SLIP, CENTRE, 1, false, true);
  return last != 0;
}

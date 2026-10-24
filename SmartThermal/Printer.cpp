#include "Log.h"
#include "Printer.h"

Printer::Printer(Transport& transport,
                 const PrinterProfile& profile,
                 const PaperProfile& paper)
  : _t(transport), _profile(profile), _paper(&paper), _esc(transport) {}

// --- paper --------------------------------------------------------------
bool Printer::setPaper(const PaperProfile& paper) {
  // Reject anything the printhead cannot physically produce, and anything
  // that would break raster row packing.
  if (paper.printableDots == 0)                                  return false;
  if (paper.printableDots % 8 != 0)                              return false;
  if (paper.offsetDots + paper.printableDots > _profile.widthDots) return false;

  _paper = &paper;
  return true;
}

void Printer::applyPaper() {
  // GS L shifts the print origin; GS W limits the print area. Together they
  // make every alignment, wrap and centre calculation in the firmware
  // operate on the PAPER rather than the printhead — so no layout code
  // above this point needs to know the roll width changed.
  _esc.leftMargin(_paper->offsetDots);
  _esc.printAreaWidth(_paper->printableDots);
}

// --- job lifecycle ------------------------------------------------------
bool Printer::begin() {
  // A latched error from a previous job must not block this one.
  _t.clearError();

  // One socket per job. open() is a no-op if a previous end() could not
  // close cleanly, which is deliberate: reconnecting over a slot this
  // printer still believes is occupied fails with a write timeout, and the
  // stale one is at least known to work.
  _ok = _t.open();
  if (!_ok) return false;

  // Fresh pacing window: idle time between jobs must not bank credit for a
  // burst at the start of this one.
  _t.beginRateWindow();

  _esc.initialise();
  applyPaper();
  _font   = Font::A;
  _scaleW = 1;
  return true;
}

bool Printer::end() {
  bool clean = !_t.failed();

  // Capture throughput before anything resets it. This is the number that
  // says whether the ESP32 kept ahead of the paper.
  _lastWindowBytes = _t.windowBytes();
  _lastWindowMs    = _t.windowMs();

  if (_t.isOpen()) {
    _t.flush();

    // Wait for the socket, do not guess at a delay. writeAll() returning
    // true means the bytes were accepted by the network stack, not that they
    // reached the printer.
    if (!_t.drain(DRAIN_TIMEOUT_MS)) {
      clean = false;
      // Whatever was still in flight when the drain gave up is owed.
      _t.addShortfall(_t.unacked());
    }

    // Closed per job, as originally. Holding it open left the printer's
    // single connection slot occupied across an ESP32 reboot and made every
    // subsequent job fail until the printer was power-cycled.
    _t.close();
  }

  _ok = false;
  return clean;
}

void Printer::disconnect() {
  _t.close();
  _ok = false;
}

// After a mid-raster failure the printer is still counting down pixel bytes
// it never received, and treats everything sent afterwards as those pixels —
// which is the sheared image and the pages of gibberish.
//
// Transport tracks exactly how many bytes never landed. Feeding it that many
// 0x00 bytes satisfies the outstanding command on BLANK paper (a clear bit
// is white in GS v 0), after which ESC @ resets the printer properly.
bool Printer::recoverStream() {
  uint32_t owed = _t.shortfall();

  // Rebuild the link first: the old socket is the one that failed.
  //
  // Open BEFORE clearing the error. clearError() zeroes the shortfall, and
  // the shortfall is the only record of how many pixel bytes the printer is
  // still waiting for. The old order cleared it first, so a reconnect that
  // failed - exactly what a stuck W5500 produces - lost the debt, and the
  // first job after the link came back was eaten as pixels: gibberish.
  // Kept latched, a failed attempt leaves the debt intact for the next one.
  _t.close();
  if (!_t.open()) return false;
  _t.clearError();
  _t.beginRateWindow();

  // Short timeouts here, not the patient ones a print gets.
  const uint32_t patient = _t.writeTimeout();
  _t.setWriteTimeout(RECOVER_TIMEOUT_MS);

  if (owed) {
    // Plus a margin. Paying too much is harmless: once the raster is
    // complete, the extra zero bytes reach the text parser as NULs, which
    // print nothing, and ESC @ follows. Paying too little is gibberish.
    owed += RESYNC_MARGIN;
    Log.printf("[printer] resyncing: %lu pixel bytes owed (incl. %u margin)\n",
                  (unsigned long)owed, (unsigned)RESYNC_MARGIN);

    static const uint8_t WHITE[256] = {0};
    uint32_t left = owed;
    while (left) {
      const size_t n = left < sizeof(WHITE) ? left : sizeof(WHITE);
      if (!_t.writeAll(WHITE, n)) {
        // writeAll recorded only what this chunk left unsent. The chunks
        // never attempted are owed too - keep the whole debt, not a
        // fragment of it, or the next job is eaten as pixels.
        _t.addShortfall(left - n);
        _t.setWriteTimeout(patient);
        _t.close();
        return false;
      }
      left -= n;
    }
  }

  _esc.initialise();
  _t.drain(RECOVER_TIMEOUT_MS);
  _t.setWriteTimeout(patient);

  const bool ok = !_t.failed();
  if (!ok) _t.close();
  return ok;
}

// --- feedback -----------------------------------------------------------
bool Printer::status(uint8_t n, uint8_t& out, uint32_t timeoutMs) {
  if (!_t.isOpen()) return false;
  _t.discardInput();                 // a stale byte shifts every answer after it
  _esc.requestStatus(n);
  if (_t.failed()) return false;
  return _t.readByte(out, timeoutMs);
}

bool Printer::waitPrinted(uint32_t timeoutMs, uint32_t* waitedMs) {
  if (!_t.isOpen()) return false;
  _t.discardInput();
  const uint32_t started = millis();
  _esc.requestPrintedAck();
  if (_t.failed()) return false;

  uint8_t b;
  const bool ok = _t.readByte(b, timeoutMs);
  if (waitedMs) *waitedMs = millis() - started;
  return ok;
}

// --- styling ------------------------------------------------------------
Printer& Printer::reset() {
  _esc.initialise();
  applyPaper();
  _font   = Font::A;
  _scaleW = 1;
  return *this;
}

Printer& Printer::align(Align a)        { _esc.align(a);              return *this; }
Printer& Printer::bold(bool on)         { _esc.bold(on);              return *this; }
Printer& Printer::underline(bool on)    { _esc.underline(on ? 1 : 0); return *this; }
Printer& Printer::invert(bool on)       { _esc.invert(on);            return *this; }

Printer& Printer::font(Font f) {
  _font = f;
  _esc.font(f);
  return *this;
}

Printer& Printer::size(uint8_t scale)   { return size(scale, scale); }

Printer& Printer::size(uint8_t w, uint8_t h) {
  _scaleW = (w < 1) ? 1 : (w > 8 ? 8 : w);
  _esc.textScale(w, h);
  return *this;
}

Printer& Printer::normal() {
  _esc.bold(false);
  _esc.underline(0);
  _esc.invert(false);
  _esc.textScale(1, 1);
  _esc.align(Align::Left);
  _scaleW = 1;
  return *this;
}

// --- content ------------------------------------------------------------
Printer& Printer::text(const String& s) { _esc.write(s.c_str());     return *this; }

Printer& Printer::line(const String& s) { _esc.writeLine(s.c_str()); return *this; }

Printer& Printer::rule(char c) {
  const uint8_t n = charsPerLine();
  String s;
  s.reserve(n);
  for (uint8_t i = 0; i < n; i++) s += c;
  return line(s);
}

Printer& Printer::columns(const String& l, const String& r) {
  const int width = charsPerLine();
  const int used  = l.length() + r.length();

  if (used >= width) {
    // No room to pad — truncate the left field rather than wrap awkwardly.
    const int room = width - r.length() - 1;
    if (room <= 0) return line(r);
    return line(l.substring(0, room) + " " + r);
  }

  String s = l;
  s.reserve(width);
  for (int i = 0; i < width - used; i++) s += ' ';
  s += r;
  return line(s);
}

// --- paper --------------------------------------------------------------
Printer& Printer::feed(uint8_t lines) { _esc.feedLines(lines); return *this; }

Printer& Printer::cut(bool full) {
  if (!_profile.cutter) return *this;
  _esc.cut(full ? CutMode::FeedAndFull : CutMode::FeedAndPartial, _feedBeforeCut);
  return *this;
}

Printer& Printer::cutIfEnabled() {
  if (_autoCut) cut(false);
  return *this;
}

// --- introspection ------------------------------------------------------
uint8_t Printer::charsPerLine() const {
  // Derived, not stored. 576 / 12 = 48 on 80 mm; 416 / 12 = 34 on 57 mm.
  const uint8_t cell = (_font == Font::A)
                     ? _profile.fontAWidthDots
                     : _profile.fontBWidthDots;
  const uint16_t chars = _paper->printableDots / (cell * _scaleW);
  return chars > 255 ? 255 : static_cast<uint8_t>(chars);
}

// Printer.h — Section 6/7. The print engine.
//
// This is the surface that application code (web handlers, queue workers,
// scheduled jobs) is allowed to use. It knows about jobs, layout and the
// printer profile. It does not know a single ESC/POS byte.

#pragma once
#include <Arduino.h>
#include "Transport.h"
#include "EscPos.h"
#include "PrinterProfile.h"

class Printer {
public:
  Printer(Transport& transport,
          const PrinterProfile& profile,
          const PaperProfile& paper = Papers::MM80);

  // --- Job lifecycle ---------------------------------------------------
  // begin() opens the transport and issues ESC @. end() flushes and closes.
  // One connection per job keeps state clean and avoids a half-written
  // stream wedging the printer. Section 18's queue will call these.
  bool begin();
  bool end();                       // false if the stream failed mid-job
  bool ok() const { return _ok; }

  // ONE CONNECTION PER JOB. begin() opens a socket, end() closes it.
  //
  // A persistent socket was tried and reverted. The evidence for it looked
  // strong — six jobs down one socket printed cleanly while six jobs down
  // six sockets froze on the fourth — but this printer has a single
  // connection slot and no way to notice the ESP32 has rebooted, so a held
  // socket left that slot occupied and every job after a reboot failed
  // with a write timeout after ~21 bytes until the printer was
  // power-cycled. The connection-count finding is still unexplained and is
  // worth revisiting only with a way to detect and recover a stale slot.
  //
  // disconnect() drops the socket deliberately - a settings change, say.
  void disconnect();

  // Resynchronise after a failed job. A mid-raster failure leaves the
  // printer waiting for pixel bytes that never arrived, so it swallows the
  // next job as pixels. This sends exactly the number of bytes still owed,
  // as white pixels, then ESC @. Call it on the printer task only.
  bool recoverStream();

  // True if any byte failed to reach the printer during this job. When this
  // is set the printer is out of sync and the job must be treated as failed
  // rather than merely imperfect.
  bool        writeFailed() const { return _t.failed(); }
  const char* writeError()  const { return _t.error(); }
  uint32_t    bytesSent()   const { return _t.txBytes(); }

  // Reachability. Delegated so callers never touch the transport directly.
  bool probe() { return _t.probe(); }
  const char* linkState() { return _t.linkState(); }
  bool resetLink() { return _t.resetLink(); }
  uint16_t lastProbeFailMs() { return _t.lastProbeFailMs(); }
  void setLinkQuiet(bool q) { _t.setQuiet(q); }
  uint32_t stalledMs() const { return _t.stalledMs(); }
  uint32_t forgetDebt() { return _t.clearShortfall(); }
  Transport& link() { return _t; }                  // gap statistics
  void dumpLink()  { _t.dumpLink(); }

  // Discovery. Printer task only.
  bool      linkUp()             { return _t.linkUp(); }
  bool      probeAt(IPAddress ip, uint32_t ms) { return _t.probeAt(ip, _t.port(), ms); }
  IPAddress address() const      { return _t.ip(); }
  void      retarget(IPAddress ip) { _t.setTarget(ip, _t.port()); }

  // --- Feedback from the printer ---------------------------------------
  // Both need an open job (begin() called). Printer task only.

  // One DLE EOT status byte. Real-time, so it answers mid-print and with a
  // full buffer. False on timeout - which during a thermal fault is the
  // answer, since the printer then replies to nothing at all.
  bool status(uint8_t n, uint8_t& out, uint32_t timeoutMs = 500);

  // Every genuine ESC/POS status byte has bits 1 and 4 set and bits 0 and
  // 7 clear. Anything else is a stale or stray byte, not a status.
  static bool statusValid(uint8_t b) { return (b & 0x93) == 0x12; }

  // Sends ESC v and waits for its reply. Queued behind everything already
  // sent, so the reply means the paper has physically finished. waitedMs
  // is how long that took from the call - the basis for detecting a pause
  // (no reply) and banding (a reply that came late).
  bool waitPrinted(uint32_t timeoutMs, uint32_t* waitedMs = nullptr);

  // Waits for one reply WITHOUT sending anything or discarding input - for
  // an ack already requested, where waitPrinted() would throw the awaited
  // reply away before re-asking.
  bool awaitReply(uint32_t timeoutMs) { uint8_t b; return _t.readByte(b, timeoutMs); }

  // Is the connection to the printer still up? Distinct from ok(), which
  // only says a job was opened.
  bool linkOpen() { return _t.isOpen(); }
  void setTrace(bool on) { _t.setTrace(on); }

  // Delivery rate cap in bytes/sec; 0 = unlimited and is the default. The
  // printer bands when fed far too fast AND when fed too slowly, and this
  // ESP32 sits at the slow end already - so this exists for experiments,
  // not for normal operation.
  void     setMaxRate(uint32_t bytesPerSec) { _t.setMaxRate(bytesPerSec); }
  uint32_t maxRate() const { return _t.maxRate(); }

  // Real measured throughput of the job just sent, in bytes per second.
  // If this is below roughly 45 KB/s the printer will outrun the data and
  // the motor will stutter, which shows on paper as light bands.
  uint32_t lastRateBps() const {
    const uint32_t ms = _lastWindowMs;
    return ms ? static_cast<uint32_t>((static_cast<uint64_t>(_lastWindowBytes)
                                       * 1000ULL) / ms) : 0;
  }
  uint32_t lastWindowMs()    const { return _lastWindowMs; }
  uint32_t lastWindowBytes() const { return _lastWindowBytes; }

  // --- Styling (chainable) ---------------------------------------------
  Printer& reset();
  Printer& align(Align a);
  Printer& left()   { return align(Align::Left); }
  Printer& center() { return align(Align::Center); }
  Printer& right()  { return align(Align::Right); }

  Printer& bold(bool on = true);
  Printer& underline(bool on = true);
  Printer& invert(bool on = true);
  Printer& font(Font f);
  Printer& size(uint8_t scale);                 // square, 1..8
  Printer& size(uint8_t w, uint8_t h);
  Printer& normal();                            // clear all styling

  // --- Content ---------------------------------------------------------
  Printer& text(const String& s);               // no trailing newline
  Printer& line(const String& s = "");          // with newline
  Printer& rule(char c = '-');                  // full-width divider
  Printer& columns(const String& l, const String& r);   // "Milk      $4.50"

  // --- Paper -----------------------------------------------------------
  Printer& feed(uint8_t lines = 1);
  Printer& cut(bool full = false);
  Printer& cutIfEnabled();                      // honours autoCut setting

  // --- Paper -----------------------------------------------------------
  // Takes effect on the next begin(). Validated against the printhead, so
  // an impossible paper definition is rejected rather than silently clipped.
  bool setPaper(const PaperProfile& paper);
  const PaperProfile& paper() const { return *_paper; }

  // Usable print area in dots, after the paper's offset is accounted for.
  uint16_t printableDots() const { return _paper->printableDots; }
  uint16_t originDots()    const { return _paper->offsetDots; }

  // --- Settings --------------------------------------------------------
  void setAutoCut(bool on)          { _autoCut = on; }

  // Rows per raster command. 0 means AUTO: the renderer uses the fewest
  // commands the printer's input buffer allows.
  //
  // This matters more than it looks. Measured on paper: every GS v 0
  // boundary leaves a faint light band, because the printer stops the motor
  // between commands. At 32 rows a 125 mm print showed dozens of bands; as
  // a single command, none. Chunking only ever existed to protect the
  // printer's buffer, and the throughput test proved the printer applies
  // proper TCP backpressure instead of overflowing — so small chunks were
  // guarding against a problem that does not exist.
  void setRasterChunkRows(uint16_t rows) {
    if (rows == 0 || (rows >= 8 && rows <= 4096)) _chunkRows = rows;
  }
  uint16_t rasterChunkRows() const { return _chunkRows; }   // 0 = auto

  // Largest single raster command to send, in bytes. Kept below the
  // printer's ~96 KB input buffer with margin.
  static const size_t MAX_COMMAND_BYTES = 64 * 1024;

  // How long end() will wait for the send queue to empty before closing.
  // Generous: a buffer-filling job under backpressure is legitimately slow,
  // and an early close corrupts the job rather than merely delaying it.
  static const uint32_t DRAIN_TIMEOUT_MS   = 600000;  // since last progress; see W5500Transport::drain
  static const uint32_t RECOVER_TIMEOUT_MS = 10000;
  static const uint16_t RESYNC_MARGIN      = 2048;   // one W5500 TX buffer

  // Fewest equal-sized bands that keep each command under the cap.
  uint16_t chunkRowsFor(uint16_t widthDots, uint16_t rows) const {
    if (_chunkRows) return _chunkRows;                 // explicit override
    const uint16_t rowBytes = (widthDots + 7) / 8;
    if (!rowBytes) return rows;
    const uint16_t maxRows = MAX_COMMAND_BYTES / rowBytes;
    if (!maxRows)   return 1;
    if (rows <= maxRows) return rows;                  // one command
    const uint16_t commands = (rows + maxRows - 1) / maxRows;
    return (rows + commands - 1) / commands;           // even split
  }
  void setFeedBeforeCut(uint8_t d)  { _feedBeforeCut = d; }
  bool autoCut() const              { return _autoCut; }

  // --- Introspection ---------------------------------------------------
  uint8_t charsPerLine() const;                 // adjusted for font + scale
  const PrinterProfile& profile() const { return _profile; }
  EscPos& escpos() { return _esc; }              // for raster / raw only

private:
  Transport&            _t;
  const PrinterProfile& _profile;
  const PaperProfile*   _paper;
  EscPos                _esc;

  void applyPaper();        // GS L + GS W, issued once per job

  bool    _ok            = false;
  uint32_t _lastWindowMs    = 0;
  uint32_t _lastWindowBytes = 0;
  Font    _font          = Font::A;
  uint8_t _scaleW        = 1;
  bool     _autoCut      = true;
  uint16_t _chunkRows    = 0;      // 0 = follow the profile
  uint8_t _feedBeforeCut = 0;
};

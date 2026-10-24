// Transport.h — Section 7.
//
// The print engine talks to this interface only. It never knows whether the
// bytes leave over Wi-Fi TCP, a W5500, a UART or USB.
//
// CRITICAL CONTRACT:
// write() is the raw, unreliable primitive — it MAY accept fewer bytes than
// offered. Callers must never use it directly. Use writeAll(), which loops
// until every byte is accepted or the attempt genuinely fails.
//
// This distinction is not academic. An ESC/POS raster command declares its
// own length in its header; the printer then consumes exactly that many
// bytes. Drop even one byte mid-stream and the printer reads the next
// command's header as pixel data and the pixel data as text — which prints
// as pages of random characters. That is precisely the failure this
// interface now prevents.
//
// TWO MEASURED CONSTRAINTS ARE ENFORCED HERE
//
// 1. RATE. There is a WINDOW, not a ceiling. A PC feeding at wire speed
//    (4.9 MB/s) banded; the same PC held to 100 KB/s printed perfectly
//    smooth. But the printer also bands when fed too SLOWLY, because its
//    buffer empties and the motor stops mid-image.
//
//    The ESP32's measured throughput is roughly 100 KB/s at best and
//    44 KB/s under backpressure - at or below the sweet spot already. So
//    rate limiting can only make it worse here and is DISABLED by default.
//    setMaxRate() exists to reproduce the measurements, not for normal use.
//
// 2. SHORTFALL. When a write fails mid-raster the printer is left waiting
//    for pixel bytes that will never arrive, and everything sent afterwards
//    is consumed as those pixels. The count of undelivered bytes is tracked
//    so the stream can be resynchronised deliberately rather than by
//    accident — see Printer::recoverStream().

#pragma once
#include <Arduino.h>
#include <IPAddress.h>

class Transport {
public:
  virtual ~Transport() {}

  virtual bool   open()                                 = 0;
  virtual void   flush()                                = 0;
  virtual void   close()                                = 0;
  virtual bool   isOpen()                               = 0;
  virtual const char* describe()                        = 0;

  // Raw, may be short. Prefer writeAll().
  virtual size_t write(const uint8_t* data, size_t len) = 0;

  // Connect-and-drop reachability check. Overridable because some
  // transports can test more cheaply than opening a real session — and on
  // TCP, opening a second connection is exactly what must NOT happen.
  virtual bool probe() { if (!open()) return false; close(); return true; }

  // Block until every byte handed to the OS has actually left for the
  // printer. Returns false if the timeout expired with data still queued.
  //
  // writeAll() returning true only means the bytes were ACCEPTED by the
  // network stack, not delivered. Any job small enough to fit the ESP32's
  // send buffer (~5.7 KB) never blocks, so without this a close() would
  // tear the socket down with most of the image still queued.
  virtual bool drain(uint32_t timeoutMs) { (void)timeoutMs; return true; }

  // --- Target ------------------------------------------------------------
  // Lifted onto the base so the web API and the sketch can hold a plain
  // Transport& whichever link was built in.
  virtual void      setTarget(IPAddress ip, uint16_t port) { (void)ip; (void)port; }
  virtual IPAddress ip()   const { return IPAddress(0, 0, 0, 0); }
  virtual uint16_t  port() const { return 0; }

  // Short human-readable link state ("up", "no cable"). MUST be a cached
  // value: the web task calls this, and on the W5500 a live query is an
  // SPI transaction that would collide with the printer task's writes.
  virtual const char* linkState() { return "n/a"; }

  // --- Link recovery ------------------------------------------------------
  // Resets the link hardware and brings it back up. Printer task only.
  // Returns false where a transport has nothing to reset (Wi-Fi: the radio
  // is shared with the web server and must not be bounced from here).
  virtual bool resetLink() { return false; }

  // Outage logging help (v0.19). How long the last failed probe took
  // (a quick failure = the printer refused; a full timeout = no answer at
  // all), and a switch that silences routine link-reset lines during a
  // long outage so the log keeps the moment it began.
  virtual uint16_t lastProbeFailMs() { return 0; }
  virtual void     setQuiet(bool q)  { (void)q; }

  // Physical link present. On a point-to-point cable, false means the
  // cable is out OR the printer is switched off - the PHY cannot tell
  // them apart. Printer task only. Links without a PHY report true.
  virtual bool linkUp() { return true; }

  // Tries a connection to an address OTHER than the committed target,
  // without changing it. The basis of printer discovery. Printer task
  // only. Links that cannot search report false.
  virtual bool probeAt(IPAddress ip, uint16_t port, uint32_t timeoutMs) {
    (void)ip; (void)port; (void)timeoutMs; return false;
  }

  // Logs the link's internal connection state - on the W5500, all eight
  // hardware sockets. The evidence for WHY the printer became unreachable,
  // captured before a reset destroys it. Printer task only.
  virtual void dumpLink() {}

  // --- Reading -----------------------------------------------------------
  // Until v0.7.1 the firmware could only SEND to the printer. It learned
  // something had gone wrong only when a write failed, which is why a
  // paused printer was invisible. The printer does talk back - the PC
  // harness proved it - and these are the primitives for hearing it.
  virtual int available() { return 0; }
  virtual int readRaw()   { return -1; }      // one byte, or -1 if none

  // Throws away anything already waiting. Call before EVERY query: a stale
  // reply byte left from an earlier question shifts every answer after it
  // by one, which the harness learned the hard way.
  void discardInput();

  // Waits up to timeoutMs for one byte. False on timeout or a dropped
  // connection. During a thermal fault the printer answers nothing at all,
  // so a timeout is itself information, not merely a failure.
  bool readByte(uint8_t& out, uint32_t timeoutMs);

  // --- The reliable write path ------------------------------------------
  // Returns false only on genuine failure (timeout or dropped connection),
  // and latches an error so the rest of the job is abandoned rather than
  // sent into a printer that is now out of sync.
  bool writeAll(const uint8_t* data, size_t len);
  bool writeAll(const char* s) {
    return writeAll(reinterpret_cast<const uint8_t*>(s), s ? strlen(s) : 0);
  }
  bool writeByte(uint8_t b) { return writeAll(&b, 1); }

  // Starts a fresh pacing window. Called at the top of every job so one
  // job's idle time cannot bank credit for the next one's burst.
  void beginRateWindow() { _rateEpochMs = millis(); _rateBytes = 0; }

  // 0 = unlimited (the default, and correct for this hardware). Any other
  // value caps delivery to that many bytes per second.
  void     setMaxRate(uint32_t bytesPerSec) { _maxRate = bytesPerSec; }
  uint32_t maxRate() const { return _maxRate; }

  // Bytes actually delivered during the current rate window, and how long
  // that took. Divide one by the other for real throughput - the only way
  // to know whether the ESP32 is keeping ahead of the paper.
  uint32_t windowBytes() const { return _rateBytes; }
  uint32_t windowMs()    const {
    return _rateEpochMs ? (millis() - _rateEpochMs) : 0;
  }

  // --- Error state -------------------------------------------------------
  bool        failed() const { return _failed; }
  const char* error()  const { return _error; }
  uint32_t    txBytes() const { return _txBytes; }

  // Bytes the caller asked to send that never reached the printer. After a
  // mid-raster failure this is how many pixel bytes the printer is still
  // waiting for.
  uint32_t    shortfall() const { return _shortfall; }

  // Adds bytes still owed that were never even attempted. See
  // Printer::recoverStream().
  void addShortfall(uint32_t n) { _shortfall += n; }

  // Voids the debt and returns what it was. Only for when the printer has
  // restarted (it then owes nothing), or when the failed job was not an
  // image (text bytes lost leave nothing pending in the printer).
  uint32_t clearShortfall() { const uint32_t n = _shortfall; _shortfall = 0; return n; }

  // Bytes the link accepted but the printer had not yet acknowledged, as
  // last seen. When the connection drops these are lost in flight: counted
  // as sent, never received. They are owed like any other unsent byte -
  // missing them left the printer up to 2 KB short after a payback, so it
  // ate the next job's raster header as pixels and printed the rest of the
  // photo as text. Links that cannot tell report 0.
  virtual uint32_t unacked() { return 0; }

  // How long a write may make no progress before it fails. Patient by
  // default (see WRITE_TIMEOUT_MS); recovery sets it short, because a
  // printer that will not take its owed bytes back is not paused, it is
  // gone, and the link resets need to get on with their work.
  void     setWriteTimeout(uint32_t ms) { _writeTimeoutMs = ms; }
  uint32_t writeTimeout() const { return _writeTimeoutMs; }

  void clearError() {
    _failed    = false;
    _error[0]  = 0;
    _txBytes   = 0;
    _shortfall = 0;
    _rateEpochMs = 0;
  }

  // --- Delivery gaps (banding evidence) -------------------------------------
  // The longest wait between two bytes moving, since resetGaps(). If the
  // printer runs out of data mid-image its motor stops, and that is a band.
  // With these figures a band on paper can be matched - or not - to a
  // delivery gap at the same point.
  void     resetGaps() { _gapMax = _gapAt = _gapOver = _gapBytes = 0; _gapLast = 0; }
  uint32_t gapMaxMs()  const { return _gapMax; }
  uint32_t gapAtByte() const { return _gapAt; }     // bytes sent before it
  uint32_t gapsOver()  const { return _gapOver; }   // gaps of GAP_REPORT_MS+
  static const uint32_t GAP_REPORT_MS = 100;

  // --- Stall reporting -----------------------------------------------------
  // How long the transfer in progress has made no progress, 0 when nothing
  // is waiting. Safe to read from any task (a racy read of a counter only
  // ever costs one poll's accuracy). This is how the web page learns that
  // a print has paused - paper out, cover open, or the head cooling - while
  // the printer task is held inside the write.
  uint32_t stalledMs() const {
    const uint32_t since = _stallSince;
    return since ? millis() - since : 0;
  }

  // For subclasses whose drain() waits on the printer: call with true when
  // a wait makes no progress, false when it moves.
  void noteStall(bool stalled) {
    if (!stalled)          _stallSince = 0;
    else if (!_stallSince) _stallSince = millis() ? millis() : 1;
  }

  // --- Diagnostics -------------------------------------------------------
  // With tracing on, every writeAll is summarised to Serial with a short
  // hex prefix. Enough to see command sequences without dumping 72 kB of
  // pixels.
  void setTrace(bool on) { _trace = on; }
  bool trace() const     { return _trace; }

protected:
  void fail(const char* why);

  // How long a write may make NO progress before the job is abandoned.
  // Raised from 8 s: the printer's own thermal protection pauses it for
  // 30-35 s mid-print, then resumes and finishes cleanly (seen four times
  // in the 80% soak). An 8 s limit would turn that harmless delay into a
  // failed job and a resync. A genuinely dead link still fails fast - the
  // socket closes and writeAll() sees "connection dropped" at once.
  //
  // v0.9.3: raised to 10 minutes. A paper-out or an open cover mid-photo
  // stops the printer reading until someone fixes it, and holding the
  // connection is proven to let the print finish by itself afterwards
  // (every soft pause in the 80% soak completed with it held). Dropping
  // the connection mid-image is what wedges the printer. The socket
  // staying alive is the condition: a hard fault closes it and still
  // fails at once.
  static const uint32_t WRITE_TIMEOUT_MS = 600000;
  static const size_t   TRACE_PREFIX     = 16;

  volatile uint32_t _stallSince = 0;   // millis() the current stall began
  uint32_t _gapMax = 0, _gapAt = 0, _gapOver = 0, _gapBytes = 0, _gapLast = 0;
  uint32_t _writeTimeoutMs = WRITE_TIMEOUT_MS;

  // Write size. One ethernet segment: small enough that pacing (when
  // enabled) has somewhere to happen, large enough not to fragment a 72 KB
  // image into hundreds of tiny TCP segments, which costs real throughput
  // on Wi-Fi with NoDelay set.
  static const size_t   SLICE_BYTES = 1460;

  // Never sleep longer than this in one go, so a stalled job still notices
  // a dropped connection promptly.
  static const uint32_t MAX_PACE_DELAY_MS = 50;

  bool     _failed  = false;
  bool     _trace   = false;
  char     _error[40] = {0};
  uint32_t _txBytes = 0;
  uint32_t _shortfall = 0;

  uint32_t _rateEpochMs = 0;
  uint32_t _rateBytes   = 0;
  uint32_t _maxRate     = 0;      // 0 = unlimited
};

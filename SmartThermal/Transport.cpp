#include "Log.h"
#include "Transport.h"

void Transport::fail(const char* why) {
  _failed = true;
  strlcpy(_error, why ? why : "transport error", sizeof(_error));
  Log.printf("[tx] FAILED: %s (after %lu bytes)\n",
                _error, (unsigned long)_txBytes);
}

bool Transport::writeAll(const uint8_t* data, size_t len) {
  // Once a write has failed the stream is desynchronised. Sending more
  // would print garbage, so every later call is refused until clearError().
  //
  // Refused bytes still count as shortfall: the printer declared a length
  // in its raster header and is waiting for every one of them, including
  // the ones we never got to attempt.
  if (_failed) { _shortfall += len; return false; }
  if (!data || len == 0) return true;

  if (_trace) {
    char hex[TRACE_PREFIX * 3 + 1] = {0};
    const size_t show = len < TRACE_PREFIX ? len : TRACE_PREFIX;
    for (size_t i = 0; i < show; i++) sprintf(hex + i * 3, "%02X ", data[i]);
    Log.printf("[tx] %u bytes: %s%s\n",
                  (unsigned)len, hex, len > show ? "..." : "");
  }

  if (_rateEpochMs == 0) beginRateWindow();

  size_t   sent  = 0;
  uint32_t moved = millis();

  while (sent < len) {
    if (!isOpen()) {
      noteStall(false);
      fail("connection dropped");
      _shortfall += (len - sent) + unacked();
      return false;
    }

    // Hand the whole remaining buffer to the transport. Capping each write
    // at SLICE_BYTES turns one 72 KB image into ~50 separate TCP segments,
    // and with NoDelay set that roughly HALVES throughput on this hardware:
    // measured 100 KB/s unsliced against 36-41 KB/s sliced. The printer
    // consumes about 45 KB/s while printing, so the sliced figure starved
    // its buffer and stalled the motor mid-image.
    //
    // Slicing only happens when rate limiting is switched on, because that
    // is the only time pacing needs somewhere to act.
    const size_t remaining = len - sent;
    const size_t want = _maxRate
                      ? (remaining < SLICE_BYTES ? remaining : SLICE_BYTES)
                      : remaining;
    const size_t n    = write(data + sent, want);

    if (n > 0) {
      sent  += n;
      moved  = millis();
      noteStall(false);
      if (_gapLast) {
        const uint32_t g = moved - _gapLast;
        if (g > _gapMax) { _gapMax = g; _gapAt = _gapBytes; }
        if (g >= GAP_REPORT_MS) _gapOver++;
      }
      _gapLast   = moved;
      _gapBytes += n;
      _rateBytes += n;

      // Pacing is OFF unless someone asked for it. On this hardware the
      // ESP32 is already slower than the printer's sweet spot, so a cap can
      // only starve the printer's buffer and stall the motor mid-image.
      if (_maxRate) {
        const uint32_t dueMs =
          static_cast<uint32_t>((static_cast<uint64_t>(_rateBytes) * 1000ULL)
                                / _maxRate);
        const uint32_t elapsed = millis() - _rateEpochMs;
        if (dueMs > elapsed) {
          uint32_t wait = dueMs - elapsed;
          if (wait > MAX_PACE_DELAY_MS) wait = MAX_PACE_DELAY_MS;
          delay(wait);
        }
      }
    } else {
      // A zero-length write means the socket buffer is full, which is
      // normal under backpressure — the printer is simply slower than the
      // network. Only give up if nothing moves at all for the timeout.
      if (millis() - moved > _writeTimeoutMs) {
        noteStall(false);
        fail("write timeout");
        _shortfall += (len - sent) + unacked();   // closed next: lost too
        return false;
      }
      // Buffer-full waits of a few ms are normal backpressure, not a stall.
      if (millis() - moved > 1000) noteStall(true);
      delay(2);
    }
  }

  noteStall(false);
  _txBytes += len;
  return true;
}

// ----------------------------------------------------------------------
// Reading
// ----------------------------------------------------------------------

void Transport::discardInput() {
  // Bounded, so a printer streaming something unexpected cannot hold the
  // printer task here forever.
  for (uint16_t n = 0; n < 512 && available() > 0; n++) readRaw();
}

bool Transport::readByte(uint8_t& out, uint32_t timeoutMs) {
  const uint32_t start = millis();
  for (;;) {
    // Checked before isOpen(): a socket the printer has just closed can
    // still hold the reply it sent on the way out.
    if (available() > 0) {
      const int b = readRaw();
      if (b >= 0) { out = static_cast<uint8_t>(b); return true; }
    }
    if (!isOpen())                     return false;
    if (millis() - start >= timeoutMs) return false;
    delay(2);
  }
}

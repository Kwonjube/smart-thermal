// Raster.h — Section 11.
//
// Turns a MonoBitmap into a sequence of GS v 0 commands, split into bands
// so the printer's ~96 KB input buffer never overflows.
//
// This is the layer the browser upload path (Phase 6+) will feed into. It
// deliberately accepts either a whole bitmap or one band at a time, because
// streaming is how images will actually arrive over HTTP.

#pragma once
#include <Arduino.h>
#include "Printer.h"
#include "MonoBitmap.h"

class Raster {
public:
  explicit Raster(Printer& printer);

  // Print a whole bitmap, chunked automatically. Assumes a job is already
  // open (printer.begin() called).
  bool print(const MonoBitmap& bmp,
             RasterMode mode = RasterMode::Normal,
             uint16_t chunkRows = 0);          // 0 = use profile default

  // Print one pre-built band. Use when rows are generated or received
  // incrementally and never all held in RAM at once.
  bool printBand(const uint8_t* data,
                 uint16_t widthDots,
                 uint16_t rows,
                 RasterMode mode = RasterMode::Normal);

  // A tall flat field built by repeating a small band, sent as the SAME
  // number of GS v 0 commands a real image that tall would use. The header
  // declares the full chunk height, then the band is streamed into it as
  // many times as needed. Lets test patterns of any length come from one
  // ~9 KB buffer without adding command boundaries - each of which stops
  // the motor and leaves a faint line of its own.
  //
  // The band height must divide evenly into the pattern's period (8 rows
  // for the Bayer fields) or the pattern phase shifts at each repeat.
  bool printTiled(const MonoBitmap& band, uint16_t totalRows);

  // How many commands printTiled() will use for this height. Each is one
  // expected faint line on the paper, so tests report it up front.
  uint16_t tiledCommands(const MonoBitmap& band, uint16_t totalRows) const;

  // Diagnostics from the last print() call.
  uint16_t lastChunkCount() const { return _chunks; }
  uint32_t lastByteCount()  const { return _bytes; }
  uint32_t lastDurationMs() const { return _durationMs; }

  // Pacing between bands. Raise this if tall images tear or drop rows —
  // it gives the printer time to clear its buffer.
  void setInterChunkDelayMs(uint16_t ms) { _delayMs = ms; }

private:
  Printer& _p;
  uint16_t _delayMs   = 0;
  uint16_t _chunks    = 0;
  uint32_t _bytes     = 0;
  uint32_t _durationMs = 0;

  uint16_t tiledChunk(const MonoBitmap& band, uint16_t totalRows) const;
};

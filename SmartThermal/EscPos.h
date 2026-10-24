// EscPos.h — Section 6/7.
//
// THIS IS THE ONLY FILE IN THE PROJECT THAT CONTAINS RAW ESC/POS BYTES.
// If you find yourself writing 0x1B anywhere else, add a method here instead.

#pragma once
#include <Arduino.h>
#include "Transport.h"

enum class Align   : uint8_t { Left = 0, Center = 1, Right = 2 };
enum class Font    : uint8_t { A = 0, B = 1 };
enum class Justify : uint8_t { Left = 0, Center = 1, Right = 2 };

// GS V function codes. 0x42 (feed-then-partial-cut) is the variant already
// proven working on this printer. 0x41 is the full-cut equivalent — untested
// here, so treat it as experimental until a real receipt confirms it.
enum class CutMode : uint8_t { FeedAndPartial = 0x42, FeedAndFull = 0x41 };

// GS v 0 scaling modes. Normal is 1:1 dots. The doubling modes are done in
// printer firmware, so they cost no extra network bytes — useful for small
// logos, useless for photographs.
enum class RasterMode : uint8_t {
  Normal        = 0,
  DoubleWidth   = 1,
  DoubleHeight  = 2,
  Quadruple     = 3
};

class EscPos {
public:
  explicit EscPos(Transport& t) : _t(t) {}

  // --- Session ---------------------------------------------------------
  void initialise();                      // ESC @  — clears all modes

  // --- Layout ----------------------------------------------------------
  void align(Align a);                    // ESC a n
  void lineSpacing(uint8_t dots);         // ESC 3 n
  void defaultLineSpacing();              // ESC 2
  void leftMargin(uint16_t dots);         // GS L nL nH
  void printAreaWidth(uint16_t dots);     // GS W nL nH

  // --- Character styling ----------------------------------------------
  void font(Font f);                      // ESC M n
  void bold(bool on);                     // ESC E n
  void underline(uint8_t weight);         // ESC - n   (0 off, 1 thin, 2 thick)
  void invert(bool on);                   // GS B n    (white on black)
  void upsideDown(bool on);               // ESC { n
  void textScale(uint8_t w, uint8_t h);   // GS ! n    (1..8 each)
  void codePage(uint8_t page);            // ESC t n

  // --- Content ---------------------------------------------------------
  void write(const char* s);
  void writeLine(const char* s);
  void newline();

  // --- Paper -----------------------------------------------------------
  void feedLines(uint8_t lines);          // ESC d n
  void feedDots(uint8_t dots);            // ESC J n
  void cut(CutMode mode, uint8_t feedDots);  // GS V m n

  // --- Raster graphics --------------------------------------------------
  // GS v 0 m xL xH yL yH <data>
  // data is packed 1-bit-per-pixel, MSB first, left to right, top to bottom.
  // A set bit prints black. widthBytes must equal (widthDots + 7) / 8.
  // A raster command of white rows, streamed as zeros: no buffer needed.
  void rasterBlank(uint16_t widthBytes, uint16_t rows);

  void rasterBitImage(RasterMode mode,
                      uint16_t widthBytes,
                      uint16_t rows,
                      const uint8_t* data,
                      size_t len);

  // --- Queries ---------------------------------------------------------
  // DLE EOT n, n = 1..4. REAL-TIME: the printer answers on receipt, ahead
  // of anything queued, even with its input buffer full. That is what makes
  // it usable while a print is stalled. One byte comes back.
  //   1 printer status   2 offline cause   3 error cause   4 paper sensor
  void requestStatus(uint8_t n);

  // ESC v. On this printer it is QUEUED, not real-time: the reply arrives
  // only once everything sent before it has printed. Measured by the PC
  // harness, which is how banding was detected by timing - a motor stall
  // of tens of ms stands out against a baseline sd under 1 ms. The reply
  // byte itself is not used; its arrival time is the measurement.
  void requestPrintedAck();

  // --- Density candidates (diagnostic only) ----------------------------
  // Three different commands that set print density on different printer
  // families. Which, if any, this printer honours is unknown - that is
  // what the density probe finds out. An unrecognised one typically prints
  // a stray character, which is itself the answer.
  void densityEpson(int8_t level);     // GS ( K fn49: -6..+6, 0 = standard
  void densityDc2(uint8_t n);          // DC2 # n: bits 0-4 = 50% + 5% x n
  void densityGsBar(uint8_t n);        // GS | n

  // --- Escape hatch ----------------------------------------------------
  // For the /api/print/raw endpoint (Section 17) and raster work (Phase 4+).
  void raw(const uint8_t* data, size_t len);

private:
  Transport& _t;

  void emit(uint8_t a);
  void emit(uint8_t a, uint8_t b);
  void emit(uint8_t a, uint8_t b, uint8_t c);
  void emit(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

  static uint8_t clampScale(uint8_t v);
};

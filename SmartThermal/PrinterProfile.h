// PrinterProfile.h — Section 8.
//
// Two separate concepts, deliberately not merged:
//
//   PrinterProfile  describes the MECHANISM: printhead width, font cell
//                   sizes, DPI, capabilities. Fixed for a given printer.
//
//   PaperProfile    describes the PAPER currently loaded: how wide it is,
//                   how much of the printhead it covers, and where it sits.
//                   Changes whenever the roll is swapped.
//
// Keeping them apart is what lets 57 mm paper be a runtime setting rather
// than a recompile, and stops narrow paper from being confused with a
// narrower printer.

#pragma once
#include <Arduino.h>

struct PrinterProfile {
  const char* name;

  uint16_t widthDots;          // physical printhead width
  uint8_t  printableWidthMM;
  uint16_t dpi;

  uint16_t tcpPort;
  uint16_t rasterChunkRows;

  // Paper speed x10, in mm/s. A MEASURED mechanism constant, not a guess:
  // 128 mm of paper took 1369-1372 ms with a standard deviation under 1 ms
  // across 40 prints, which is 93.5 mm/s. Print time is therefore known in
  // advance from the row count alone, and that is what the thermal pacing
  // in PrintQueue is built on.
  uint16_t paperSpeedMmPerSec10;

  uint8_t  fontAWidthDots;     // 12x24 cell
  uint8_t  fontBWidthDots;     // 9x17 cell

  bool cutter;
  bool qr;
  bool raster;
};

struct PaperProfile {
  const char* name;

  uint8_t  widthMM;            // nominal roll width
  uint16_t printableDots;      // dots usable across that roll
  uint16_t offsetDots;         // left margin from the printhead's dot 0

  // printableDots must be a multiple of 8 so raster rows pack cleanly,
  // and offsetDots + printableDots must not exceed the printhead width.
};

namespace Profiles {

// Confirmed on hardware (80-VI-UL, fw V1.3): 576 dots/line, port 9100,
// cutter enabled, all four GS v 0 raster modes honoured, TCP backpressure
// working correctly at 128-row bands.
static const PrinterProfile POS8370 = {
  /* name             */ "POS-8370 / 80-VI-UL",
  /* widthDots        */ 576,
  /* printableWidthMM */ 72,
  /* dpi              */ 203,
  /* tcpPort          */ 9100,
  /* rasterChunkRows  */ 128,
  /* paperSpeed x10   */ 935,          // 93.5 mm/s, measured
  /* fontAWidthDots   */ 12,
  /* fontBWidthDots   */ 9,
  /* cutter           */ true,
  /* qr               */ true,
  /* raster           */ true
};

}  // namespace Profiles

namespace Papers {

// 80 mm: the documented roll. Full printhead, no offset. Validated.
static const PaperProfile MM80 = { "80 mm", 80, 576, 0 };

// 57 mm, left-aligned against the mechanism's fixed wall.
//   57 mm x 8 dots/mm = 456 dots of paper
//   less ~2.5 mm total edge margin      = 416 dots usable (52 mm)
// 416 is a multiple of 8 and gives 34 chars/line in Font A.
// UNVALIDATED — offsetDots in particular must be measured once a roll is
// actually loaded, because it depends on how the mechanism holds it.
static const PaperProfile MM57_LEFT   = { "57 mm (left)",   57, 416, 0 };

// 57 mm, centred under the printhead. Use if the roll sits mid-mechanism.
//   (576 - 456) / 2 = 60 dots of dead printhead each side
static const PaperProfile MM57_CENTRE = { "57 mm (centre)", 57, 416, 64 };

// Indexed table, so a paper choice can be stored in NVS and sent over the
// API as a small integer rather than a string that has to be parsed.
static const PaperProfile* const TABLE[] = { &MM80, &MM57_LEFT, &MM57_CENTRE };
static const uint8_t COUNT = sizeof(TABLE) / sizeof(TABLE[0]);

inline const PaperProfile& byIndex(uint8_t i) {
  return *TABLE[i < COUNT ? i : 0];
}

}  // namespace Papers

// BandTest.h - one short print that tells the banding causes apart.
//
// Round 2 (v0.9.8). Round 1 showed: no lines at the command joins, solid
// even black bars (line power is fine), and one clear step to lighter at
// ~46-47 mm in BOTH grey panels - the same place with and without joins.
// Something inside the printer changes about 0.5 s / ~27 KB into an image.
// These two panels tell time from data:
//
//   A  full width (576 dots), 128 mm, 30% grey, ONE command
//   B  HALF width (288 dots), 128 mm, 30% grey, ONE command
//
// Reading it:
//   step at ~47 mm in BOTH          -> it is TIME (~0.5 s into printing):
//                                      a speed or heat-compensation change
//                                      inside the printer
//   step at ~47 mm in A, ~94 in B   -> it is DATA (~27 KB): the printer's
//                                      own buffer handling
//   steps repeating down A          -> a cycle; the spacing names it
//
// Loop task only. Builds one panel at a time; the buffer is handed to the
// queue, which owns it from then on.

#pragma once
#include <Arduino.h>

namespace BandTest {
  static const uint16_t PANEL_ROWS = 1024;         // 128 mm
  static const uint8_t  PANELS     = 2;

  // Printed width of each panel, in dots.
  uint16_t width(uint8_t panel);

  // malloc'd 1-bit raster for the panel, or nullptr. len set on success.
  uint8_t* build(uint8_t panel, size_t& len);

  // A ruled field of evenly spread dots at level/64 coverage (Bayer 8x8),
  // any width and height. 0 = white, 64 = solid black.
  uint8_t* field(uint16_t widthDots, uint16_t rows, uint8_t level64, size_t& len);

  // The label printed above each panel, and its rows per command.
  const char* label(uint8_t panel);
  uint16_t    chunkRows(uint8_t panel);
}

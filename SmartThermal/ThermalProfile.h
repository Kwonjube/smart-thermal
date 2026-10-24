// ThermalProfile.h — what the trainer measured, kept for the lend-out build.
//
// Written by the TRAINER build, read by BOTH. It lives in its own NVS
// namespace, separate from Settings, so flashing the lend-out build over
// the trainer keeps it - provided "Erase All Flash Before Sketch Upload"
// stays disabled in the Arduino IDE (the default).
//
// WHY A TABLE RATHER THAN ONE NUMBER
// The v0.7.3 soaks showed the safe ceiling depends on coverage: 50% ran
// clean at coverage x duty = 1000, but 80% paused four times in 40 prints
// at 960. A dense print heats the head harder DURING the print, so it must
// start from a cooler head - a longer rest than the average-heat formula
// gives. So the trainer measures the ceiling at several coverages and the
// lend-out build interpolates between them.
//
// "limit" keeps exactly the meaning the pacing formula already uses:
//
//   gap_ms = basis_ms x (coverage x 100 / limit - 1)
//   basis_ms = rows / 748 x 1000
//
// 748 rows/s is NOT the paper speed - the paper really moves nearer 160
// mm/s. It is the basis the original measurements were expressed in (a
// whole 128 mm slip, including feed and cut, took ~1370 ms). Keeping it
// means every limit ever recorded, old and new, is directly comparable.
// The real speed is measured separately, below, for motion estimates.

#pragma once
#include <Arduino.h>

struct ThermalProfile {
  static const uint8_t VERSION = 1;
  static const uint8_t POINTS  = 5;

  // Basis for the limit formula; see above. Not the paper speed.
  static const uint16_t BASIS_ROWS_PER_SEC = 748;

  uint8_t  version;
  uint8_t  valid;              // 1 once a full training run completed
  uint16_t rowsPerSec;         // MEASURED image speed, dot rows per second
  uint16_t overheadMs;         // MEASURED label + feed + cut, per slip
  uint8_t  coverage[POINTS];   // realised coverage of each point, ascending
  uint16_t limit[POINTS];      // safe coverage% x duty% there; 0 = none
  uint32_t trainedEpoch;       // unix seconds, 0 if the clock was not set
  char     firmware[12];

  void reset();

  // Safe limit at any coverage: linear between trained points, held flat
  // beyond the ends. 0 if the profile holds no usable point.
  uint16_t limitFor(uint8_t coveragePct) const;

  // Rest after a print, from a limit, in the formula's own terms.
  static uint32_t gapMs(uint8_t coveragePct, uint16_t limit, uint32_t rows);

  // Human-readable table to Serial: limit, rest and throughput per point.
  void print() const;
};

namespace ThermalProfileStore {
  bool load(ThermalProfile& out);   // false if absent or a different version
  bool save(const ThermalProfile& p);
  void clear();
}

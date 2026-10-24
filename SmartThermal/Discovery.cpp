#include "Discovery.h"

// Factory defaults, most likely first. Extend here and nowhere else.
//
//   192.168.1.100    this printer (POS-8370) and many generic 80 mm units
//   192.168.11.100   this printer as currently configured
//   192.168.123.100  Xprinter
//   192.168.0.100    generic, 0-subnet variant
//   192.168.1.87     Rongta
//   192.168.192.168  Epson TM and Star
//   192.168.1.200    some generic units (the W5500 steps to .201)
static const uint8_t CANDIDATES[][4] = {
  { 192, 168,   1, 100 },
  { 192, 168,  11, 100 },
  { 192, 168, 123, 100 },
  { 192, 168,   0, 100 },
  { 192, 168,   1,  87 },
  { 192, 168, 192, 168 },
  { 192, 168,   1, 200 },
};

static const uint8_t COUNT = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);

uint8_t Discovery::candidates() { return COUNT; }

bool Discovery::find(Printer& printer, IPAddress saved, IPAddress& found) {
  if (!(saved == IPAddress(0, 0, 0, 0)) &&
      printer.probeAt(saved, GUESS_TIMEOUT_MS)) {
    found = saved;
    return true;
  }

  for (uint8_t i = 0; i < COUNT; i++) {
    const IPAddress ip(CANDIDATES[i][0], CANDIDATES[i][1],
                       CANDIDATES[i][2], CANDIDATES[i][3]);
    if (ip == saved) continue;               // already tried
    if (!printer.linkUp()) return false;     // cable pulled mid-search
    if (printer.probeAt(ip, GUESS_TIMEOUT_MS)) {
      found = ip;
      return true;
    }
  }
  return false;
}

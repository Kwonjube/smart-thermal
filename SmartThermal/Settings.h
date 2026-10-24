// Settings.h — Section 22.
//
// Persistent configuration in ESP32 NVS. Config.h supplies bootstrap
// defaults only; once a value has been saved here, NVS wins. That means
// the printer IP and paper choice survive a reflash, which matters as soon
// as the web UI can change them.

#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// Canonical dither ordering, defined once. The API sends and accepts these
// NAMES rather than the index, so firmware and browser can no longer drift
// apart — which is exactly what went wrong when the ordering changed and a
// stored "1" silently became Floyd-Steinberg instead of Atkinson.
enum class DitherMode : uint8_t {
  Atkinson       = 0,   // default: clusters dots, best on this printhead
  FloydSteinberg = 1,
  Ordered        = 2,
  Threshold      = 3
};

inline const char* ditherName(uint8_t i) {
  switch (i) {
    case 0: return "atkinson";
    case 1: return "floyd";
    case 2: return "ordered";
    case 3: return "threshold";
  }
  return "atkinson";
}

inline uint8_t ditherIndex(const char* s) {
  if (!s) return 0;
  if (!strcasecmp(s, "floyd"))     return 1;
  if (!strcasecmp(s, "ordered"))   return 2;
  if (!strcasecmp(s, "threshold")) return 3;
  return 0;
}

struct Settings {
  char     wifiSsid[33];
  char     wifiPass[65];
  char     hostname[33];        // mDNS name, without ".local"

  uint8_t  printerIp[4];
  uint16_t printerPort;

  uint8_t  paperIndex;          // index into Papers::TABLE
  bool     autoCut;
  uint8_t  feedBeforeCut;       // dots fed before the blade

  // Rows per GS v 0 command, or 0 for auto (fewest commands that fit the
  // printer's input buffer). Auto is correct for normal use; the explicit
  // values exist only for reproducing the banding measurements.
  uint16_t chunkRows;

  // Thermal ceiling: the largest coverage% x duty% the printer tolerates
  // without faulting. 1000 is the measured value at Dark density — 39.1% at
  // 31% duty (1227) faulted only above it, 53.1% at 19.6% duty (1042) did
  // not, and everything at or under 1000 ran 40 prints clean.
  //
  // A SETTING rather than a profile constant because it moves with the
  // printer's density, which is configured in the printer itself with the
  // vendor utility. Dropping density from Dark to Normal puts in less
  // energy per dot and should raise this number; that is a measurement to
  // be made on paper, not a value to guess in firmware.
  //
  // 0 disables pacing entirely. Useful only for deliberately reproducing
  // the fault — a sustained run will fault within about twenty prints.
  uint16_t thermalLimit;

  // Debug mode: hex-traces every write to Serial. Nothing is printed on the
  // paper. An earlier version stamped the job id on each receipt, but that
  // added a line between the artwork and the cut, shifting the cut position
  // and spoiling the very prints being measured.
  bool     debugMode;

  uint8_t  defaultDither;       // DitherMode
  uint8_t  defaultThreshold;    // 0-255
  uint8_t  defaultGamma;        // gamma x100. 60 = 0.60
  int8_t   defaultBrightness;   // -100..100
  int8_t   defaultContrast;     // -100..100

  IPAddress printerAddress() const {
    return IPAddress(printerIp[0], printerIp[1], printerIp[2], printerIp[3]);
  }
};

class SettingsStore {
public:
  // Loads from NVS, falling back to Config.h defaults for any missing key.
  bool begin();

  Settings&       get()       { return _s; }
  const Settings& get() const { return _s; }

  bool save();                 // writes every field
  void factoryReset();         // clears NVS and reloads defaults

  // True if the last save() changed something that only takes effect on
  // reboot (currently Wi-Fi credentials and hostname).
  bool rebootRequired() const { return _rebootRequired; }

private:
  Settings _s;
  bool     _rebootRequired = false;

  void loadDefaults();
};

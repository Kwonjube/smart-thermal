#include "Settings.h"
#include "Config.h"
#include <Preferences.h>

static const char* NS = "thermal";

void SettingsStore::loadDefaults() {
  memset(&_s, 0, sizeof(_s));

  strlcpy(_s.wifiSsid, WIFI_SSID,     sizeof(_s.wifiSsid));
  strlcpy(_s.wifiPass, WIFI_PASSWORD, sizeof(_s.wifiPass));
  strlcpy(_s.hostname, "receipt",     sizeof(_s.hostname));

  _s.printerIp[0] = PRINTER_IP_A;
  _s.printerIp[1] = PRINTER_IP_B;
  _s.printerIp[2] = PRINTER_IP_C;
  _s.printerIp[3] = PRINTER_IP_D;
  _s.printerPort  = PRINTER_PORT;

  _s.paperIndex    = 0;      // 80 mm, the validated roll
  // 0 = auto: fewest commands that fit the printer's buffer. Measured as
  // the only setting that avoids banding at every chunk boundary.
  _s.chunkRows     = 0;
  // Measured ceiling at Dark density. See Settings.h.
  _s.thermalLimit  = 1000;
  _s.debugMode     = false;
  _s.autoCut       = true;
  _s.feedBeforeCut = 0;

  _s.defaultDither     = static_cast<uint8_t>(DitherMode::Atkinson);
  _s.defaultThreshold  = 128;
  // 0.60, measured on paper. This printer's density is set to Dark, so dots
  // bleed past their cell and midtones fill in; gamma below 1 compensates.
  _s.defaultGamma      = 60;
  _s.defaultBrightness = 0;
  _s.defaultContrast   = 0;
}

// Bumped whenever the MEANING of a stored value changes rather than just
// its default, because a stored value always beats a new compiled default
// and would otherwise keep a superseded choice alive indefinitely.
//
//   v2  renumbered the dither modes, so a stored index now selected the
//       wrong algorithm. Image defaults are reset.
//   v3  chunkRows 0 came to mean "auto", and measurement showed every
//       extra raster command costs a visible band. A stored 128 was a
//       provisional guess that is now actively harmful, so it is migrated
//       to auto rather than preserved.
//   v4  thermalLimit added. Nothing stored before v4 has one, and a
//       missing key must NOT read as 0 — 0 means pacing off, which is the
//       one value guaranteed to fault the printer. Absent keys therefore
//       take the compiled default of 1000.
static const uint8_t SCHEMA_VERSION = 4;

bool SettingsStore::begin() {
  loadDefaults();

  Preferences p;
  if (!p.begin(NS, true)) return false;   // read-only; absent namespace is fine

  const uint8_t schema = p.getUChar("schema", 1);
  const bool imageStale = (schema < 2);   // dither renumbering
  const bool chunkStale = (schema < 3);   // chunkRows now means auto

  if (p.isKey("ssid")) p.getString("ssid", _s.wifiSsid, sizeof(_s.wifiSsid));
  if (p.isKey("pass")) p.getString("pass", _s.wifiPass, sizeof(_s.wifiPass));
  if (p.isKey("host")) p.getString("host", _s.hostname, sizeof(_s.hostname));

  if (p.isKey("pip")) p.getBytes("pip", _s.printerIp, 4);

  _s.printerPort      = p.getUShort("pport", _s.printerPort);
  _s.paperIndex       = p.getUChar ("paper", _s.paperIndex);
  _s.autoCut          = p.getBool  ("autocut", _s.autoCut);
  _s.feedBeforeCut    = p.getUChar ("feedcut", _s.feedBeforeCut);

  // Left at the default (0 = auto) when migrating, so an old explicit
  // value does not survive the change in meaning.
  if (!chunkStale) _s.chunkRows = p.getUShort("chunk", _s.chunkRows);

  // The default passed here is the compiled 1000, so a settings file
  // written before v4 comes back paced rather than unpaced.
  _s.thermalLimit      = p.getUShort("thermal", _s.thermalLimit);
  _s.debugMode         = p.getBool  ("debug", _s.debugMode);

  if (!imageStale) {
    _s.defaultDither     = p.getUChar("dither", _s.defaultDither);
    _s.defaultThreshold  = p.getUChar("thresh", _s.defaultThreshold);
    _s.defaultGamma      = p.getUChar("gamma",  _s.defaultGamma);
    _s.defaultBrightness = p.getChar ("bright", _s.defaultBrightness);
    _s.defaultContrast   = p.getChar ("contr",  _s.defaultContrast);
  }

  p.end();
  return true;
}

bool SettingsStore::save() {
  Preferences p;
  if (!p.begin(NS, false)) return false;

  // Detect reboot-sensitive changes before overwriting.
  char oldSsid[33] = {0}, oldHost[33] = {0};
  p.getString("ssid", oldSsid, sizeof(oldSsid));
  p.getString("host", oldHost, sizeof(oldHost));
  _rebootRequired = (strcmp(oldSsid, _s.wifiSsid) != 0) ||
                    (strcmp(oldHost, _s.hostname) != 0);

  p.putString("ssid", _s.wifiSsid);
  p.putString("pass", _s.wifiPass);
  p.putString("host", _s.hostname);

  p.putBytes ("pip",     _s.printerIp, 4);
  p.putUShort("pport",   _s.printerPort);
  p.putUChar ("paper",   _s.paperIndex);
  p.putBool  ("autocut", _s.autoCut);
  p.putUChar ("feedcut", _s.feedBeforeCut);
  p.putUShort("chunk",   _s.chunkRows);
  p.putUShort("thermal", _s.thermalLimit);
  p.putBool  ("debug",   _s.debugMode);
  p.putUChar ("schema",  SCHEMA_VERSION);

  p.putUChar("dither", _s.defaultDither);
  p.putUChar("thresh", _s.defaultThreshold);
  p.putUChar("gamma",  _s.defaultGamma);
  p.putChar ("bright", _s.defaultBrightness);
  p.putChar ("contr",  _s.defaultContrast);

  p.end();
  return true;
}

void SettingsStore::factoryReset() {
  Preferences p;
  if (p.begin(NS, false)) {
    p.clear();
    p.end();
  }
  loadDefaults();
  _rebootRequired = true;
}

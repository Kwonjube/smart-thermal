// Unit.h - who this unit is, independent of any network or setting.
//
// Kept in its own NVS namespace ("unit"), which a factory reset does not
// clear. The hotspot password is printed on the label on the case, so it
// must never change once generated.

#pragma once
#include <Arduino.h>
#include <IPAddress.h>

namespace Unit {
  // Call once, early in setup().
  void begin();

  const char* suffix();     // "a1b2": last four hex digits of the MAC
  const char* apSsid();     // "SmartThermal-A1B2"
  // 8 characters. Generated on first call and kept for good. Call only
  // once the radio is running, so the random source is truly random.
  const char* apPass();

  // The default network name for an unnamed unit: "smartthermal-a1b2".
  const char* defaultHost();

  // The address slip prints once per address. True if this address and
  // name are the ones already printed.
  bool announced(IPAddress ip, const char* host);
  void markAnnounced(IPAddress ip, const char* host);
}

// Net.h - small Wi-Fi helpers shared by setup and normal running.

#pragma once
#include <Arduino.h>

namespace Net {
  // Starts recording why the station last disconnected. Call once.
  void begin();

  // The last disconnect reason code, 0 if none since clearReason().
  uint8_t lastReason();
  void    clearReason();

  // A reason in words a borrower understands: "password rejected",
  // "network not found", or "no answer from the network".
  const char* reasonText(uint8_t reason);

  // Turns a typed name into a valid network name: lowercase letters,
  // digits and single hyphens, at most 24 characters. Empty in, empty out.
  void slug(const char* in, char* out, size_t outLen);
}

#include "Log.h"
#include "Unit.h"
#include "Config.h"
#include <Preferences.h>
#include <esp_system.h>   // esp_random (core 2.x)
#if __has_include(<esp_random.h>)
  #include <esp_random.h>  // esp_random (core 3.x)
#endif

static const char* NS = "unit";

static char s_suffix[5];
static char s_ssid[33];
static char s_pass[9];
static char s_host[33];

// No 0/O, 1/l/I: the password is also printed as text, to be typed by
// hand if a camera will not read the QR code.
static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";

void Unit::begin() {
  const uint64_t mac = ESP.getEfuseMac();     // byte 0 is the first octet
  snprintf(s_suffix, sizeof(s_suffix), "%02x%02x",
           (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));

  char upper[5];
  for (uint8_t i = 0; i < 5; i++) upper[i] = toupper(s_suffix[i]);
  snprintf(s_ssid, sizeof(s_ssid), "%s%s", SETUP_AP_PREFIX, upper);
  snprintf(s_host, sizeof(s_host), "smartthermal-%s", s_suffix);

  Preferences p;
  if (p.begin(NS, true)) {
    if (p.getString("appass", s_pass, sizeof(s_pass)) != 8) s_pass[0] = 0;
    p.end();
  }
  Log.printf("[unit] %s, password %s\n", s_ssid,
                s_pass[0] ? s_pass : "(not generated yet)");
}

// Generated on first use rather than in begin(), because the ESP32's
// random numbers are only truly random once the radio is running - and
// the setup portal asks for this only after starting its hotspot.
const char* Unit::apPass() {
  if (s_pass[0]) return s_pass;
  for (uint8_t i = 0; i < 8; i++)
    s_pass[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
  s_pass[8] = 0;

  Preferences p;
  if (p.begin(NS, false)) { p.putString("appass", s_pass); p.end(); }
  Log.printf("[unit] hotspot password generated: %s\n", s_pass);
  return s_pass;
}

const char* Unit::suffix()      { return s_suffix; }
const char* Unit::apSsid()      { return s_ssid; }
const char* Unit::defaultHost() { return s_host; }

bool Unit::announced(IPAddress ip, const char* host) {
  Preferences p;
  if (!p.begin(NS, true)) return false;
  uint8_t a[4] = {0};
  char h[33] = {0};
  p.getBytes("annip", a, 4);
  p.getString("annhost", h, sizeof(h));
  p.end();
  return a[0] == ip[0] && a[1] == ip[1] && a[2] == ip[2] && a[3] == ip[3] &&
         !strcmp(h, host);
}

void Unit::markAnnounced(IPAddress ip, const char* host) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  const uint8_t a[4] = { ip[0], ip[1], ip[2], ip[3] };
  p.putBytes("annip", a, 4);
  p.putString("annhost", host);
  p.end();
}

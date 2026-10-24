#include "Net.h"
#include <WiFi.h>

static volatile uint8_t s_reason = 0;

void Net::begin() {
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    s_reason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

uint8_t Net::lastReason()  { return s_reason; }
void    Net::clearReason() { s_reason = 0; }

// ESP-IDF wifi_err_reason_t values. A wrong password rarely reports as
// AUTH_FAIL: on WPA2 it usually surfaces as a handshake timeout.
const char* Net::reasonText(uint8_t r) {
  switch (r) {
    case 2:    // AUTH_EXPIRE
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 202:  // AUTH_FAIL
    case 204:  // HANDSHAKE_TIMEOUT
      return "password rejected";
    case 201:  // NO_AP_FOUND
      return "network not found";
    default:
      return "no answer from the network";
  }
}

void Net::slug(const char* in, char* out, size_t outLen) {
  size_t n = 0;
  bool hyphen = false;
  const size_t cap = (outLen - 1 < 24) ? outLen - 1 : 24;
  for (; in && *in && n < cap; in++) {
    const char c = tolower(*in);
    if (isalnum((unsigned char)c)) { out[n++] = c; hyphen = false; }
    else if (n && !hyphen)         { out[n++] = '-'; hyphen = true; }
  }
  while (n && out[n - 1] == '-') n--;
  out[n] = 0;
}

#include "Log.h"
#include <esp_attr.h>
#include <esp_system.h>
#include <time.h>
#include <LittleFS.h>

LogTee Log;

// In .noinit RAM: not cleared at startup, so the ring survives a reset, a
// crash, a watchdog and the BOOT restart - exactly the moments a log is
// wanted. A power cut still loses it; RAM cannot hold anything unpowered.
// The magic number tells a surviving ring from power-on garbage.
static const uint32_t MAGIC = 0x4C4F4732;   // "LOG2": changes with the size
static __NOINIT_ATTR char     s_ring[LogTee::LOG_BYTES];
static __NOINIT_ATTR uint32_t s_head;       // next byte to write
static __NOINIT_ATTR uint32_t s_full;
static __NOINIT_ATTR uint32_t s_magic;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready = false;
static uint32_t s_written = 0;      // bytes into the ring since start
static uint32_t s_saved   = 0;      // ... of which already in /trace.log

const char* LogTee::SAVED     = "/trace.log";
const char* LogTee::SAVED_OLD = "/trace.old";

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "CRASH (panic)";
    case ESP_RST_INT_WDT:  return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "CRASH (task watchdog)";
    case ESP_RST_WDT:      return "CRASH (watchdog)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply dipped)";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    default:               return "unknown";
  }
}

void LogTee::begin() {
  const esp_reset_reason_t why = esp_reset_reason();
  const bool kept = (why != ESP_RST_POWERON) && (s_magic == MAGIC) &&
                    (s_head < LOG_BYTES) && (s_full <= 1);
  if (!kept) {
    s_head  = 0;
    s_full  = 0;
    s_magic = MAGIC;
  }
  s_ready = true;
  printf("\n===== start: %s%s =====\n", resetName(why),
         kept ? ", log kept from before" : "");
}

// Every line starts with the time: local time once the clock has synced,
// otherwise seconds since start ("+42s"), so lines before the sync still
// order correctly. Bare blank lines are left unstamped.
static bool s_lineStart = true;

static size_t stamp(char* out, size_t cap) {
  const time_t now = time(nullptr);
  if (now > 1700000000) {                  // synced: after Nov 2023
    struct tm t;
    localtime_r(&now, &t);
    return strftime(out, cap, "%H:%M:%S ", &t);
  }
  return snprintf(out, cap, "+%lus ", (unsigned long)(millis() / 1000));
}

size_t LogTee::write(const uint8_t* buf, size_t len) {
  size_t i = 0;
  while (i < len) {
    const uint8_t* nl = static_cast<const uint8_t*>(memchr(buf + i, '\n', len - i));
    const size_t seg = nl ? static_cast<size_t>(nl - (buf + i)) + 1 : len - i;
    if (s_lineStart && !(seg == 1 && buf[i] == '\n')) {
      char p[20];
      raw(reinterpret_cast<const uint8_t*>(p), stamp(p, sizeof(p)));
    }
    raw(buf + i, seg);
    s_lineStart = (nl != nullptr);
    i += seg;
  }
  return len;
}

size_t LogTee::raw(const uint8_t* buf, size_t len) {
  Serial.write(buf, len);
  if (!s_ready) return len;              // before begin(): Serial only

  // Only the newest LOG_BYTES of an oversized write can survive anyway.
  const uint8_t* p = buf;
  size_t n = len;
  if (n > LOG_BYTES) { p += n - LOG_BYTES; n = LOG_BYTES; }

  portENTER_CRITICAL(&s_mux);
  const size_t first = (n < LOG_BYTES - s_head) ? n : LOG_BYTES - s_head;
  memcpy(s_ring + s_head, p, first);
  memcpy(s_ring, p + first, n - first);
  if (s_head + n >= LOG_BYTES) s_full = 1;
  s_head = (s_head + n) % LOG_BYTES;
  s_written += n;
  portEXIT_CRITICAL(&s_mux);
  return len;
}

size_t LogTee::snapshot(char* out, size_t cap) {
  if (!cap) return 0;

  portENTER_CRITICAL(&s_mux);
  const size_t head = s_head;
  const bool   full = s_full;
  size_t n = full ? LOG_BYTES : head;
  if (n > cap) n = cap;
  const size_t start = full ? head : 0;
  for (size_t i = 0; i < n; i++) out[i] = s_ring[(start + i) % LOG_BYTES];
  portEXIT_CRITICAL(&s_mux);

  // Once the ring has wrapped, its oldest line is cut off at the front.
  // Drop that partial line so the copy starts cleanly.
  if (full) {
    size_t skip = 0;
    while (skip < n && out[skip] != '\n') skip++;
    if (skip < n) {
      skip++;
      memmove(out, out + skip, n - skip);
      n -= skip;
    }
  }
  return n;
}

// Appends what is new in the ring to /trace.log. Runs on loop() only.
void LogTee::save(bool now) {
  if (!_save) return;
  uint32_t pending;
  portENTER_CRITICAL(&s_mux);
  pending = s_written - s_saved;
  portEXIT_CRITICAL(&s_mux);
  if (!pending) return;
  if (!now && pending < 4096 && millis() - _savedAt < 30000) return;
  _savedAt = millis();

  File f = LittleFS.open(SAVED, "a");
  if (!f) return;
  if (pending > LOG_BYTES) {                       // more than the ring holds: say so
    f.printf("... %lu bytes of log lost (too much at once) ...\n", (unsigned long)(pending - LOG_BYTES));
    portENTER_CRITICAL(&s_mux);
    s_saved = s_written - LOG_BYTES;
    portEXIT_CRITICAL(&s_mux);
    pending = LOG_BYTES;
  }
  char chunk[512];
  while (pending) {
    const size_t n = pending > sizeof(chunk) ? sizeof(chunk) : pending;
    portENTER_CRITICAL(&s_mux);
    // The oldest unsaved byte sits (written - saved) bytes behind the head.
    const uint32_t back = s_written - s_saved;
    const size_t start = (s_head + LOG_BYTES - (back > LOG_BYTES ? LOG_BYTES : back)) % LOG_BYTES;
    for (size_t i = 0; i < n; i++) chunk[i] = s_ring[(start + i) % LOG_BYTES];
    s_saved += n;
    portEXIT_CRITICAL(&s_mux);
    f.write(reinterpret_cast<const uint8_t*>(chunk), n);
    pending -= n;
  }
  const size_t size = f.size();
  f.close();
  if (size > 96 * 1024) {
    LittleFS.remove(SAVED_OLD);
    LittleFS.rename(SAVED, SAVED_OLD);
  }
}

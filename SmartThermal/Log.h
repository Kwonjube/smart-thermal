// Log.h - everything written to Serial, also kept in RAM for the web.
//
// Every log line in the firmware goes through Log instead of Serial. Log
// writes to Serial exactly as before and keeps the most recent LOG_BYTES
// in a ring, served at /log (a page with a Copy button) and /log.txt.
// So a log can be sent from a phone or browser, without the Arduino
// serial monitor and its copy limits.
//
// It survives a reset, crash or BOOT restart (not a power cut): see
// Log.cpp. 16 KB, about 200 lines. It is static memory; the first
// 8 KB left the largest free block (what an image needs) unchanged at
// 108 KB. Check "largest" in the /log header still reads 108 KB.
//
// Safe from any task: the ring is guarded by a spinlock, held only for
// the memcpy of one write.

#pragma once
#include <Arduino.h>

class LogTee : public Print {
public:
  static const size_t LOG_BYTES = 16384;

  // Call once, straight after Serial.begin(). Keeps the ring from before a
  // reset or crash, and marks the start with the reason for it.
  void begin();

  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t len) override;

  // The ring, oldest first, starting at a whole line. Returns bytes copied.
  size_t snapshot(char* out, size_t cap);

  // Saved log (v0.22, for troubleshooting): new lines are also appended to
  // /trace.log on LittleFS, so they survive a power cut. At 96 KB it becomes
  // /trace.old (the previous one is dropped): about two weeks of normal
  // running. Call enableSave() once LittleFS is mounted, and save() from
  // loop() - it writes at most every 30 s, or sooner past 4 KB.
  void enableSave() { _save = true; }
  // While an image streams from storage, routine saves wait, so a flash
  // write never stalls it mid-print (v0.26). save(true) still runs.
  void hold(bool on) { _hold = on; }
  void save(bool now = false);
  static const char* SAVED;
  static const char* SAVED_OLD;

private:
  size_t raw(const uint8_t* buf, size_t len);   // no timestamping
  bool     _save = false;
  volatile bool _hold = false;
  uint32_t _savedAt = 0;
};

extern LogTee Log;

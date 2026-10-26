#include "Audit.h"
#include "Mem.h"
#include "Config.h"
#include "Log.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>

#if AUDIT_TRACE

namespace {
  TaskHandle_t s_printer = nullptr, s_online = nullptr, s_loop = nullptr;

  struct Big { char path[48]; size_t size; };
  Big    s_big[10];
  size_t s_dirs[8]; char s_dirName[8][20]; uint8_t s_nDirs = 0;

  void note(const char* path, size_t size, const char* top) {
    // Per top-level folder totals.
    uint8_t i = 0;
    for (; i < s_nDirs; i++) if (!strcmp(s_dirName[i], top)) break;
    if (i == s_nDirs && s_nDirs < 8) { strlcpy(s_dirName[i], top, sizeof(s_dirName[0])); s_dirs[i] = 0; s_nDirs++; }
    if (i < s_nDirs) s_dirs[i] += size;
    // The ten biggest files.
    uint8_t k = 0;
    for (uint8_t j = 1; j < 10; j++) if (s_big[j].size < s_big[k].size) k = j;
    if (size > s_big[k].size) { strlcpy(s_big[k].path, path, sizeof(s_big[k].path)); s_big[k].size = size; }
  }

  void walk(const char* dir, const char* top, uint8_t depth) {
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      String p = String(dir);
      if (!p.endsWith("/")) p += "/";
      p += f.name();
      const char* t = depth ? top : f.name();
      if (f.isDirectory()) { if (depth < 4) { String pp = p; f.close(); walk(pp.c_str(), t, depth + 1); continue; } }
      else note(p.c_str(), f.size(), depth ? top : "(root)");
      f.close();
    }
  }
}

void Audit::setTask(const char* name, TaskHandle_t t) {
  if (!strcmp(name, "printer")) s_printer = t;
  else if (!strcmp(name, "online")) s_online = t;
}

void Audit::mem(const char* stage) {
  // Byte memory only (Mem.h); IRAM is the 32-bit-only spare, shown apart.
  Log.printf("[audit] mem %s: free %uK, block %uK, lowest ever %uK, iram %uK\n", stage,
             (unsigned)(Mem::free8() / 1024), (unsigned)(Mem::block8() / 1024),
             (unsigned)(Mem::lowest8() / 1024), (unsigned)(Mem::iramBlock() / 1024));
}

void Audit::tasks() {
  if (!s_loop) s_loop = xTaskGetCurrentTaskHandle();     // called from loop()
  // Headroom = stack never used so far (bytes). Small = near overflow.
  Log.printf("[audit] stacks unused: loop %u B (of 8192), printer %u B (of 6144), online %u B (of 16384)\n",
             (unsigned)uxTaskGetStackHighWaterMark(s_loop),
             s_printer ? (unsigned)uxTaskGetStackHighWaterMark(s_printer) : 0,
             s_online  ? (unsigned)uxTaskGetStackHighWaterMark(s_online)  : 0);
}

void Audit::every5min() {
  static uint32_t last = 0;
  if (last && millis() - last < 300000) return;
  last = millis() | 1;
  mem("5-min");
  tasks();
}

void Audit::storage() {
  const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  Log.printf("[audit] storage: %u KB total, %u KB used, %u KB free\n",
             (unsigned)(total / 1024), (unsigned)(used / 1024),
             (unsigned)(used < total ? (total - used) / 1024 : 0));
  memset(s_big, 0, sizeof(s_big)); s_nDirs = 0;
  walk("/", "", 0);
  for (uint8_t i = 0; i < s_nDirs; i++)
    Log.printf("[audit] storage /%s: %u KB\n", s_dirName[i], (unsigned)(s_dirs[i] / 1024));
  for (uint8_t i = 0; i < 10; i++)
    if (s_big[i].size) Log.printf("[audit] storage file %s: %u KB\n", s_big[i].path, (unsigned)(s_big[i].size / 1024));
}

#else
void Audit::setTask(const char*, TaskHandle_t) {}
void Audit::mem(const char*) {}
void Audit::tasks() {}
void Audit::every5min() {}
void Audit::storage() {}
#endif

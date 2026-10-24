#pragma once
#include <Arduino.h>
struct FakeLog { bool quiet = false;
  void println(const char* s) { if (!quiet) printf("    log: %s\n", s); }
  void printf(const char* f, ...) { if (quiet) return; va_list a; va_start(a, f); ::printf("    log: "); vprintf(f, a); va_end(a); } };
extern FakeLog Log;

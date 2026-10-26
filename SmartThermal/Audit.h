#pragma once
// Audit.h - MEASUREMENT ONLY (v0.24.9-rz-audit, AUDIT_TRACE in Config.h).
//
// Records what the Firmware Optimisation and Printing Audit asks for:
// memory (free, largest block, lowest ever) at each stage, task stack
// headroom, the secure link's cost and connect time, storage use and its
// biggest files, and upload/print timings. Lines are tagged [audit]. Nothing
// here changes behaviour. Remove with the AUDIT_TRACE switch once settled.

#include <Arduino.h>

namespace Audit {
  void mem(const char* stage);        // [audit] mem <stage>: free, block, lowest
  void storage();                     // [audit] storage: totals + biggest files
  void tasks();                       // [audit] stacks: headroom per task
  void every5min();                   // mem + tasks, from loop()
  void setTask(const char* name, TaskHandle_t t);   // printer, online
}

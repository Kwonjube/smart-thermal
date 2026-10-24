// Diagnostics.h — the three questions the trainer depends on.
//
// Each probe answers something the trainer's design turns on, and each is
// cheap. Run them in order; if the first shows the ESP32 cannot hear the
// printer, stop, because the failsafe and the trainer both rest on it.
//
//   l  LINK       which cable, is it up, how fast does the printer answer
//   r  READ PATH  can the ESP32 hear status and print-done replies, and is
//                 print time steady enough to detect banding by timing
//   o  PAPER OUT  what the printer reports when it runs dry, and what
//                 happens to a job sent while it is empty
//   y  DENSITY    does the printer accept a density command over ESC/POS
//                 (answered in v0.7.1: no - see HANDOVER)
//   e  LINK RESET reset the W5500 without rebooting, and report whether
//                 that brought an unreachable printer back
//
// All run on the printer task via PrintQueue::runOnPrinterTask(), because
// they drive the Printer directly. Results go to Serial.
//
// In the finished product this file belongs to the TRAINER build only.

#pragma once
#include <Arduino.h>
#include "Printer.h"

namespace Diagnostics {
  void link(Printer& p, void* ctx);
  void readPath(Printer& p, void* ctx);
  void paperOut(Printer& p, void* ctx);
  void density(Printer& p, void* ctx);
  void resetLink(Printer& p, void* ctx);
}

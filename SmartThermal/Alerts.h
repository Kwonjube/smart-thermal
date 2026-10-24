// Alerts.h - what the borrower is told, in one place.
//
// Short, plain, and always ending in what to do (or that nothing needs
// doing). The code is for when they ring you: "it says E04".

#pragma once
#include "PrintQueue.h"

struct AlertText { const char* code; const char* message; };

inline AlertText alertText(PrintQueue::Alert a) {
  switch (a) {
    case PrintQueue::Alert::Paused:
      return { "E01", "The printer has paused. If it's out of paper or the "
                      "cover is open, fix that - otherwise it's cooling and "
                      "will carry on by itself." };
    case PrintQueue::Alert::Hot:
      return { "E02", "The printer got too hot and stopped. It's recovering "
                      "- your print will be retried in about 3 minutes." };
    case PrintQueue::Alert::Stuck:
      return { "E03", "The printer isn't responding. Switch it off, wait 5 "
                      "seconds, switch it on, then wait 1 minute. If this "
                      "message is still here, do it again - it can take "
                      "two or three goes. Your print will continue." };
    case PrintQueue::Alert::PaperOut:
      return { "E04", "Out of paper. Load a new roll and close the cover. "
                      "Your print will continue." };
    case PrintQueue::Alert::Cover:
      return { "E05", "The cover is open. Close it firmly to carry on." };
    case PrintQueue::Alert::NoLink:
      return { "E06", "Can't see the printer. Check it's switched on and "
                      "its cable is plugged in." };
    default:
      return { "", "" };
  }
}

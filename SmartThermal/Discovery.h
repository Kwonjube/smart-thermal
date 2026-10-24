// Discovery.h - finds the printer on the cable when it is not where the
// saved address says.
//
// The cable is point-to-point, so this is a short list of guesses, not a
// network scan: the saved address, then the factory defaults of common
// ESC/POS printers. Each guess costs one TCP connect with a short timeout.
//
// If the printer has been given an arbitrary address, no list will find
// it. That case falls to setup, which asks for the address printed on the
// printer's self-test slip.
//
// Printer task only: every guess is an SPI transaction.

#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include "Printer.h"

namespace Discovery {

  // Per-guess connect timeout. The cable has one device on it and replies
  // in well under a millisecond; this only bounds a guess that is wrong.
  static const uint32_t GUESS_TIMEOUT_MS = 500;

  // Tries `saved` first, then every candidate. True and `found` set on the
  // first that accepts a connection. Leaves the committed target alone -
  // the caller decides whether to move to what was found.
  bool find(Printer& printer, IPAddress saved, IPAddress& found);

  // Number of candidates after the saved address, for the log.
  uint8_t candidates();
}

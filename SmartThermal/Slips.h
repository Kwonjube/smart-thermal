// Slips.h - the paper a borrower reads. For someone who just plugs things
// in, these slips are the whole user interface.
//
//   SETUP        no network saved: QR code to join the setup hotspot
//   ADDRESSES    joined a network: the welcome slip - name, QR code to the
//                page, addresses, how to use. Once per address.
//   SETUP_FAILED the network chosen on the setup page refused to join
//   ONLINE       approved for online printing: "Welcome to the internet!",
//                the link (QR), owner code. Again on a new link/owner code
//   KNOCK        someone asked to print here (extra secure): approve at -a
//
// THE DESK
// A slip is wanted, then submitted when the printer is reachable, then
// confirmed from the job history. A slip whose job failed goes back to
// wanted and is retried. The record of a slip still owed is never cleared
// until its job is confirmed printed - the same rule as the pixel-byte
// shortfall: do not destroy the record of work owed until it is done.
//
// Loop task only.

#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include "PrintQueue.h"
#include "JobStore.h"

class SlipDesk {
public:
  enum Kind : uint8_t { SETUP, ADDRESSES, SETUP_FAILED, ONLINE, KNOCK, KINDS };

  void begin(PrintQueue& queue, JobStore& store, uint16_t widthDots);

  void wantSetup();
  void wantAddresses(IPAddress ip, const char* host);
  void wantSetupFailed(const char* ssid, const char* why);
  void wantOnline(const char* name, const char* url, const char* ownerCode,
                  bool secure, bool visible, const char* key);
  // dev / loc (v0.26): the asking device and its rough location, from the
  // website (shown to the visitor as "Information collected").
  void wantKnock(const char* who, const char* printer, const char* url,
                 const char* dev = "", const char* loc = "");
  bool busy(Kind k) const { return _state[k] != IDLE; }

  // Submits and confirms. Call every loop.
  void loop();

private:
  enum State : uint8_t { IDLE, WANTED, SUBMITTED };

  PrintQueue* _q = nullptr;
  JobStore*   _store = nullptr;
  uint16_t    _width = 576;

  State    _state[KINDS]   = { IDLE, IDLE, IDLE, IDLE, IDLE };
  uint32_t _first[KINDS]   = {0};
  uint32_t _last[KINDS]    = {0};
  uint32_t _retryAt[KINDS] = {0};
  uint8_t  _fails[KINDS]   = {0};    // failed attempts in a row (v0.24.1)

  IPAddress _ip;
  char _host[33] = {0};
  char _ssid[33] = {0};
  char _why[40]  = {0};
  char _oName[33] = {0}, _oUrl[96] = {0}, _oCode[8] = {0}, _oKey[24] = {0};
  bool _oSec = false, _oVis = false;
  char _kWho[33] = {0}, _kName[33] = {0}, _kUrl[96] = {0};
  char _kDev[48] = {0}, _kLoc[64] = {0};

  static const uint32_t RETRY_MS = 30000;
  // After this many failed tries a slip prints without its QR code (the
  // address is printed in words anyway) rather than not at all.
  static const uint8_t  QR_TRIES = 3;
  bool _noQr = false;

  bool submit(Kind k);
  void confirmed(Kind k);

  // Each returns false if the queue refused anything; ids are the first
  // and last job of the slip.
  bool printSetup(uint32_t& first, uint32_t& last);
  bool printAddresses(uint32_t& first, uint32_t& last);
  bool printFailed(uint32_t& first, uint32_t& last);
  bool printOnline(uint32_t& first, uint32_t& last);
  bool printKnock(uint32_t& first, uint32_t& last);
};

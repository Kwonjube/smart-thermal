// SetupPortal.h - setup mode: the unit's own hotspot and the page that
// joins it to a network.
//
// Runs only when no network is saved. Owns port 80 and DNS in setup mode;
// WebApi is not started at all, so nothing else can answer.
//
// JOINING IS TESTED BEFORE IT IS SAVED. The hotspot stays up while the
// unit tries the chosen network, so a wrong password is reported on the
// page (and on paper) and the borrower simply tries again. Only a network
// that actually joined is saved; then the unit restarts into normal mode,
// where the address slip confirms it on paper.
//
// Joining can move the hotspot to the home network's channel, which drops
// the phone off the hotspot for a moment. The page may therefore miss the
// result. The paper does not: success prints the address slip after the
// restart, failure prints a "can't join" slip.

#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "Settings.h"
#include "PrintQueue.h"
#include "Slips.h"

class SetupPortal {
public:
  SetupPortal(SettingsStore& settings, PrintQueue& queue, SlipDesk& desk,
              Printer& printer);

  void begin();
  void loop();

private:
  enum Join : uint8_t { IDLE, CONNECTING, OK, FAILED };

  SettingsStore& _settings;
  PrintQueue&    _queue;
  SlipDesk&      _desk;
  Printer&       _printer;
  WebServer      _server;
  DNSServer      _dns;

  Join     _join = IDLE;
  uint32_t _joinStart = 0;
  uint32_t _restartAt = 0;
  char     _ssid[33] = {0};
  char     _pass[65] = {0};
  char     _host[33] = {0};
  char     _reason[40] = {0};
  // Online printing chosen on the setup page; saved once the Wi-Fi joins.
  bool     _onEn = false, _onVis = false, _onSec = false;

  static const uint32_t JOIN_TIMEOUT_MS  = 20000;
  static const uint32_t RESTART_DELAY_MS = 8000;

  void routes();
  void handlePage();
  void handleNetworks();
  void handleStatus();
  void handleJoin();
  void handlePrinter();
  void redirect();

  void serviceJoin();
};

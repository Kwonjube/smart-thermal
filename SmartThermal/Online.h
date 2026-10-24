// Online.h - online printing through the website (v0.21).
//
// Off unless the owner ticks "Allow online printing" (setup page or
// Settings). Then the unit, by itself:
//   1. makes its identity key (OnlineKey) the first time,
//   2. connects out to the relay at RELAY_HOST and introduces itself:
//      its local name (the one in its .local address), MAC, visible /
//      extra-secure choices - the relay adds the external IP, place and
//      Perth time for the admin,
//   3. waits, connected, while the request is pending,
//   4. once approved, prints "Welcome to the internet!" with its link and
//      owner code, and from then on passes visitors' requests to its own
//      web server (RelaySession).
// Nothing reaches the printer through the website until it is approved.
//
// Status wording (the page shows "Online printing: <text>"):
//   Off / Waiting for the internet / Connecting / Waiting for approval /
//   On / Not approved / Can't reach <host>, retrying
//
// Config lives in NVS namespace "online"; a factory reset clears it (and so
// the identity: the printer must be approved again).

#pragma once
#include <Arduino.h>

class SlipDesk;
class PrintQueue;

namespace Online {
  enum State : uint8_t { OFF, WAITING, CONNECTING, PENDING, ON, DECLINED, UNREACHABLE };

  // The printer's name online is its local name (Settings > Hostname); no
  // separate name and no owner's name (v0.21.2, as the relay asked).
  // host is the relay address; wss://<host>/relay/connect is built from it.
  // paused (v0.23): visitors through the website can still open the page,
  // but nothing they send prints (the unit refuses, and the relay is told
  // in the hello so it can say so before forwarding).
  struct Config {
    bool en = false, vis = false, sec = false, paused = false;
    char host[64] = "";
  };

  void begin(SlipDesk& desk, PrintQueue& queue);   // normal mode only
  void loop();                                     // from loop()

  Config config();
  bool   save(const Config& c, String& err);       // checks, saves, reconnects
  // Setup page: saved before the restart into normal mode.
  void   saveFromSetup(bool en, bool vis, bool sec);
  void   forget();                                 // factory reset

  State  state();
  const char* stateKey();       // "off", "pending", ... for the page
  String stateText();           // the wording above
  String link();                // the visitors' link once approved, else ""
  String ownerCode();           // shown only on the home network and on paper
  String mac();
  String name();                // the local name, as sent to the relay
  bool   paused();

  void   newLink();             // asks the relay for a new code
  void   newOwnerCode();

  // From the slip desk, once the welcome slip is on paper.
  void   welcomed(const char* key);
}

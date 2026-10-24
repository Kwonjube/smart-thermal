#include "Log.h"
#include "SetupPortal.h"
#include "Online.h"
#include "SetupPage.h"
#include "LogPage.h"
#include "Unit.h"
#include "Net.h"
#include <WiFi.h>
#include <ArduinoJson.h>

static const IPAddress AP_IP(192, 168, 4, 1);

SetupPortal::SetupPortal(SettingsStore& settings, PrintQueue& queue,
                         SlipDesk& desk, Printer& printer)
  : _settings(settings), _queue(queue), _desk(desk), _printer(printer),
    _server(80) {}

void SetupPortal::begin() {
  // AP+STA: the hotspot for the phone, the station for scanning and for
  // testing the chosen network before anything is saved.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(Unit::apSsid(), Unit::apPass());   // radio on: see Unit.h

  // Every name resolves to the unit, which is what makes phones decide
  // this is a sign-in network and open the page by themselves.
  _dns.setErrorReplyCode(DNSReplyCode::NoError);
  _dns.start(53, "*", AP_IP);

  routes();
  _server.begin();
  WiFi.scanNetworks(true);

  Log.printf("[setup] hotspot %s up, page at http://192.168.4.1\n",
                Unit::apSsid());
}

void SetupPortal::loop() {
  _dns.processNextRequest();
  _server.handleClient();
  serviceJoin();

  if (_restartAt && (int32_t)(millis() - _restartAt) >= 0) {
    Log.println("[setup] restarting into normal mode");
    delay(200);
    ESP.restart();
  }
}

// ----------------------------------------------------------------------
// Routes
// ----------------------------------------------------------------------

void SetupPortal::routes() {
  _server.on("/",               HTTP_GET,  [this]{ handlePage(); });
  _server.on("/setup/networks", HTTP_GET,  [this]{ handleNetworks(); });
  _server.on("/setup/status",   HTTP_GET,  [this]{ handleStatus(); });
  _server.on("/setup/join",     HTTP_POST, [this]{ handleJoin(); });
  _server.on("/setup/printer",  HTTP_POST, [this]{ handlePrinter(); });
  _server.on("/log",     HTTP_GET, [this]{ _server.send_P(200, "text/html", LOG_PAGE); });
  _server.on("/log.txt", HTTP_GET, [this]{ sendLogText(_server); });
  // Everything else - including each phone maker's connectivity check
  // (/generate_204, /hotspot-detect.html, /connecttest.txt, ...) - is sent
  // to the page. A check that does not get its expected answer is what
  // makes the phone show the sign-in sheet.
  _server.onNotFound([this]{ redirect(); });
}

void SetupPortal::redirect() {
  _server.sendHeader("Location", "http://192.168.4.1/", true);
  _server.send(302, "text/plain", "");
}

void SetupPortal::handlePage() {
  _server.sendHeader("Cache-Control", "no-store");
  _server.send_P(200, "text/html", SETUP_PAGE);
}

// Deduplicated by name (a mesh shows one name many times), strongest
// first. A rescan starts after each answer, so the list stays fresh.
void SetupPortal::handleNetworks() {
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING || _join == CONNECTING) {
    _server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (n < 0) {                         // none yet, or failed: start one
    WiFi.scanNetworks(true);
    _server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }

  JsonDocument doc;
  JsonArray list = doc["list"].to<JsonArray>();
  for (int16_t i = 0; i < n; i++) {
    const String s = WiFi.SSID(i);
    if (!s.length()) continue;                         // hidden
    bool dup = false;
    for (int16_t j = 0; j < i && !dup; j++) dup = (WiFi.SSID(j) == s);
    if (dup) continue;
    JsonObject o = list.add<JsonObject>();
    o["s"]    = s;
    o["r"]    = WiFi.RSSI(i);
    o["lock"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();

  String out;
  serializeJson(doc, out);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "application/json", out);
}

void SetupPortal::handleStatus() {
  JsonDocument doc;
  doc["unit"] = Unit::apSsid();
  doc["def"]  = Unit::defaultHost();

  const char* printer =
      _queue.printerReachable()                        ? "found"
    : _queue.printerSearching()                        ? "searching"
    : _queue.printerLinkDown()                         ? "nolink"
    : _queue.printerSearches() == 0                    ? "searching"
                                                       : "notfound";
  doc["printer"] = printer;

  static const char* JOIN[] = { "idle", "connecting", "ok", "failed" };
  doc["join"]   = JOIN[_join];
  doc["ssid"]   = _ssid;
  doc["host"]   = _host;
  doc["reason"] = _reason;

  String out;
  serializeJson(doc, out);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "application/json", out);
}

// ----------------------------------------------------------------------
// Joining
// ----------------------------------------------------------------------

void SetupPortal::handleJoin() {
  if (_join == CONNECTING || _join == OK) {
    _server.send(409, "text/plain", "busy");
    return;
  }
  const String ssid = _server.arg("ssid");
  const String pass = _server.arg("pass");
  if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) {
    _server.send(400, "text/plain", "bad network or password");
    return;
  }

  strlcpy(_ssid, ssid.c_str(), sizeof(_ssid));
  strlcpy(_pass, pass.c_str(), sizeof(_pass));
  Net::slug(_server.arg("name").c_str(), _host, sizeof(_host));
  if (!_host[0]) strlcpy(_host, Unit::defaultHost(), sizeof(_host));
  _reason[0] = 0;
  _onEn  = _server.arg("online") == "1";
  _onVis = _server.arg("ovis") == "1";
  _onSec = _server.arg("osec") == "1";

  WiFi.scanDelete();
  Net::clearReason();
  WiFi.begin(_ssid, _pass);
  _join      = CONNECTING;
  _joinStart = millis();
  Log.printf("[setup] trying %s as %s\n", _ssid, _host);
  _server.send(202, "text/plain", "joining");
}

void SetupPortal::serviceJoin() {
  if (_join != CONNECTING) return;

  if (WiFi.status() == WL_CONNECTED) {
    Settings& s = _settings.get();
    strlcpy(s.wifiSsid, _ssid, sizeof(s.wifiSsid));
    strlcpy(s.wifiPass, _pass, sizeof(s.wifiPass));
    strlcpy(s.hostname, _host, sizeof(s.hostname));
    _settings.save();
    Online::saveFromSetup(_onEn, _onVis, _onSec);

    _join      = OK;
    _restartAt = millis() + RESTART_DELAY_MS;
    Log.printf("[setup] joined %s, ip %s - saved\n", _ssid,
                  WiFi.localIP().toString().c_str());
    return;
  }

  if (millis() - _joinStart < JOIN_TIMEOUT_MS) return;

  const uint8_t r = Net::lastReason();
  strlcpy(_reason, Net::reasonText(r), sizeof(_reason));
  WiFi.disconnect(false, true);       // stop trying; the hotspot stays up
  _join = FAILED;
  Log.printf("[setup] could not join %s: %s (reason %u)\n",
                _ssid, _reason, r);

  // On paper as well: the phone may have been dropped off the hotspot
  // while the unit was trying, and then the page never shows this.
  _desk.wantSetupFailed(_ssid, _reason);
  WiFi.scanNetworks(true);
}

// ----------------------------------------------------------------------
// Manual printer address
// ----------------------------------------------------------------------

struct Retarget { Printer* printer; IPAddress ip; };
static Retarget s_retarget;

void SetupPortal::handlePrinter() {
  IPAddress ip;
  if (!ip.fromString(_server.arg("ip")) || ip[0] == 0 || ip[3] == 0 ||
      ip[3] == 255) {
    _server.send(400, "text/plain", "invalid address");
    return;
  }

  Settings& s = _settings.get();
  for (uint8_t i = 0; i < 4; i++) s.printerIp[i] = ip[i];
  _settings.save();

  // The transport belongs to the printer task: hand the change over.
  s_retarget = { &_printer, ip };
  _queue.requestReconfig([](void* ctx) {
    Retarget* r = static_cast<Retarget*>(ctx);
    r->printer->retarget(r->ip);
    Log.printf("[setup] printer address set by hand: %s\n",
                  r->ip.toString().c_str());
  }, &s_retarget);
  _queue.reprobeSoon();

  _server.send(200, "text/plain", "ok");
}

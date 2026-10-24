#include "Online.h"
#include "OnlineKey.h"
#include "RelaySession.h"
#include "Slips.h"
#include "Scheduler.h"
#include "Config.h"
#include "Log.h"
#include "B64.h"
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

const char*    NS          = "online";
const uint32_t DECLINED_MS = 6UL * 3600UL * 1000UL;   // ask again after 6 h
const uint32_t MIN_HEAP    = 48000;                   // TLS needs ~40 KB

SemaphoreHandle_t g_lock = nullptr;
Online::Config g_cfg;
char     g_code[12] = "", g_url[96] = "", g_ocode[8] = "", g_welc[24] = "";
uint8_t  g_d[32];
bool     g_haveKey = false;
volatile Online::State g_state = Online::OFF;
volatile bool g_restart = false, g_newCode = false, g_evWelcome = false;
char     g_knock[3][33];
uint8_t  g_knockN = 0;
SlipDesk*   g_desk = nullptr;
TaskHandle_t g_task = nullptr;

struct Lock {
  Lock()  { if (g_lock) xSemaphoreTake(g_lock, portMAX_DELAY); }
  ~Lock() { if (g_lock) xSemaphoreGive(g_lock); }
};

void setState(Online::State s) {
  if (g_state != s) {
    g_state = s;
    Log.printf("[online] %s\n", Online::stateText().c_str());
  }
}

void putStr(const char* k, const char* v) {
  Preferences p;
  if (p.begin(NS, false)) { p.putString(k, v); p.end(); }
}

// Six characters that cannot be misread (no 0/O, 1/I/L).
void makeOwnerCode(char out[8]) {
  static const char A[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
  uint8_t r[6];
  OnlineKey::fill(r, 6);
  for (int i = 0; i < 6; i++) out[i] = A[r[i] % (sizeof(A) - 1)];
  out[6] = 0;
}

void load() {
  Preferences p;
  if (!p.begin(NS, true)) return;
  g_cfg.en  = p.getBool("en", false);
  g_cfg.vis = p.getBool("vis", false);
  g_cfg.sec = p.getBool("sec", false);
  g_cfg.paused = p.getBool("pause", false);
  strlcpy(g_cfg.host,  p.getString("host", RELAY_HOST).c_str(), sizeof(g_cfg.host));
  strlcpy(g_code,  p.getString("code", "").c_str(),  sizeof(g_code));
  strlcpy(g_url,   p.getString("url", "").c_str(),   sizeof(g_url));
  strlcpy(g_ocode, p.getString("oc", "").c_str(),    sizeof(g_ocode));
  strlcpy(g_welc,  p.getString("welc", "").c_str(),  sizeof(g_welc));
  g_haveKey = p.getBytes("d", g_d, 32) == 32;
  p.end();
  if (!g_cfg.host[0]) strlcpy(g_cfg.host, RELAY_HOST, sizeof(g_cfg.host));
}

// ----------------------------------------------------------------------
// Connections
// ----------------------------------------------------------------------

class TlsConn : public Conn {
  NetworkClientSecure c;
public:
  bool open(const char* host, uint16_t port, uint32_t timeoutMs) override {
    c.useBuiltinCACertBundle();          // the core's own root list: no keys to update by hand
    c.setHandshakeTimeout(20);
    return c.connect(host, port, (int32_t)timeoutMs) == 1;
  }
  int readSome(uint8_t* b, size_t n) override {
    const int a = c.available();
    if (a > 0) { const int r = c.read(b, (size_t)a < n ? (size_t)a : n); return r < 0 ? -1 : r; }
    return c.connected() ? 0 : -1;
  }
  size_t writeAll(const uint8_t* b, size_t n) override {
    size_t o = 0;
    while (o < n) { const size_t w = c.write(b + o, n - o); if (!w) break; o += w; }
    return o;
  }
  bool isOpen() override { return c.connected(); }
  void close() override { c.stop(); }
};

class LoopConn : public Conn {
  NetworkClient c;
public:
  bool open(const char*, uint16_t port, uint32_t timeoutMs) override {
    return c.connect(IPAddress(127, 0, 0, 1), port, (int32_t)timeoutMs) == 1;
  }
  int readSome(uint8_t* b, size_t n) override {
    const int a = c.available();
    if (a > 0) { const int r = c.read(b, (size_t)a < n ? (size_t)a : n); return r < 0 ? -1 : r; }
    return c.connected() ? 0 : -1;
  }
  size_t writeAll(const uint8_t* b, size_t n) override {
    size_t o = 0;
    const uint32_t t0 = millis();
    while (o < n && millis() - t0 < 20000) {
      const size_t w = c.write(b + o, n - o);
      if (w) o += w; else delay(2);
    }
    return o;
  }
  bool isOpen() override { return c.connected(); }
  void close() override { c.stop(); }
};

// ----------------------------------------------------------------------
// What the relay session asks of the unit
// ----------------------------------------------------------------------

// Why the last connection ended, told to the relay in the next hello so the
// website's admin log shows both sides of a drop (v0.23.1). Online task only.
static char     g_lastDrop[100] = "";
static uint16_t g_drops = 0;

class Host : public RelayHost {
public:
  bool hello(const char* nonce, String& json) override {
    uint8_t pub[65], sig[64];
    char msg[96];
    snprintf(msg, sizeof(msg), "smartthermal-relay-v1:%s", nonce);
    if (!OnlineKey::publicKey(g_d, pub) || !OnlineKey::sign(g_d, msg, sig)) return false;
    char k[96], s[96], oh[65], pre[40];
    b64encode(pub, 65, k, true);
    b64encode(sig, 64, s, true);
    Online::Config c; char oc[8];
    { Lock l; c = g_cfg; strlcpy(oc, g_ocode, sizeof(oc)); }
    snprintf(pre, sizeof(pre), "smartthermal-owner-v1:%s", oc);
    OnlineKey::sha256hex(pre, oh);
    JsonDocument d;
    d["t"] = "hello"; d["v"] = 1; d["fw"] = FIRMWARE_VERSION;
    d["key"] = k; d["sig"] = s;
    d["mac"] = WiFi.macAddress();
    d["name"] = Online::name();
    d["visible"] = c.vis; d["secure"] = c.sec; d["paused"] = c.paused;
    d["ownerHash"] = oh;
    // For the relay's admin log only: nothing here identifies a visitor.
    d["up"] = (uint32_t)(millis() / 1000);
    d["heap"] = (uint32_t)ESP.getMaxAllocHeap();
    d["drops"] = g_drops;
    if (g_lastDrop[0]) d["last"] = g_lastDrop;
    serializeJson(d, json);
    return true;
  }

  void onState(const char* s, const char* code, const char* url, const char* msg) override {
    if (msg && *msg) Log.printf("[online] relay says: %s\n", msg);
    if (!strcmp(s, "pending")) { setState(Online::PENDING); return; }
    if (!strcmp(s, "on")) {
      char key[24];
      {
        Lock l;
        if (strcmp(g_code, code) || strcmp(g_url, url)) {
          strlcpy(g_code, code, sizeof(g_code));
          strlcpy(g_url, url, sizeof(g_url));
          putStr("code", g_code);
          putStr("url", g_url);
        }
        snprintf(key, sizeof(key), "%s:%s:%c", g_code, g_ocode, g_cfg.sec ? 'S' : '-');
        // On paper again for a new link, a new owner code, or extra secure
        // switched on or off (its instructions are on the slip).
        if (strcmp(key, g_welc)) g_evWelcome = true;
      }
      setState(Online::ON);
      return;
    }
    if (!strcmp(s, "declined") || !strcmp(s, "off")) {
      { Lock l; g_code[0] = g_url[0] = 0; putStr("code", ""); putStr("url", ""); }
      setState(Online::DECLINED);
    }
  }

  void onNotice(const char* kind, const char* name) override {
    if (strcmp(kind, "device-request")) return;
    Lock l;
    if (g_knockN < 3) strlcpy(g_knock[g_knockN++], (name && *name) ? name : "Someone", sizeof(g_knock[0]));
    Log.printf("[online] %s asked to print here\n", name);
  }

  bool stopWanted() override { return g_restart || !g_cfg.en; }
  bool takeNewCode() override { if (!g_newCode) return false; g_newCode = false; return true; }
  Conn* loopback() override { return new LoopConn(); }
  void random(uint8_t* out, size_t n) override { OnlineKey::fill(out, n); }
  uint32_t ms() override { return millis(); }
  void pause(uint32_t m) override { delay(m); }
  void note(const char* w) override {
    Log.printf("[online] %s\n", w);
    if (!strncmp(w, "connection ended: ", 18) || !strncmp(w, "relay closed the connection: ", 29)) {
      strlcpy(g_lastDrop, w, sizeof(g_lastDrop));
      g_drops++;
    }
  }
  const char* userAgent() override { return "SmartThermal/" FIRMWARE_VERSION " (ESP32)"; }
};

// Waits up to ms, returning early if the settings change.
void waitFor(uint32_t ms) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms && !g_restart) delay(250);
}

// Reconnecting (as the relay asks): 2 s doubling to 5 min, with jitter.
// "Too many join requests waiting" (1013): at least 10 min. Hello refused
// (1008): straight to 5 min - it is a key or signature fault, and retrying
// quickly will not help.
const uint32_t BACKOFF_MIN = 2000, BACKOFF_MAX = 300000, BUSY_WAIT = 600000;

void taskFn(void*) {
  uint32_t backoff = BACKOFF_MIN;
  uint32_t declinedAt = 0;
  for (;;) {
    if (!g_cfg.en) { setState(Online::OFF); g_restart = false; declinedAt = 0; delay(1000); continue; }
    if (g_restart) { g_restart = false; declinedAt = 0; backoff = BACKOFF_MIN; }
    if (declinedAt && millis() - declinedAt < DECLINED_MS) { delay(1000); continue; }
    declinedAt = 0;
    if (WiFi.status() != WL_CONNECTED || !Scheduler::clockOk()) {
      // TLS checks certificate dates, so it needs the real time first.
      setState(Online::WAITING); delay(2000); continue;
    }
    if (!g_haveKey) {
      Log.println("[online] making this printer's identity key");
      uint8_t d[32];
      if (!OnlineKey::generate(d)) { Log.println("[online] key generation failed"); delay(10000); continue; }
      Preferences p;
      if (p.begin(NS, false)) { p.putBytes("d", d, 32); p.end(); }
      { Lock l; memcpy(g_d, d, 32); g_haveKey = true; }
    }
    if (ESP.getMaxAllocHeap() < MIN_HEAP) { delay(5000); continue; }   // a photo is in memory

    if (g_state != Online::PENDING && g_state != Online::ON) setState(Online::CONNECTING);
    char host[64];
    { Lock l; strlcpy(host, g_cfg.host, sizeof(host)); }
    const uint32_t t0 = millis();
    RelaySession::End e;
    {
      TlsConn link;
      Host h;
      RelaySession s(link, h);
      e = s.run(host, 443, RELAY_PATH);
    }
    const uint32_t lasted = millis() - t0;
    if (e == RelaySession::STOPPED) continue;
    if (e == RelaySession::DECLINED) { setState(Online::DECLINED); declinedAt = millis(); if (!declinedAt) declinedAt = 1; continue; }
    if (lasted > 60000) backoff = BACKOFF_MIN;
    uint32_t wait = backoff;
    if (e == RelaySession::BUSY)     wait = BUSY_WAIT;
    if (e == RelaySession::REJECTED) wait = BACKOFF_MAX;
    wait += esp_random() % (wait / 4 + 1);             // jitter: up to +25 %
    Log.printf("[online] connection %s after %lu s - retrying in %lu s\n",
               e == RelaySession::UNREACHABLE ? "failed" : e == RelaySession::BAD_SERVER ? "refused"
               : e == RelaySession::REJECTED ? "refused the hello (1008)"
               : e == RelaySession::BUSY ? "busy: too many join requests waiting (1013)" : "lost",
               (unsigned long)(lasted / 1000), (unsigned long)(wait / 1000));
    setState(Online::UNREACHABLE);
    waitFor(wait);
    backoff = backoff * 2 > BACKOFF_MAX ? BACKOFF_MAX : backoff * 2;
  }
}

void startTask() {
  if (g_task) return;
  xTaskCreatePinnedToCore(taskFn, "online", 16384, nullptr, 1, &g_task, 0);
}

}  // namespace

// ----------------------------------------------------------------------
// Public
// ----------------------------------------------------------------------

void Online::begin(SlipDesk& desk, PrintQueue&) {
  g_desk = &desk;
  if (!g_lock) g_lock = xSemaphoreCreateMutex();
  load();
  Log.printf("[online] online printing %s%s\n", g_cfg.en ? "on, via " : "off",
             g_cfg.en ? g_cfg.host : "");
  if (g_cfg.en) startTask();
}

void Online::loop() {
  if (!g_desk) return;
  if (g_evWelcome && !g_desk->busy(SlipDesk::ONLINE)) {
    char name[33], url[96], oc[8], key[24]; bool sec, vis;
    {
      Lock l;
      strlcpy(name, Online::name().c_str(), sizeof(name)); strlcpy(url, g_url, sizeof(url));
      strlcpy(oc, g_ocode, sizeof(oc)); sec = g_cfg.sec; vis = g_cfg.vis;
      snprintf(key, sizeof(key), "%s:%s:%c", g_code, g_ocode, g_cfg.sec ? 'S' : '-');
      g_evWelcome = false;
    }
    if (url[0]) g_desk->wantOnline(name, url, oc, sec, vis, key);
  }
  if (g_knockN && !g_desk->busy(SlipDesk::KNOCK)) {
    char who[33], name[33], url[96];
    {
      Lock l;
      strlcpy(who, g_knock[0], sizeof(who));
      for (uint8_t i = 1; i < g_knockN; i++) strlcpy(g_knock[i - 1], g_knock[i], sizeof(g_knock[0]));
      g_knockN--;
      strlcpy(name, Online::name().c_str(), sizeof(name)); strlcpy(url, g_url, sizeof(url));
    }
    g_desk->wantKnock(who, name, url);
  }
}

Online::Config Online::config() { Lock l; return g_cfg; }

bool Online::save(const Config& in, String& err) {
  Config c = in;
  if (!c.host[0]) strlcpy(c.host, RELAY_HOST, sizeof(c.host));
  for (const char* p = c.host; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') { err = "server name not valid"; return false; }
  Preferences p;
  if (!p.begin(NS, false)) { err = "storage"; return false; }
  p.putBool("en", c.en); p.putBool("vis", c.vis); p.putBool("sec", c.sec); p.putBool("pause", c.paused);
  p.putString("host", c.host);
  {
    Lock l;
    if (c.en && !g_ocode[0]) { makeOwnerCode(g_ocode); p.putString("oc", g_ocode); }
    g_cfg = c;
  }
  p.end();
  g_restart = true;
  Log.printf("[online] settings saved: %s\n", c.en ? "on" : "off");
  if (c.en) startTask();
  return true;
}

void Online::saveFromSetup(bool en, bool vis, bool sec) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putBool("en", en); p.putBool("vis", vis); p.putBool("sec", sec);
  if (en && !p.getString("oc", "").length()) { char oc[8]; makeOwnerCode(oc); p.putString("oc", oc); }
  p.end();
}

void Online::forget() {
  Preferences p;
  if (p.begin(NS, false)) { p.clear(); p.end(); }
  Lock l;
  g_cfg = Config();
  strlcpy(g_cfg.host, RELAY_HOST, sizeof(g_cfg.host));
  g_code[0] = g_url[0] = g_ocode[0] = g_welc[0] = 0;
  memset(g_d, 0, sizeof(g_d));
  g_haveKey = false;
  g_restart = true;
}

Online::State Online::state() { return g_state; }

const char* Online::stateKey() {
  switch (g_state) {
    case OFF: return "off";          case WAITING: return "waiting";
    case CONNECTING: return "connecting"; case PENDING: return "pending";
    case ON: return "on";            case DECLINED: return "declined";
    case UNREACHABLE: return "unreachable";
  }
  return "off";
}

String Online::stateText() {
  switch (g_state) {
    case OFF:         return "Off";
    case WAITING:     return "Waiting for the internet";
    case CONNECTING:  return "Connecting";
    case PENDING:     return "Waiting for approval";
    case ON:          return g_cfg.paused ? "On, but paused: visitors cannot print" : "On";
    case DECLINED:    return "Not approved";
    case UNREACHABLE: { Lock l; return String("Can't reach ") + g_cfg.host + ", retrying"; }
  }
  return "Off";
}

String Online::link()      { Lock l; return g_state == ON ? String(g_url) : String(); }
String Online::ownerCode() { Lock l; return String(g_ocode); }
String Online::mac()       { return WiFi.macAddress(); }
bool   Online::paused()    { return g_cfg.paused; }
String Online::name()      { const char* h = WiFi.getHostname(); return String(h && *h ? h : "Printer"); }

void Online::newLink() { g_newCode = true; }

void Online::newOwnerCode() {
  char oc[8];
  makeOwnerCode(oc);
  { Lock l; strlcpy(g_ocode, oc, sizeof(g_ocode)); }
  putStr("oc", oc);
  g_restart = true;           // the relay learns the new code on reconnecting
  Log.println("[online] new owner code");
}

void Online::welcomed(const char* key) {
  { Lock l; strlcpy(g_welc, key, sizeof(g_welc)); }
  putStr("welc", key);
}

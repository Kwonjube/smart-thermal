// RelaySession.h - one connection from this unit to the website's printer
// relay (v0.21). Portable: no ESP32 calls, so the same code is tested on a
// PC against a stand-in relay (tests/relay).
//
// The unit dials OUT (so it works behind any home router), over a secure
// WebSocket, and keeps the connection open. The relay sends visitors'
// requests down it, one at a time; the unit replays each one to its own web
// server on 127.0.0.1 and streams the answer back. Every handler, and the
// rule that Settings are local-only, therefore applies unchanged: replayed
// requests always carry X-Via-Relay.
//
// Protocol v1 (full text in tests/relay/PROTOCOL.md):
//   relay -> unit  {"t":"challenge","n":"<random>"}
//   unit  -> relay {"t":"hello", ...identity, signature over the challenge}
//   relay -> unit  {"t":"state","s":"pending|on|declined|off","code","url"}
//   relay -> unit  {"t":"req","id","m","p","ct","len","who","ua"} + body
//                  as binary frames totalling len
//   unit  -> relay {"t":"res","id","s","ct","cc"} + binary frames +
//                  {"t":"end","id"}
//   relay -> unit  {"t":"notice","k":"device-request","name":"..."}
//   unit  -> relay {"t":"newcode"}        owner asked for a new link
//   unit  -> relay "ping" every 45 s; relay answers "pong"

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// A byte stream: the TLS link to the relay, or plain TCP to our own server.
class Conn {
public:
  virtual ~Conn() {}
  virtual bool   open(const char* host, uint16_t port, uint32_t timeoutMs) = 0;
  // Bytes waiting now (0 = none yet), or -1 once closed with nothing left.
  virtual int    readSome(uint8_t* buf, size_t len) = 0;
  virtual size_t writeAll(const uint8_t* buf, size_t len) = 0;
  virtual bool   isOpen() = 0;
  virtual void   close() = 0;
};

// What the session needs from the unit.
class RelayHost {
public:
  virtual ~RelayHost() {}
  // The hello message, signed over the relay's challenge.
  virtual bool  hello(const char* nonce, String& json) = 0;
  virtual void  onState(const char* s, const char* code, const char* url, const char* msg) = 0;
  virtual void  onNotice(const char* kind, const char* name) = 0;
  virtual bool  stopWanted() = 0;          // switched off or settings changed
  virtual bool  takeNewCode() = 0;         // "New link" pressed
  virtual Conn* loopback() = 0;            // a fresh connection object (deleted by caller)
  virtual uint16_t localPort() { return 80; }
  virtual const char* userAgent() { return "SmartThermal"; }
  virtual void  random(uint8_t* out, size_t n) = 0;
  virtual uint32_t ms() = 0;
  virtual void  pause(uint32_t ms) = 0;
  virtual void  note(const char* what) = 0;  // one log line
};

class RelaySession {
public:
  // REJECTED: the relay closed with 1008 (it did not accept the hello or
  // its signature). BUSY: 1013, too many join requests waiting.
  enum End : uint8_t { STOPPED, LOST, UNREACHABLE, DECLINED, BAD_SERVER, REJECTED, BUSY };

  RelaySession(Conn& link, RelayHost& host) : _c(link), _h(host) {}
  End run(const char* server, uint16_t port, const char* path);
  uint16_t closeCode() const { return _closeCode; }     // 0 if none was sent

  static const uint32_t PING_MS      = 45000;
  static const uint32_t SILENCE_MS   = 120000;   // nothing heard: reconnect
  static const uint32_t MAX_BODY     = 200 * 1024;
  static const size_t   BUF          = 4096;

  // Paths the relay may never reach, whatever it asks (the unit's own
  // handlers refuse them too). Exposed for tests.
  static bool blocked(const char* method, const char* path);

private:
  Conn&      _c;
  RelayHost& _h;
  uint8_t*   _buf = nullptr;
  bool       _declined = false;
  uint16_t   _closeCode = 0;
  const char* _why = nullptr;    // why the session ended, for the log
  uint32_t   _reqStart = 0;

  bool handshake(const char* server, const char* path);
  bool readExact(uint8_t* b, size_t n, uint32_t timeoutMs);
  bool readHeader(uint8_t& op, uint64_t& len, bool& masked, uint8_t mask[4], uint32_t timeoutMs);
  bool skip(uint64_t n, bool masked);
  bool sendFrame(uint8_t op, const uint8_t* d, size_t n);
  bool sendText(const char* s) { return sendFrame(1, (const uint8_t*)s, strlen(s)); }
  bool sendJson(JsonDocument& doc);
  bool handleText(char* s, size_t n, End& end);
  bool forward(JsonObjectConst req);
  bool reply(uint32_t id, int status, const char* msg);   // a small JSON error answer
};

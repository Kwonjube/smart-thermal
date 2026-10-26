#include "RelaySession.h"
#include "B64.h"
#include <mbedtls/sha1.h>

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

static bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

// Header values from the relay are re-typed into an HTTP request, so
// anything that could end a header line (or confuse a parser) is dropped.
static void clean(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (; in && *in && o + 1 < cap; in++)
    if (*in >= 0x20 && *in <= 0x7E) out[o++] = *in;
  out[o] = 0;
}

bool RelaySession::blocked(const char* method, const char* path) {
  static const char* never[] = { "/api/settings", "/api/system", "/api/print/raw", "/api/jobs/clear",
                                 "/api/online", "/log", "/setup" };
  for (const char* p : never) if (startsWith(path, p)) return true;
  const bool post = strcmp(method, "GET") != 0;
  if (post && (startsWith(path, "/api/kiosk") || startsWith(path, "/api/clock"))) return true;
  return false;
}

bool RelaySession::readExact(uint8_t* b, size_t n, uint32_t timeoutMs) {
  const uint32_t t0 = _h.ms();
  size_t got = 0;
  while (got < n) {
    const int r = _c.readSome(b + got, n - got);
    if (r < 0) return false;
    if (r == 0) {
      if (_h.ms() - t0 > timeoutMs) return false;
      _h.pause(2);
      continue;
    }
    got += r;
  }
  return true;
}

bool RelaySession::readHeader(uint8_t& op, uint64_t& len, bool& masked, uint8_t mask[4], uint32_t timeoutMs) {
  uint8_t h[2];
  if (!readExact(h, 2, timeoutMs)) return false;
  op = h[0] & 0x0F;
  masked = h[1] & 0x80;
  len = h[1] & 0x7F;
  if (len == 126) {
    uint8_t e[2]; if (!readExact(e, 2, 10000)) return false;
    len = (e[0] << 8) | e[1];
  } else if (len == 127) {
    uint8_t e[8]; if (!readExact(e, 8, 10000)) return false;
    len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
  }
  if (masked && !readExact(mask, 4, 10000)) return false;
  return true;
}

bool RelaySession::skip(uint64_t n, bool masked) {
  (void)masked;
  while (n) {
    const size_t k = n > BUF ? BUF : (size_t)n;
    if (!readExact(_buf, k, 10000)) return false;
    n -= k;
  }
  return true;
}

// Client frames are always masked (RFC 6455).
bool RelaySession::sendFrame(uint8_t op, const uint8_t* d, size_t n) {
  uint8_t h[14];
  size_t hl = 0;
  h[hl++] = 0x80 | op;
  if (n < 126)        h[hl++] = 0x80 | n;
  else if (n < 65536) { h[hl++] = 0x80 | 126; h[hl++] = n >> 8; h[hl++] = n & 0xFF; }
  else { h[hl++] = 0x80 | 127; for (int i = 7; i >= 0; i--) h[hl++] = (i >= 4) ? 0 : (uint8_t)(n >> (8 * i)); }
  uint8_t mask[4];
  _h.random(mask, 4);
  memcpy(h + hl, mask, 4); hl += 4;
  if (_c.writeAll(h, hl) != hl) return false;
  uint8_t tmp[512];
  size_t off = 0;
  while (off < n) {
    const size_t k = (n - off) > sizeof(tmp) ? sizeof(tmp) : (n - off);
    for (size_t i = 0; i < k; i++) tmp[i] = d[off + i] ^ mask[(off + i) & 3];
    if (_c.writeAll(tmp, k) != k) return false;
    off += k;
  }
  return true;
}

bool RelaySession::sendJson(JsonDocument& doc) {
  String s;
  serializeJson(doc, s);
  return sendFrame(1, (const uint8_t*)s.c_str(), s.length());
}

// ----------------------------------------------------------------------
// Opening the WebSocket
// ----------------------------------------------------------------------

bool RelaySession::handshake(const char* server, const char* path) {
  uint8_t key[16];
  _h.random(key, sizeof(key));
  char k64[32];
  b64encode(key, sizeof(key), k64, false);

  char req[400];
  const int n = snprintf(req, sizeof(req),
    "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: %s\r\n\r\n",
    path, server, k64, _h.userAgent());
  if (n <= 0 || n >= (int)sizeof(req)) return false;
  if (_c.writeAll((const uint8_t*)req, n) != (size_t)n) return false;

  // The response head, up to the blank line.
  size_t got = 0;
  const uint32_t t0 = _h.ms();
  while (got < BUF - 1) {
    const int r = _c.readSome(_buf + got, 1);
    if (r < 0) return false;
    if (r == 0) { if (_h.ms() - t0 > 15000) return false; _h.pause(5); continue; }
    got++;
    if (got >= 4 && !memcmp(_buf + got - 4, "\r\n\r\n", 4)) break;
  }
  _buf[got] = 0;
  const char* head = (const char*)_buf;
  if (strncmp(head, "HTTP/1.1 101", 12) != 0) {
    char line[64]; clean(head, line, sizeof(line));
    char m[96]; snprintf(m, sizeof(m), "relay refused the connection: %s", line);
    _h.note(m);
    return false;
  }

  // The server must prove it read our key.
  char want[96];
  snprintf(want, sizeof(want), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", k64);
  uint8_t sha[20];
  mbedtls_sha1((const unsigned char*)want, strlen(want), sha);
  char acc[32];
  b64encode(sha, 20, acc, false);
  // Header names are case-insensitive: find it in a lower-cased copy, then
  // compare the value exactly in the original.
  static char low[1024];
  const size_t hl = got < sizeof(low) - 1 ? got : sizeof(low) - 1;
  for (size_t i = 0; i < hl; i++) low[i] = (head[i] >= 'A' && head[i] <= 'Z') ? head[i] + 32 : head[i];
  low[hl] = 0;
  const char* a = strstr(low, "sec-websocket-accept:");
  if (!a) { _h.note("relay answer had no WebSocket accept"); return false; }
  const char* v = head + (a - low) + 21;
  while (*v == ' ') v++;
  if (strncmp(v, acc, strlen(acc)) != 0) { _h.note("relay WebSocket accept did not match"); return false; }
  return true;
}

// ----------------------------------------------------------------------
// The session
// ----------------------------------------------------------------------

RelaySession::End RelaySession::run(const char* server, uint16_t port, const char* path) {
  if (!_c.open(server, port, 15000)) return UNREACHABLE;
  _buf = (uint8_t*)malloc(BUF + 1);
  if (!_buf) { _c.close(); return LOST; }
  End end = LOST;
  if (!handshake(server, path)) { end = BAD_SERVER; goto done; }

  {
    uint32_t lastRx = _h.ms(), lastPing = _h.ms();
    for (;;) {
      if (_h.stopWanted()) { sendFrame(8, nullptr, 0); end = STOPPED; break; }
      if (_h.takeNewCode()) sendText("{\"t\":\"newcode\"}");
      const uint32_t now = _h.ms();
      if (now - lastPing > PING_MS) { if (!sendText("ping")) { _why = "could not send (link dropped)"; break; } lastPing = now; }
      if (now - lastRx > SILENCE_MS) { _why = "relay silent for 2 minutes"; break; }

      uint8_t op; uint64_t len; bool masked; uint8_t mask[4];
      // Wait briefly for a frame; most of the time there is none.
      {
        const uint32_t t0 = _h.ms();
        uint8_t h0;
        int got = 0;
        while ((got = _c.readSome(&h0, 1)) == 0 && _h.ms() - t0 < 250) _h.pause(10);
        if (got < 0) { _why = "link closed without a goodbye (network or relay restart)"; break; }
        if (got == 0) continue;
        uint8_t h1;
        _why = "link dropped mid-message";
        if (!readExact(&h1, 1, 10000)) break;
        op = h0 & 0x0F; masked = h1 & 0x80; len = h1 & 0x7F;
        if (len == 126) { uint8_t e[2]; if (!readExact(e, 2, 10000)) break; len = (e[0] << 8) | e[1]; }
        else if (len == 127) { uint8_t e[8]; if (!readExact(e, 8, 10000)) break; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i]; }
        if (masked && !readExact(mask, 4, 10000)) break;
      }
      lastRx = _h.ms();
      _why = "link dropped mid-message";

      if (op == 1) {                                   // text
        if (len > BUF) { if (!skip(len, masked)) break; continue; }
        if (!readExact(_buf, (size_t)len, 10000)) break;
        if (masked) for (size_t i = 0; i < len; i++) _buf[i] ^= mask[i & 3];
        _buf[len] = 0;
        bool keep = handleText((char*)_buf, (size_t)len, end);
        if (!keep) { if (!_why || !strcmp(_why, "link dropped mid-message")) _why = "a reply could not be sent"; break; }
      } else if (op == 9) {                            // ping -> pong
        uint8_t p[125]; const size_t k = len > 125 ? 125 : (size_t)len;
        if (!readExact(p, k, 10000)) break;
        if (masked) for (size_t i = 0; i < k; i++) p[i] ^= mask[i & 3];
        sendFrame(10, p, k);
      } else if (op == 8) {                            // close: its code says why
        uint8_t p[125]; const size_t k = len > 125 ? 125 : (size_t)len;
        if (readExact(p, k, 5000) && k >= 2) {
          if (masked) for (size_t i = 0; i < k; i++) p[i] ^= mask[i & 3];
          _closeCode = (p[0] << 8) | p[1];
          char why[80]; const size_t wl = k - 2 < sizeof(why) - 1 ? k - 2 : sizeof(why) - 1;
          memcpy(why, p + 2, wl); why[wl] = 0;
          char m[120]; snprintf(m, sizeof(m), "relay closed the connection: %u %s", _closeCode, why);
          _h.note(m);
        }
        end = _declined ? DECLINED : _closeCode == 1008 ? REJECTED : _closeCode == 1013 ? BUSY : LOST;
        _why = nullptr;                                // already said
        break;
      } else {                                         // pong, stray binary
        if (!skip(len, masked)) break;
      }
    }
  }
done:
  if (end == LOST && _why) { char m[96]; snprintf(m, sizeof(m), "connection ended: %s", _why); _h.note(m); }
  _c.close();
  free(_buf); _buf = nullptr;
  if (end == LOST && _declined) end = DECLINED;
  return end;
}

// Returns false to end the session.
bool RelaySession::handleText(char* s, size_t n, End& end) {
  if (n == 4 && !memcmp(s, "pong", 4)) return true;
  JsonDocument doc;
  if (deserializeJson(doc, s, n)) return true;           // not ours: ignore
  const char* t = doc["t"] | "";

  if (!strcmp(t, "challenge")) {
    String hello;
    if (!_h.hello(doc["n"] | "", hello)) { _h.note("could not sign the hello"); return false; }
    return sendFrame(1, (const uint8_t*)hello.c_str(), hello.length());
  }
  if (!strcmp(t, "state")) {
    const char* st = doc["s"] | "";
    _h.onState(st, doc["code"] | "", doc["url"] | "", doc["msg"] | "");
    if (!strcmp(st, "declined") || !strcmp(st, "off")) { _declined = true; end = DECLINED; }
    return true;
  }
  if (!strcmp(t, "notice")) {
    _h.onNotice2(doc["k"] | "", doc["name"] | "", doc["dev"] | "", doc["loc"] | "");
    return true;
  }
  if (!strcmp(t, "owner")) {                             // v0.26
    if (!strcmp(doc["a"] | "", "off")) { _h.onOwnerOff(); }
    return true;
  }
  if (!strcmp(t, "req")) return forward(doc.as<JsonObjectConst>());
  return true;
}

bool RelaySession::reply(uint32_t id, int status, const char* msg) {
  JsonDocument r;
  r["t"] = "res"; r["id"] = id; r["s"] = status; r["ct"] = "application/json"; r["cc"] = "no-store";
  if (!sendJson(r)) return false;
  char body[160];
  char m[120]; clean(msg, m, sizeof(m));
  const int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", m);
  if (!sendFrame(2, (const uint8_t*)body, n)) return false;
  JsonDocument e; e["t"] = "end"; e["id"] = id;
  return sendJson(e);
}

// One visitor request: replayed to our own web server, answer streamed back.
bool RelaySession::forward(JsonObjectConst req) {
  _reqStart = _h.ms();
  const uint32_t id  = req["id"] | 0UL;
  const uint32_t len = req["len"] | 0UL;
  char method[8], path[640], ct[160], who[12], ua[200];
  clean(req["m"] | "", method, sizeof(method));
  clean(req["p"] | "", path, sizeof(path));
  clean(req["ct"] | "", ct, sizeof(ct));
  clean(req["who"] | "guest", who, sizeof(who));
  clean(req["ua"] | "", ua, sizeof(ua));

  const char* refuse = nullptr;
  int refuseCode = 400;
  if (strcmp(method, "GET") && strcmp(method, "POST")) refuse = "method not allowed";
  else if (path[0] != '/' || strchr(path, ' ') || strlen(path) > 600) refuse = "bad path";
  else if (len > MAX_BODY) { refuse = "too large"; refuseCode = 413; }
  else if (blocked(method, path)) { refuse = "Settings can only be changed on the printer's own network"; refuseCode = 403; }
  if (strcmp(who, "guest") && strcmp(who, "owner")) strcpy(who, "guest");

  Conn* lc = nullptr;
  if (!refuse) {
    lc = _h.loopback();
    if (!lc || !lc->open("127.0.0.1", _h.localPort(), 3000)) { refuse = "printer busy"; refuseCode = 503; }
  }
  if (lc && !refuse) {
    char head[1200];
    const int hn = snprintf(head, sizeof(head),
      "%s %s HTTP/1.0\r\nHost: localhost\r\nX-Via-Relay: %s\r\nUser-Agent: %s\r\n"
      "Content-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
      method, path, who, ua[0] ? ua : "online", ct[0] ? ct : "application/octet-stream",
      (unsigned long)len);
    if (hn <= 0 || hn >= (int)sizeof(head) || lc->writeAll((const uint8_t*)head, hn) != (size_t)hn) {
      refuse = "printer busy"; refuseCode = 503;
    }
  }

  // The body always follows as binary frames: read all of it, even when
  // refusing, so the next message starts where it should.
  uint32_t left = len;
  bool bodyOk = true;
  while (left) {
    uint8_t op; uint64_t fl; bool masked; uint8_t mask[4];
    if (!readHeader(op, fl, masked, mask, 30000)) { bodyOk = false; break; }
    if ((op != 2 && op != 0) || fl > left) { bodyOk = false; break; }
    uint64_t rem = fl;
    uint64_t pos = 0;
    while (rem) {
      const size_t k = rem > BUF ? BUF : (size_t)rem;
      if (!readExact(_buf, k, 30000)) { bodyOk = false; break; }
      if (masked) for (size_t i = 0; i < k; i++) _buf[i] ^= mask[(pos + i) & 3];
      if (lc && !refuse && lc->writeAll(_buf, k) != k) { refuse = "printer busy"; refuseCode = 503; }
      rem -= k; pos += k;
    }
    if (!bodyOk) break;
    left -= (uint32_t)fl;
  }
  if (!bodyOk) {                     // the stream is out of step: start again
    if (lc) { lc->close(); delete lc; }
    _h.note("relay request body broken - reconnecting");
    return false;
  }

  if (refuse) {
    if (lc) { lc->close(); delete lc; }
    char p0[80]; strlcpy(p0, path, sizeof(p0));
    char* q = strchr(p0, '?'); if (q) *q = 0;
    char m[200]; snprintf(m, sizeof(m), "%s %s -> %d: %s (refused here)", method, p0, refuseCode, refuse);
    _h.note(m);
    return reply(id, refuseCode, refuse);
  }

  // The answer: status line and headers, then the body until the server
  // closes (HTTP/1.0, so it is never chunked).
  size_t got = 0;
  size_t bodyAt = 0;
  uint32_t t0 = _h.ms();
  while (got < BUF) {
    const int r = lc->readSome(_buf + got, BUF - got);
    if (r < 0) break;
    if (r == 0) { if (_h.ms() - t0 > 60000) break; _h.pause(3); continue; }
    got += r;
    _buf[got < BUF ? got : BUF - 1] = 0;
    const char* e = nullptr;
    for (size_t i = 3; i < got; i++)
      if (_buf[i - 3] == '\r' && _buf[i - 2] == '\n' && _buf[i - 1] == '\r' && _buf[i] == '\n') { e = (const char*)_buf + i + 1; break; }
    if (e) { bodyAt = (const uint8_t*)e - _buf; break; }
  }
  if (!bodyAt) {
    lc->close(); delete lc;
    return reply(id, 502, "no answer from the printer");
  }

  int status = 502;
  char ctype[120] = "application/octet-stream", cc[60] = "";
  {
    // Parse a copy of the head (lower-cased) for the few headers we pass on.
    char headc[1024];
    const size_t hl = bodyAt < sizeof(headc) - 1 ? bodyAt : sizeof(headc) - 1;
    memcpy(headc, _buf, hl); headc[hl] = 0;
    if (!strncmp(headc, "HTTP/1.", 7)) status = atoi(headc + 9);
    for (char* p = headc; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    auto grab = [&](const char* name, char* out, size_t cap) {
      const char* h = strstr(headc, name);
      if (!h) return;
      h += strlen(name); while (*h == ' ') h++;
      size_t o = 0; while (*h && *h != '\r' && o + 1 < cap) out[o++] = *h++;
      out[o] = 0;
    };
    grab("\r\ncontent-type:", ctype, sizeof(ctype));
    grab("\r\ncache-control:", cc, sizeof(cc));
  }
  {
    JsonDocument r;
    r["t"] = "res"; r["id"] = id; r["s"] = status; r["ct"] = ctype;
    if (cc[0]) r["cc"] = cc;
    if (!sendJson(r)) { lc->close(); delete lc; return false; }
  }
  // One line in the unit's log per visitor request worth knowing about:
  // every POST, the page itself, and anything refused or failed (v0.22).
  {
    char p0[80]; strlcpy(p0, path, sizeof(p0));
    char* q = strchr(p0, '?'); if (q) *q = 0;
    const bool routine = !strcmp(method, "GET") && status < 400 && startsWith(p0, "/api/");
    if (!routine) {
      char err[90] = "";
      if (status >= 400 && got > bodyAt) {
        const size_t bl = got - bodyAt < 200 ? got - bodyAt : 200;
        char b[201]; memcpy(b, _buf + bodyAt, bl); b[bl] = 0;
        const char* e = strstr(b, "\"error\":\"");
        if (e) { e += 9; size_t o = 0; while (e[o] && e[o] != '"' && o < sizeof(err) - 4) { err[o + 2] = e[o]; o++; }
                 err[0] = ':'; err[1] = ' '; err[o + 2] = 0; }
      }
      char m[220];
      snprintf(m, sizeof(m), "%s %s -> %d%s (%lu KB in, %lu ms)", method, p0, status, err,
               (unsigned long)((len + 1023) / 1024), (unsigned long)(_h.ms() - _reqStart));
      _h.note(m);
    }
  }
  bool ok = true;
  if (got > bodyAt) ok = sendFrame(2, _buf + bodyAt, got - bodyAt);
  t0 = _h.ms();
  while (ok) {
    const int r = lc->readSome(_buf, BUF);
    if (r < 0) break;                                   // done
    if (r == 0) {
      if (_h.ms() - t0 > 60000) { _h.note("printer stopped answering mid-reply"); break; }
      _h.pause(2);
      continue;
    }
    t0 = _h.ms();
    ok = sendFrame(2, _buf, r);
  }
  lc->close(); delete lc;
  if (!ok) return false;
  JsonDocument e; e["t"] = "end"; e["id"] = id;
  return sendJson(e);
}

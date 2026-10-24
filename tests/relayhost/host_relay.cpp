// PC harness: runs the unit's RelaySession against the stand-in relay,
// replaying requests to the mock unit (tests/mock.js) as if it were the
// unit's own web server. Plain TCP stands in for TLS.
//   ./host_relay <relay-port> <unit-port> <seconds> [statefile]
#include "Arduino.h"
#include "../../SmartThermal/RelaySession.h"
#include "../../SmartThermal/OnlineKey.h"
#include "../../SmartThermal/B64.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

class PosixConn : public Conn {
  int fd = -1;
public:
  ~PosixConn() { close(); }
  bool open(const char* host, uint16_t port, uint32_t) override {
    addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[8]; snprintf(ps, 8, "%u", port);
    if (getaddrinfo(strcmp(host, "localhost") ? host : "127.0.0.1", ps, &hints, &res)) return false;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    bool ok = ::connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) { close(); return false; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return true;
  }
  int readSome(uint8_t* b, size_t n) override {
    if (fd < 0) return -1;
    if (!n) return 0;
    ssize_t r = ::recv(fd, b, n, 0);
    if (r > 0) return (int)r;
    if (r == 0) return -1;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  }
  size_t writeAll(const uint8_t* b, size_t n) override {
    size_t o = 0;
    while (o < n) {
      ssize_t r = ::send(fd, b + o, n - o, MSG_NOSIGNAL);
      if (r > 0) { o += r; continue; }
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { pollfd p{fd, POLLOUT, 0}; poll(&p, 1, 1000); continue; }
      return o;
    }
    return o;
  }
  bool isOpen() override { return fd >= 0; }
  void close() override { if (fd >= 0) ::close(fd); fd = -1; }
};

struct Host : RelayHost {
  uint8_t d[32];
  uint16_t unitPort;
  uint32_t until;
  std::string state, code, url;
  int notices = 0, newCodeAsks = 0;
  bool hello(const char* nonce, String& json) override {
    uint8_t pub[65], sig[64];
    if (!OnlineKey::publicKey(d, pub)) return false;
    std::string m = std::string("smartthermal-relay-v1:") + nonce;
    if (!OnlineKey::sign(d, m.c_str(), sig)) return false;
    if (getenv("BADSIG")) sig[10] ^= 1;
    char k[100], s[100], oh[65];
    b64encode(pub, 65, k, true); b64encode(sig, 64, s, true);
    OnlineKey::sha256hex("smartthermal-owner-v1:K7F3Q9", oh);
    JsonDocument doc;
    doc["t"] = "hello"; doc["v"] = 1; doc["fw"] = "0.21.0"; doc["key"] = k; doc["sig"] = s;
    doc["mac"] = "AA:BB:CC:DD:EE:FF"; doc["name"] = getenv("NAME") ? getenv("NAME") : "Test kitchen"; doc["owner"] = "Tester";
    doc["visible"] = !getenv("HIDDEN"); doc["secure"] = !!getenv("SECURE"); doc["paused"] = !!getenv("PAUSED"); doc["ownerHash"] = oh;
    if (getenv("LAST")) { doc["last"] = getenv("LAST"); doc["up"] = 9000; doc["heap"] = 51200; doc["drops"] = 3; }
    serializeJson(doc, json);
    return true;
  }
  void onState(const char* s, const char* c, const char* u, const char*) override {
    state = s; code = c; url = u;
    printf("STATE %s %s %s\n", s, c, u); fflush(stdout);
    if (newCodeAsks == 0 && state == "on" && getenv("NEWCODE")) newCodeAsks = 1;
  }
  void onNotice(const char* k, const char* n) override { notices++; printf("NOTICE %s %s\n", k, n); fflush(stdout); }
  bool stopWanted() override { return millis() > until; }
  bool takeNewCode() override { if (newCodeAsks == 1) { newCodeAsks = 2; return true; } return false; }
  Conn* loopback() override { return new PosixConn(); }
  uint16_t localPort() override { return unitPort; }
  void random(uint8_t* o, size_t n) override { OnlineKey::fill(o, n); }
  uint32_t ms() override { return millis(); }
  void pause(uint32_t m) override { delay(m); }
  void note(const char* w) override { printf("NOTE %s\n", w); fflush(stdout); }
};

int main(int argc, char** argv) {
  const uint16_t rp = argc > 1 ? atoi(argv[1]) : 8790;
  Host h; h.unitPort = argc > 2 ? atoi(argv[2]) : 8765;
  h.until = millis() + 1000 * (argc > 3 ? atoi(argv[3]) : 20);
  const char* keyfile = argc > 4 ? argv[4] : "key.bin";
  FILE* f = fopen(keyfile, "rb");
  if (!f || fread(h.d, 1, 32, f) != 32) {
    if (!OnlineKey::generate(h.d)) { puts("keygen failed"); return 1; }
    f = fopen(keyfile, "wb"); fwrite(h.d, 1, 32, f);
  }
  if (f) fclose(f);
  PosixConn link;
  RelaySession s(link, h);
  const char* names[] = { "STOPPED", "LOST", "UNREACHABLE", "DECLINED", "BAD_SERVER", "REJECTED", "BUSY" };
  const int e = s.run("localhost", rp, "/relay/connect");
  printf("END %s close=%u\n", names[e], s.closeCode());
  return 0;
}

#include "TcpTransport.h"
#include <lwip/sockets.h>
#include <sys/ioctl.h>

TcpTransport::TcpTransport(IPAddress ip, uint16_t port, uint32_t timeoutMs)
  : _ip(ip), _port(port), _timeoutMs(timeoutMs) {
  snprintf(_desc, sizeof(_desc), "tcp://%u.%u.%u.%u:%u",
           _ip[0], _ip[1], _ip[2], _ip[3], _port);
}

void TcpTransport::setTarget(IPAddress ip, uint16_t port) {
  _client.stop();
  _ip   = ip;
  _port = port;
  snprintf(_desc, sizeof(_desc), "tcp://%u.%u.%u.%u:%u",
           _ip[0], _ip[1], _ip[2], _ip[3], _port);
}

bool TcpTransport::open() {
  if (_client.connected()) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  _client.setNoDelay(true);
  return _client.connect(_ip, _port, _timeoutMs);
}

size_t TcpTransport::write(const uint8_t* data, size_t len) {
  if (!_client.connected()) return 0;
  return _client.write(data, len);
}

void TcpTransport::flush() {
  if (_client.connected()) _client.flush();
}

void TcpTransport::close() {
  _client.stop();
}

bool TcpTransport::isOpen() {
  return _client.connected();
}

const char* TcpTransport::describe() {
  return _desc;
}

// NOTE: WiFiClient::flush() on ESP32 drains the RECEIVE buffer, not the send
// buffer. It does nothing for us here, which is why a fixed delay appeared to
// be necessary and why 60 ms was not enough.
bool TcpTransport::drain(uint32_t timeoutMs) {
  if (!_client.connected()) return false;

#if defined(TIOCOUTQ)
  const int fd = _client.fd();
  if (fd < 0) { delay(FALLBACK_DRAIN_MS); return true; }

  const uint32_t started = millis();
  for (;;) {
    int pending = 0;
    if (ioctl(fd, TIOCOUTQ, &pending) != 0) {
      delay(FALLBACK_DRAIN_MS);
      return true;
    }
    if (pending == 0) return true;
    if (millis() - started > timeoutMs) return false;
    delay(5);
  }
#else
  (void)timeoutMs;
  delay(FALLBACK_DRAIN_MS);
  return true;
#endif
}

int TcpTransport::available() { return _client.available(); }
int TcpTransport::readRaw()   { return _client.read(); }

bool TcpTransport::probe() {
  WiFiClient p;
  bool ok = p.connect(_ip, _port, 1500);
  p.stop();
  return ok;
}

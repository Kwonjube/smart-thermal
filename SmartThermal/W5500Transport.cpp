#include "Log.h"
#include "W5500Transport.h"
#include "Config.h"

// Reaches into the Ethernet library's internals for one thing only: reading
// each hardware socket's state for dumpLink(). The public API exposes a
// socket's state only through the client that owns it, and the question
// being asked is precisely about sockets nobody owns any more.
#include <utility/w5100.h>

W5500Transport::W5500Transport(IPAddress printerIp, uint16_t port,
                               uint32_t timeoutMs)
  : _ip(printerIp), _port(port), _timeoutMs(timeoutMs) {
  deriveLocal();
  formatDesc();
}

// The W5500's own address: a spare host on the printer's /24. .200 unless
// the printer itself is .200. The cable carries two devices, so any free
// host number works; a fixed one keeps the logs predictable.
IPAddress W5500Transport::localFor(IPAddress p) {
  const uint8_t host = (p[3] == 200) ? 201 : 200;
  return IPAddress(p[0], p[1], p[2], host);
}

void W5500Transport::deriveLocal() { _local = localFor(_ip); }

void W5500Transport::formatDesc() {
  snprintf(_desc, sizeof(_desc), "w5500://%u.%u.%u.%u:%u (local .%u)",
           _ip[0], _ip[1], _ip[2], _ip[3], _port, _local[3]);
}

// ----------------------------------------------------------------------
// Bring-up
// ----------------------------------------------------------------------

bool W5500Transport::begin() { return bringUp(true); }

// Recovery without a reboot. Pulsing RST clears all eight hardware sockets
// and the chip's ARP state - everything an ESP32 reset button was
// clearing on the bench, minus the reboot.
bool W5500Transport::resetLink() {
  _client.stop();
  const bool ok = bringUp(false);
  if (!_quiet || !ok)
    Log.printf("[w5500] link reset: %s, link %s\n",
                  ok ? "chip back" : "CHIP NOT FOUND", _linkText);
  return ok;
}

bool W5500Transport::bringUp(bool verbose) {
  // Hardware reset. An ESP32 reset does not reset the W5500, so without
  // this a chip left mid-transaction by the previous boot can come up
  // wedged. Datasheet: hold low at least 500 us, then allow the PLL to
  // lock before talking to it.
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(2);
  digitalWrite(W5500_RST, HIGH);
  delay(60);

  SPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, W5500_CS);
  Ethernet.init(W5500_CS);

  // A real, stable MAC derived from the ESP32's own factory address, with
  // the locally-administered bit set and the multicast bit clear. A point
  // to point cable barely needs uniqueness, but a fixed made-up MAC shared
  // by every unit ever built is the kind of shortcut that bites later.
  uint8_t mac[6];
  const uint64_t base = ESP.getEfuseMac();
  for (uint8_t i = 0; i < 6; i++) mac[i] = (base >> (8 * i)) & 0xFF;
  mac[0] = (mac[0] | 0x02) & 0xFE;

  // No gateway and no DNS: the cable has exactly one other device on it.
  // The library wants values, so it is given its own address, which is
  // never used for anything on a single subnet.
  Ethernet.begin(mac, _local, _local, _local, IPAddress(255, 255, 255, 0));

  _chip = (Ethernet.hardwareStatus() == EthernetW5500);

  // Wait for the link to renegotiate before reporting it. v0.7.2 checked
  // immediately, logged "no cable" after every reset, and its link-reset
  // probe then tested a link that was not up yet - so its verdict that
  // the reset had not helped was measured too early to mean anything.
  if (_chip) {
    const uint32_t t0 = millis();
    while (Ethernet.linkStatus() != LinkON && millis() - t0 < LINK_WAIT_MS)
      delay(50);
  }
  refreshLink();
  formatDesc();

  if (verbose)
    Log.printf("[w5500] %s, mac %02X:%02X:%02X:%02X:%02X:%02X, "
                  "local %u.%u.%u.%u, link %s\n",
                  _chip ? "found" : "NOT FOUND - check wiring and CS pin",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  _local[0], _local[1], _local[2], _local[3], _linkText);
  return _chip;
}

void W5500Transport::refreshLink() {
  if (!_chip) { strlcpy(_linkText, "no W5500", sizeof(_linkText)); return; }

  switch (Ethernet.linkStatus()) {
    case LinkON:  strlcpy(_linkText, "up",       sizeof(_linkText)); break;
    case LinkOFF: strlcpy(_linkText, "no cable", sizeof(_linkText)); break;
    default:      strlcpy(_linkText, "unknown",  sizeof(_linkText)); break;
  }
}

void W5500Transport::setTarget(IPAddress ip, uint16_t port) {
  _client.stop();
  _ip   = ip;
  _port = port;
  deriveLocal();
  if (_chip) Ethernet.setLocalIP(_local);   // cheap; no re-init needed
  formatDesc();
}

// ----------------------------------------------------------------------
// Connection
// ----------------------------------------------------------------------

bool W5500Transport::open() {
  if (_client.connected()) return true;
  if (!_chip) return false;

  // An unplugged cable fails in microseconds here instead of costing a
  // full connect timeout - and the reason is named rather than guessed.
  refreshLink();
  if (Ethernet.linkStatus() == LinkOFF) return false;

  _client.setConnectionTimeout(static_cast<uint16_t>(_timeoutMs));
  if (!_client.connect(_ip, _port)) return false;

  // Free TX space on a fresh, idle socket. drain() compares against this
  // to know when every byte has been acknowledged.
  _emptyRoom = _client.availableForWrite();
  _lastRoom  = _emptyRoom;
  return true;
}

// THE WRITE PATH. Two library traps, both designed around here.
//
// 1. EthernetClient::write() can report a larger count than it sent. The
//    library copies at most one socket buffer (2 KB) per call and takes
//    its length as a 16-bit value, but reports success for the full size
//    it was asked to send. A 72 KB raster handed over whole would put 2 KB
//    on the wire and claim 72 KB - the exact silent short write that
//    printed pages of gibberish on Wi-Fi in v0.5.
//
// 2. When the socket buffer is full, the library does not return 0; it
//    waits, with no timeout, for space to free up. A printer that stops
//    reading would hang the printer task indefinitely, and writeAll()'s
//    8 s stall detector would never get to run.
//
// So each call hands over no more than there is room for, and no more than
// one buffer. That guarantees the count returned is the count sent, and
// that a full buffer returns 0 at once so writeAll() can time out cleanly.
//
// This is NOT the slicing mistake from Wi-Fi. There, cutting writes into
// 1460-byte segments with NoDelay halved throughput through lwIP. Here the
// chip's socket buffer IS 2 KB, so the library can never move more than
// that per call anyway; this only makes the limit explicit and honest.
size_t W5500Transport::write(const uint8_t* data, size_t len) {
  if (!_client.connected()) return 0;

  const int room = _client.availableForWrite();
  _lastRoom = room;
  if (room <= 0) return 0;

  size_t n = len;
  if (n > static_cast<size_t>(room)) n = static_cast<size_t>(room);
  if (n > CHUNK_BYTES)              n = CHUNK_BYTES;

  const size_t sent = _client.write(data, n);
  _lastRoom = room - static_cast<int>(sent);   // assume none acked yet: overcounts, never under
  return sent;
}

// Deliberately does nothing. EthernetClient::flush() spins until the TX
// buffer empties with no timeout at all, so a stalled printer would hang
// the task in it. drain() does the same job with a deadline.
void W5500Transport::flush() {}

// The W5500 keeps unacknowledged data in its TX buffer for retransmission,
// so free space returning to the idle figure means the PRINTER has
// acknowledged every byte - a stronger guarantee than the Wi-Fi path,
// where the send queue emptying only means the bytes left the ESP32.
//
// The timeout counts from the last PROGRESS, not from the call: a printer
// that pauses with the tail of a photo in its buffer (paper out, cover
// open, head cooling) is still alive, and the print finishes by itself
// once it reads again. While nothing moves the stall is reported, so the
// web page can say what is happening.
bool W5500Transport::drain(uint32_t timeoutMs) {
  if (!_client.connected()) return false;

  int      last  = _client.availableForWrite();
  uint32_t moved = millis();
  for (;;) {
    const int room = _client.availableForWrite();
    if (_emptyRoom > 0 && room >= _emptyRoom) { noteStall(false); return true; }
    if (!_client.connected())                 { noteStall(false); return false; }
    _lastRoom = room;
    if (room != last) { last = room; moved = millis(); noteStall(false); }
    else if (millis() - moved > 1000) noteStall(true);
    if (millis() - moved > timeoutMs)         { noteStall(false); return false; }
    delay(5);
  }
}

void W5500Transport::close() {
  // stop() sends FIN and waits this long for the printer to finish the
  // close handshake before force-closing locally. A forced close sends
  // nothing to the printer, so it can leave the printer's ONE connection
  // slot holding a half-open socket. Two seconds costs nothing after a
  // clean job and gives a slow, beeping, error-state printer time to let
  // go properly.
  _client.setConnectionTimeout(CLOSE_WAIT_MS);
  _client.stop();
}

bool W5500Transport::isOpen() { return _client.connected(); }

const char* W5500Transport::describe() { return _desc; }

int W5500Transport::available() { return _client.available(); }
int W5500Transport::readRaw()   { return _client.read(); }

// Same caveat as on Wi-Fi: this opens a second connection, and the
// printer accepts only one. The printer task probes only when idle.
bool W5500Transport::probe() {
  if (!_chip) return false;
  refreshLink();
  if (Ethernet.linkStatus() == LinkOFF) return false;

  EthernetClient p;
  p.setConnectionTimeout(1500);
  const uint32_t t0 = millis();
  const bool ok = p.connect(_ip, _port);
  _probeFailMs = ok ? 0 : (uint16_t)min<uint32_t>(millis() - t0, 60000);
  p.setConnectionTimeout(CLOSE_WAIT_MS);    // see close()
  p.stop();
  return ok;
}

// ----------------------------------------------------------------------
// Discovery support
// ----------------------------------------------------------------------

bool W5500Transport::linkUp() {
  if (!_chip) return false;
  refreshLink();
  // "Unknown" counts as up: only a definite LinkOFF means no printer.
  return Ethernet.linkStatus() != LinkOFF;
}

// The W5500 takes a spare address on the CANDIDATE's subnet for the
// attempt, then goes back to the committed one. Its own stack has no
// routing table, so an address on the wrong /24 is simply unreachable -
// that is why the local address has to move with each guess.
bool W5500Transport::probeAt(IPAddress ip, uint16_t port, uint32_t timeoutMs) {
  if (!linkUp()) return false;

  Ethernet.setLocalIP(localFor(ip));
  EthernetClient p;
  p.setConnectionTimeout(static_cast<uint16_t>(timeoutMs));
  const bool ok = p.connect(ip, port);
  p.setConnectionTimeout(CLOSE_WAIT_MS);    // see close()
  p.stop();
  Ethernet.setLocalIP(_local);
  return ok;
}

// ----------------------------------------------------------------------
// Socket dump
// ----------------------------------------------------------------------
//
// With nothing printing, every socket should read CLOSED. Anything else
// held open while the printer is unreachable is the leak - and if all
// eight are held, no new connection can ever be made until the chip is
// reset, however healthy the printer is.

static const char* socketStateName(uint8_t st) {
  switch (st) {
    case 0x00: return "closed";
    case 0x13: return "INIT";
    case 0x14: return "LISTEN";
    case 0x15: return "SYNSENT";
    case 0x16: return "SYNRECV";
    case 0x17: return "ESTABLISHED";
    case 0x18: return "FIN_WAIT";
    case 0x1A: return "CLOSING";
    case 0x1B: return "TIME_WAIT";
    case 0x1C: return "CLOSE_WAIT";
    case 0x1D: return "LAST_ACK";
    default:   return "other";
  }
}

void W5500Transport::dumpLink() {
  if (!_chip) { Log.println("[w5500] sockets: no chip"); return; }

  char line[160] = "[w5500] sockets:";
  uint8_t held = 0;
  for (uint8_t s = 0; s < MAX_SOCK_NUM; s++) {
    SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
    const uint8_t st = W5100.readSnSR(s);
    SPI.endTransaction();
    if (st != 0x00) held++;

    char cell[20];
    snprintf(cell, sizeof(cell), " %u=%s", s, socketStateName(st));
    strlcat(line, cell, sizeof(line));
  }
  Log.println(line);
  Log.printf("[w5500] %u of %u sockets held%s\n", held, (unsigned)MAX_SOCK_NUM,
                held >= MAX_SOCK_NUM ? "  <-- EXHAUSTED: no connection possible "
                                       "until the chip is reset" : "");
}

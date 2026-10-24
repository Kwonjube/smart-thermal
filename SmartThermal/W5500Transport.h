// W5500Transport.h — raw ESC/POS over a W5500, cabled straight to the printer.
//
// Requires the Arduino "Ethernet" library (Library Manager: "Ethernet",
// v2.0 or later).
//
// TOPOLOGY
// The ESP32 joins the LAN over Wi-Fi; the W5500 is a separate cable to
// the printer and nothing else. The Ethernet library runs the W5500's OWN
// hardware TCP/IP stack, independent of the ESP32's lwIP stack and its
// routing table. So there is no routing conflict to manage: the two
// networks cannot see each other even if their addresses overlap. The
// printer keeps its address; the W5500 takes a spare one on the same /24.
//
// ONE CONNECTION PER JOB, exactly as on Wi-Fi. The printer has a single
// connection slot whichever cable it arrives on.
//
// THREADING
// Every call that touches the chip is an SPI transaction. After begin(),
// only the printer task may make them - the same rule that already
// applied to the printer itself. describe() and linkState() are safe from
// any task because they return cached text and never touch SPI.

#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include "Transport.h"

class W5500Transport : public Transport {
public:
  W5500Transport(IPAddress printerIp, uint16_t port, uint32_t timeoutMs = 4000);

  // Hardware bring-up: reset pulse, SPI, chip detection, static address.
  // Call ONCE from setup(), after setTarget() and before the printer task
  // starts. Returns false if no W5500 answers on the SPI bus.
  bool begin();

  bool   open()                                 override;
  size_t write(const uint8_t* data, size_t len) override;
  void   flush()                                override;
  void   close()                                override;
  bool   isOpen()                               override;
  const char* describe()                        override;
  bool   probe()                                override;
  bool   drain(uint32_t timeoutMs)              override;

  uint32_t unacked() override {
    return (_emptyRoom > _lastRoom) ? static_cast<uint32_t>(_emptyRoom - _lastRoom) : 0;
  }

  int    available() override;
  int    readRaw()   override;

  void      setTarget(IPAddress ip, uint16_t port) override;
  IPAddress ip()   const override { return _ip; }
  uint16_t  port() const override { return _port; }

  const char* linkState() override { return _linkText; }

  // Hardware reset pulse and full re-initialisation, without rebooting the
  // ESP32. Clears every hardware socket. Printer task only.
  bool resetLink() override;
  uint16_t lastProbeFailMs() override { return _probeFailMs; }
  void     setQuiet(bool q)  override { _quiet = q; }

  bool linkUp() override;
  bool probeAt(IPAddress ip, uint16_t port, uint32_t timeoutMs) override;

  // All eight hardware socket states to Serial. Printer task only.
  void dumpLink() override;

  IPAddress localIp()   const { return _local; }
  bool      chipFound() const { return _chip; }

private:
  // Largest single hand-off to the library. See write() for why this
  // exists and why it costs nothing here, unlike slicing on Wi-Fi.
  static const size_t CHUNK_BYTES = 2048;

  // How long a close waits for the printer's side of the handshake before
  // force-closing locally. See close().
  static const uint16_t CLOSE_WAIT_MS = 2000;

  // The PHY takes 1-3 s to autonegotiate after a reset. Anything reported
  // or probed before then says "no cable" about a cable that is fine.
  static const uint32_t LINK_WAIT_MS = 4000;

  IPAddress      _ip;
  IPAddress      _local;
  uint16_t       _port;
  uint32_t       _timeoutMs;
  EthernetClient _client;

  bool _chip      = false;
  bool _quiet     = false;
  uint16_t _probeFailMs = 0;
  int  _emptyRoom = 0;          // TX free space on an idle socket
  int  _lastRoom  = 0;          // TX free space as last seen; see unacked()
  char _desc[72]     = {0};
  char _linkText[16] = "not started";

  bool bringUp(bool verbose);   // shared by begin() and resetLink()
  void deriveLocal();
  static IPAddress localFor(IPAddress printer);
  void refreshLink();           // SPI: printer task / setup() only
  void formatDesc();
};

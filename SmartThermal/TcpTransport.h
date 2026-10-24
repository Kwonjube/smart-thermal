// TcpTransport.h — raw ESC/POS over TCP (port 9100).
//
// ONE CONNECTION PER JOB. Opened at begin(), closed at end().
//
// An experiment in v0.6.2 held the socket open across jobs, on the evidence
// that six jobs down one socket printed cleanly while six jobs down six
// sockets froze on the fourth. On the bench that made things worse, not
// better: the printer has a single connection slot and no way to know the
// ESP32 has rebooted, so a held socket left the slot occupied and every job
// after a reboot failed with a write timeout after ~21 bytes until the
// printer was power-cycled. Reverted.
//
// The connection-count finding still stands and is unexplained. It is worth
// revisiting only with a way to detect and recover a stale slot.

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "Transport.h"

class TcpTransport : public Transport {
public:
  TcpTransport(IPAddress ip, uint16_t port, uint32_t timeoutMs = 4000);

  bool   open()                                 override;
  size_t write(const uint8_t* data, size_t len) override;
  void   flush()                                override;
  void   close()                                override;
  bool   isOpen()                               override;
  const char* describe()                        override;

  // Reachability probe. IMPORTANT: this opens a second TCP connection, and
  // this printer accepts only ONE at a time — so it must never run while a
  // job is in flight. Only the printer task calls it.
  bool probe() override;

  // Waits until the TCP send queue is empty before allowing close().
  // Closing early discards the tail of the stream: a job that fits the
  // ESP32's ~5.7 KB send buffer never blocks in writeAll(), so without this
  // the socket was torn down with most of the image still queued. That was
  // the cause of the 50% image printing only its top half.
  bool drain(uint32_t timeoutMs) override;

  // Replies from the printer: DLE EOT status, and the ESC v that marks the
  // end of a print.
  int available() override;
  int readRaw()   override;

  // Retarget at runtime, so a settings change does not need a reboot.
  // Closes any open connection first.
  void      setTarget(IPAddress ip, uint16_t port) override;
  IPAddress ip()   const override { return _ip; }
  uint16_t  port() const override { return _port; }

  const char* linkState() override {
    return WiFi.status() == WL_CONNECTED ? "wifi" : "wifi down";
  }

private:
  // Used only when the lwIP build cannot report the send queue length.
  static const uint32_t FALLBACK_DRAIN_MS = 1500;

  IPAddress  _ip;
  uint16_t   _port;
  uint32_t   _timeoutMs;
  WiFiClient _client;
  char       _desc[48];
};

// OnlineKey.h - the unit's online identity (v0.21).
//
// A P-256 key pair made on the unit the first time online printing is
// switched on. The private half never leaves the unit (NVS "online"); the
// relay keeps only the public half, so even a leak of the website's data
// cannot be used to pretend to be this printer. Each connection is proven
// by signing the relay's one-off challenge. Factory reset makes a new key,
// which shows up on the website as a new printer needing approval.
//
// Signatures are ECDSA P-256 over SHA-256, raw r||s (64 bytes): exactly
// what the browser/Worker WebCrypto verify() expects.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace OnlineKey {
  void fill(uint8_t* out, size_t n);                         // hardware random
  bool generate(uint8_t d[32]);
  bool publicKey(const uint8_t d[32], uint8_t pub[65]);      // uncompressed
  bool sign(const uint8_t d[32], const char* msg, uint8_t sig[64]);
  void sha256hex(const char* msg, char out[65]);
}

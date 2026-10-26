#pragma once
// An image held in up to MAX pieces of memory instead of one block
// (v0.24.6). With the online link connected the unit has no single block
// big enough for a 60+ KB photo, so it is held in pieces. The printer is
// sent exactly the same commands as for one block - the pieces only change
// where the bytes are read from.
//
// Word pieces (v0.24.11): a piece may live in the ESP32's spare IRAM heap
// (~35 KB), which faults on byte access and is only usable 32 bits at a
// time. Such pieces are marked in `word` and are only ever touched through
// aligned 32-bit loads and stores below (put, each, byteAt). Everything that
// reads or writes image bytes must go through these - never buf[i][j]
// directly.
//
// Pure code (no Arduino calls), so it is tested on the PC.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct RasterPieces {
  static const uint8_t MAX = 8;
  // No initialisers: PrintJob is calloc'd. Declare as RasterPieces x{};
  uint8_t* buf[MAX];
  uint32_t len[MAX];     // a word piece is allocated rounded up to 4 bytes
  uint8_t  count;
  uint8_t  word;         // bit i set: piece i is 32-bit access only

  bool isWord(uint8_t i) const { return (word >> i) & 1; }

  size_t total() const {
    size_t t = 0;
    for (uint8_t i = 0; i < count; i++) t += len[i];
    return t;
  }

  // 32-bit-only access, little-endian (the ESP32, and the PC tests).
  static uint8_t wordGet(const uint8_t* base, size_t o) {
    const uint32_t w = reinterpret_cast<const volatile uint32_t*>(base)[o >> 2];
    return (uint8_t)(w >> ((o & 3) * 8));
  }
  static void wordPut(uint8_t* base, size_t o, const uint8_t* src, size_t k) {
    volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(base);
    while (k) {
      if ((o & 3) == 0 && k >= 4) {                       // whole word
        p[o >> 2] = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                    ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        o += 4; src += 4; k -= 4;
      } else {                                            // edge byte: read-modify-write
        const unsigned s = (o & 3) * 8;
        const uint32_t w = p[o >> 2];
        p[o >> 2] = (w & ~(0xFFu << s)) | ((uint32_t)*src << s);
        o++; src++; k--;
      }
    }
  }

  // One byte from piece i at offset o (for the decoder's bit reader).
  uint8_t byteAt(uint8_t i, size_t o) const {
    return isWord(i) ? wordGet(buf[i], o) : buf[i][o];
  }

  // Copies src into the image at byte offset `at`, across piece borders.
  // False if it would run past the end.
  bool put(size_t at, const uint8_t* src, size_t n) {
    for (uint8_t i = 0; i < count && n; i++) {
      if (at >= len[i]) { at -= len[i]; continue; }
      const size_t k = (len[i] - at) < n ? (len[i] - at) : n;
      if (isWord(i)) wordPut(buf[i], at, src, k);
      else           memcpy(buf[i] + at, src, k);
      src += k; n -= k; at = 0;
    }
    return n == 0;
  }

  // Calls out(ptr, n) for each run of bytes [from, from + n), in order.
  // Word pieces are copied out through a small buffer first, so `out` only
  // ever sees ordinary byte memory. False if the range runs past the end.
  template <typename F>
  bool each(size_t from, size_t n, F out) const {
    for (uint8_t i = 0; i < count && n; i++) {
      if (from >= len[i]) { from -= len[i]; continue; }
      const size_t k = (len[i] - from) < n ? (len[i] - from) : n;
      if (isWord(i)) {
        uint8_t bounce[256];
        for (size_t d = 0; d < k; ) {
          const size_t m = (k - d) < sizeof(bounce) ? (k - d) : sizeof(bounce);
          for (size_t j = 0; j < m; j++) bounce[j] = wordGet(buf[i], from + d + j);
          out(bounce, m);
          d += m;
        }
      } else {
        out(buf[i] + from, k);
      }
      n -= k; from = 0;
    }
    return n == 0;
  }
};

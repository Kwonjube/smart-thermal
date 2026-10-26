#pragma once
// RasterZ.h - compressed images (trialled as v0.24.8-rz; standard from v0.25).
//
// Heatshrink-compatible decoder. The page sends compressed images to
// /api/print/rasterz: online photos (dithered with an ordered Bayer pattern,
// which compresses to ~40 %; Atkinson does not compress), and at home and
// online "Text & drawings" images (~13-30 %). Heatshrink window 2^11,
// lookahead 2^4. The unit keeps the COMPRESSED
// bytes as the job (so a reprint after a fault decodes again from the
// start) and decodes while printing, through a 2 KB window - never the
// whole image at once.
//
// Format (heatshrink, MSB-first bits): tag 1 = literal (8 bits);
// tag 0 = back-reference: W bits (distance - 1), then L bits (count - 1),
// copied from the last 2^W output bytes (initially zeros). The last byte is
// padded with zero bits.
//
// Pure code (no Arduino calls), so it is tested on the PC against the
// reference implementation (tests/host/rasterz_test.cpp).

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "RasterPieces.h"

struct RasterZ {
  static const uint8_t W = 11, L = 4;          // what the page uses
  static const size_t  WINDOW = 1u << W;       // decoder RAM: 2 KB + this struct

  // Decoder state. begin() before use; window is WINDOW bytes, caller-owned.
  struct Decoder {
    const RasterPieces* in;
    uint8_t  piece;
    uint32_t off;          // byte offset within the current piece
    uint8_t  cur;          // the byte being read
    uint8_t  bitsLeft;     // bits still unread in cur
    uint8_t* win;
    uint16_t head;
    uint16_t refLeft, refDist;
    bool     ended;        // input ran out

    void begin(const RasterPieces& src, uint8_t* window) {
      in = &src; piece = 0; off = 0; cur = 0; bitsLeft = 0;
      win = window; memset(win, 0, WINDOW); head = 0;
      refLeft = refDist = 0; ended = false;
    }

    // Whole bytes still unread (not counting the partly read one).
    size_t bytesLeft() const {
      size_t n = 0;
      for (uint8_t i = piece; i < in->count; i++)
        n += in->len[i] - (i == piece ? off : 0);
      return n;
    }

    int bits(uint8_t n) {
      int v = 0;
      while (n--) {
        if (!bitsLeft) {
          while (piece < in->count && off >= in->len[piece]) { piece++; off = 0; }
          if (piece >= in->count) { ended = true; return -1; }
          cur = in->byteAt(piece, off++); bitsLeft = 8;   // word pieces too
        }
        v = (v << 1) | ((cur >> --bitsLeft) & 1);
      }
      return v;
    }

    // Produces up to n bytes. Fewer only when the input has ended.
    size_t read(uint8_t* out, size_t n) {
      size_t got = 0;
      while (got < n) {
        uint8_t c;
        if (refLeft) {
          c = win[(uint16_t)(head - refDist) & (WINDOW - 1)];
          refLeft--;
        } else {
          const int tag = bits(1);
          if (tag < 0) break;
          if (tag) {
            const int v = bits(8);
            if (v < 0) break;
            c = (uint8_t)v;
          } else {
            const int d = bits(W);
            const int k = d < 0 ? -1 : bits(L);
            if (k < 0) break;
            refDist = (uint16_t)(d + 1);
            refLeft = (uint16_t)(k + 1);
            continue;
          }
        }
        out[got++] = c;
        win[head & (WINDOW - 1)] = c;
        head++;
      }
      return got;
    }
  };

  // Checks a whole compressed image before it is accepted: it must decode
  // to exactly `raw` bytes, with nothing left over but the last byte's
  // padding. Counts the black dots on the way (for pacing). Uses `window`
  // (WINDOW bytes) and a small stack buffer. Returns nullptr when good,
  // else the reason.
  static const char* verify(const RasterPieces& src, size_t raw,
                            uint8_t* window, uint8_t* coverage) {
    Decoder d; d.begin(src, window);
    uint8_t  buf[256];
    size_t   done = 0;
    uint64_t set  = 0;
    while (done < raw) {
      const size_t want = raw - done < sizeof(buf) ? raw - done : sizeof(buf);
      const size_t got = d.read(buf, want);
      for (size_t i = 0; i < got; i++) set += __builtin_popcount(buf[i]);
      done += got;
      if (got < want) return "compressed image is cut short";
    }
    if (d.refLeft) return "compressed image is longer than declared";
    if (d.bytesLeft()) return "compressed image is longer than declared";
    if (coverage) {
      const uint64_t bits = (uint64_t)raw * 8ULL;
      *coverage = raw ? (uint8_t)((set * 100ULL + bits / 2) / bits) : 0;
    }
    return nullptr;
  }
};

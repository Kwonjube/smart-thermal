// B64.h - base64 (standard or URL-safe, no padding for URL-safe) and hex.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Writes a NUL-terminated string; out needs 4*ceil(n/3)+1 bytes.
inline size_t b64encode(const uint8_t* in, size_t n, char* out, bool url) {
  const char* T = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                      : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0, i = 0;
  for (; i + 2 < n; i += 3) {
    const uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63]; out[o++] = T[(v >> 6) & 63]; out[o++] = T[v & 63];
  }
  if (i < n) {
    const uint32_t v = (in[i] << 16) | (i + 1 < n ? in[i + 1] << 8 : 0);
    out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63];
    if (i + 1 < n) out[o++] = T[(v >> 6) & 63]; else if (!url) out[o++] = '=';
    if (!url) out[o++] = '=';
  }
  out[o] = 0;
  return o;
}

inline void hexencode(const uint8_t* in, size_t n, char* out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = H[in[i] >> 4]; out[2 * i + 1] = H[in[i] & 15]; }
  out[2 * n] = 0;
}

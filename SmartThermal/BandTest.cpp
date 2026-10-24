#include "BandTest.h"

// 3x5 digits, drawn at 3x scale. Enough to number a ruler.
static const uint8_t DIGITS[10][5] = {
  {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
  {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};

namespace {
struct Canvas {
  uint8_t* b; uint16_t w, h, rb;
  void set(int x, int y, bool on) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t& v = b[y * rb + (x >> 3)];
    const uint8_t m = 0x80 >> (x & 7);
    if (on) v |= m; else v &= ~m;
  }
  void rect(int x, int y, int ww, int hh, bool on) {
    for (int j = 0; j < hh; j++) for (int i = 0; i < ww; i++) set(x + i, y + j, on);
  }
  void digit(int x, int y, uint8_t d) {
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < 3; c++)
        if (DIGITS[d][r] & (4 >> c)) rect(x + c * 3, y + r * 3, 3, 3, true);
  }
  void number(int x, int y, uint16_t n) {
    if (n >= 100) { digit(x, y, n / 100); x += 12; }
    if (n >= 10)  { digit(x, y, (n / 10) % 10); x += 12; }
    digit(x, y, n % 10);
  }
};
}

static void rulers(Canvas& c) {
  for (int mm = 0; mm <= c.h / 8; mm += 5) {
    const int y = mm * 8;
    const int len10 = (mm % 10 == 0) ? 22 : 10;
    c.rect(0, y, len10, 2, true);
    c.rect(c.w - len10, y, len10, 2, true);
    if (mm % 10 == 0 && y + 18 < c.h) {
      c.number(2, y + 4, mm);
      c.number(c.w - (mm >= 100 ? 38 : 26), y + 4, mm);
    }
  }
}

uint8_t* BandTest::field(uint16_t W, uint16_t H, uint8_t level, size_t& len) {
  static const uint8_t B8[8][8] = {
    { 0,32, 8,40, 2,34,10,42},{48,16,56,24,50,18,58,26},
    {12,44, 4,36,14,46, 6,38},{60,28,52,20,62,30,54,22},
    { 3,35,11,43, 1,33, 9,41},{51,19,59,27,49,17,57,25},
    {15,47, 7,39,13,45, 5,37},{63,31,55,23,61,29,53,21}};
  const uint16_t rb = W / 8;
  len = static_cast<size_t>(rb) * H;
  uint8_t* buf = static_cast<uint8_t*>(calloc(1, len));
  if (!buf) return nullptr;
  Canvas c = { buf, W, H, rb };
  const int margin = 44;
  for (int y = 0; y < H; y++)
    for (int x = margin; x < W - margin; x++)
      if (B8[y & 7][x & 7] < level) c.set(x, y, true);
  rulers(c);
  return buf;
}

const char* BandTest::label(uint8_t p) {
  return p == 0 ? "A  FULL width, 128 mm, ONE command\n"
                : "B  HALF width, 128 mm, ONE command\n";
}

uint16_t BandTest::chunkRows(uint8_t) { return PANEL_ROWS; }
uint16_t BandTest::width(uint8_t p)   { return p == 0 ? 576 : 288; }

uint8_t* BandTest::build(uint8_t p, size_t& len) {
  const uint16_t W = width(p);
  const uint16_t rb = W / 8, H = PANEL_ROWS;
  len = static_cast<size_t>(rb) * H;
  uint8_t* buf = static_cast<uint8_t*>(calloc(1, len));
  if (!buf) return nullptr;
  Canvas c = { buf, W, H, rb };

  // Even ~30% grey: a 4x4 ordered pattern, identical on every repeat, so
  // any change in density down the paper is the printer's doing.
  static const uint8_t BAYER[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
  const int margin = 44;                       // ruler strip each side
  for (int y = 0; y < H; y++)
    for (int x = margin; x < W - margin; x++)
      if (BAYER[y & 3][x & 3] < 5) c.set(x, y, true);

  // Rulers: a tick every 5 mm, a long tick and number every 10 mm.
  for (int mm = 0; mm <= H / 8; mm += 5) {
    const int y = mm * 8;
    const int len10 = (mm % 10 == 0) ? 22 : 10;
    c.rect(0, y, len10, 2, true);
    c.rect(W - len10, y, len10, 2, true);
    if (mm % 10 == 0 && y + 18 < H) {
      c.number(2, y + 4, mm);
      c.number(W - (mm >= 100 ? 38 : 26), y + 4, mm);
    }
  }
  return buf;
}

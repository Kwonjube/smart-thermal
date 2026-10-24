#include "MonoBitmap.h"

// Standard 8x8 Bayer matrix, used for the ordered-dither gradient.
static const uint8_t BAYER8[8][8] = {
  {  0, 32,  8, 40,  2, 34, 10, 42 },
  { 48, 16, 56, 24, 50, 18, 58, 26 },
  { 12, 44,  4, 36, 14, 46,  6, 38 },
  { 60, 28, 52, 20, 62, 30, 54, 22 },
  {  3, 35, 11, 43,  1, 33,  9, 41 },
  { 51, 19, 59, 27, 49, 17, 57, 25 },
  { 15, 47,  7, 39, 13, 45,  5, 37 },
  { 63, 31, 55, 23, 61, 29, 53, 21 }
};

MonoBitmap::MonoBitmap(uint16_t widthDots, uint16_t heightRows)
  : _w(widthDots), _h(heightRows), _rowBytes((widthDots + 7) / 8) {
  _buf = static_cast<uint8_t*>(calloc(size(), 1));
}

MonoBitmap::~MonoBitmap() {
  free(_buf);
  _buf = nullptr;
}

void MonoBitmap::clear() { if (_buf) memset(_buf, 0x00, size()); }
void MonoBitmap::fill()  { if (_buf) memset(_buf, 0xFF, size()); }

void MonoBitmap::invert() {
  if (!_buf) return;
  const size_t n = size();
  for (size_t i = 0; i < n; i++) _buf[i] = ~_buf[i];
}

void MonoBitmap::setPixel(uint16_t x, uint16_t y, bool black) {
  if (!_buf || x >= _w || y >= _h) return;
  uint8_t* b = &_buf[static_cast<size_t>(y) * _rowBytes + (x >> 3)];
  const uint8_t mask = 0x80 >> (x & 7);
  if (black) *b |= mask; else *b &= ~mask;
}

bool MonoBitmap::getPixel(uint16_t x, uint16_t y) const {
  if (!_buf || x >= _w || y >= _h) return false;
  return _buf[static_cast<size_t>(y) * _rowBytes + (x >> 3)] & (0x80 >> (x & 7));
}

// --- primitives ---------------------------------------------------------
void MonoBitmap::hLine(uint16_t x, uint16_t y, uint16_t len, bool black) {
  for (uint16_t i = 0; i < len; i++) setPixel(x + i, y, black);
}

void MonoBitmap::vLine(uint16_t x, uint16_t y, uint16_t len, bool black) {
  for (uint16_t i = 0; i < len; i++) setPixel(x, y + i, black);
}

void MonoBitmap::rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black) {
  if (w == 0 || h == 0) return;
  hLine(x, y,         w, black);
  hLine(x, y + h - 1, w, black);
  vLine(x,         y, h, black);
  vLine(x + w - 1, y, h, black);
}

void MonoBitmap::rectFilled(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black) {
  for (uint16_t r = 0; r < h; r++) hLine(x, y + r, w, black);
}

// --- patterns -----------------------------------------------------------
void MonoBitmap::checkerboard(uint16_t squareSize) {
  if (squareSize == 0) squareSize = 1;
  for (uint16_t y = 0; y < _h; y++) {
    for (uint16_t x = 0; x < _w; x++) {
      const bool black = ((x / squareSize) + (y / squareSize)) & 1;
      setPixel(x, y, black);
    }
  }
}

void MonoBitmap::verticalStripes(uint16_t onDots, uint16_t offDots) {
  const uint16_t period = onDots + offDots;
  if (period == 0) return;
  for (uint16_t x = 0; x < _w; x++) {
    if ((x % period) < onDots) vLine(x, 0, _h, true);
  }
}

void MonoBitmap::horizontalStripes(uint16_t onDots, uint16_t offDots) {
  const uint16_t period = onDots + offDots;
  if (period == 0) return;
  for (uint16_t y = 0; y < _h; y++) {
    if ((y % period) < onDots) hLine(0, y, _w, true);
  }
}

void MonoBitmap::diagonal(uint16_t spacing) {
  if (spacing == 0) spacing = 1;
  for (uint16_t y = 0; y < _h; y++) {
    for (uint16_t x = 0; x < _w; x++) {
      if (((x + y) % spacing) == 0) setPixel(x, y, true);
    }
  }
}

void MonoBitmap::uniformField(uint8_t percent) {
  if (percent > 100) percent = 100;

  // Threshold count out of 64. A dot is black where the matrix value falls
  // below it, so the coverage is exactly level/64 over every 8x8 tile and
  // therefore over the whole field once it is a multiple of 8 dots wide.
  const uint8_t level = static_cast<uint8_t>((percent * 64 + 50) / 100);

  for (uint16_t y = 0; y < _h; y++)
    for (uint16_t x = 0; x < _w; x++)
      setPixel(x, y, BAYER8[y & 7][x & 7] < level);
}

void MonoBitmap::orderedGradient() {
  // Left edge white, right edge black, dithered through the Bayer matrix.
  // This is the single most useful diagnostic for thermal print density.
  for (uint16_t y = 0; y < _h; y++) {
    for (uint16_t x = 0; x < _w; x++) {
      const uint8_t level     = (static_cast<uint32_t>(x) * 64) / (_w ? _w : 1);
      const uint8_t threshold = BAYER8[y & 7][x & 7];
      setPixel(x, y, level > threshold);
    }
  }
}

// MonoBitmap.h — Section 7 (/image).
//
// A packed 1-bit-per-pixel buffer in exactly the layout ESC/POS GS v 0
// expects: MSB first, left to right, top to bottom, set bit = black.
//
// Memory cost is (width/8) x height bytes. At 576 dots that is 72 bytes
// per row, so a 256-row band costs 18 KB — comfortable on a WROOM-32
// with no PSRAM, which is why the design streams in bands rather than
// holding whole images.

#pragma once
#include <Arduino.h>

class MonoBitmap {
public:
  MonoBitmap(uint16_t widthDots, uint16_t heightRows);
  ~MonoBitmap();

  bool valid() const { return _buf != nullptr; }

  uint16_t width()    const { return _w; }
  uint16_t height()   const { return _h; }
  uint16_t rowBytes() const { return _rowBytes; }
  size_t   size()     const { return static_cast<size_t>(_rowBytes) * _h; }
  const uint8_t* data() const { return _buf; }

  void clear();                                   // all white
  void fill();                                    // all black
  void setPixel(uint16_t x, uint16_t y, bool black = true);
  bool getPixel(uint16_t x, uint16_t y) const;
  void invert();

  // --- primitives -------------------------------------------------------
  void hLine(uint16_t x, uint16_t y, uint16_t len, bool black = true);
  void vLine(uint16_t x, uint16_t y, uint16_t len, bool black = true);
  void rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black = true);
  void rectFilled(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black = true);

  // --- patterns ---------------------------------------------------------
  void checkerboard(uint16_t squareSize);
  void verticalStripes(uint16_t onDots, uint16_t offDots);
  void horizontalStripes(uint16_t onDots, uint16_t offDots);
  void diagonal(uint16_t spacing);
  void orderedGradient();                         // 8x8 Bayer ramp, left to right

  // A flat field of a KNOWN coverage, dithered through the same Bayer
  // matrix. Coverage is what decides whether the printer faults, so the
  // thermal soak needs a pattern whose coverage is exact rather than
  // approximately grey: at 8x8 the matrix holds 64 thresholds, so the
  // realised coverage is the requested percentage rounded to 1/64
  // (about 1.6 points).
  void uniformField(uint8_t percent);

private:
  uint16_t _w, _h, _rowBytes;
  uint8_t* _buf = nullptr;
};

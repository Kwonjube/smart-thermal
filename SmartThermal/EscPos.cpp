#include "EscPos.h"

// --- ASCII control codes ------------------------------------------------
static const uint8_t LF  = 0x0A;
static const uint8_t DLE = 0x10;
static const uint8_t EOT = 0x04;
static const uint8_t DC2 = 0x12;
static const uint8_t ESC = 0x1B;
static const uint8_t GS  = 0x1D;

// --- private emit helpers ----------------------------------------------
void EscPos::emit(uint8_t a)                                     { _t.writeAll(&a, 1); }
void EscPos::emit(uint8_t a, uint8_t b)                          { uint8_t d[2] = {a,b};       _t.writeAll(d,2); }
void EscPos::emit(uint8_t a, uint8_t b, uint8_t c)               { uint8_t d[3] = {a,b,c};     _t.writeAll(d,3); }
void EscPos::emit(uint8_t a, uint8_t b, uint8_t c, uint8_t dd)   { uint8_t d[4] = {a,b,c,dd};  _t.writeAll(d,4); }

uint8_t EscPos::clampScale(uint8_t v) {
  if (v < 1) return 1;
  if (v > 8) return 8;
  return v;
}

// --- session ------------------------------------------------------------
void EscPos::initialise() { emit(ESC, '@'); }

// --- layout -------------------------------------------------------------
void EscPos::align(Align a)              { emit(ESC, 'a', static_cast<uint8_t>(a)); }
void EscPos::lineSpacing(uint8_t dots)   { emit(ESC, '3', dots); }
void EscPos::defaultLineSpacing()        { emit(ESC, '2'); }

void EscPos::leftMargin(uint16_t dots) {
  emit(GS, 'L', dots & 0xFF, (dots >> 8) & 0xFF);
}

void EscPos::printAreaWidth(uint16_t dots) {
  emit(GS, 'W', dots & 0xFF, (dots >> 8) & 0xFF);
}

// --- character styling --------------------------------------------------
void EscPos::font(Font f)                { emit(ESC, 'M', static_cast<uint8_t>(f)); }
void EscPos::bold(bool on)               { emit(ESC, 'E', on ? 1 : 0); }
void EscPos::underline(uint8_t weight)   { emit(ESC, '-', weight > 2 ? 2 : weight); }
void EscPos::invert(bool on)             { emit(GS,  'B', on ? 1 : 0); }
void EscPos::upsideDown(bool on)         { emit(ESC, '{', on ? 1 : 0); }
void EscPos::codePage(uint8_t page)      { emit(ESC, 't', page); }

void EscPos::textScale(uint8_t w, uint8_t h) {
  const uint8_t n = ((clampScale(w) - 1) << 4) | (clampScale(h) - 1);
  emit(GS, '!', n);
}

// --- content ------------------------------------------------------------
void EscPos::write(const char* s) {
  if (s && *s) _t.writeAll(reinterpret_cast<const uint8_t*>(s), strlen(s));
}

void EscPos::newline() { emit(LF); }

void EscPos::writeLine(const char* s) {
  write(s);
  newline();
}

// --- paper --------------------------------------------------------------
void EscPos::feedLines(uint8_t lines)  { emit(ESC, 'd', lines); }
void EscPos::feedDots(uint8_t dots)    { emit(ESC, 'J', dots); }

void EscPos::cut(CutMode mode, uint8_t feed) {
  emit(GS, 'V', static_cast<uint8_t>(mode), feed);
}

// --- raster graphics ----------------------------------------------------
void EscPos::rasterBitImage(RasterMode mode,
                            uint16_t widthBytes,
                            uint16_t rows,
                            const uint8_t* data,
                            size_t len) {
  // Header and pixels must both land in full. If the header goes and the
  // pixels are cut short, the printer is left waiting mid-image and reads
  // whatever follows as image data.
  if (!rasterHeader(mode, widthBytes, rows)) return;
  if (data && len) _t.writeAll(data, len);
}

bool EscPos::rasterHeader(RasterMode mode, uint16_t widthBytes, uint16_t rows) {
  const uint8_t header[8] = {
    GS, 'v', '0',
    static_cast<uint8_t>(mode),
    static_cast<uint8_t>(widthBytes & 0xFF),
    static_cast<uint8_t>((widthBytes >> 8) & 0xFF),
    static_cast<uint8_t>(rows & 0xFF),
    static_cast<uint8_t>((rows >> 8) & 0xFF)
  };

  return _t.writeAll(header, sizeof(header));
}

void EscPos::rasterBlank(uint16_t widthBytes, uint16_t rows) {
  const uint8_t header[8] = {
    GS, 'v', '0', 0,
    static_cast<uint8_t>(widthBytes & 0xFF),
    static_cast<uint8_t>((widthBytes >> 8) & 0xFF),
    static_cast<uint8_t>(rows & 0xFF),
    static_cast<uint8_t>((rows >> 8) & 0xFF)
  };
  if (!_t.writeAll(header, sizeof(header))) return;
  static const uint8_t ZERO[256] = {0};
  size_t left = static_cast<size_t>(widthBytes) * rows;
  while (left) {                    // a failure is tracked as shortfall
    const size_t n = left < sizeof(ZERO) ? left : sizeof(ZERO);
    _t.writeAll(ZERO, n);
    left -= n;
  }
}

// --- queries ------------------------------------------------------------
void EscPos::requestStatus(uint8_t n) {
  if (n < 1 || n > 4) return;
  emit(DLE, EOT, n);
}

void EscPos::requestPrintedAck() { emit(ESC, 'v'); }

// --- density candidates -------------------------------------------------
void EscPos::densityEpson(int8_t level) {
  if (level < -6) level = -6;
  if (level >  6) level =  6;
  const uint8_t m = static_cast<uint8_t>(level);     // -6 -> 0xFA
  const uint8_t d[7] = { GS, '(', 'K', 0x02, 0x00, 0x31, m };
  _t.writeAll(d, sizeof(d));
}

void EscPos::densityDc2(uint8_t n)   { emit(DC2, '#', n); }
void EscPos::densityGsBar(uint8_t n) { emit(GS, '|', n); }

// --- escape hatch -------------------------------------------------------
void EscPos::raw(const uint8_t* data, size_t len) {
  if (data && len) _t.writeAll(data, len);
}

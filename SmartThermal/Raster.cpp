#include "Raster.h"

Raster::Raster(Printer& printer) : _p(printer) {}

bool Raster::printBand(const uint8_t* data,
                       uint16_t widthDots,
                       uint16_t rows,
                       RasterMode mode) {
  if (!data || rows == 0 || widthDots == 0) return false;
  if (!_p.ok()) return false;

  // Clipping silently would produce a mysteriously wrong print. Refuse.
  // Note this is validated against the loaded PAPER, not the printhead:
  // a 576-dot image on a 57 mm roll is an error, not something to trim.
  if (widthDots > _p.printableDots()) return false;

  const uint16_t rowBytes = (widthDots + 7) / 8;
  _p.escpos().rasterBitImage(mode, rowBytes, rows, data,
                             static_cast<size_t>(rowBytes) * rows);
  return true;
}

bool Raster::print(const MonoBitmap& bmp, RasterMode mode, uint16_t chunkRows) {
  _chunks = 0;
  _bytes  = 0;
  _durationMs = 0;

  if (!bmp.valid()) return false;
  if (!_p.ok())     return false;
  if (bmp.width() > _p.printableDots()) return false;

  if (chunkRows == 0) chunkRows = _p.rasterChunkRows();
  if (chunkRows == 0) chunkRows = 128;

  const uint32_t started  = millis();
  const uint16_t rowBytes = bmp.rowBytes();

  for (uint16_t y = 0; y < bmp.height(); y += chunkRows) {
    const uint16_t rows = min<uint16_t>(chunkRows, bmp.height() - y);
    const uint8_t* band = bmp.data() + static_cast<size_t>(y) * rowBytes;

    if (!printBand(band, bmp.width(), rows, mode)) return false;

    _chunks++;
    _bytes += static_cast<uint32_t>(rowBytes) * rows + 8;   // + command header

    if (_delayMs) delay(_delayMs);
  }

  _durationMs = millis() - started;
  return true;
}

// Chunk height for a tiled field: what the printer's buffer allows, rounded
// down to whole bands so every repeat starts on the same pattern phase.
uint16_t Raster::tiledChunk(const MonoBitmap& band, uint16_t totalRows) const {
  const uint16_t bh = band.height();
  if (!bh) return totalRows;
  uint16_t chunk = _p.chunkRowsFor(band.width(), totalRows);
  if (chunk > bh) chunk -= chunk % bh;
  else            chunk  = bh;
  return chunk;
}

uint16_t Raster::tiledCommands(const MonoBitmap& band, uint16_t totalRows) const {
  const uint16_t chunk = tiledChunk(band, totalRows);
  return chunk ? (totalRows + chunk - 1) / chunk : 0;
}

bool Raster::printTiled(const MonoBitmap& band, uint16_t totalRows) {
  _chunks = 0;
  _bytes  = 0;

  if (!band.valid() || !totalRows || !_p.ok()) return false;
  if (band.width() > _p.printableDots())       return false;

  const uint16_t rowBytes = band.rowBytes();
  const uint16_t chunk    = tiledChunk(band, totalRows);

  for (uint16_t y = 0; y < totalRows; y += chunk) {
    const uint16_t rows = min<uint16_t>(chunk, totalRows - y);

    // Header only: nullptr/0 declares the command and its length without
    // pixels, which then follow as repeats of the band.
    _p.escpos().rasterBitImage(RasterMode::Normal, rowBytes, rows, nullptr, 0);

    uint16_t left = rows;
    while (left) {
      const uint16_t n = min<uint16_t>(left, band.height());
      _p.escpos().raw(band.data(), static_cast<size_t>(rowBytes) * n);
      left -= n;
    }

    _chunks++;
    _bytes += static_cast<uint32_t>(rowBytes) * rows + 8;
  }
  return !_p.writeFailed();
}

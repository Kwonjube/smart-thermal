#pragma once
// Mem.h - memory figures that match what malloc() can actually give, and
// the spare 32-bit-only IRAM used to hold images (v0.24.11, v0.25).
//
// ESP.getFreeHeap() / getMaxAllocHeap() count MALLOC_CAP_INTERNAL, which on
// the ESP32 includes IRAM that can only be read and written 32 bits at a
// time; malloc() never returns it for byte buffers. Decisions based on those
// figures were ~35 KB optimistic and ran byte memory out (25 Oct). Use
// free8() / block8() instead.
//
// "IRAM" here means only the pure IRAM heap (0x40080000-0x4009FFFF), found
// once at boot by probeIram(). MALLOC_CAP_EXEC also covers the dual-mapped
// D/IRAM, which is the same physical memory as ordinary RAM - taking that
// "as IRAM" just takes ordinary RAM (seen 25 Oct as "lowest ever 1K").
// iramTake() therefore refuses anything outside the pure range.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <soc/soc.h>

namespace Mem {
  inline size_t free8()   { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
  inline size_t block8()  { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

  inline size_t g_iramPure = 0;     // biggest pure-IRAM block, found at boot
  inline bool   g_iramBusy = false; // an image is holding it

  inline bool isPureIram(const void* p) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a >= SOC_IRAM_LOW && a < SOC_DIRAM_IRAM_LOW;
  }

  // Once at boot, before Wi-Fi: the biggest block that lands in pure IRAM.
  // A request too big for it lands in D/IRAM instead, so try sizes down.
  inline void probeIram() {
    for (size_t n = 64 * 1024; n >= 4096; n -= 1024) {
      void* p = heap_caps_malloc(n, MALLOC_CAP_EXEC);
      if (!p) continue;
      const bool pure = isPureIram(p);
      heap_caps_free(p);
      if (pure) { g_iramPure = n; return; }
    }
  }

  inline size_t iramFree() { return g_iramBusy ? 0 : g_iramPure; }

  // n bytes of pure IRAM (32-bit access only), or nullptr. Never takes
  // ordinary RAM: the request is sized to fit the pure heap, and anything
  // that lands elsewhere is given straight back.
  inline uint8_t* iramTake(size_t n) {
    if (g_iramBusy || !n || n > g_iramPure) return nullptr;
    void* p = heap_caps_malloc((n + 3) & ~static_cast<size_t>(3), MALLOC_CAP_EXEC);
    if (p && !isPureIram(p)) { heap_caps_free(p); return nullptr; }
    if (p) g_iramBusy = true;
    return static_cast<uint8_t*>(p);
  }

  // Frees any image piece, IRAM or not.
  inline void release(void* p) {
    if (!p) return;
    if (isPureIram(p)) g_iramBusy = false;
    heap_caps_free(p);
  }
}

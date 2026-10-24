#include "Log.h"
#include "ThermalProfile.h"
#include <Preferences.h>
#include <time.h>

static const char* NS  = "thermprof";
static const char* KEY = "p";

void ThermalProfile::reset() {
  memset(this, 0, sizeof(*this));
  version = VERSION;
}

uint32_t ThermalProfile::gapMs(uint8_t coveragePct, uint16_t limit, uint32_t rows) {
  if (!limit || !coveragePct || !rows) return 0;
  const uint32_t basis   = (rows * 1000UL) / BASIS_ROWS_PER_SEC;
  const uint32_t product = static_cast<uint32_t>(coveragePct) * 100UL;
  if (product <= limit) return 0;
  return static_cast<uint32_t>(
    (static_cast<uint64_t>(basis) * (product - limit)) / limit);
}

uint16_t ThermalProfile::limitFor(uint8_t cov) const {
  // Collect the usable points; the trainer writes them in ascending order.
  int8_t lo = -1, hi = -1;
  for (uint8_t i = 0; i < POINTS; i++) {
    if (!limit[i]) continue;
    if (coverage[i] <= cov) lo = i;
    if (coverage[i] >= cov && hi < 0) hi = i;
  }
  if (lo < 0 && hi < 0) return 0;
  if (lo < 0) return limit[hi];              // below the lowest point
  if (hi < 0) return limit[lo];              // above the highest point
  if (lo == hi || coverage[hi] == coverage[lo]) return limit[lo];

  const int32_t span = coverage[hi] - coverage[lo];
  const int32_t pos  = cov - coverage[lo];
  return static_cast<uint16_t>(
    limit[lo] + ((static_cast<int32_t>(limit[hi]) - limit[lo]) * pos) / span);
}

void ThermalProfile::print() const {
  if (!valid) {
    Log.println("[profile] none trained - pacing uses the single thermal "
                   "limit from Settings");
    return;
  }

  char when[24] = "clock not set";
  if (trainedEpoch) {
    const time_t t = trainedEpoch;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
  }

  Log.printf("[profile] trained %s on firmware %s\n", when, firmware);
  if (rowsPerSec)
    Log.printf("[profile] paper %u rows/s (%.1f mm/s), %u ms label+feed+cut "
                  "per slip\n",
                  rowsPerSec, rowsPerSec / 8.0, overheadMs);

  // What each limit means in practice, for a 128 mm print.
  const uint32_t slip = rowsPerSec
                      ? overheadMs + (1024UL * 1000UL) / rowsPerSec
                      : (1024UL * 1000UL) / BASIS_ROWS_PER_SEC;
  Log.println("[profile]  coverage  limit  rest after 128 mm  prints/min");
  for (uint8_t i = 0; i < POINTS; i++) {
    if (!limit[i]) {
      Log.printf("[profile]    %3u%%     -    not measured\n", coverage[i]);
      continue;
    }
    const uint32_t gap = gapMs(coverage[i], limit[i], 1024);
    Log.printf("[profile]    %3u%%   %4u   %6lu ms           %5.1f\n",
                  coverage[i], limit[i], (unsigned long)gap,
                  60000.0 / (slip + gap));
  }
}

// ----------------------------------------------------------------------
// Storage
// ----------------------------------------------------------------------

bool ThermalProfileStore::load(ThermalProfile& out) {
  out.reset();
  Preferences p;
  if (!p.begin(NS, true)) return false;
  const size_t len = p.getBytesLength(KEY);
  bool ok = false;
  if (len == sizeof(ThermalProfile)) {
    ThermalProfile tmp;
    p.getBytes(KEY, &tmp, sizeof(tmp));
    if (tmp.version == ThermalProfile::VERSION) { out = tmp; ok = true; }
  }
  p.end();
  return ok;
}

bool ThermalProfileStore::save(const ThermalProfile& prof) {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  const bool ok = p.putBytes(KEY, &prof, sizeof(prof)) == sizeof(prof);
  p.end();
  return ok;
}

void ThermalProfileStore::clear() {
  Preferences p;
  if (p.begin(NS, false)) { p.clear(); p.end(); }
}

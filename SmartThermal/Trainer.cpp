#include "Log.h"
#include "Trainer.h"

#if BUILD_TRAINER
#include <Preferences.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "Raster.h"
#include "MonoBitmap.h"
#include "ThermalProfile.h"

namespace {

// ----------------------------------------------------------------------
// The plan
// ----------------------------------------------------------------------

// Coverage points. Photographs average well under 70%; 95% covers the
// darkest artwork anyone will send. Realised values differ slightly - the
// Bayer field quantises to 1/64 - and the realised value is what is stored.
const uint8_t COVERAGES[ThermalProfile::POINTS] = { 30, 50, 65, 80, 95 };

const uint16_t TRIAL_ROWS    = 1024;   // 128 mm: the length every past
                                       // measurement used
const uint16_t TRIAL_PRINTS  = 30;     // the 80% pauses began at print 19,
                                       // so fewer would miss them
const uint16_t VERIFY_PRINTS = 40;

// Search. Starts above anything measured clean and steps down 15% per
// failed trial. A failing trial stops at its first pause, so descending
// from the top wastes little paper; only the final, clean trial runs in
// full. The value stored is the first limit that ran clean - up to 15%
// below the true edge, which is margin, not error.
// 1300 for the Normal-density run. At Dark, 30% and 50% passed at the old
// start of 1100 first time, so their true edge was never found; Normal
// puts less heat into each dot and should tolerate more. Stepping 15%
// down from 1300 lands on 1105 and 939 - almost exactly the Dark results
// (1100 clean/fault, 935 clean) - so the two runs compare directly.
const uint16_t START_LIMIT = 1300;
const uint8_t  STEP_PCT    = 85;
const uint16_t MIN_LIMIT   = 250;

// Every trial starts from a cooled head, so trials are comparable. The
// 80% pauses arrived sooner each time (prints 19, 27, 34, 40) - heat
// builds in the printer's body over minutes, not just in the head.
const uint32_t COOLDOWN_MS = 150000;

// A thermal pause lasted 30-35 s on the bench. The ack wait must outlast
// one comfortably, or a pause would be mistaken for a dead printer.
const uint32_t ACK_TIMEOUT_MS = 60000;

// A print this much slower than expected is a thermal pause. Normal
// variation is about 1 ms; a pause is about 30 000 ms.
const uint32_t PAUSE_MARGIN_MS = 3000;

// After a write failure, how long to wait for the printer to come back by
// itself (a thermal fault does, in ~30 s) before asking for a human.
const uint32_t SELF_RECOVER_MS = 120000;

// Link resets while waiting. The v0.8.0 run showed a healthy printer left
// unreachable until the W5500 was reset - and the trainer's wait loops
// never reset it, so it waited forever. A reset only sticks once the
// printer is back, so it is repeated.
const uint32_t RESET_EVERY_MS       = 30000;   // while it may recover itself
const uint32_t HUMAN_RESET_EVERY_MS = 60000;   // while waiting for a person

// Mechanics: low coverage so heat cannot distort the timing.
const uint8_t  MECH_COVERAGE = 20;
const uint16_t MECH_SHORT    = 256;
const uint16_t MECH_LONG     = 1024;
const uint8_t  MECH_REPS     = 3;

enum Phase : uint8_t { PH_MECHANICS = 0, PH_MAP = 1, PH_VERIFY = 2, PH_DONE = 3 };

struct State {
  uint8_t  version;
  uint8_t  phase;
  uint8_t  idx;          // coverage point being mapped
  uint16_t limit;        // next limit to try at that point
  uint16_t trials;
  uint16_t prints;
  uint16_t pauses;       // thermal pauses provoked so far
  uint16_t interrupts;   // times a human had to step in
  ThermalProfile prof;   // the result, as far as it has got
};
const uint8_t STATE_VERSION = 1;
const char*   STATE_NS  = "trainer";
const char*   STATE_KEY = "s";

volatile bool g_stop  = false;
// Set by the 'O' key: the stop that is being waited out was the paper
// running out, not heat. Without it, a stop that needs a person is taken
// as the edge having been crossed.
volatile bool g_paper = false;

enum Outcome   { CLEAN, PAUSED, INTERRUPTED, STOPPED };
enum SlipResult { SLIP_OK, SLIP_NOCONNECT, SLIP_WRITEFAIL, SLIP_NOACK };

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

// Sleeps in slices so a stop request is honoured within a quarter second.
bool sleepFor(uint32_t ms) {
  const uint32_t start = millis();
  while (millis() - start < ms) {
    if (g_stop) return false;
    const uint32_t left = ms - (millis() - start);
    vTaskDelay(pdMS_TO_TICKS(left > 250 ? 250 : left));
  }
  return !g_stop;
}

uint8_t realisedCoverage(const MonoBitmap& band) {
  uint32_t set = 0;
  const uint8_t* d = band.data();
  for (size_t i = 0; i < band.size(); i++) set += __builtin_popcount(d[i]);
  const uint64_t bits = static_cast<uint64_t>(band.size()) * 8ULL;
  return static_cast<uint8_t>((static_cast<uint64_t>(set) * 100ULL + bits / 2) / bits);
}

// Expected slip time once mechanics are measured; the basis before then.
uint32_t expectedSlipMs(const ThermalProfile& prof, uint32_t rows) {
  if (prof.rowsPerSec) return prof.overheadMs + (rows * 1000UL) / prof.rowsPerSec;
  return (rows * 1000UL) / ThermalProfile::BASIS_ROWS_PER_SEC + 600;
}

uint32_t epochNow() {
  const time_t t = time(nullptr);
  return t > 1609459200 ? static_cast<uint32_t>(t) : 0;
}

bool loadState(State& st) {
  Preferences p;
  if (!p.begin(STATE_NS, true)) return false;
  bool ok = false;
  if (p.getBytesLength(STATE_KEY) == sizeof(State)) {
    p.getBytes(STATE_KEY, &st, sizeof(st));
    ok = (st.version == STATE_VERSION &&
          st.prof.version == ThermalProfile::VERSION &&
          st.phase < PH_DONE);
  }
  p.end();
  return ok;
}

void saveState(const State& st) {
  Preferences p;
  if (p.begin(STATE_NS, false)) { p.putBytes(STATE_KEY, &st, sizeof(st)); p.end(); }
}

void clearState() {
  Preferences p;
  if (p.begin(STATE_NS, false)) { p.clear(); p.end(); }
}

void freshState(State& st) {
  memset(&st, 0, sizeof(st));
  st.version = STATE_VERSION;
  st.phase   = PH_MECHANICS;
  st.limit   = START_LIMIT;
  st.prof.reset();
  for (uint8_t i = 0; i < ThermalProfile::POINTS; i++)
    st.prof.coverage[i] = COVERAGES[i];
}

// ----------------------------------------------------------------------
// One slip, timed to the paper stopping
// ----------------------------------------------------------------------

SlipResult printSlip(Printer& p, const MonoBitmap& band, uint16_t rows,
                     const char* label, uint32_t& ms) {
  ms = 0;
  if (!p.begin()) return SLIP_NOCONNECT;

  Raster raster(p);
  p.align(Align::Left).font(Font::B).line(label).normal();
  p.align(Align::Center);

  const uint32_t t0 = millis();
  raster.printTiled(band, rows);
  p.align(Align::Left).feed(1).cutIfEnabled();

  const bool failed = p.writeFailed();
  // Sent AFTER the cut: the reply cannot come until the paper has stopped.
  const bool got = !failed && p.waitPrinted(ACK_TIMEOUT_MS);
  ms = millis() - t0;
  p.end();

  if (failed) return SLIP_WRITEFAIL;
  if (!got)   return SLIP_NOACK;
  return SLIP_OK;
}

// ----------------------------------------------------------------------
// When the printer stops answering
// ----------------------------------------------------------------------

void sayWhatToDo() {
  Log.println("[train] >>> PRINTER HAS STOPPED and did not recover by itself.");
  Log.println("[train] >>> Most likely heat: switch it OFF, wait 5 s, ON.");
  Log.println("[train] >>> If it RAN OUT OF PAPER instead: press 'O' first,");
  Log.println("[train] >>> reload and close the cover, and wait 2 minutes -");
  Log.println("[train] >>> power-cycle only if it has not resumed by then.");
  Log.println("[train] >>> Training resumes by itself. 'X' stops and saves.");
}

// A reachable printer, once the stream is resynchronised. Pays back any
// pixel bytes still owed from the interrupted slip: needed if the printer
// recovered by itself (it is still mid-image), harmless if it was power-
// cycled (a fresh printer ignores NUL bytes).
bool reconnectClean(Printer& p) {
  if (!p.recoverStream()) return false;
  p.disconnect();
  return true;
}

// One probe, with a link reset first if it is due.
bool probeWithReset(Printer& p, uint32_t& lastReset, uint32_t every) {
  if (p.probe()) return true;
  if (millis() - lastReset < every) return false;
  lastReset = millis();
  Log.println("[train] resetting the W5500 link");
  return p.resetLink() && p.probe();
}

// Waits for a human to fix the printer. True once it is back with paper;
// false if a stop was requested.
bool waitForHuman(Printer& p, State& st) {
  st.interrupts++;
  saveState(st);
  sayWhatToDo();

  uint32_t lastSaid  = millis();
  uint32_t lastReset = millis();
  const uint32_t since = millis();
  for (;;) {
    if (!sleepFor(10000)) return false;

    if (probeWithReset(p, lastReset, HUMAN_RESET_EVERY_MS)) {
      if (!reconnectClean(p)) continue;

      // Reachable and resynchronised. Make sure it has paper.
      bool paperOk = true;
      if (p.begin()) {
        uint8_t b;
        if (p.status(4, b, 500) && Printer::statusValid(b) && (b & 0x60))
          paperOk = false;
        p.end();
      }
      if (paperOk) {
        Log.printf("[train] printer is back after %lu s - resuming\n",
                      (unsigned long)((millis() - since) / 1000));
        return sleepFor(3000);
      }
      Log.println("[train] printer answers but reports PAPER OUT - reload it");
    }

    if (millis() - lastSaid > 60000) { sayWhatToDo(); lastSaid = millis(); }
  }
}

// A slip failed outright. Tells a thermal fault (it comes back by itself)
// from a paper-out (it does not).
Outcome handleTrouble(Printer& p, State& st, SlipResult r) {
  Log.printf("[train] slip failed (%s) - waiting up to %lu s to see whether "
                "the printer recovers by itself\n",
                r == SLIP_WRITEFAIL ? "write failed"
                : r == SLIP_NOACK   ? "no completion reply"
                                    : "no connection",
                (unsigned long)(SELF_RECOVER_MS / 1000));

  const uint32_t start = millis();
  uint32_t lastReset = start;
  while (millis() - start < SELF_RECOVER_MS) {
    if (!sleepFor(5000)) return STOPPED;
    if (probeWithReset(p, lastReset, RESET_EVERY_MS) && reconnectClean(p)) {
      // Came back with no human: a thermal fault. reconnectClean() has paid
      // back the pixel bytes the printer was still waiting for.
      Log.printf("[train] recovered by itself after %lu s - counted as a "
                    "thermal fault\n", (unsigned long)((millis() - start) / 1000));
      return PAUSED;
    }
  }
  // Not back: needs a person. If a print was under way when it stopped, the
  // printer faulted at this limit hard enough to wedge - that IS the edge,
  // so it counts as thermal at once. v0.8.1 retried the same limit first,
  // which cost a second fault and a second power cycle at every point.
  // The exceptions: the paper ran out ('O'), or it was unreachable before
  // the print began - neither says anything about this limit.
  g_paper = false;
  if (!waitForHuman(p, st)) return STOPPED;
  const bool thermal = (r != SLIP_NOCONNECT) && !g_paper;
  if (!thermal)
    Log.println(g_paper ? "[train] paper-out noted - the same trial runs again"
                           : "[train] was unreachable before printing - the same "
                             "trial runs again");
  g_paper = false;
  return thermal ? PAUSED : INTERRUPTED;
}

// ----------------------------------------------------------------------
// A trial: N back-to-back prints at one coverage and one limit
// ----------------------------------------------------------------------

Outcome trial(Printer& p, State& st, uint8_t coverage, uint16_t limit,
              uint16_t prints, const char* tag, uint16_t& pausedAt) {
  pausedAt = 0;

  MonoBitmap band(p.printableDots(), 128);
  if (!band.valid()) { Log.println("[train] out of memory"); return STOPPED; }
  band.uniformField(coverage);
  const uint8_t  realised = realisedCoverage(band);
  const uint32_t gap      = ThermalProfile::gapMs(realised, limit, TRIAL_ROWS);
  const uint32_t expected = expectedSlipMs(st.prof, TRIAL_ROWS);

  Log.printf("[train] %s: %u%% ink, limit %u -> rest %lu ms, %u prints "
                "(cooling %lu s first)\n",
                tag, realised, limit, (unsigned long)gap, prints,
                (unsigned long)(COOLDOWN_MS / 1000));
  if (!sleepFor(COOLDOWN_MS)) return STOPPED;

  st.trials++;
  for (uint16_t i = 1; i <= prints; i++) {
    char label[48];
    snprintf(label, sizeof(label), "TRAIN %s %u%% L%u %u/%u",
             tag, realised, limit, i, prints);

    uint32_t ms;
    const SlipResult r = printSlip(p, band, TRIAL_ROWS, label, ms);
    st.prints++;

    if (r != SLIP_OK) {
      const Outcome o = handleTrouble(p, st, r);
      if (o == PAUSED) { st.pauses++; pausedAt = i; }
      return o;
    }

    const bool pause = ms > expected + PAUSE_MARGIN_MS;
    Log.printf("[train]   %2u/%u  %5lu ms%s\n", i, prints, (unsigned long)ms,
                  pause ? "  <-- THERMAL PAUSE" : "");
    if (pause) {
      st.pauses++;
      pausedAt = i;
      return PAUSED;
    }

    // Rest from the paper stopping - which is exactly when the ack came.
    if (i < prints && !sleepFor(gap)) return STOPPED;
  }
  return CLEAN;
}

// A trial interrupted twice in a row at the same point and limit is not
// bad luck with paper - a roll lasts hundreds of prints - it is the limit
// faulting the printer hard enough to need a person. Treated as a thermal
// result, so the search steps down instead of retrying the same limit for
// ever. Held in RAM: a reboot starts the count again, which costs at most
// one repeat.
bool repeatedInterrupt(uint8_t idx, uint16_t limit) {
  static uint8_t  lastIdx   = 0xFF;
  static uint16_t lastLimit = 0;
  static uint8_t  count     = 0;
  if (idx == lastIdx && limit == lastLimit) count++;
  else { lastIdx = idx; lastLimit = limit; count = 1; }
  if (count < 2) return false;
  count = 0;
  Log.println("[train] interrupted twice at the same limit - treating it as "
                 "a thermal fault and stepping down");
  return true;
}

// ----------------------------------------------------------------------
// Phase 1: mechanics
// ----------------------------------------------------------------------

uint32_t median3(uint32_t a, uint32_t b, uint32_t c) {
  if (a > b) { const uint32_t t = a; a = b; b = t; }
  if (b > c) { const uint32_t t = b; b = c; c = t; }
  if (a > b) { const uint32_t t = a; a = b; b = t; }
  return b;
}

// Two lengths, three of each, at low coverage. Slip time is a straight
// line in the row count: the slope is the paper speed, the intercept the
// fixed label + feed + cut time.
bool mechanics(Printer& p, State& st) {
  Log.println("[train] PHASE 1 - MECHANICS: real paper speed and cut time");

  MonoBitmap band(p.printableDots(), 128);
  if (!band.valid()) { Log.println("[train] out of memory"); return false; }
  band.uniformField(MECH_COVERAGE);

  for (;;) {                       // repeats only if a human had to step in
    uint32_t shortMs[MECH_REPS] = {0}, longMs[MECH_REPS] = {0};
    bool clean = true;

    for (uint8_t r = 0; r < MECH_REPS && clean; r++) {
      for (uint8_t k = 0; k < 2 && clean; k++) {
        const uint16_t rows = k ? MECH_LONG : MECH_SHORT;
        char label[40];
        snprintf(label, sizeof(label), "TRAIN MECH %u rows %u/%u",
                 rows, r + 1, MECH_REPS);

        uint32_t ms;
        const SlipResult res = printSlip(p, band, rows, label, ms);
        st.prints++;
        if (res != SLIP_OK) {
          const Outcome o = handleTrouble(p, st, res);
          if (o == STOPPED) return false;
          clean = false;           // measure again from the top
          break;
        }
        (k ? longMs : shortMs)[r] = ms;
        Log.printf("[train]   %4u rows  %5lu ms\n", rows, (unsigned long)ms);

        const uint32_t gap = ThermalProfile::gapMs(MECH_COVERAGE, 1000, rows);
        if (!sleepFor(gap + 500)) return false;
      }
    }
    if (!clean) continue;

    const uint32_t tS = median3(shortMs[0], shortMs[1], shortMs[2]);
    const uint32_t tL = median3(longMs[0],  longMs[1],  longMs[2]);

    // ms per row, scaled x1000 to keep precision in integers.
    const uint32_t spanRows = MECH_LONG - MECH_SHORT;
    if (tL <= tS) {
      Log.println("[train] mechanics inconsistent (long not slower than short) "
                     "- keeping the basis constants");
      return true;
    }
    const uint32_t rowsPerSec = (spanRows * 1000UL) / (tL - tS);
    const int32_t  overhead   = static_cast<int32_t>(tS)
                              - static_cast<int32_t>((MECH_SHORT * 1000UL) / rowsPerSec);

    if (rowsPerSec < 400 || rowsPerSec > 4000 || overhead < 0 || overhead > 3000) {
      Log.printf("[train] mechanics out of range (%lu rows/s, %ld ms) - keeping "
                    "the basis constants\n", (unsigned long)rowsPerSec, (long)overhead);
      return true;
    }

    st.prof.rowsPerSec = static_cast<uint16_t>(rowsPerSec);
    st.prof.overheadMs = static_cast<uint16_t>(overhead);
    Log.printf("[train] paper %lu rows/s = %.1f mm/s; label+feed+cut %ld ms\n",
                  (unsigned long)rowsPerSec, rowsPerSec / 8.0, (long)overhead);
    Log.printf("[train] a 128 mm slip therefore takes %lu ms\n",
                  (unsigned long)expectedSlipMs(st.prof, 1024));
    return true;
  }
}

// ----------------------------------------------------------------------
// The run
// ----------------------------------------------------------------------

void banner(const char* s) {
  Log.println();
  Log.println("==============================================================");
  Log.printf ("  %s\n", s);
  Log.println("==============================================================");
}

bool runPhases(Printer& p, State& st) {
  // --- 1. mechanics ----------------------------------------------------
  if (st.phase == PH_MECHANICS) {
    if (!mechanics(p, st)) return false;
    st.phase = PH_MAP;
    st.idx   = 0;
    st.limit = START_LIMIT;
    saveState(st);
  }

  // --- 2. map ------------------------------------------------------------
  if (st.phase == PH_MAP) {
    Log.println("[train] PHASE 2 - MAP: safe limit at each coverage");

    while (st.idx < ThermalProfile::POINTS) {
      uint16_t pausedAt;
      char tag[12];
      snprintf(tag, sizeof(tag), "MAP%u", st.idx + 1);

      Outcome o = trial(p, st, COVERAGES[st.idx], st.limit,
                        TRIAL_PRINTS, tag, pausedAt);

      if (o == STOPPED) { saveState(st); return false; }

      if (o == INTERRUPTED) {
        if (repeatedInterrupt(st.idx, st.limit)) {
          o = PAUSED;                        // the limit is the cause
        } else {
          saveState(st);                     // paper, most likely: same again
          continue;
        }
      }

      if (o == CLEAN) {
        Log.printf("[train] %u%% CLEAN at limit %u - recorded\n",
                      COVERAGES[st.idx], st.limit);
        st.prof.limit[st.idx] = st.limit;
      } else {                               // PAUSED
        const uint16_t next = static_cast<uint16_t>((st.limit * STEP_PCT) / 100);
        Log.printf("[train] %u%% paused at print %u with limit %u - trying %u\n",
                      COVERAGES[st.idx], pausedAt, st.limit, next);
        if (next >= MIN_LIMIT) {
          st.limit = next;
          saveState(st);
          continue;
        }
        Log.printf("[train] %u%% never ran clean down to %u - recorded as %u "
                      "and flagged\n", COVERAGES[st.idx], MIN_LIMIT, MIN_LIMIT);
        st.prof.limit[st.idx] = MIN_LIMIT;
      }

      // Next point. Denser prints never tolerate MORE than lighter ones, so
      // the search there starts no higher than the limit just found - which
      // saves the trials that could only fail.
      const uint16_t found = st.prof.limit[st.idx];
      st.idx++;
      st.limit = found < START_LIMIT ? found : START_LIMIT;
      saveState(st);
    }
    st.phase = PH_VERIFY;
    saveState(st);
  }

  // --- 3. verify ---------------------------------------------------------
  if (st.phase == PH_VERIFY) {
    // The hardest point that has a real (not floor) result.
    int8_t hard = -1;
    for (int8_t i = ThermalProfile::POINTS - 1; i >= 0; i--)
      if (st.prof.limit[i] > MIN_LIMIT) { hard = i; break; }

    if (hard < 0) {
      Log.println("[train] PHASE 3 - VERIFY skipped: no point ran clean");
    } else {
      Log.printf("[train] PHASE 3 - VERIFY: %u prints at %u%%, limit %u\n",
                    VERIFY_PRINTS, COVERAGES[hard], st.prof.limit[hard]);
      for (;;) {
        uint16_t pausedAt;
        Outcome o = trial(p, st, COVERAGES[hard], st.prof.limit[hard],
                          VERIFY_PRINTS, "VERIFY", pausedAt);
        if (o == STOPPED) { saveState(st); return false; }
        if (o == INTERRUPTED) {
          if (repeatedInterrupt(0x80 | hard, st.prof.limit[hard])) o = PAUSED;
          else { saveState(st); continue; }
        }
        if (o == CLEAN) {
          Log.println("[train] verify CLEAN - the profile holds past 30 prints");
        } else {
          // Heat that builds beyond 30 prints. Take the step and say so.
          const uint16_t lower =
            static_cast<uint16_t>((st.prof.limit[hard] * STEP_PCT) / 100);
          Log.printf("[train] verify PAUSED at print %u - heat builds beyond 30 "
                        "prints. Lowering %u%% to %u; denser points follow.\n",
                        pausedAt, COVERAGES[hard], lower);
          st.prof.limit[hard] = lower;
          for (uint8_t i = hard + 1; i < ThermalProfile::POINTS; i++)
            if (st.prof.limit[i] > lower) st.prof.limit[i] = lower;
        }
        break;
      }
    }
    st.phase = PH_DONE;
  }
  return true;
}

}  // namespace

// ----------------------------------------------------------------------
// Public
// ----------------------------------------------------------------------

void Trainer::requestStop() { g_stop = true; }

void Trainer::notePaperOut() {
  g_paper = true;
  Log.println("[train] noted: this stop is PAPER, not heat");
}

bool Trainer::hasSavedProgress() {
  State st;
  return loadState(st);
}

void Trainer::discardProgress() { clearState(); }

void Trainer::run(Printer& p, void*) {
  g_stop = false;

  State st;
  const bool resumed = loadState(st);
  if (!resumed) freshState(st);

  banner(resumed ? "TRAINER - resuming saved progress" : "TRAINER - starting");
  if (resumed)
    Log.printf("[train] phase %u, point %u, next limit %u; %u prints and %u "
                  "pauses so far\n", st.phase, st.idx + 1, st.limit,
                  st.prints, st.pauses);
  else {
    Log.println("[train] Unattended. Expect about 40-60 m of paper (have a");
    Log.println("[train] second roll ready) and roughly 2 hours. It will");
    Log.println("[train] deliberately cause several beeping thermal pauses");
    Log.println("[train] to find the edge - that is expected and harmless.");
    Log.println("[train] 'X' stops and saves; 'T' later resumes.");
  }

  // The trainer needs the cut: the after-cut ack is its only clock.
  const bool savedCut = p.autoCut();
  p.setAutoCut(true);
  saveState(st);

  // A slip interrupted before a reboot left no record of the pixel bytes
  // the printer still wants - the shortfall lives in RAM. Power-cycle the
  // printer before resuming after a reflash, or its first slip may garble.
  if (!p.probe() && !waitForHuman(p, st)) {
    p.setAutoCut(savedCut);
    Log.println("[train] stopped before starting - progress saved");
    return;
  }

  const bool finished = runPhases(p, st);
  p.setAutoCut(savedCut);

  if (!finished) {
    Log.printf("[train] stopped - progress saved (%u prints, %u pauses). "
                  "'T' resumes.\n", st.prints, st.pauses);
    return;
  }

  st.prof.valid        = 1;
  st.prof.trainedEpoch = epochNow();
  strlcpy(st.prof.firmware, FIRMWARE_VERSION, sizeof(st.prof.firmware));

  // Store the realised coverages the limits were measured at.
  for (uint8_t i = 0; i < ThermalProfile::POINTS; i++) {
    MonoBitmap probe(p.printableDots(), 8);
    if (probe.valid()) {
      probe.uniformField(COVERAGES[i]);
      st.prof.coverage[i] = realisedCoverage(probe);
    }
  }

  const bool saved = ThermalProfileStore::save(st.prof);
  clearState();

  banner("TRAINER - COMPLETE");
  Log.printf("[train] %u prints, %u trials, %u thermal pauses, %u human "
                "interventions\n", st.prints, st.trials, st.pauses, st.interrupts);
  st.prof.print();
  Log.println(saved
    ? "[train] profile SAVED. Flash the lend-out build (BUILD_TRAINER 0) - it "
      "keeps this profile."
    : "[train] profile could NOT be saved to NVS - copy the table above");
}

#endif  // BUILD_TRAINER

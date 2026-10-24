#include "Log.h"
#include "Diagnostics.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "Raster.h"
#include "MonoBitmap.h"

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

// Same arithmetic as PrintQueue's pacing, repeated here so the probes pace
// themselves without reaching into the queue.
static uint32_t printMsFor(const Printer& p, uint32_t rows) {
  uint16_t s10 = p.profile().paperSpeedMmPerSec10;
  if (!s10) s10 = 935;
  return (rows * 1000UL) / ((static_cast<uint32_t>(s10) * 8UL) / 10UL);
}

// The thermal rest for a print, at the measured ceiling of 1000.
static uint32_t restMsFor(uint32_t printMs, uint8_t coverage) {
  const uint32_t product = static_cast<uint32_t>(coverage) * 100UL;
  return product <= 1000 ? 0 : (printMs * (product - 1000)) / 1000;
}

static void rest(uint32_t ms) {
  if (ms) vTaskDelay(pdMS_TO_TICKS(ms));
}

static void banner(const char* title) {
  Log.println();
  Log.println("==============================================================");
  Log.printf ("  %s\n", title);
  Log.println("==============================================================");
}

// Plain-English reading of one status byte. Standard ESC/POS bit meanings;
// the probe exists partly to learn whether this printer follows them.
static void describeStatus(uint8_t n, uint8_t b, char* out, size_t len) {
  out[0] = 0;
  auto add = [&](const char* s) {
    if (out[0]) strlcat(out, ", ", len);
    strlcat(out, s, len);
  };

  switch (n) {
    case 1:
      if (b & 0x08) add("OFFLINE");
      if (b & 0x20) add("waiting for recovery");
      break;
    case 2:
      if (b & 0x04) add("COVER OPEN");
      if (b & 0x08) add("feed button held");
      if (b & 0x20) add("STOPPED: PAPER END");
      if (b & 0x40) add("ERROR");
      break;
    case 3:
      if (b & 0x08) add("CUTTER ERROR");
      if (b & 0x20) add("UNRECOVERABLE ERROR");
      if (b & 0x40) add("auto-recoverable error (head temperature?)");
      break;
    case 4:
      if (b & 0x0C) add("paper NEAR END");
      if (b & 0x60) add("PAPER OUT");
      break;
  }
  if (!out[0]) strlcpy(out, "ok", len);
}

static const char* REG_NAME[5] = {
  "", "printer", "offline cause", "error cause", "paper sensor"
};

// ----------------------------------------------------------------------
// l  LINK
// ----------------------------------------------------------------------

void Diagnostics::link(Printer& p, void*) {
  banner("LINK");

  const bool reachable = p.probe();
  Log.printf("  link      : %s\n", p.linkState());
  Log.printf("  reachable : %s\n", reachable ? "yes" : "NO");
  if (!reachable) {
    Log.println("  Printer does not accept a connection. Check the cable, that");
    Log.println("  the printer is on, and that its IP matches the settings.");
    Log.println("  Socket state (the evidence for a stuck link):");
    p.dumpLink();
    Log.println("  If it stays unreachable, press 'e' to reset the link.");
    return;
  }

  if (!p.begin()) { Log.println("  open failed"); return; }

  // Round-trip on a real-time query: how fast the printer answers at all.
  uint32_t best = 0xFFFFFFFF, worst = 0;
  uint8_t  answered = 0;
  for (uint8_t i = 0; i < 10; i++) {
    uint8_t b;
    const uint32_t t0 = millis();
    if (p.status(1, b, 500)) {
      const uint32_t ms = millis() - t0;
      if (ms < best)  best  = ms;
      if (ms > worst) worst = ms;
      answered++;
    }
  }
  p.end();

  if (answered)
    Log.printf("  status    : %u/10 answered, round trip %lu-%lu ms\n",
                  answered, (unsigned long)best, (unsigned long)worst);
  else
    Log.println("  status    : NO ANSWER to any status query");
  p.dumpLink();
}

// ----------------------------------------------------------------------
// e  LINK RESET
// ----------------------------------------------------------------------
//
// The deliberate version of what the recovery ladder does on its own
// after a minute. For testing the stuck-link case directly: when the
// printer is unreachable, press this INSTEAD of the ESP32 reset button.
// If the printer comes back, the stuck state was on the ESP32's side and
// the ladder will handle it unattended.

void Diagnostics::resetLink(Printer& p, void*) {
  banner("LINK RESET");
  Log.println("  Before:");
  p.dumpLink();
  const bool before = p.probe();
  Log.printf("  reachable before: %s\n", before ? "yes" : "NO");

  if (!p.resetLink()) {
    Log.println("  This link has nothing to reset (Wi-Fi build).");
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(500));

  Log.println("  After:");
  p.dumpLink();
  const bool after = p.probe();
  Log.printf("  reachable after : %s\n", after ? "yes" : "NO");

  Log.println();
  if (!before && after)
    Log.println("  RESULT: the reset FIXED it - the stuck side was the ESP32's.");
  else if (!before && !after)
    Log.println("  RESULT: still unreachable - the stuck side is the PRINTER.");
  else
    Log.println("  RESULT: it was reachable already; nothing to prove this time.");
}

// ----------------------------------------------------------------------
// r  READ PATH
// ----------------------------------------------------------------------
//
// Three things in one pass, each depending on the one before:
//   1. Does the printer answer DLE EOT at all, over this link?
//   2. Does ESC v come back, and at once on an idle printer?
//   3. Across five identical slips, is print time steady? The PC harness
//      saw an sd under 1 ms, which is what made banding detectable by
//      timing. If the ESP32 sees the same, the trainer can do it too.

void Diagnostics::readPath(Printer& p, void*) {
  banner("READ PATH - can the ESP32 hear the printer?");
  Log.println("  Prints 5 short slips (about 35 cm of paper).");

  if (!p.begin()) { Log.println("  cannot open the printer - run 'l' first"); return; }

  // --- 1. status registers ---------------------------------------------
  Log.println();
  Log.println("  1. STATUS REGISTERS (DLE EOT)");
  uint8_t answers = 0, valid = 0;
  char text[96];

  for (uint8_t n = 1; n <= 4; n++) {
    uint8_t b = 0;
    const uint32_t t0 = millis();
    if (p.status(n, b, 500)) {
      answers++;
      const bool ok = Printer::statusValid(b);
      if (ok) valid++;
      describeStatus(n, b, text, sizeof(text));
      Log.printf("     %u %-14s 0x%02X  %lu ms  %s%s\n",
                    n, REG_NAME[n], b, (unsigned long)(millis() - t0),
                    ok ? "" : "[unexpected pattern] ", text);
    } else {
      Log.printf("     %u %-14s no reply\n", n, REG_NAME[n]);
    }
  }

  // --- 2. ESC v on an idle printer --------------------------------------
  Log.println();
  Log.println("  2. PRINT-DONE ACK (ESC v) on an idle printer");
  uint32_t waited = 0;
  const bool ack = p.waitPrinted(2000, &waited);
  Log.printf("     %s after %lu ms\n", ack ? "reply" : "NO REPLY",
                (unsigned long)waited);
  p.end();

  if (!answers && !ack) {
    Log.println();
    Log.println("  RESULT: the ESP32 hears NOTHING from the printer.");
    Log.println("  Stop here and send this log - the failsafe and the");
    Log.println("  trainer both depend on hearing it.");
    return;
  }

  // --- 3. the ack, before and after the cut ---------------------------
  //
  // v0.7.2: an ack sent straight after the raster came back on ARRIVAL at
  // every size, 18 to 73 KB - the printer parses ahead of the paper. The PC
  // harness got a true print-end ack because it sent the ack AFTER THE CUT,
  // and a cut cannot fire until the paper has fed past the blade.
  //
  // So each slip here asks twice: once right after the raster, once after
  // the cut. Same slip, two answers. If the second lands after the paper
  // time with a small spread, prints can be timed - and banding and heat
  // slowdown detected - after all.

  Log.println();
  Log.println("  3. THE ACK, BEFORE AND AFTER THE CUT (30% ink, 3 slips each)");
  if (!p.autoCut()) {
    Log.println("     auto-cut is OFF - this test needs the cut. Skipped.");
    return;
  }
  Log.println("     size         sent  before-cut  after-cut  paper");

  struct Case { uint16_t rows; bool single; };
  static const Case CASES[] = {
    {  512, true  },     // 36 KB, one command
    { 1024, true  },     // 73 KB, one command - the harness case
    { 1024, false },     // 73 KB as production sends it
  };
  static const uint8_t COV  = 30;
  static const uint8_t REPS = 3;

  MonoBitmap band(p.printableDots(), 128);
  if (!band.valid()) { Log.println("     out of memory"); return; }
  band.uniformField(COV);

  Raster raster(p);
  const uint16_t savedChunk = p.rasterChunkRows();
  bool allGated = true;
  int32_t overSum = 0;
  uint8_t overN   = 0;
  uint32_t worstSpread = 0;

  for (uint8_t c = 0; c < sizeof(CASES) / sizeof(CASES[0]); c++) {
    const Case& k = CASES[c];
    p.setRasterChunkRows(k.single ? k.rows : 0);

    const uint32_t paperMs = printMsFor(p, k.rows);
    const uint32_t restMs  = restMsFor(paperMs, COV);
    const uint16_t cmds    = raster.tiledCommands(band, k.rows);
    const uint32_t kb      = (static_cast<uint32_t>(band.rowBytes()) * k.rows) / 1024;
    uint32_t lo = 0xFFFFFFFF, hi = 0;

    for (uint8_t r = 0; r < REPS; r++) {
      if (!p.begin()) { Log.println("     cannot open"); allGated = false; continue; }

      char label[48];
      snprintf(label, sizeof(label), "ACK %u rows, %u cmd", k.rows, cmds);
      p.align(Align::Left).font(Font::B).line(label).normal();
      p.align(Align::Center);

      const uint32_t t0 = millis();
      raster.printTiled(band, k.rows);
      const uint32_t sent = millis() - t0;

      const bool gotA = p.waitPrinted(3000);                 // before the cut
      const uint32_t ackA = millis() - t0;

      p.align(Align::Left).feed(1).cutIfEnabled();
      const bool gotB = p.waitPrinted(paperMs * 3 + 5000);   // after the cut
      const uint32_t ackB = millis() - t0;
      p.end();

      // Waited for the paper if it came well after the before-cut ack and
      // within 10% of the paper estimate. v0.7.3 demanded it EXCEED the
      // estimate, which is itself ~1% off, so 1357 vs 1368 read as a fail.
      if (!gotB || ackB * 100 < paperMs * 90 || (gotA && ackB < ackA + 200))
        allGated = false;
      if (gotB) {
        if (ackB < lo) lo = ackB;
        if (ackB > hi) hi = ackB;
        overSum += static_cast<int32_t>(ackB) - static_cast<int32_t>(paperMs);
        overN++;
      }

      char aTxt[12], bTxt[12];
      if (gotA) snprintf(aTxt, sizeof(aTxt), "%lu", (unsigned long)ackA);
      else      strlcpy(aTxt, "none", sizeof(aTxt));
      if (gotB) snprintf(bTxt, sizeof(bTxt), "%lu", (unsigned long)ackB);
      else      strlcpy(bTxt, "none", sizeof(bTxt));

      Log.printf("     %2luKB %u cmd  %5lu  %10s  %9s  %5lu\n",
                    (unsigned long)kb, cmds, (unsigned long)sent,
                    aTxt, bTxt, (unsigned long)paperMs);

      // The after-cut ack means the paper HAS stopped, so rest from it.
      rest(restMs);
    }
    if (hi >= lo && hi - lo > worstSpread) worstSpread = hi - lo;
  }
  p.setRasterChunkRows(savedChunk);

  Log.println();
  if (allGated && overN) {
    Log.printf("  RESULT: the after-cut ack waits for the paper - on average\n"
                  "  %ld ms after the image finishes (label, feed and cut), with a\n"
                  "  spread of at most %lu ms between identical slips.\n",
                  (long)(overSum / overN), (unsigned long)worstSpread);
    Log.println(worstSpread <= 20
      ? "  Prints CAN be timed: stalls and heat slowdown are detectable."
      : "  It waits, but the spread is wide - timing is only a coarse signal.");
  } else {
    Log.println("  RESULT: the after-cut ack did NOT reliably wait for the paper.");
    Log.println("  Timing-based detection is not available on this printer.");
  }
}

// ----------------------------------------------------------------------
// o  PAPER OUT
// ----------------------------------------------------------------------
//
// The "pauses completely" case. Two phases:
//   A. watch every status register while you empty the printer, so we
//      learn which bits this printer actually sets
//   B. send a short slip to the EMPTY printer, then wait while you reload,
//      so we learn whether it holds the job, drops it, or drops the link
//
// The slip is 20 mm (about 11 KB), deliberately smaller than the printer's
// ~96 KB buffer so the write itself cannot stall. What matters is what the
// printer does with it afterwards.

void Diagnostics::paperOut(Printer& p, void*) {
  banner("PAPER OUT - what does a pause look like?");

  static const uint32_t WATCH_MS  = 45000;
  static const uint32_t RELOAD_MS = 90000;

  if (!p.begin()) { Log.println("  cannot open the printer - run 'l' first"); return; }

  // --- A. watch --------------------------------------------------------
  Log.println();
  Log.println("  PHASE A (45 s): open the cover, TAKE THE ROLL OUT,");
  Log.println("  close the cover. Leave it EMPTY. Every change is logged.");
  Log.println();

  uint8_t last[5]   = {0};
  bool    seen[5]   = {false};
  bool    silent    = false;
  char    text[96];
  const uint32_t start = millis();

  while (millis() - start < WATCH_MS) {
    for (uint8_t n = 1; n <= 4; n++) {
      uint8_t b;
      const bool got = p.status(n, b, 400);
      const uint32_t t = (millis() - start) / 1000;

      if (!got) {
        if (!silent) {
          Log.printf("  %3lus  NO REPLY from register %u%s\n",
                        (unsigned long)t, n,
                        p.writeFailed() ? " (connection lost)" : "");
          silent = true;
        }
        continue;
      }
      if (silent) {
        Log.printf("  %3lus  replies resumed\n", (unsigned long)t);
        silent = false;
      }
      if (!seen[n] || b != last[n]) {
        describeStatus(n, b, text, sizeof(text));
        Log.printf("  %3lus  %-14s 0x%02X  %s\n",
                      (unsigned long)t, REG_NAME[n], b, text);
        last[n] = b;
        seen[n] = true;
      }
    }
    if (p.writeFailed()) break;
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  const bool lost = p.writeFailed();
  p.end();
  if (lost) {
    Log.println();
    Log.println("  The printer DROPPED the connection while empty.");
    Log.println("  Phase B is skipped: that alone answers it.");
    return;
  }

  // --- B. a job sent to an empty printer --------------------------------
  Log.println();
  Log.println("  PHASE B: sending a 20 mm slip to the EMPTY printer...");

  MonoBitmap band(p.printableDots(), 32);
  if (!band.valid()) { Log.println("  out of memory"); return; }
  band.uniformField(30);

  if (!p.begin()) {
    Log.println("  the printer REFUSED a new connection while empty");
    return;
  }

  Raster raster(p);
  p.align(Align::Left).font(Font::B).line("PAPER-OUT TEST SLIP").normal();
  p.align(Align::Center);
  const uint32_t t0 = millis();
  raster.printTiled(band, 160);
  p.align(Align::Left);

  if (p.writeFailed()) {
    Log.printf("  write FAILED after %lu ms: %s\n",
                  (unsigned long)(millis() - t0), p.writeError());
    p.end();
    return;
  }
  Log.printf("  write accepted in %lu ms\n", (unsigned long)(millis() - t0));
  Log.println();
  Log.println("  >>> NOW RELOAD THE PAPER and close the cover. <<<");
  Log.println("  Waiting up to 90 s for the slip to print...");

  // One ESC v, then wait in 5 s slices so progress is visible. Not
  // waitPrinted() in a loop: each call re-sends ESC v and discards input,
  // which could throw away the very reply being waited for.
  p.escpos().requestPrintedAck();
  const uint32_t w0 = millis();
  bool printed = false, dropped = false;
  while (millis() - w0 < RELOAD_MS) {
    if (p.awaitReply(5000))                { printed = true; break; }
    if (!p.linkOpen() || p.writeFailed())  { dropped = true; break; }
    Log.printf("  ...%lus\n", (unsigned long)((millis() - w0) / 1000));
  }

  const uint32_t waitedS = (millis() - w0) / 1000;
  p.feed(1).cutIfEnabled();
  p.end();

  Log.println();
  if (printed)
    Log.printf("  RESULT: the printer HELD the job and printed it on reload "
                  "(%lu s). Nothing is lost during paper-out.\n",
                  (unsigned long)waitedS);
  else if (dropped)
    Log.println("  RESULT: the printer DROPPED the connection while waiting.");
  else
    Log.println("  RESULT: no print within 90 s. Either it dropped the job, or it\n"
                   "  is still holding it - watch whether it prints later.");
}

// ----------------------------------------------------------------------
// y  DENSITY
// ----------------------------------------------------------------------
//
// Five labelled 25 mm swatches at 50% ink, each its own job so the ESC @
// at the start of every job clears the previous command:
//
//   A  baseline
//   B  GS ( K  -6   (Epson)
//   C  DC2 #   0    (Xprinter family)
//   D  GS |    0    (other clones)
//   E  baseline again
//
// Judged by eye, once: a swatch visibly LIGHTER than A means that command
// works. The one probe that cannot be timed, because density changes how
// dark the paper is, not how long it takes.
//
// SMALL RISK: if the printer stores one of these permanently, E will come
// out lighter than A and your Dark setting has moved. The vendor utility
// restores it. Skip this probe if that matters to you today.

void Diagnostics::density(Printer& p, void*) {
  banner("DENSITY - does the printer accept a density command?");
  Log.println("  Prints 5 swatches (about 20 cm). Compare them by eye.");

  static const uint16_t ROWS = 200;      // 25 mm
  static const uint8_t  COV  = 50;

  MonoBitmap band(p.printableDots(), 40);
  if (!band.valid()) { Log.println("  out of memory"); return; }
  band.uniformField(COV);

  Raster raster(p);
  const uint32_t restMs = restMsFor(printMsFor(p, ROWS), COV);

  static const char* LABELS[5] = {
    "A  baseline",
    "B  GS ( K  -6   (Epson)",
    "C  DC2 #   0    (Xprinter)",
    "D  GS |    0    (other)",
    "E  baseline again"
  };

  for (uint8_t i = 0; i < 5; i++) {
    if (!p.begin()) { Log.printf("  %s: cannot open\n", LABELS[i]); continue; }

    p.align(Align::Left).font(Font::B).line(LABELS[i]).normal();

    switch (i) {
      case 1: p.escpos().densityEpson(-6); break;
      case 2: p.escpos().densityDc2(0);    break;
      case 3: p.escpos().densityGsBar(0);  break;
      default: break;
    }

    p.align(Align::Center);
    raster.printTiled(band, ROWS);
    p.align(Align::Left).feed(1).cutIfEnabled();
    const bool ok = p.end();

    Log.printf("  %-30s %s\n", LABELS[i], ok ? "printed" : "FAILED");
    rest(restMs);
  }

  Log.println();
  Log.println("  Compare B, C and D against A:");
  Log.println("    visibly lighter  -> that command works; tell me which");
  Log.println("    same as A        -> ignored");
  Log.println("    stray characters -> not recognised (harmless)");
  Log.println("  Then check E against A. If E is lighter, a setting STUCK -");
  Log.println("  restore Dark density with the vendor utility.");
}

// Trainer.h — the unattended characterisation run. TRAINER BUILD ONLY.
//
// Compiled only when Config.h sets BUILD_TRAINER 1. The lend-out build
// contains none of it, and reads only the ThermalProfile this leaves in NVS.
//
// WHAT IT DOES, IN ORDER
//   1. MECHANICS  times slips of two lengths to measure real paper speed
//                 and the fixed label + feed + cut time (~0.6 m of paper)
//   2. MAP        at 30, 50, 65, 80 and 95% coverage, finds the highest
//                 thermal limit that survives 30 back-to-back prints with
//                 no thermal pause
//   3. VERIFY     40 prints at the hardest mapped point, using the result
//   4. SAVE       writes the ThermalProfile to NVS
//
// HOW IT DETECTS TROUBLE, WITH NO HUMAN WATCHING
// Every print is timed from the first pixel byte to an ESC v sent AFTER
// THE CUT. The printer cannot read past a cut until the paper has fed past
// the blade, so that reply marks the paper physically stopping - measured
// at a spread of about 1 ms between identical slips. A thermal pause adds
// about 30 s. The two cannot be confused.
//
// PAPER-OUT
// A two-roll run WILL run out of paper, and this printer's network does
// not survive a paper-out - only a power cycle restores it (proven with
// nothing connected). So progress is saved after every trial. When the
// printer stops answering, the trainer says so on Serial every minute and
// waits: reload, power-cycle the printer, and it resumes by itself. The
// trial that was interrupted is simply run again.

#pragma once
#include "Config.h"

#if BUILD_TRAINER
#include <Arduino.h>
#include "Printer.h"

namespace Trainer {
  // Entry point for PrintQueue::runOnPrinterTask(). Resumes saved progress
  // if there is any, otherwise starts fresh. Runs for hours.
  void run(Printer& p, void* ctx);

  // Asks a running trainer to stop at the next safe point, saving progress.
  // Safe from any task.
  void requestStop();

  // Tells a waiting trainer that the stop was the paper running out, so it
  // retries the trial instead of recording a thermal limit. Menu 'O'.
  void notePaperOut();

  bool hasSavedProgress();
  void discardProgress();
}
#endif

#include "Mem.h"
#include "Log.h"
#include "PrintQueue.h"
#include "RasterZ.h"
#include "Config.h"
#include "Discovery.h"
#include <time.h>

// Unix seconds, or 0 if NTP has not set the clock yet. Anything before
// 2021 means the RTC is still at its power-on epoch.
static uint32_t nowEpoch() {
  const time_t t = time(nullptr);
  return (t > 1609459200) ? static_cast<uint32_t>(t) : 0;
}

// ----------------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------------

bool PrintQueue::begin(Printer& printer, JobStore& store, uint8_t depth) {
  _printer = &printer;
  _store   = &store;
  _raster  = new Raster(printer);

  _queue = xQueueCreate(depth, sizeof(PrintJob*));
  _lock  = xSemaphoreCreateMutex();
  if (!_queue || !_lock || !_raster) return false;

  // Continue numbering from the persisted history rather than restarting
  // at 1, so job ids stay unique across reboots.
  _nextId = store.highestId() + 1;
  _printer->setTrace(_debug);

  // Core 1 alongside the Arduino loop; core 0 is busy with the Wi-Fi stack.
  const BaseType_t ok = xTaskCreatePinnedToCore(
    taskEntry, "printer", 6144, this, 2, &_task, 1);

  return ok == pdPASS;
}

void PrintQueue::taskEntry(void* arg) {
  static_cast<PrintQueue*>(arg)->run();
}

void PrintQueue::run() {
  for (;;) {
    PrintJob* job = nullptr;

    // Wake periodically even with an empty queue, so a settings change
    // applies promptly rather than waiting for the next print.
    if (xQueueReceive(_queue, &job, pdMS_TO_TICKS(250)) == pdTRUE && job) {
      // One job, however many attempts it takes. Order within an attempt
      // matters: the fault cooldown first, because the printer answers
      // nothing during it; then the printer must be there; then the paper
      // wait, which is the pacing this firmware exists for.
      const uint32_t deadline = millis() + HOLD_MAX_MS;
      bool retried = false;
      for (;;) {
        waitOutFault();
        if (!waitForPrinter(deadline)) {
          StoredJob rec = {};
          strlcpy(rec.error, "printer unreachable", sizeof(rec.error));
          finishJob(job, rec, false);
          break;
        }
        waitForHeat(job);
        waitForPaper();
        applyReconfig();
        _busy = true;
        const Outcome o = process(job, retried);
        _busy = false;

        if (o == Outcome::Done || o == Outcome::Failed) break;
        if (o == Outcome::RetryFault) { retried = true; continue; }

        // Hold (paper/cover) or Unreachable: wait, then try again.
        if ((int32_t)(millis() - deadline) >= 0) {
          StoredJob rec = {};
          strlcpy(rec.error, o == Outcome::Hold ? "paper out or cover open"
                                               : "printer unreachable",
                  sizeof(rec.error));
          finishJob(job, rec, false);
          break;
        }
        if (o == Outcome::Hold) {
          const uint32_t until = millis() + HOLD_RECHECK_MS;
          while ((int32_t)(millis() - until) < 0) {
            vTaskDelay(pdMS_TO_TICKS(250));
            maybeHeartbeat();
          }
        }
      }
      _held = 0;
      freeJob(job);
    } else {
      // Cooling down: resynchronise the moment the window closes, so the
      // printer is ready before the next job rather than after it.
      watchLink();
      serviceFault();
      applyReconfig();
      if (_heatFactor10 > 10 && _lastJobEndMs &&
          millis() - _lastJobEndMs > BACKSTOP_DECAY_MS) {
        _heatFactor10 = 10;
        Log.println("[pace] idle 5 min - backstop cleared, normal rests");
      }
      // The probes run only on an idle queue, and only here on the
      // printer task - they drive the Printer directly.
      runPendingTask();
      maybeProbe();
      maybeHeartbeat();
    }
  }
}

void PrintQueue::setDebug(bool on) {
  _debug = on;
  if (_printer) _printer->setTrace(on);
}

// ----------------------------------------------------------------------
// Pacing
// ----------------------------------------------------------------------

// Paper time for a given number of dot rows, from the measured speed.
// 93.5 mm/s at 8 dots/mm is 748 rows per second, and the print time of a
// 128 mm slip held to within 1 ms over 40 prints - so this is arithmetic,
// not an estimate to be calibrated at runtime.
uint32_t PrintQueue::printMsForRows(uint32_t rows) const {
  uint16_t speed10 = _printer ? _printer->profile().paperSpeedMmPerSec10 : 0;
  if (!speed10) speed10 = PAPER_SPEED_MM_PER_SEC_10;

  const uint32_t rowsPerSec =
    (static_cast<uint32_t>(speed10) * DOTS_PER_MM) / 10;
  if (!rowsPerSec) return 0;

  return (rows * 1000UL) / rowsPerSec;
}

// Rows that actually put ink on paper. Raster: its own height. Text: a
// Font A cell is 24 dots tall, so a line is 3 mm. Raw or event: assume a
// short slip, since the content cannot be inspected.
uint32_t PrintQueue::contentRows(const PrintJob* job) const {
  if (!job) return 0;

  if (job->type == JobType::Raster) return job->rows;

  if (job->type == JobType::Text) {
    uint32_t lines = 1;
    if (job->payload)
      for (size_t i = 0; i < job->payloadLen; i++)
        if (job->payload[i] == '\n') lines++;
    return lines * 24;
  }

  return 120;
}

// How long the paper will still be MOVING after a job's bytes are
// delivered. This is the mechanical wait: sending the next job while the
// previous one is still printing holds it at the printer's consumption
// rate instead of letting it load into an empty buffer.
uint32_t PrintQueue::estimatePaperMs(const PrintJob* job) const {
  if (!job) return 0;

  uint32_t rows = contentRows(job);
  if (job->cut) rows += CUT_TRAVEL_ROWS;
  // The white tail is motion without heat, like the cut travel.
  if (job->type == JobType::Raster && !job->noTail)
    rows += tailRowsFor(job->widthDots, job->rows);

  uint32_t ms = printMsForRows(rows) + PAPER_MARGIN_MS;
  if (ms > PAPER_MAX_WAIT_MS) ms = PAPER_MAX_WAIT_MS;
  return ms;
}

// The thermal rest, from the measured ceiling. See the table in
// PrintQueue.h: the printer faults when coverage% x duty% exceeds roughly
// 1000, and with duty = print / (print + gap) that rearranges to
//
//   gap = print x (coverage x 100 / limit - 1)
//
// A coverage at or below limit/100 needs no gap at all, because even a
// 100% duty cycle stays under the ceiling.
// The fixed pacing table (v0.9.4). The ceiling falls as coverage rises: a
// dense print heats the head harder DURING the print, so it must start
// from a cooler head. Set conservatively below every point that faulted
// on the bench. Linear between points, held flat beyond the ends.
//
//   coverage   30%   50%   66%   80%   95%
//   limit     1100  1000   850   700   600
uint16_t PrintQueue::limitFor(uint8_t cov) {
  static const uint8_t  C[] = {   30,   50,  66,  80,  95 };
  static const uint16_t L[] = { 1100, 1000, 850, 700, 600 };
  const uint8_t n = sizeof(C);
  if (cov <= C[0])     return L[0];
  if (cov >= C[n - 1]) return L[n - 1];
  for (uint8_t i = 1; i < n; i++)
    if (cov <= C[i])
      return L[i - 1] + (int32_t)(L[i] - L[i - 1]) * (cov - C[i - 1])
                        / (C[i] - C[i - 1]);
  return L[n - 1];
}

uint32_t PrintQueue::coolMsFor(uint8_t coverage, uint32_t printMs) const {
  if (!coverage || !printMs) return 0;

  const uint16_t limit   = limitFor(coverage);
  const uint32_t product = static_cast<uint32_t>(coverage) * 100;  // at 100% duty
  if (product <= limit) return 0;

  uint64_t gap = (static_cast<uint64_t>(printMs) * (product - limit)) / limit;
  gap = gap * _heatFactor10 / 10;         // the backstop, after a fault
  if (gap > COOL_MAX_MS) gap = COOL_MAX_MS;
  return static_cast<uint32_t>(gap);
}

// Exact percentage of black dots. Raster rows are a whole number of bytes
// and the API rejects widths that are not a multiple of 8, so every bit in
// the payload is a real dot and there is no padding to discount.
uint8_t PrintQueue::countCoverage(const uint8_t* data, size_t len) {
  if (!data || !len) return 0;

  uint32_t set = 0;
  for (size_t i = 0; i < len; i++) set += __builtin_popcount(data[i]);

  // Round to nearest: at 96 KB this is under a millisecond of work, and
  // rounding down would understate the rest period.
  const uint64_t bits = static_cast<uint64_t>(len) * 8ULL;
  return static_cast<uint8_t>((static_cast<uint64_t>(set) * 100ULL + bits / 2) / bits);
}

// The browser counts coverage while packing and sends it with the upload,
// because it has the buffer in hand. A job that arrives without one is
// counted here rather than assumed safe - an unpaced dense image faults the
// printer within about twenty prints, and "the client forgot" is not a
// reason to let that happen.
uint8_t PrintQueue::coverageOf(const PrintJob* job) const {
  if (!job) return 0;

  if (job->type == JobType::Raster) {
    if (job->coverage) return job->coverage > 100 ? 100 : job->coverage;
    if (job->packedZ) return 0;      // counted when it was verified: truly blank
    if (job->file[0]) return countCoverageFile(job->file);
    if (job->pieces.count) {
      uint64_t set = 0, bytes = 0;
      job->pieces.each(0, job->payloadLen, [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i++) set += __builtin_popcount(p[i]);
        bytes += n;
      });
      if (!bytes) return 0;
      const uint64_t bits = bytes * 8ULL;
      return static_cast<uint8_t>((set * 100ULL + bits / 2) / bits);
    }
    return countCoverage(job->payload, job->payloadLen);
  }

  // Text and raw: an estimate, documented in PrintQueue.h.
  return TEXT_COVERAGE_PCT;
}

// Blocks the printer task - never a web handler - until the previous job's
// paper has stopped moving and the head has had its rest.
// After a fault, images wait out the heat hold. Text is light enough to go.
void PrintQueue::waitForHeat(const PrintJob* job) {
  if (!job || job->type != JobType::Raster) return;
  while ((int32_t)(_heatHoldUntilMs - millis()) > 0 && _heatHoldUntilMs) {
    vTaskDelay(pdMS_TO_TICKS(250));
    maybeHeartbeat();
  }
  _heatHoldUntilMs = 0;
}

void PrintQueue::waitForPaper() {
  if (!_paperBusyUntilMs) return;

  const uint32_t now = millis();
  if (static_cast<int32_t>(_paperBusyUntilMs - now) > 0) {
    const uint32_t wait = _paperBusyUntilMs - now;
    if (_debug) Log.printf("[pace] holding %lu ms before the next job\n",
                              (unsigned long)wait);
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
  _paperBusyUntilMs = 0;
}

// ----------------------------------------------------------------------
// Faults
// ----------------------------------------------------------------------
//
// A thermal fault is not an error condition the printer will tell us
// about. The write fails, the printer beeps, and for about thirty seconds
// it answers nothing at all - no status register carries a flag, on any of
// the four, sampled every 500 ms through the whole window. Then it comes
// back as if nothing happened. There is nothing to poll and nothing to
// clear: the only correct response is to wait longer than the window and
// carry on.

uint32_t PrintQueue::coolingMs() const {
  if (!_faultUntilMs) return 0;
  const int32_t left = static_cast<int32_t>(_faultUntilMs - millis());
  return left > 0 ? static_cast<uint32_t>(left) : 0;
}

void PrintQueue::noteFault(const char* why) {
  _faults++;
  _watchFaultMs = millis();
  _watchUntilMs = _watchFaultMs + FAULT_COOLDOWN_MS;
  _watchDropMs  = _watchBackMs = 0;
  _faultUntilMs = millis() + FAULT_COOLDOWN_MS;
  _needsResync  = true;
  _reachable    = false;

  // The paper wait is subsumed by the cooldown, which is longer than any
  // of them.
  _paperBusyUntilMs = 0;

  // The backstop. A fault means the head is hotter than the table assumed,
  // and it stays hot for minutes: both retries in the v0.9.3 log faulted
  // again ~70 s after the first fault. So images are held for HEAT_HOLD_MS
  // after a fault, and rests stay longer for the rest of the session
  // (1.5x per fault, capped at 3x) until the printer has idled for
  // BACKSTOP_DECAY_MS.
  _heatHoldUntilMs = millis() + HEAT_HOLD_MS;
  _heatFactor10    = (_heatFactor10 * 3) / 2;
  if (_heatFactor10 > 30) _heatFactor10 = 30;
  Log.printf("[fault] backstop: no images for %lu s, rests now x%u.%u\n",
             (unsigned long)(HEAT_HOLD_MS / 1000),
             _heatFactor10 / 10, _heatFactor10 % 10);

  Log.printf("[fault] %s - printer is unreachable for ~30 s after this. "
                "Waiting %lu s, then resynchronising.\n",
                why && *why ? why : "write failed",
                (unsigned long)(FAULT_COOLDOWN_MS / 1000));
  Log.printf("[fault] last job: %u%% coverage, %lu ms print, %lu ms gap. "
                "If this repeats, lower coverage or the thermal limit.\n",
                (unsigned)_lastCoverage,
                (unsigned long)_lastPrintMs,
                (unsigned long)_lastGapMs);
}

// Blocking form, used when a job is waiting. Sliced so the heartbeat still
// reports during a 45 s wait rather than the log going silent.
void PrintQueue::waitOutFault() {
  while (coolingMs()) {
    const uint32_t left = coolingMs();
    vTaskDelay(pdMS_TO_TICKS(left > 250 ? 250 : left));
    watchLink();
    maybeHeartbeat();
  }
  watchLink();
  serviceFault(true);
}

// Samples the cable link every pass through the cooldown after a fault.
// A thermal protection leaves the printer's network alive: link stays up.
// A restart - a brownout from the power supply, for one - takes the link
// down for a few seconds. The two point at different fixes, so the answer
// is logged once per fault. A restart also means the printer owes nothing.
void PrintQueue::watchLink() {
  if (!_watchUntilMs) return;
  const uint32_t now = millis();
  const bool up = _printer->linkUp();
  if (!up && !_watchDropMs) _watchDropMs = now;
  if (up && _watchDropMs && !_watchBackMs) _watchBackMs = now;
  if ((int32_t)(now - _watchUntilMs) < 0) return;

  _watchUntilMs = 0;
  if (!_watchDropMs) {
    Log.println("[fault] link stayed UP through the cooldown - the printer "
                "did NOT restart (protection, not a power loss)");
    return;
  }
  Log.printf("[fault] link DROPPED %lu ms after the fault, back after %lu ms "
             "- the printer RESTARTED. Heat does not do that; a supply dip "
             "does.\n",
             (unsigned long)(_watchDropMs - _watchFaultMs),
             (unsigned long)(_watchBackMs ? _watchBackMs - _watchDropMs : 0));
  const uint32_t owed = _printer->forgetDebt();
  if (owed) Log.printf("[fault] %lu owed bytes voided - it restarted clean\n",
                       (unsigned long)owed);
}

// Non-blocking form, used on the idle path.
void PrintQueue::serviceFault(bool force) {
  if (coolingMs()) return;
  _faultUntilMs = 0;

  if (!_needsResync) return;

  // Retried every RESYNC_RETRY_MS while idle, and always before a job, until
  // it succeeds. Giving up after one attempt left the printer waiting for
  // pixel bytes that the next job would then supply by accident.
  const uint32_t now = millis();
  if (!force && _lastResyncTryMs && now - _lastResyncTryMs < RESYNC_RETRY_MS) return;
  _lastResyncTryMs = now ? now : 1;

  // The printer is still counting down pixel bytes it never received, so
  // pay the shortfall back as white before anything else is sent.
  if (_printer->recoverStream()) {
    _needsResync = false;
    _resyncNoted = false;
    _printer->disconnect();
    Log.println("[queue] printer stream resynchronised");
    _reachable = true;
  } else {
    // Said once per outage, not every 15 s: repeated, it pushed the
    // useful lines out of the log.
    if (!_resyncNoted)
      Log.println("[queue] resync not possible yet - printer unreachable; "
                  "retrying quietly until it is back");
    _resyncNoted = true;
    _reachable = false;
  }
}

// Only ever called from the idle branch of the printer task, so a probe can
// never collide with a job for the printer's single TCP slot - and never
// during a cooldown, when it would only time out.
void PrintQueue::maybeProbe() {
  if (coolingMs()) return;
  if (!_probing)   return;       // quiet mode: nothing touches the printer

  const uint32_t now = millis();
  const uint32_t interval = _waitingJob ? WAIT_PROBE_MS : PROBE_INTERVAL_MS;
  if (_lastProbeMs && now - _lastProbeMs < interval) return;
  _lastProbeMs = now ? now : 1;

  // No cable link: the printer is off or unplugged. Nothing to search for.
  // A link reset every few checks still runs, in case the PHY itself is
  // what is stuck - it costs nothing while there is no printer to reach.
  if (!_printer->linkUp()) {
    if (!_linkDown) {
      Log.println("[find] no cable link - printer off or unplugged");
      // A printer that loses power restarts clean: it is waiting for no
      // pixel bytes at all. Paying an old debt into it sends tens of KB of
      // filler to a printer that never asked for them - in the v0.9.4 log
      // that was followed by a failed job every time (17 KB, then 40 KB),
      // and printing only recovered once the debt was gone. The debt
      // belongs to the printer that was switched off, and dies with it.
      const uint32_t owed = _printer->forgetDebt();
      if (owed) Log.printf("[find] %lu owed pixel bytes voided - the printer "
                           "restarts clean\n", (unsigned long)owed);
    }
    _linkDown  = true;
    _reachable = false;
    if (++_linkDownChecks % LINK_RESET_EVERY == 0) _printer->resetLink();
    return;
  }
  const bool linkReturned = _linkDown;
  if (linkReturned) { Log.println("[find] cable link back"); _downSinceMs = 0; }
  _linkDown       = false;
  _linkDownChecks = 0;

  bool ok = _printer->probe();

  // The start of an outage is the evidence that matters, so it is logged
  // in full and the minute-by-minute resets after it are kept quiet (v0.19:
  // a 2-hour outage had pushed its own start out of the 16 KB log).
  if (!ok && _probeFails == 0) {
    const uint16_t ms = _printer->lastProbeFailMs();
    Log.printf("[link] printer stopped answering: connect %s after %u ms, "
               "cable link up, %lu min since the last job, unit up %lu h\n",
               ms < 1400 ? "REFUSED" : "got NO ANSWER", ms,
               _lastJobEndMs ? (unsigned long)((now - _lastJobEndMs) / 60000) : 0UL,
               (unsigned long)(now / 3600000));
  }

  if (!ok) {
    const uint16_t fails = _probeFails + 1;
    const bool search = !_everFound || linkReturned ||
                        fails % DISCOVER_EVERY == 0;

    if (search) {
      ok = discover();                 // resets the link itself
    } else if (fails % LINK_RESET_EVERY == 0) {
      if (!_resetsThisOutage) _printer->dumpLink();   // evidence, once
      _resetsThisOutage++;
      const bool loud = _resetsThisOutage <= 3 || _resetsThisOutage % 30 == 0;
      _printer->setLinkQuiet(!loud);
      if (loud)
        Log.printf("[link] unreachable for %u probes - link reset #%u%s\n",
                      fails, _resetsThisOutage,
                      _resetsThisOutage == 3 ? " (further resets logged every 30)" : "");
      if (_printer->resetLink() && _printer->probe()) {
        Log.printf("[link] back IMMEDIATELY after link reset #%u - the W5500 "
                      "was holding it up\n", _resetsThisOutage);
        ok = true;
      }
    }
  }

  if (ok) {
    if (_probeFails)
      Log.printf("[link] printer back after %u failed probes and %u link "
                 "resets (~%u min)\n", _probeFails, _resetsThisOutage,
                    (unsigned)(_downSinceMs ? (now - _downSinceMs) / 60000 : 0));
    _printer->setLinkQuiet(false);
    _probeFails       = 0;
    _resetsThisOutage = 0;
    _everFound        = true;
    _downSinceMs      = 0;
  } else {
    _probeFails++;
    if (!_downSinceMs) _downSinceMs = now ? now : 1;
    if (_probeFails == POWER_CYCLE_AFTER ||
        (_probeFails > POWER_CYCLE_AFTER && _probeFails % (POWER_CYCLE_AFTER * 10) == 0))
      Log.println("[link] still unreachable after repeated link resets. If "
                     "it is out of paper, reload it. Otherwise switch the "
                     "printer OFF, wait 5 s, ON - link resets continue and "
                     "will pick it up.");
  }
  _reachable = ok;
}

// One search of the cable. Resets the link first: after the printer drops
// off, the W5500 may be unable to reach even a healthy printer until it is
// reset, and a search through a stuck link would find nothing anywhere.
bool PrintQueue::discover() {
  _searching = true;
  const IPAddress saved = _printer->address();
  const uint32_t  t0    = millis();

  if (!_notFoundNoted)
    Log.printf("[find] searching: %s, then %u defaults\n",
               saved.toString().c_str(), Discovery::candidates());
  _printer->resetLink();

  IPAddress found;
  const bool ok = Discovery::find(*_printer, saved, found);
  const uint32_t ms = millis() - t0;

  if (!ok) {
    if (!_notFoundNoted)
      Log.printf("[find] not found (%lu ms). If the printer's address was "
                 "changed, its self-test slip shows it: hold FEED while "
                 "switching on. Searching quietly from here.\n",
                 (unsigned long)ms);
    _notFoundNoted = true;
  } else if (found == saved) {
    Log.printf("[find] printer at %s (%lu ms)\n",
                  found.toString().c_str(), (unsigned long)ms);
  } else {
    Log.printf("[find] printer MOVED: %s, was %s (%lu ms) - saving\n",
                  found.toString().c_str(), saved.toString().c_str(),
                  (unsigned long)ms);
    _printer->retarget(found);
    xSemaphoreTake(_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < 4; i++) _newAddr[i] = found[i];
    _newAddrReady = true;
    xSemaphoreGive(_lock);
  }

  if (ok) _notFoundNoted = false;
  _searches++;
  _searching = false;
  return ok;
}

bool PrintQueue::takeNewPrinterAddress(IPAddress& ip) {
  if (!_lock) return false;
  xSemaphoreTake(_lock, portMAX_DELAY);
  const bool ready = _newAddrReady;
  if (ready) ip = IPAddress(_newAddr[0], _newAddr[1], _newAddr[2], _newAddr[3]);
  _newAddrReady = false;
  xSemaphoreGive(_lock);
  return ready;
}

// Every HEARTBEAT_INTERVAL_MS while idle. Deliberately one line, so a long
// serial capture stays readable.
// Prints when the printer's state or the job counters change, and
// otherwise once every HEARTBEAT_INTERVAL_MS - so an idle unit is quiet
// but a long capture still shows it alive.
void PrintQueue::maybeHeartbeat() {
  const uint32_t now = millis();
  const uint32_t cooling = coolingMs();

  const uint32_t state =
      (!_probing ? 1u : cooling ? 2u : _reachable ? 3u : 4u) |
      (_completed << 3) ^ (_failed << 17) ^ (_faults << 25);
  const bool changed = (state != _lastBeatState);
  if (!changed && _lastBeatMs && now - _lastBeatMs < HEARTBEAT_INTERVAL_MS)
    return;
  // Checked every idle pass, so a change is logged within a second; but a
  // flapping state still logs at most once per 5 s.
  if (changed && _lastBeatMs && now - _lastBeatMs < 5000) return;
  _lastBeatMs    = now ? now : 1;
  _lastBeatState = state;

  Log.printf(
    "[pulse] up %lus | heap %lu largest %lu | printer %s | "
    "queue %u pending %lu done %lu failed %lu faults | last %lu KB/s | "
    "pace %u%% cov, %lu+%lu ms%s\n",
    (unsigned long)(now / 1000),
    (unsigned long)Mem::free8(),
    (unsigned long)Mem::block8(),
    !_probing ? "NOT PROBED (quiet)"
              : cooling ? "COOLING" : (_reachable ? "reachable" : "UNREACHABLE"),
    (unsigned)pending(),
    (unsigned long)_completed,
    (unsigned long)_failed,
    (unsigned long)_faults,
    (unsigned long)(_printer ? _printer->lastRateBps() / 1024 : 0),
    (unsigned)_lastCoverage,
    (unsigned long)_lastPrintMs,
    (unsigned long)_lastGapMs,
    cooling ? "  <-- fault cooldown" : "");
}

bool PrintQueue::runOnPrinterTask(TaskFn fn, void* ctx) {
  if (!fn) return false;
  xSemaphoreTake(_lock, portMAX_DELAY);
  const bool free = (_taskFn == nullptr);
  if (free) { _taskFn = fn; _taskCtx = ctx; }
  xSemaphoreGive(_lock);
  return free;
}

void PrintQueue::runPendingTask() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  TaskFn fn  = _taskFn;
  void*  ctx = _taskCtx;
  xSemaphoreGive(_lock);
  if (!fn) return;

  _busy = true;
  fn(*_printer, ctx);
  _busy = false;

  // Cleared only AFTER it ran, so a second request cannot slip in while
  // the first is still printing.
  xSemaphoreTake(_lock, portMAX_DELAY);
  _taskFn  = nullptr;
  _taskCtx = nullptr;
  xSemaphoreGive(_lock);

  // A probe paces itself; the queue should not add a stale wait on top.
  _paperBusyUntilMs = 0;
}

void PrintQueue::requestReconfig(ReconfigFn fn, void* ctx) {
  xSemaphoreTake(_lock, portMAX_DELAY);
  _reconfigFn  = fn;
  _reconfigCtx = ctx;
  xSemaphoreGive(_lock);
}

void PrintQueue::applyReconfig() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  ReconfigFn fn  = _reconfigFn;
  void*      ctx = _reconfigCtx;
  _reconfigFn    = nullptr;
  _reconfigCtx   = nullptr;
  xSemaphoreGive(_lock);

  if (fn) fn(ctx);
}

// ----------------------------------------------------------------------
// Job allocation and submission
// ----------------------------------------------------------------------

PrintJob* PrintQueue::allocJob(JobType type, const uint8_t* payload,
                               size_t len, const JobSource& source) {
  PrintJob* job = static_cast<PrintJob*>(calloc(1, sizeof(PrintJob)));
  if (!job) return nullptr;

  if (len) {
    // One extra byte so text payloads are always NUL-terminated.
    job->payload = static_cast<uint8_t*>(malloc(len + 1));
    if (!job->payload) { free(job); return nullptr; }
    memcpy(job->payload, payload, len);
    job->payload[len] = 0;
  }

  job->type       = type;
  job->payloadLen = len;
  job->createdMs  = millis();
  job->epoch      = nowEpoch();
  job->scale      = 1;
  job->cut        = true;

  strlcpy(job->source, source.label ? source.label : "unknown",
          sizeof(job->source));
  for (uint8_t i = 0; i < 4; i++) job->sourceIp[i] = source.ip[i];

  return job;
}

uint32_t PrintQueue::enqueue(PrintJob* job) {
  if (!job) return 0;

  xSemaphoreTake(_lock, portMAX_DELAY);
  job->id = _nextId++;
  xSemaphoreGive(_lock);

  // Never block a web handler waiting for queue space — reject instead, so
  // the caller gets an immediate 503 rather than a hung request.
  if (xQueueSend(_queue, &job, 0) != pdTRUE) {
    freeJob(job);
    return 0;
  }

  noteStatus(job, JobStatus::Queued);
  return job->id;
}

uint32_t PrintQueue::submitText(const String& text, const JobSource& source,
                                uint8_t align, uint8_t scale,
                                bool bold, bool cut, uint8_t font) {
  PrintJob* job = allocJob(JobType::Text,
                           reinterpret_cast<const uint8_t*>(text.c_str()),
                           text.length(), source);
  if (!job) return 0;

  job->align = align > 2 ? 0 : align;
  job->scale = (scale < 1) ? 1 : (scale > 8 ? 8 : scale);
  job->bold  = bold;
  job->cut   = cut;
  job->font  = font ? 1 : 0;
  return enqueue(job);
}

uint32_t PrintQueue::submitRaster(const uint8_t* data, size_t len,
                                  uint16_t widthDots, uint16_t rows,
                                  const JobSource& source, bool cut,
                                  uint16_t chunkRows, uint8_t coverage,
                                  bool tail) {
  // Reject an inconsistent payload here rather than mid-print.
  const uint16_t rowBytes = (widthDots + 7) / 8;
  if (!data || !rows || !widthDots) return 0;
  if (len != static_cast<size_t>(rowBytes) * rows) return 0;

  PrintJob* job = allocJob(JobType::Raster, data, len, source);
  if (!job) return 0;

  job->widthDots = widthDots;
  job->rows      = rows;
  job->cut       = cut;
  job->chunkRows = chunkRows;
  job->coverage  = coverage > 100 ? 100 : coverage;
  job->noTail    = !tail;
  return enqueue(job);
}

uint32_t PrintQueue::submitRasterOwned(uint8_t* data, size_t len,
                                       uint16_t widthDots, uint16_t rows,
                                       const JobSource& source, bool cut,
                                       uint16_t chunkRows, uint8_t coverage,
                                       const uint8_t* pre, uint8_t preLen,
                                       bool tail) {
  const uint16_t rowBytes = (widthDots + 7) / 8;
  if (!data) return 0;

  if (!rows || !widthDots ||
      len != static_cast<size_t>(rowBytes) * rows) {
    free(data);                     // we own it now, even on rejection
    return 0;
  }

  PrintJob* job = allocJob(JobType::Raster, nullptr, 0, source);
  if (!job) { free(data); return 0; }

  job->payload    = data;           // adopted, not copied
  job->noTail = !tail;
  if (pre && preLen && preLen <= sizeof(job->pre)) {
    memcpy(job->pre, pre, preLen);
    job->preLen = preLen;
  }
  job->payloadLen = len;
  job->widthDots  = widthDots;
  job->rows       = rows;
  job->cut        = cut;
  job->chunkRows  = chunkRows;
  job->coverage   = coverage > 100 ? 100 : coverage;

  const uint32_t id = enqueue(job);
  // enqueue() frees the job (and its payload) if the queue was full.
  return id;
}

uint32_t PrintQueue::submitRasterPieces(const RasterPieces& pieces,
                                        uint16_t widthDots, uint16_t rows,
                                        const JobSource& source, bool cut,
                                        uint8_t coverage, bool tail,
                                        bool packedZ) {
  const uint16_t rowBytes = (widthDots + 7) / 8;
  const size_t   len      = pieces.total();
  auto drop = [&pieces]() {
    for (uint8_t i = 0; i < pieces.count; i++) Mem::release(pieces.buf[i]);
  };
  // A compressed image (experiment) was checked whole by RasterZ::verify
  // before it got here; its length is the compressed size.
  if (!pieces.count || pieces.count > RasterPieces::MAX || !rows || !widthDots ||
      (!packedZ && len != static_cast<size_t>(rowBytes) * rows)) {
    drop();
    return 0;
  }

  PrintJob* job = allocJob(JobType::Raster, nullptr, 0, source);
  if (!job) { drop(); return 0; }

  job->pieces     = pieces;         // adopted, not copied
  job->packedZ    = packedZ;
  job->payloadLen = len;
  job->widthDots  = widthDots;
  job->rows       = rows;
  job->cut        = cut;
  job->chunkRows  = 0;
  job->coverage   = coverage > 100 ? 100 : coverage;
  job->noTail     = !tail;

  // enqueue() frees the job (and its pieces) if the queue was full.
  return enqueue(job);
}

uint32_t PrintQueue::submitRasterFile(const char* path, size_t len,
                                      uint16_t widthDots, uint16_t rows,
                                      const JobSource& source, bool cut,
                                      uint8_t coverage, bool tail) {
  const uint16_t rowBytes = (widthDots + 7) / 8;
  if (!path || !*path || strlen(path) >= sizeof(PrintJob::file) ||
      !rows || !widthDots || len != static_cast<size_t>(rowBytes) * rows) {
    if (path && *path) LittleFS.remove(path);
    return 0;
  }

  PrintJob* job = allocJob(JobType::Raster, nullptr, 0, source);
  if (!job) { LittleFS.remove(path); return 0; }

  strlcpy(job->file, path, sizeof(job->file));
  job->payloadLen = len;
  job->widthDots  = widthDots;
  job->rows       = rows;
  job->cut        = cut;
  job->chunkRows  = 0;
  job->coverage   = coverage > 100 ? 100 : coverage;
  job->noTail     = !tail;

  _photoBusy = true;
  // enqueue() frees the job if the queue is full, which deletes the file
  // and clears _photoBusy (freeJob).
  return enqueue(job);
}

uint32_t PrintQueue::submitRaw(const uint8_t* data, size_t len,
                               const JobSource& source) {
  PrintJob* job = allocJob(JobType::Raw, data, len, source);
  if (!job) return 0;
  job->cut = false;      // raw callers send their own cut if they want one
  return enqueue(job);
}

void PrintQueue::freeJob(PrintJob* job) {
  if (!job) return;
  if (job->file[0]) {                 // a stored image: its file goes with it
    LittleFS.remove(job->file);
    _photoBusy = false;
  }
  for (uint8_t i = 0; i < job->pieces.count; i++) Mem::release(job->pieces.buf[i]);
  free(job->payload);
  free(job);
}

// ----------------------------------------------------------------------
// Printing — runs only on the printer task
// ----------------------------------------------------------------------

PrintQueue::Outcome PrintQueue::process(PrintJob* job, bool finalAttempt) {
  StoredJob rec = {};
  rec.id     = job->id;
  rec.epoch  = job->epoch ? job->epoch : nowEpoch();
  rec.type   = static_cast<uint8_t>(job->type);
  strlcpy(rec.source, job->source, sizeof(rec.source));
  memcpy(rec.sourceIp, job->sourceIp, 4);

  const uint32_t started = millis();
  noteStatus(job, JobStatus::Printing);

  // Counted BEFORE printing. The payload is still ours either way, but
  // having the figure early means a failed job still reports the coverage
  // that caused it.
  const uint8_t coverage = coverageOf(job);
  job->coverage = coverage;

  const uint32_t printMs = printMsForRows(contentRows(job));
  const uint32_t coolMs  = coolMsFor(coverage, printMs);

  _lastCoverage = coverage;
  _lastPrintMs  = printMs;
  _lastGapMs    = coolMs;

  if (job->type == JobType::Raster || _debug)
    Log.printf("[pace] job #%lu: %lu rows, %u%% coverage -> "
                  "%lu ms print, %lu ms rest%s\n",
                  (unsigned long)job->id,
                  (unsigned long)contentRows(job),
                  (unsigned)coverage,
                  (unsigned long)printMs,
                  (unsigned long)coolMs,
                  _heatFactor10 > 10 ? "  (backstop)" : "");

  bool ok        = false;
  bool linkFault = false;

  if (!_printer->begin()) {
    // Nothing was sent, so nothing is owed: wait for the printer and try
    // the whole job again. Leave a stopped printer alone meanwhile.
    Log.printf("[queue] job #%lu: printer not answering - waiting for it\n",
               (unsigned long)job->id);
    _reachable = false;
    noteStatus(job, JobStatus::Queued);
    return Outcome::Unreachable;
  } else {
    _reachable   = true;
    _everFound   = true;
    _downSinceMs = 0;

    // Paper and cover, asked on a fresh connection before a single job
    // byte is sent - never on a connection held through an error, which
    // preceded every wedge.
    if (!checkReady(rec)) {
      _printer->end();
      noteStatus(job, JobStatus::Queued);
      return Outcome::Hold;
    }
    _held = 0;

    switch (job->type) {
      case JobType::Text:   ok = renderText(job, rec);   break;
      case JobType::Raster: ok = renderRaster(job, rec); break;
      case JobType::Raw:    ok = renderRaw(job, rec);    break;
      default: break;
    }

    // No debug stamp is printed on the paper. An earlier version appended
    // the job id here, but adding a line between the artwork and the cut
    // shifted the cut position and spoiled the very prints being measured.
    // Serial tracing gives the same information without touching the paper.

    if (ok && job->cut) _printer->feed(1).cutIfEnabled();

    // A mid-stream write failure means the printer consumed a different
    // number of bytes than the command headers declared. The paper may
    // already show garbage; what matters is that the job is reported as
    // failed rather than silently "done".
    if (_printer->writeFailed()) {
      ok        = false;
      linkFault = true;
      strlcpy(rec.error, _printer->writeError(), sizeof(rec.error));
    }

    rec.bytes = _printer->bytesSent();

    // end() reports whether the stream actually reached the printer. Its
    // result used to be discarded, which is why jobs that stalled the
    // printer mid-image were still logged as "done": the bytes had been
    // accepted by the network stack, so nothing else noticed.
    if (!_printer->end()) {
      linkFault = true;
      if (ok) {
        ok = false;
        strlcpy(rec.error, "stream not drained", sizeof(rec.error));
      }
    }

    // Throughput of the job just sent. Below roughly 45 KB/s the printer
    // consumes faster than the ESP32 delivers, its input buffer empties,
    // the motor stops and restarts, and the paper shows light bands. This
    // is a DELIVERY problem, entirely separate from the thermal fault, and
    // the two were conflated for a long time.
    // Only for images: a text job is too small for a rate to mean anything,
    // and every one was flagged SLOW.
    const uint32_t bps = _printer->lastRateBps();
    if (bps && _printer->lastWindowBytes() >= 4096)
      Log.printf("[rate] %lu bytes in %lu ms = %lu KB/s%s\n",
                    (unsigned long)_printer->lastWindowBytes(),
                    (unsigned long)_printer->lastWindowMs(),
                    (unsigned long)(bps / 1024),
                    bps < 45UL * 1024UL ? "  <-- SLOW, expect banding" : "");

    if (!ok) {
      if (linkFault) {
        // The printer has gone away. Resynchronising now would fail, so
        // the cooldown is started and the resync deferred until it ends.
        noteFault(rec.error);
        // Only an image leaves the printer waiting for bytes. Lost text
        // leaves nothing pending, so there is nothing to pay back - just
        // the reconnect and ESC @ that resync does anyway.
        if (job->type != JobType::Raster) _printer->forgetDebt();
        // E02: one reprint after the cooldown and resync. A second fault
        // on the same job fails it.
        if (!finalAttempt) {
          Log.printf("[queue] job #%lu will be reprinted once after the "
                     "printer recovers\n", (unsigned long)job->id);
          noteStatus(job, JobStatus::Queued);
          return Outcome::RetryFault;
        }
      } else {
        // The link is fine; the printer is merely mid-command because a
        // band was rejected. Left alone it consumes the NEXT job as those
        // pixels, which is how one bad print used to poison every print
        // after it. Resynchronise here, on the printer task.
        if (_printer->recoverStream())
          Log.println("[queue] printer stream resynchronised");
        else {
          Log.println("[queue] resync FAILED - printer may need a power cycle");
          _reachable = false;
        }
      }
    }
  }

  setRestAfter(job, started, coolMs);
  rec.durationMs = millis() - started;
  finishJob(job, rec, ok);
  return ok ? Outcome::Done : Outcome::Failed;
}

// Hold the next job until the paper has stopped AND the head has rested.
// Motion is measured from when this job started, because the printer
// begins moving as soon as the first bytes land; the thermal rest is
// measured from now, because it is idle time the head actually needs and
// delivery may have taken longer than the print itself.
void PrintQueue::setRestAfter(const PrintJob* job, uint32_t started,
                              uint32_t coolMs) {
  if (_faultUntilMs) return;
  const uint32_t motionUntil = started + estimatePaperMs(job);
  const uint32_t restUntil   = millis() + coolMs;
  _paperBusyUntilMs =
    (static_cast<int32_t>(restUntil - motionUntil) > 0) ? restUntil
                                                        : motionUntil;
}

// The one place a job's final state is written.
void PrintQueue::finishJob(PrintJob* job, StoredJob& rec, bool ok) {
  rec.id    = job->id;
  if (!rec.epoch) rec.epoch = job->epoch ? job->epoch : nowEpoch();
  rec.type  = static_cast<uint8_t>(job->type);
  strlcpy(rec.source, job->source, sizeof(rec.source));
  memcpy(rec.sourceIp, job->sourceIp, 4);
  rec.status = static_cast<uint8_t>(ok ? JobStatus::Done : JobStatus::Failed);
  if (!ok) Log.printf("[queue] job #%lu FAILED: %s\n",
                      (unsigned long)job->id, rec.error);

  xSemaphoreTake(_lock, portMAX_DELAY);
  if (ok) _completed++; else _failed++;
  _store->upsert(rec);              // terminal state: this one hits flash
  xSemaphoreGive(_lock);
  _lastJobEndMs = millis();
}

// E04 / E05. Standard ESC/POS status, confirmed on this printer: DLE EOT 4
// bits 0x60 = paper end, DLE EOT 2 bit 0x04 = cover open. A missing or
// malformed reply is not treated as a problem - printing goes ahead, as it
// did before these checks existed.
bool PrintQueue::checkReady(StoredJob& rec) {
  uint8_t b;
  if (_printer->status(4, b, 400) && Printer::statusValid(b) && (b & 0x60)) {
    if (_held != 4) Log.println("[queue] E04 out of paper - job held");
    _held = 4;
    strlcpy(rec.error, "out of paper", sizeof(rec.error));
    return false;
  }
  if (_printer->status(2, b, 400) && Printer::statusValid(b) && (b & 0x04)) {
    if (_held != 5) Log.println("[queue] E05 cover open - job held");
    _held = 5;
    strlcpy(rec.error, "cover open", sizeof(rec.error));
    return false;
  }
  if (_held) Log.println("[queue] printer ready again - printing");
  return true;
}

// Blocks while the printer is missing, probing faster than the idle rate
// so the job starts within seconds of it coming back. Discovery and the
// stuck-link resets all run as usual inside maybeProbe().
bool PrintQueue::waitForPrinter(uint32_t deadline) {
  if (_reachable && !_linkDown && !_needsResync) return true;

  _waitingJob  = true;
  _lastProbeMs = 0;                    // probe now, not in 15 s
  while (!(_reachable && !_linkDown && !_needsResync)) {
    if ((int32_t)(millis() - deadline) >= 0) { _waitingJob = false; return false; }
    serviceFault();
    maybeProbe();
    maybeHeartbeat();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  _waitingJob = false;
  return true;
}

// ----------------------------------------------------------------------
// Alerts
// ----------------------------------------------------------------------

PrintQueue::Alert PrintQueue::alert() const {
  if (_linkDown)                        return Alert::NoLink;
  if (_held == 4)                       return Alert::PaperOut;
  if (_held == 5)                       return Alert::Cover;
  if (coolingMs() || _needsResync)      return Alert::Hot;
  if (downForMs() >= STUCK_AFTER_MS)    return Alert::Stuck;

  const uint32_t stalled = _printer ? _printer->stalledMs() : 0;
  if (stalled >= STUCK_AFTER_MS) return Alert::Stuck;
  if (stalled >= 3000)           return Alert::Paused;

  // Searched and not found, with the cable link up: the printer is there
  // but not answering. Said once someone is waiting, or after a minute.
  if (!_reachable && _searches && (_waitingJob || downForMs() >= 60000))
    return Alert::Stuck;
  return Alert::None;
}

uint32_t PrintQueue::restingMs() const {
  if (!pending() && !_busy && !_waitingJob) return 0;
  const uint32_t now  = millis();
  int32_t left = static_cast<int32_t>(_paperBusyUntilMs - now);
  if (_heatHoldUntilMs) {
    const int32_t heat = static_cast<int32_t>(_heatHoldUntilMs - now);
    if (heat > left) left = heat;
  }
  return left > 0 ? static_cast<uint32_t>(left) : 0;
}

bool PrintQueue::renderText(PrintJob* job, StoredJob& rec) {
  // An empty payload is valid: it means feed and cut only.
  if (!job->payload || job->payloadLen == 0) { rec.bytes = 0; return true; }

  _printer->align(static_cast<Align>(job->align));
  if (job->font)      _printer->font(Font::B);
  if (job->scale > 1) _printer->size(job->scale);
  if (job->bold)      _printer->bold(true);

  // The payload is written whole. The printer wraps at the print-area width
  // set by GS W, so paper width is already accounted for, and any embedded
  // newlines pass through as line feeds.
  _printer->text(reinterpret_cast<const char*>(job->payload));

  const size_t len = job->payloadLen;
  if (len == 0 || job->payload[len - 1] != '\n') _printer->line();

  _printer->normal();
  if (job->font) _printer->font(Font::A);   // normal() leaves the font alone
  rec.bytes = len;
  return true;
}

bool PrintQueue::renderRaster(PrintJob* job, StoredJob& rec) {
  if (!job->payload && !job->pieces.count && !job->file[0]) return false;

  if (job->widthDots > _printer->printableDots()) {
    strlcpy(rec.error, "image wider than paper", sizeof(rec.error));
    return false;
  }

  const uint16_t rowBytes  = (job->widthDots + 7) / 8;
  const uint16_t chunkRows = job->chunkRows
                           ? job->chunkRows
                           : _printer->chunkRowsFor(job->widthDots, job->rows);
  const uint16_t commands  = (job->rows + chunkRows - 1) / chunkRows;

  if (_debug)
    Log.printf("[raster] %u x %u, %u rows/command, %u command%s\n",
                  job->widthDots, job->rows, chunkRows,
                  commands, commands == 1 ? "" : "s");

  // A compressed image (v0.25): decoded here, 1 KB at a time, through a
  // 2 KB window, straight into the same commands. A reprint decodes again
  // from the start, because the job keeps the compressed bytes.
  uint8_t* zwin = nullptr;
  RasterZ::Decoder zdec;
  if (job->packedZ) {
    zwin = static_cast<uint8_t*>(malloc(RasterZ::WINDOW + 1024));
    if (!zwin) {
      strlcpy(rec.error, "out of memory (decoder)", sizeof(rec.error));
      return false;
    }
    zdec.begin(job->pieces, zwin);
  }

  // A stored image (print from storage, v0.26): opened and checked before
  // a byte is sent, so a missing file fails the job cleanly.
  File photo;
  uint8_t* pbuf = nullptr;
  size_t   pcap = 0;
  if (job->file[0]) {
    photo = LittleFS.open(job->file, "r");
    if (!photo || photo.size() != job->payloadLen) {
      free(zwin);
      strlcpy(rec.error, "stored image missing", sizeof(rec.error));
      return false;
    }
    // Whole rows, about 8 KB per read: small next to the printer's ~18 KB
    // buffer, so a read never leaves it short.
    pcap = static_cast<size_t>(rowBytes) * max<uint16_t>(1, 8192 / rowBytes);
    pbuf = static_cast<uint8_t*>(malloc(pcap));
    if (!pbuf) {
      free(zwin);
      strlcpy(rec.error, "out of memory", sizeof(rec.error));
      return false;
    }
    Log.hold(true);                    // no log write to flash mid-image
  }

  if (job->preLen) _printer->escpos().raw(job->pre, job->preLen);
  _printer->align(Align::Center);
  _printer->link().resetGaps();

  for (uint16_t y = 0; y < job->rows; y += chunkRows) {
    const uint16_t rows = min<uint16_t>(chunkRows, job->rows - y);

    if (job->packedZ) {
      if (!_printer->ok() || job->widthDots > _printer->printableDots() ||
          !_printer->escpos().rasterHeader(RasterMode::Normal, rowBytes, rows)) {
        free(zwin);
        strlcpy(rec.error, "raster band rejected", sizeof(rec.error));
        return false;
      }
      // The header promised rows x rowBytes: send exactly that, padding with
      // white if decoding fails, so the printer never waits mid-command.
      uint8_t* zout = zwin + RasterZ::WINDOW;
      size_t   left = static_cast<size_t>(rows) * rowBytes;
      bool     zok  = true;
      while (left) {
        const size_t want = left < 1024 ? left : 1024;
        const size_t got  = zok ? zdec.read(zout, want) : 0;
        if (got < want) { zok = false; memset(zout + got, 0, want - got); }
        _printer->escpos().raw(zout, want);
        left -= want;
      }
      if (!zok) {
        free(zwin);
        strlcpy(rec.error, "compressed image damaged", sizeof(rec.error));
        return false;
      }
      rec.bytes += static_cast<uint32_t>(rowBytes) * rows + 8;
      continue;
    }
    if (pbuf) {
      if (!streamBandFromFile(photo, pbuf, pcap, rowBytes, rows, rec)) {
        free(pbuf);
        Log.hold(false);
        return false;
      }
      rec.bytes += static_cast<uint32_t>(rowBytes) * rows + 8;
      continue;
    }
    if (job->pieces.count) {
      // Held in pieces (v0.24.6): the same command, header then its rows,
      // the rows read from whichever pieces hold them.
      if (!_printer->ok() || job->widthDots > _printer->printableDots() ||
          !_printer->escpos().rasterHeader(RasterMode::Normal, rowBytes, rows)) {
        strlcpy(rec.error, "raster band rejected", sizeof(rec.error));
        return false;
      }
      job->pieces.each(static_cast<size_t>(y) * rowBytes,
                       static_cast<size_t>(rows) * rowBytes,
                       [this](const uint8_t* p, size_t n) {
                         _printer->escpos().raw(p, n);
                       });
    } else {
      const uint8_t* band = job->payload + static_cast<size_t>(y) * rowBytes;
      if (!_raster->printBand(band, job->widthDots, rows)) {
        strlcpy(rec.error, "raster band rejected", sizeof(rec.error));
        return false;
      }
    }
    rec.bytes += static_cast<uint32_t>(rowBytes) * rows + 8;
  }

  free(zwin);
  if (pbuf) { free(pbuf); Log.hold(false); }

  // The white tail. Fixes the step to lighter near the end of photos.
  const uint16_t tail = job->noTail ? 0 : tailRowsFor(job->widthDots, job->rows);
  if (tail) {
    _printer->escpos().rasterBlank(rowBytes, tail);
    rec.bytes += static_cast<uint32_t>(rowBytes) * tail + 8;
  }

  _printer->align(Align::Left);

  // Banding evidence, one line per image. Positions are in mm down the
  // image. The printer holds ~18 KB it has not printed yet, so a gap after
  // byte N starves the paper near N minus 18 KB - both are given.
  {
    Transport& t = _printer->link();
    const uint32_t at    = t.gapAtByte();
    const uint32_t mmAt  = at / rowBytes / 8;
    const uint32_t mmPap = (at > PRINTER_BUFFER_BYTES)
                         ? (at - PRINTER_BUFFER_BYTES) / rowBytes / 8 : 0;
    char cuts[48] = "none";
    if (commands > 1) {
      size_t n = 0;
      for (uint16_t c = 1; c < commands && n + 6 < sizeof(cuts); c++)
        n += snprintf(cuts + n, sizeof(cuts) - n, "%s%u", c > 1 ? "," : "",
                      (unsigned)((uint32_t)c * chunkRows / 8));
    }
    Log.printf("[band] job #%lu: %u mm, %u command%s, joins at mm: %s | "
               "longest gap %lu ms at %lu mm sent (~%lu mm on paper), "
               "%lu gap%s over %lu ms\n",
               (unsigned long)job->id, (unsigned)(job->rows / 8),
               commands, commands == 1 ? "" : "s", cuts,
               (unsigned long)t.gapMaxMs(), (unsigned long)mmAt,
               (unsigned long)mmPap, (unsigned long)t.gapsOver(),
               t.gapsOver() == 1 ? "" : "s",
               (unsigned long)Transport::GAP_REPORT_MS);
  }
  return true;
}

// WHY THE TAIL (bench, v0.10.1, tests E and U)
// The printer prints at ~213 mm/s, which at full width needs ~120 KB/s,
// but takes data in at only ~90 KB/s once its buffer is full. So through
// a wide image it runs slightly short of data, and the moment the last
// byte arrives it runs free: everything still unprinted comes out
// lighter, with a line at the switch. The step sits wherever the paper
// was when the data ended.
//   - White rows appended to the image move the switch into the white,
//     where it cannot be seen (test E: step gone, before and after
//     cleaning).
//   - Images at 70% width need ~84 KB/s, never run short, and need no
//     tail (test U: 70% clean, 80% and 90% still stepped).
// How much is unprinted when the data ends, fitted to three measured
// steps (60, 64 and 128 mm images): about 26 - 0.137 x length mm. Images
// under ~23 mm finish arriving before the paper starts, so step nowhere.
// A 4 mm margin is added. A 128 mm photo gets ~12 mm of tail; a 64 mm
// one ~21 mm. Cost: that much paper per photo, nothing for text.
uint16_t PrintQueue::tailRowsFor(uint16_t widthDots, uint16_t rows) const {
  const uint16_t rowBytes = (widthDots + 7) / 8;
  if (rowBytes <= TAIL_MIN_ROW_BYTES) return 0;     // narrow: never short
  const float mm = rows / 8.0f;
  if (mm < 23.0f) return 0;
  float tailMm = 26.0f - 0.137f * mm;
  if (tailMm < 0) tailMm = 0;
  tailMm += 4.0f;
  return static_cast<uint16_t>(tailMm * 8.0f + 0.5f);
}

// One GS v 0 command whose pixels come from the stored photo, read in
// pieces of whole rows. The header promises rowBytes x rows bytes, so if
// the file runs short the rest is sent as white: the printer must never be
// left waiting mid-command (it would read the next job as pixels).
bool PrintQueue::streamBandFromFile(File& f, uint8_t* buf, size_t cap,
                                    uint16_t rowBytes, uint16_t rows,
                                    StoredJob& rec) {
  if (!_printer->ok()) {
    strlcpy(rec.error, "raster band rejected", sizeof(rec.error));
    return false;
  }
  if (!_printer->escpos().rasterHeader(RasterMode::Normal, rowBytes, rows)) {
    strlcpy(rec.error, "raster band rejected", sizeof(rec.error));
    return false;
  }
  size_t left = static_cast<size_t>(rowBytes) * rows;
  bool   ok   = true;
  while (left) {
    const size_t want = left < cap ? left : cap;
    size_t got = ok ? f.read(buf, want) : 0;
    if (got != want) {
      if (ok) strlcpy(rec.error, "stored photo could not be read", sizeof(rec.error));
      ok = false;
      memset(buf + got, 0, want - got);   // pad with white, keep in step
    }
    _printer->escpos().raw(buf, want);
    left -= want;
  }
  return ok;
}

// Coverage of a stored photo, counted in pieces (only when the page did
// not send one).
uint8_t PrintQueue::countCoverageFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f || !f.size()) return 0;
  uint8_t  buf[512];
  uint64_t set = 0, bytes = 0;
  for (;;) {
    const size_t n = f.read(buf, sizeof(buf));
    if (!n) break;
    for (size_t i = 0; i < n; i++) set += __builtin_popcount(buf[i]);
    bytes += n;
  }
  if (!bytes) return 0;
  const uint64_t bits = bytes * 8ULL;
  return static_cast<uint8_t>((set * 100ULL + bits / 2) / bits);
}

bool PrintQueue::renderRaw(PrintJob* job, StoredJob& rec) {
  if (!job->payload) return false;
  _printer->escpos().raw(job->payload, job->payloadLen);
  rec.bytes = job->payloadLen;
  return true;
}



// ----------------------------------------------------------------------
// Status
// ----------------------------------------------------------------------

// Non-terminal states are recorded in RAM only; JobStore decides what
// reaches flash.
void PrintQueue::noteStatus(const PrintJob* job, JobStatus status) {
  if (!job) return;

  StoredJob rec = {};
  rec.id     = job->id;
  rec.epoch  = job->epoch;
  rec.type   = static_cast<uint8_t>(job->type);
  rec.status = static_cast<uint8_t>(status);
  strlcpy(rec.source, job->source, sizeof(rec.source));
  memcpy(rec.sourceIp, job->sourceIp, 4);

  xSemaphoreTake(_lock, portMAX_DELAY);
  _store->upsert(rec);
  xSemaphoreGive(_lock);
}

size_t PrintQueue::pending() const {
  return _queue ? uxQueueMessagesWaiting(_queue) : 0;
}

// PrintQueue.h — Section 18.
//
// The single most important rule in this file: the Printer object is owned
// exclusively by the printer task. Nothing else may call it. Web handlers
// submit jobs and return immediately.
//
// This exists because printing blocks. Measured on this hardware:
// connect up to 4 s, and a 110 KB raster took 2.5 s once the printer's
// input buffer filled and TCP backpressure kicked in. Doing that inside an
// HTTP handler would stall the server; doing it from two handlers at once
// would interleave two ESC/POS streams into garbage.
//
// The task is also where PACING lives, because pacing is the difference
// between a machine that prints all day and one that wedges. See the
// thermal section below.

#pragma once
#include <LittleFS.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "PrintJob.h"
#include "JobStore.h"
#include "Printer.h"
#include "Raster.h"

class PrintQueue {
public:
  // Starts the printer task. Both references must outlive this object.
  // The store is where history lives; the queue keeps none of its own.
  bool begin(Printer& printer, JobStore& store, uint8_t depth = 8);

  // --- Submission (safe from any task) ---------------------------------
  // Return the new job id, or 0 if the queue is full or memory ran out.
  uint32_t submitText(const String& text,
                      const JobSource& source,
                      uint8_t align = 0,
                      uint8_t scale = 1,
                      bool bold = false,
                      bool cut = true,
                      uint8_t font = 0);     // 0 Font A, 1 Font B

  // coverage is the percentage of black dots in the raster, 0-100, and 0
  // means "unknown — count it on the printer task". It is not cosmetic:
  // it sets the rest period after the print, and an image sent with a
  // wrong low figure will fault the printer within about twenty prints.
  uint32_t submitRaster(const uint8_t* data, size_t len,
                        uint16_t widthDots, uint16_t rows,
                        const JobSource& source,
                        bool cut = true,
                        uint16_t chunkRows = 0,
                        uint8_t coverage = 0,
                        bool tail = true);    // see tailRowsFor()

  // Takes OWNERSHIP of an already-malloc'd buffer instead of copying it.
  // The queue frees it when the job completes; the caller must not.
  //
  // This exists because the copying version needs the payload twice over
  // at once, and a 96 KB image would peak at 192 KB against roughly 180 KB
  // of free heap on a WROOM-32. Upload handlers use this; everything that
  // already owns a MonoBitmap uses submitRaster.
  //
  // Returns 0 on rejection, in which case the buffer is freed here.
  uint32_t submitRasterOwned(uint8_t* data, size_t len,
                             uint16_t widthDots, uint16_t rows,
                             const JobSource& source,
                             bool cut = true,
                             uint16_t chunkRows = 0,
                             uint8_t coverage = 0,
                             const uint8_t* pre = nullptr,
                             uint8_t preLen = 0,
                             bool tail = true);

  // Takes ownership of an image held in pieces (see RasterPieces.h). Printed
  // as the same commands as one block. Returns 0 on rejection, in which case
  // the pieces are freed here.
  uint32_t submitRasterPieces(const RasterPieces& pieces,
                              uint16_t widthDots, uint16_t rows,
                              const JobSource& source,
                              bool cut, uint8_t coverage, bool tail,
                              bool packedZ = false);   // experiment, RasterZ.h

  // An image already written to storage (print from storage, v0.26). The
  // pixels are streamed from the file while printing. One at a time:
  // photoBusy() is true from submission until the job has finished, and the
  // file is deleted then. Returns 0 on rejection (file deleted here).
  static constexpr const char* PHOTO_FILE = "/photo.bin";
  uint32_t submitRasterFile(const char* path, size_t len,
                            uint16_t widthDots, uint16_t rows,
                            const JobSource& source,
                            bool cut, uint8_t coverage, bool tail);
  bool photoBusy() const { return _photoBusy; }

  uint32_t submitRaw(const uint8_t* data, size_t len,
                     const JobSource& source);

  // --- Reconfiguration -------------------------------------------------
  // Settings changes that touch the Printer or its transport must not
  // happen while a job is mid-stream. A web handler registers the change
  // here; the printer task applies it between jobs. Takes effect from the
  // next job onward.
  typedef void (*ReconfigFn)(void* ctx);
  void requestReconfig(ReconfigFn fn, void* ctx);
  bool reconfigPending() const { return _reconfigFn != nullptr; }

  // --- Thermal pacing ---------------------------------------------------
  // THE FINDING THIS FIRMWARE IS BUILT ROUND.
  //
  // The printer has a thermal limit. Sustained printing faults when too
  // much energy goes in too fast, and the fault is ugly: the TCP write
  // fails, the printer beeps, and then it answers NOTHING — every DLE EOT
  // status query times out — for about thirty seconds. Sampling all four
  // status registers every 500 ms through the whole window caught no error
  // bit on any of them. There is no flag to poll. It goes away and comes
  // back.
  //
  // Measured on a 576 x 1024 dot print (128 mm) at Dark density:
  //
  //   coverage  duty   product  result
  //     18.8%    31%      583   40 prints, clean
  //     29.7%    31%      932   40 prints, clean
  //     39.1%    31%     1227   40 prints, clean
  //     53.1%  19.6%     1042   40 prints, clean
  //     53.1%  20.9%     1110   FAULT at print 22
  //     50.0%    31%     1567   FAULT at print 18
  //     79.7%  ~31%     2470    FAULT at print 6
  //
  // So the safe region is coverage% x duty% <= 1000. Rearranged for the
  // idle gap between prints, with duty = print / (print + gap):
  //
  //   gap_ms = print_ms x (coverage x 100 / limit - 1)     floored at 0
  //   print_ms = rows / 748                                (93.5 mm/s)
  //
  // That is the whole model. It is arithmetic on two measured constants,
  // which is why it replaced the self-calibrating loop that used to live
  // here: that loop inferred paper speed from delivery rate, delivery rate
  // mostly measures Wi-Fi, and it ratcheted 70 -> 28 mm/s chasing noise.
  //
  // 0 disables pacing. It exists to reproduce the fault, nothing else.
  // The pacing ceiling at a coverage, from the fixed table (PrintQueue.cpp).
  static uint16_t limitFor(uint8_t coverage);
  uint16_t thermalLimit() const { return limitFor(_lastCoverage); }
  uint8_t  heatFactor10() const { return _heatFactor10; }
  // Since the last job finished; 0 if none since boot.
  uint32_t idleMs() const { return _lastJobEndMs ? millis() - _lastJobEndMs : 0; }

  // Milliseconds left of a post-fault cooldown, 0 when not cooling. The
  // status endpoint reports this so "nothing is printing" has a visible
  // reason rather than looking like a hang.
  uint32_t coolingMs() const;

  // Pacing figures from the last job, for the status endpoint and the log.
  uint8_t  lastCoverage() const { return _lastCoverage; }
  uint32_t lastPrintMs()  const { return _lastPrintMs; }
  uint32_t lastGapMs()    const { return _lastGapMs; }


  // --- Work on the printer task ------------------------------------------
  // Runs fn(printer, ctx) on the printer task at the next idle moment, with
  // the queue marked busy. The only sanctioned way for code outside this
  // class to drive the Printer directly - the probes in Diagnostics.cpp,
  // and the trainer after them, use it. False if one is already waiting.
  typedef void (*TaskFn)(Printer& printer, void* ctx);
  bool runOnPrinterTask(TaskFn fn, void* ctx);

  // --- Diagnostics -----------------------------------------------------
  // Reachability is decided HERE, on the printer task, because probing
  // opens a TCP connection and this printer allows only one. Web handlers
  // read the cached value; they must never probe themselves.
  bool printerReachable() const { return _reachable; }

  // True once the printer has been unreachable long enough that only a
  // power cycle is likely to bring it back. For the web UI to tell a
  // borrower what to do, rather than just "unreachable".
  bool printerNeedsPowerCycle() const { return downForMs() >= STUCK_AFTER_MS; }

  // How long probes have been failing, by the clock rather than by count:
  // the probe rate rises while a job waits, so a count would mislead.
  uint32_t downForMs() const {
    const uint32_t since = _downSinceMs;
    return since ? millis() - since : 0;
  }

  // --- What the borrower is told ------------------------------------------
  // One code at a time, the most useful first. Messages live in Alerts.h.
  enum class Alert : uint8_t {
    None     = 0,
    Paused   = 1,    // E01 a print stopped mid-way; it resumes by itself
    Hot      = 2,    // E02 heat fault; recovering, the print is retried
    Stuck    = 3,    // E03 not responding; switch the printer off and on
    PaperOut = 4,    // E04 out of paper
    Cover    = 5,    // E05 cover open
    NoLink   = 6     // E06 printer off or cable out
  };
  Alert alert() const;

  // Milliseconds until the next print may start because the head is
  // resting, while a job is waiting for it. 0 otherwise. Not an error.
  uint32_t restingMs() const;

  // --- Discovery --------------------------------------------------------
  // True while the printer task is searching the cable for the printer.
  bool printerSearching() const { return _searching; }

  // True while the cable link is down: printer off or cable out.
  bool printerLinkDown() const { return _linkDown; }

  // Searches completed since boot. 0 means the verdict is not in yet.
  uint16_t printerSearches() const { return _searches; }

  // Probe at the next idle moment instead of waiting out the interval -
  // after the printer address is changed by hand, for instance.
  void reprobeSoon() { _lastProbeMs = 0; }

  // The printer was found at a new address. Returns it once, for the
  // caller (loop()) to save - Settings belongs to loop(), not this task.
  bool takeNewPrinterAddress(IPAddress& ip);

  // Background probing on or off. Off means NOTHING connects to the printer
  // unless a job or probe asks - which is what the paper-out test needs, to
  // learn whether the printer wedges by itself or only when a connection
  // is open at the moment it enters its error state.
  void setProbing(bool on) { _probing = on; }
  bool probing() const { return _probing; }

  // Debug mode: traces every byte sent to Serial. Never marks the paper.
  void setDebug(bool on);
  bool debug() const { return _debug; }

  // --- Introspection ---------------------------------------------------
  size_t    pending() const;
  uint32_t  completed() const { return _completed; }
  uint32_t  failed()    const { return _failed; }
  uint32_t  faults()    const { return _faults; }
  bool      busy()      const { return _busy; }

private:
  Printer*       _printer = nullptr;
  JobStore*      _store   = nullptr;
  Raster*        _raster  = nullptr;
  QueueHandle_t  _queue   = nullptr;
  SemaphoreHandle_t _lock = nullptr;
  TaskHandle_t   _task    = nullptr;

  uint32_t  _nextId    = 1;
  volatile bool _reachable = false;
  volatile bool _debug     = false;
  volatile bool _probing   = true;
  uint32_t  _lastProbeMs   = 0;
  static const uint32_t PROBE_INTERVAL_MS = 15000;

  // --- Link recovery ------------------------------------------------------
  // What the bench showed, across three sessions: when the printer drops
  // off the network - a thermal fault, a paper-out, a power cycle - the
  // W5500 can be left unable to reconnect even once the printer is healthy
  // again, and only a W5500 reset clears it. A reset made BEFORE the
  // printer is back does not stick. So one reset per outage (v0.7.3) was
  // wrong: the reset is repeated every LINK_RESET_EVERY failed probes for
  // as long as the outage lasts, and the one that works is logged.
  //
  // In session 2 the printer was power-cycled and still unreachable until
  // a W5500 reset; in session 1 resets did nothing until the printer was
  // power-cycled. Both are this same rule, seen from different ends.
  static const uint8_t LINK_RESET_EVERY  = 4;    // ~1 minute of probes
  static const uint8_t POWER_CYCLE_AFTER = 12;   // ~3 minutes, 3 resets
  uint16_t  _probeFails       = 0;
  uint16_t  _resetsThisOutage = 0;

  // --- Discovery ----------------------------------------------------------
  // A search runs when the printer has never answered this boot, when the
  // cable link has just returned (it may be a different printer), and
  // every DISCOVER_EVERY failed probes during a long outage. Each search
  // resets the link first, so it also serves the stuck-link rule.
  static const uint8_t DISCOVER_EVERY = 8;      // ~2 minutes of probes
  volatile bool _searching  = false;
  volatile uint16_t _searches = 0;
  bool          _resyncNoted   = false;    // log each outage once
  bool          _notFoundNoted = false;
  volatile bool _linkDown   = false;
  bool          _everFound  = false;
  uint16_t      _linkDownChecks = 0;
  uint8_t       _newAddr[4] = {0};
  bool          _newAddrReady = false;          // guarded by _lock
  bool          discover();

  // A resync that failed is retried, not abandoned - the printer is still
  // waiting for its pixel bytes until one succeeds.
  uint32_t  _lastResyncTryMs  = 0;
  static const uint32_t RESYNC_RETRY_MS = 15000;

  // Heartbeat to Serial while idle. Uptime, heap, the largest contiguous
  // block (the real limit on an image), reachability, queue counters and
  // the pacing of the last job - so a machine left running shows its own
  // decline instead of having to be interrogated after the fact.
  uint32_t  _lastBeatMs = 0;
  uint32_t  _lastBeatState = 0;
  static const uint32_t HEARTBEAT_INTERVAL_MS = 600000;   // 10 minutes
  void      maybeHeartbeat();

  // --- Pacing constants -------------------------------------------------
  // Fallback only. The real figure is PrinterProfile::paperSpeedMmPerSec10,
  // so a different mechanism does not need this file edited.
  static const uint16_t PAPER_SPEED_MM_PER_SEC_10 = 935;

  // Dots per millimetre of paper. 203 DPI is 7.99 dots/mm; the firmware has
  // used 8 everywhere since the start and the 0.1% error is far below the
  // margin below.
  static const uint16_t DOTS_PER_MM = 8;

  // Added to every motion estimate. Covers the printer's own start-up and
  // the fact that the first print after an idle runs about 35 ms slow.
  static const uint32_t PAPER_MARGIN_MS = 300;

  // Ceilings. The motion cap is a sanity limit on a corrupt row count; the
  // cool cap bounds a pathological 100%-coverage image, which at the
  // 96 KB maximum works out near 16 s of rest.
  static const uint32_t PAPER_MAX_WAIT_MS = 30000;
  static const uint32_t COOL_MAX_MS       = 60000;

  // After a write failure the printer is unreachable for about 30 s and
  // then returns by itself. Retrying inside that window achieves nothing
  // except another failed job and another entry in the history, so the
  // task simply waits it out with margin. Opening the cover does not help
  // and no intervention is needed.
  static const uint32_t FAULT_COOLDOWN_MS = 45000;
  // Bytes the printer holds before printing them (bench: ~18-20 KB).
  static const uint32_t PRINTER_BUFFER_BYTES = 18 * 1024;

  // Link watch after a fault: did the printer RESTART (its network link
  // drops) or protect itself (link stays up)? See watchLink().
  uint32_t _watchUntilMs = 0, _watchFaultMs = 0, _watchDropMs = 0, _watchBackMs = 0;
  void     watchLink();

  // Nominal coverage for a job whose pixels cannot be counted: text and
  // raw ESC/POS. An ESTIMATE, not a measurement — Font A glyphs on a full
  // line come out near 15%, and at 15% the formula still allows a 50% duty
  // cycle, so a receipt is barely delayed. Raster jobs never use this:
  // their coverage is counted exactly.
  static const uint8_t TEXT_COVERAGE_PCT = 15;

  // Paper fed and pulled through the cutter after a job. Motion, not
  // energy: it costs time but puts no heat into the head, so it is added
  // to the motion estimate and left out of the thermal one.
  static const uint16_t CUT_TRAVEL_ROWS = 160;

  // White tail after wide images; see tailRowsFor(). 52 bytes = 416 dots,
  // 72% of the head: test U printed 70% clean and 80% stepped.
  static const uint16_t TAIL_MIN_ROW_BYTES = 52;
  uint16_t tailRowsFor(uint16_t widthDots, uint16_t rows) const;

  // Backstop after a hard fault. See noteFault().
  static const uint32_t HEAT_HOLD_MS      = 180000;
  static const uint32_t BACKSTOP_DECAY_MS = 300000;
  volatile uint32_t _heatHoldUntilMs = 0;
  uint8_t  _heatFactor10 = 10;         // rest multiplier x10
  uint32_t _lastJobEndMs = 0;

  uint32_t _paperBusyUntilMs = 0;    // paper still moving until this millis()
  uint32_t _faultUntilMs     = 0;    // post-fault cooldown ends at this millis()
  bool     _needsResync      = false;

  uint8_t  _lastCoverage = 0;
  uint32_t _lastPrintMs  = 0;
  uint32_t _lastGapMs    = 0;
  uint32_t _faults       = 0;

  uint32_t printMsForRows(uint32_t rows) const;
  uint32_t contentRows(const PrintJob* job) const;   // rows that put ink down
  uint32_t estimatePaperMs(const PrintJob* job) const;
  uint32_t coolMsFor(uint8_t coverage, uint32_t printMs) const;
  uint8_t  coverageOf(const PrintJob* job) const;
  static uint8_t countCoverage(const uint8_t* data, size_t len);

  void     waitForPaper();
  void     waitForHeat(const PrintJob* job);
  void     waitOutFault();           // blocks the cooldown, then resyncs
  void     serviceFault(bool force = false);   // non-blocking, when idle
  void     noteFault(const char* why);


  uint32_t  _completed = 0;
  uint32_t  _failed    = 0;
  volatile bool _busy  = false;

  TaskFn     _taskFn      = nullptr;
  void*      _taskCtx     = nullptr;
  void       runPendingTask();

  ReconfigFn _reconfigFn  = nullptr;
  void*      _reconfigCtx = nullptr;
  void       applyReconfig();

  static void taskEntry(void* arg);
  void run();

  void      maybeProbe();
  // What happened to one attempt at a job. Only Done and Failed are final;
  // the others send it round again after the matching wait.
  enum class Outcome : uint8_t { Done, Failed, RetryFault, Hold, Unreachable };
  Outcome   process(PrintJob* job, bool finalAttempt);
  void      finishJob(PrintJob* job, StoredJob& rec, bool ok);
  void      setRestAfter(const PrintJob* job, uint32_t started, uint32_t coolMs);
  bool      waitForPrinter(uint32_t deadline);
  bool      checkReady(StoredJob& rec);     // E04/E05 before sending

  // A held or waiting job gives up after this long, rather than printing
  // hours later to an empty room.
  static const uint32_t HOLD_MAX_MS     = 30UL * 60UL * 1000UL;
  static const uint32_t HOLD_RECHECK_MS = 5000;
  // A print stalled this long is reported as E03 instead of E01: a pause
  // (paper, cover, cooling) is resolved long before this.
  static const uint32_t STUCK_AFTER_MS  = 3UL * 60UL * 1000UL;
  // Probe interval while a job is waiting for the printer.
  static const uint32_t WAIT_PROBE_MS   = 5000;

  volatile uint8_t _held        = 0;       // 4 paper out, 5 cover open
  volatile bool    _photoBusy   = false;   // a stored image is queued or printing
  volatile bool    _waitingJob  = false;   // a job is waiting on the printer
  volatile uint32_t _downSinceMs = 0;      // first failed probe of this outage
  bool      renderText(PrintJob* job, StoredJob& rec);
  bool      renderRaster(PrintJob* job, StoredJob& rec);
  bool      streamBandFromFile(File& f, uint8_t* buf, size_t cap,
                               uint16_t rowBytes, uint16_t rows, StoredJob& rec);
  static uint8_t countCoverageFile(const char* path);
  bool      renderRaw(PrintJob* job, StoredJob& rec);

  PrintJob* allocJob(JobType type, const uint8_t* payload, size_t len,
                     const JobSource& source);
  uint32_t  enqueue(PrintJob* job);
  void      freeJob(PrintJob* job);

  void      noteStatus(const PrintJob* job, JobStatus status);
};

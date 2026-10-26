// SmartThermal.ino — Phases 3-5 complete, Phase 8/10 in progress.
//
// Board:   ESP32 Dev Module (WROOM-32)
// Serial:  115200
// Library: ArduinoJson v7  (Library Manager -> "ArduinoJson")
//
// ARCHITECTURAL RULE ENFORCED HERE:
// The Printer object is owned exclusively by the print queue's task. Neither
// loop(), nor the web handlers, nor the serial menu may call it directly.
// Everything goes through queue.submit*(). This is what stops two clients
// interleaving ESC/POS streams, and what keeps HTTP handlers from blocking
// for the seconds a real print takes.

#include "Mem.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <time.h>

#include "Log.h"
#include "Config.h"
#include "Settings.h"
#include "PrinterProfile.h"
#include "PrinterLink.h"
#include "Diagnostics.h"
#include "ThermalProfile.h"
#include "Trainer.h"
#include "Printer.h"
#include "MonoBitmap.h"
#include "Raster.h"
#include "JobStore.h"
#include "PrintQueue.h"
#include "WebApi.h"
#include "Unit.h"
#include "Net.h"
#include "Slips.h"
#include "SetupPortal.h"
#include "BandTest.h"
#include "Online.h"

SettingsStore settings;

// Constructed with placeholders; retargeted from NVS in setup(). The
// concrete type - Wi-Fi TCP or W5500 - is chosen in Config.h.
PrinterLink transport(IPAddress(0, 0, 0, 0), PRINTER_PORT,
                      PRINTER_CONNECT_TIMEOUT_MS);

Printer    printer(transport, Profiles::POS8370, Papers::MM80);
JobStore   jobStore;
PrintQueue queue;
WebApi     api(settings, queue, jobStore, printer, transport);
ItemStore  items;          // drafts, saved, scheduled (v0.19)
Scheduler  sched;
SlipDesk   desk;
SetupPortal portal(settings, queue, desk, printer);

// True when no network is saved: the unit runs its setup hotspot instead
// of joining a network, and the web API is not started.
static bool setupMode = false;

// Everything submitted from the serial menu is labelled as such, so the
// history distinguishes local testing from real network requests.
static const JobSource SERIAL_SOURCE = { "serial", IPAddress(0, 0, 0, 0) };

// Forward declarations. Arduino normally generates these itself, using a
// parser that quietly gives up on some definitions — and a missing
// prototype surfaces as a confusing "not declared in this scope". Declaring
// them here removes the guesswork.
void testBoxGlyph();
void testCheckerboard();
void testFullWidth();
void testPaperAlignment();
void testTextJob();
bool testGreyField(uint16_t rows, uint8_t coverage, uint16_t chunkRows,
                   uint16_t widthDots = 0);
void printHistory(uint16_t limit);
void printSettingsSummary();
void forgetWifi();
void startLadder();
void startBandTest();
void startHeatStepTest();
void startCoverageTest();
void startSpeedTest();
void startTailTest();
void startWidthTest();
void selectPaper(uint8_t index);
void showMenu();


// ======================================================================
// Test patterns — all submitted as queue jobs, never printed directly
// ======================================================================

// Builds a bitmap and hands it to the queue. The job takes its own copy of
// the pixels, so the MonoBitmap can safely go out of scope immediately.
static uint32_t submitBitmap(const MonoBitmap& bmp, bool cut) {
  if (!bmp.valid()) { Log.println("[bmp] alloc failed"); return 0; }
  return queue.submitRaster(bmp.data(), bmp.size(), bmp.width(),
                            bmp.height(), SERIAL_SOURCE, cut);
}

void testBoxGlyph() {
  MonoBitmap bmp(32, 32);
  if (!bmp.valid()) return;
  bmp.rectFilled(0,  0, 32,  8);
  bmp.rectFilled(0, 24, 32,  8);
  bmp.rectFilled(0,  8,  8, 16);
  bmp.rectFilled(24, 8,  8, 16);

  queue.submitText("BOX GLYPH\n", SERIAL_SOURCE, 1, 1, true, false);
  Log.printf("[test] box glyph job #%lu\n",
                (unsigned long)submitBitmap(bmp, true));
}

void testCheckerboard() {
  MonoBitmap bmp(32, 32);
  if (!bmp.valid()) return;
  bmp.checkerboard(4);

  queue.submitText("CHECKERBOARD 32x32\n", SERIAL_SOURCE, 1, 1, true, false);
  Log.printf("[test] checkerboard job #%lu\n",
                (unsigned long)submitBitmap(bmp, true));
}

// Paper-width diagnostic. Each band is its own job; the queue is FIFO and
// single-threaded, so they print in submission order.
void testFullWidth() {
  const uint16_t W = printer.printableDots();

  queue.submitText(String("FULL WIDTH - ") + printer.paper().name + "\n"
                   + String(W) + " dots\n", SERIAL_SOURCE, 1, 1, true, false);

  {
    MonoBitmap bmp(W, 48);
    if (bmp.valid()) {
      bmp.rectFilled(0, 0, W, 16);
      for (uint16_t x = 0; x < W; x += 64) bmp.rectFilled(x, 22, 2, 18);
      bmp.rectFilled(W - 2, 22, 2, 26);
      submitBitmap(bmp, false);
    }
  }
  queue.submitText("solid bar + 64-dot ticks\n", SERIAL_SOURCE, 0, 1, false, false);

  const uint16_t widths[] = { 1, 2, 4 };
  for (uint8_t i = 0; i < 3; i++) {
    MonoBitmap bmp(W, 20);
    if (!bmp.valid()) break;
    bmp.verticalStripes(widths[i], widths[i]);
    submitBitmap(bmp, false);
    queue.submitText(String(widths[i]) + "-dot vertical stripes\n", SERIAL_SOURCE, 0, 1, false, false);
  }

  {
    MonoBitmap bmp(W, 64);
    if (bmp.valid()) { bmp.orderedGradient(); submitBitmap(bmp, false); }
  }
  queue.submitText("ordered dither, white -> black\n", SERIAL_SOURCE, 0, 1, false, true);

  Log.printf("[test] full width queued, %u jobs pending\n", queue.pending());
}

// Prints the paper's own edges, so offsetDots can be measured on real paper.
void testPaperAlignment() {
  const uint16_t W = printer.printableDots();

  MonoBitmap bmp(W, 40);
  if (!bmp.valid()) return;
  bmp.rect(0, 0, W, 40, true);
  bmp.rectFilled(0, 0, 8, 40, true);
  bmp.rectFilled(W - 8, 0, 8, 40, true);
  for (uint16_t x = 0; x < W; x += 80) bmp.rectFilled(x, 16, 2, 8, true);

  queue.submitText(String("PAPER ALIGNMENT\n") + printer.paper().name
                   + ", offset " + String(printer.originDots()) + "\n",
                   SERIAL_SOURCE, 1, 1, true, false);
  submitBitmap(bmp, false);
  queue.submitText("Both thick bars must sit fully on paper.\n"
                   "If one is clipped, adjust offsetDots.\n", SERIAL_SOURCE, 0, 1, false, true);
}

// Text job through the full HTTP-equivalent path.
void testTextJob() {
  const Settings& s = settings.get();

  String r;
  r += "SMART THERMAL\n";
  r += "----------------------------\n";
  r += "Host     : " + String(s.hostname) + ".local\n";
  r += "ESP32 IP : " + WiFi.localIP().toString() + "\n";
  r += "Printer  : " + String(transport.describe()) + "\n";
  r += "Paper    : " + String(printer.paper().name) + "\n";
  r += "Dots     : " + String(printer.printableDots()) + "\n";
  r += "Chars    : " + String(printer.charsPerLine()) + "\n";
  r += "Heap     : " + String(Mem::free8() / 1024) + " kB\n";
  r += "Uptime   : " + String(millis() / 1000) + " s\n";
  r += "----------------------------\n";

  Log.printf("[test] text job #%lu\n",
                (unsigned long)queue.submitText(r, SERIAL_SOURCE, 0, 1, false, true));
}

// ======================================================================
// Startup
// ======================================================================

// Non-blocking: the network may take a while, or be gone for good, and
// nothing else should wait on it. serviceWifi() watches from loop().
void startWifi() {
  const Settings& s = settings.get();
  Log.printf("[wifi] joining %s\n", s.wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(s.hostname);
  WiFi.setSleep(false);            // sleep adds latency to every print
  WiFi.setAutoReconnect(true);
  WiFi.begin(s.wifiSsid, s.wifiPass);
}

void startClock() {
  // Local time is only needed for the job log, so nothing waits on it.
  // Records submitted before the first sync store epoch 0 and display as
  // blank rather than as a wrong date.
  // v0.19: the zone can be changed on the Future page (NVS "unit" "tz").
  Scheduler::startClock();
}

void startMdns() {
  const Settings& s = settings.get();
  if (!MDNS.begin(s.hostname)) { Log.println("[mdns] failed"); return; }
  MDNS.addService("http", "tcp", 80);
  Log.printf("[mdns] http://%s.local\n", s.hostname);
}

void showMenu() {
  Log.println();
  Log.printf("  %s  |  %s, %u dots, %u chars\n",
                transport.describe(), printer.paper().name,
                printer.printableDots(), printer.charsPerLine());
  Log.printf("  queue: %u pending, %lu done, %lu failed\n",
                queue.pending(),
                (unsigned long)queue.completed(),
                (unsigned long)queue.failed());
  Log.println("  1 box glyph      4 full width       t text job");
  Log.println("  2 checkerboard   a paper alignment  c feed & cut");
  Log.println("  8 paper 80mm     9 paper 57mm L     0 paper 57mm C");
  Log.println("  p probe          h heap             s settings");
  Log.println("  j job history    x clear history  d debug toggle");
  Log.println("  W forget Wi-Fi and restart into setup (same as holding BOOT 5 s)");
  Log.println("  A reprint the welcome slip   M memory ladder: 96-128 KB images");
#if BUILD_BENCH
  Log.println("  BENCH (each logs how long the printer was idle first)");
  Log.println("  J heat step: 3 panels back to back, from cold      ~45 cm");
  Log.println("  K coverage: 5 strips 5-100% ink, 2 min apart      ~40 cm, 10 min");
  Log.println("  V speed: answered - the printer ignores the command");
  Log.println("  E end step: no tail vs production tail            ~20 cm");
  Log.println("  U width: 100/90/80/70% wide                       ~30 cm");
  Log.println("  H width: full vs half                             ~30 cm");
#endif
  Log.printf("  pacing: limit %u, last %u%% ink, %lu ms print, %lu ms rest\n",
                queue.thermalLimit(), queue.lastCoverage(),
                (unsigned long)queue.lastPrintMs(),
                (unsigned long)queue.lastGapMs());
  Log.println();
  Log.println("  LINE DIAGNOSTIC (ruled uniform field)");
  Log.println("  g 50% grey, saved chunk   G 25% grey, saved chunk");
  Log.println("  [ 50% grey, ONE command  ] 50% grey, 32-row chunks");
  Log.println("  w 50% grey at HALF WIDTH (288 dots), auto chunk");
  Log.println();
  Log.println("  PROBES - run in this order, send me the log");
  Log.println("  l link: reachability, reply speed, sockets   (no paper)");
  Log.println("  r read path: ack before vs after the cut    (~1.2 m)");
  Log.println("  o paper out: pull the roll when told        (~3 cm)");
  Log.println("  e link reset: press INSTEAD of ESP32 reset   (no paper)");
  Log.printf ("  q quiet: background probing is %s\n",
                 queue.probing() ? "ON" : "OFF - nothing touches the printer");
  Log.println("  y density - answered: no software density   (~20 cm)");
  Log.println();
  Log.println("  P show the saved thermal profile");
#if BUILD_TRAINER
  Log.println("  TRAINER (unattended, ~2 h, 40-60 m of paper)");
  Log.printf ("  T %s   X stop and save   Z discard progress\n",
                 Trainer::hasSavedProgress() ? "RESUME training" : "start training ");
  Log.println("  O the stop was PAPER, not heat (press before reloading)");
#endif
}

// Paper changes go through the queue's reconfig hook, so they can never
// land in the middle of a job.
static uint8_t g_pendingPaper = 0;
static void applyPaperOnPrinterTask(void* ctx) {
  printer.setPaper(Papers::byIndex(*static_cast<uint8_t*>(ctx)));
}

void selectPaper(uint8_t index) {
  if (index >= Papers::COUNT) return;
  settings.get().paperIndex = index;
  settings.save();
  g_pendingPaper = index;
  queue.requestReconfig(applyPaperOnPrinterTask, &g_pendingPaper);
  Log.printf("[paper] %s selected (applies before next job)\n",
                Papers::byIndex(index).name);
}

void printHistory(uint16_t limit) {
  StoredJob recs[20];
  if (limit > 20) limit = 20;
  const uint16_t n = jobStore.recent(recs, limit);

  Log.printf("[history] %u stored, showing %u newest\n",
                jobStore.count(), n);

  for (uint16_t i = 0; i < n; i++) {
    char when[24] = "(no clock)";
    if (recs[i].epoch) {
      const time_t t = recs[i].epoch;
      struct tm tm_local;
      localtime_r(&t, &tm_local);
      strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm_local);
    }
    Log.printf("  #%-4lu %-6s %-8s %-12s %-15s %s %s\n",
                  (unsigned long)recs[i].id,
                  jobTypeName(static_cast<JobType>(recs[i].type)),
                  jobStatusName(static_cast<JobStatus>(recs[i].status)),
                  recs[i].source,
                  IPAddress(recs[i].sourceIp[0], recs[i].sourceIp[1],
                            recs[i].sourceIp[2], recs[i].sourceIp[3])
                    .toString().c_str(),
                  when,
                  recs[i].error);
  }
}

void printSettingsSummary() {
  const Settings& s = settings.get();
  Log.printf("[settings] host=%s.local printer=%s:%u paper=%u autocut=%d\n",
                s.hostname, s.printerAddress().toString().c_str(),
                s.printerPort, s.paperIndex, s.autoCut);
  Log.println("[settings] pacing: fixed table, limit 1100 at 30% down to "
              "600 at 95% coverage");

  if (s.chunkRows == 0)
    Log.printf("[settings] chunkRows=auto dither=%s gamma=%.2f debug=%d\n",
                  ditherName(s.defaultDither), s.defaultGamma / 100.0,
                  s.debugMode);
  else
    Log.printf("[settings] chunkRows=%u (MANUAL - auto is recommended) "
                  "dither=%s gamma=%.2f debug=%d\n",
                  s.chunkRows, ditherName(s.defaultDither),
                  s.defaultGamma / 100.0, s.debugMode);
}

// ======================================================================
// LINE ARTEFACT DIAGNOSTIC
// ======================================================================
//
// A long, perfectly uniform field with a millimetre ruler down the left
// edge. Uniform because a faint band cannot be located against
// photographic detail; ruled because WHERE the bands fall is the answer:
//
//   bands at a CONSTANT PITCH      -> mechanical. Platen roller or motor
//                                     step; the pitch is one revolution.
//                                     Not fixable in software.
//   ONE band at a repeatable DEPTH -> thermal. The head throttles after
//                                     that much dense printing. Fixable by
//                                     lowering print density.
//   bands in DIFFERENT places      -> neither; check the 24 V supply.
//
// malloc'd and handed over by pointer rather than built as a MonoBitmap,
// because a MonoBitmap is copied on submission and 72 kB twice will not fit.

// chunkRows == 0 follows the saved setting; anything else overrides it for
// this job only. Passing `rows` sends the whole field as ONE GS v 0
// command, which is the decisive test: if the evenly spaced lines vanish,
// they were command boundaries, not thermal and not mechanical.
bool testGreyField(uint16_t rows, uint8_t coverage, uint16_t chunkRows,
                   uint16_t widthDots) {
  // widthDots 0 means full paper width. A narrower value reproduces the
  // reduced-width path with known-good artwork, separating a geometry fault
  // from anything to do with the uploaded image.
  uint16_t W = widthDots ? widthDots : printer.printableDots();
  W -= (W % 8);
  if (W == 0 || W > printer.printableDots()) W = printer.printableDots();
  const uint16_t rowBytes = W / 8;
  const size_t   bytes    = (size_t)rowBytes * rows;

  uint8_t* buf = (uint8_t*)malloc(bytes);
  if (!buf) {
    Log.printf("[grey] need %u bytes, largest block is %u\n",
                  (unsigned)bytes, Mem::block8());
    return false;
  }
  memset(buf, 0, bytes);

  const uint16_t RULER = 40;          // left gutter reserved for the ruler

  for (uint16_t y = 0; y < rows; y++) {
    uint8_t* row = buf + (size_t)y * rowBytes;

    // Uniform field. coverage 2 = every 2nd dot (50%), 4 = every 4th (25%).
    for (uint16_t x = RULER + 8; x < W; x++) {
      if (((x + y) % coverage) == 0) row[x >> 3] |= 0x80 >> (x & 7);
    }

    // 1 mm ticks every 8 rows, 10 mm ticks every 80 rows.
    const bool tick10 = (y % 80) == 0;
    const bool tick1  = (y % 8)  == 0;
    const uint16_t len = tick10 ? 32 : (tick1 ? 12 : 0);
    for (uint16_t x = 0; x < len; x++) row[x >> 3] |= 0x80 >> (x & 7);
  }

  const uint16_t effective = chunkRows ? chunkRows
                                      : printer.chunkRowsFor(W, rows);

  // The REAL coverage, counted from the buffer rather than inferred from
  // the pattern. The ruler gutter is white, so "every second dot" is not
  // 50% of the paper, and pacing off the nominal figure would rest the
  // head slightly longer than it needs.
  uint32_t setBits = 0;
  for (size_t i = 0; i < bytes; i++) setBits += __builtin_popcount(buf[i]);
  const uint8_t pct = (uint8_t)((setBits * 100ULL + bytes * 4ULL)
                                / (bytes * 8ULL));

  // Label the paper itself, so prints are self-documenting and nothing has
  // to be written on them by hand.
  queue.submitText(String("GREY ") + pct + "%  " + W + " x " + rows
                   + "  chunk " + effective
                   + (effective >= rows ? " (SINGLE COMMAND)" : "")
                   + "\n", SERIAL_SOURCE, 1, 1, true, false);

  const uint32_t id = queue.submitRasterOwned(buf, bytes, W, rows,
                                              SERIAL_SOURCE, true, chunkRows,
                                              pct);
  if (!id) { Log.println("[grey] submit rejected"); return false; }

  Log.printf("[grey] job #%lu: %u x %u (%u mm), %u%% coverage, "
                "chunk %u rows, %u bytes\n",
                (unsigned long)id, W, rows, rows / 8, pct,
                effective, (unsigned)bytes);
  Log.println("[grey] long ticks are 10 mm apart");
  return true;
}


void setup() {
  Serial.begin(115200);
  Log.begin();
  // Before Wi-Fi takes memory: find the spare 32-bit-only IRAM that holds
  // images (Mem.h, v0.25).
  Mem::probeIram();
  Log.printf("[mem] %u KB of IRAM for images\n", (unsigned)(Mem::g_iramPure / 1024));
  delay(400);
  Log.println("\n\nSmart Thermal");

  if (!settings.begin()) Log.println("[nvs] load failed, using defaults");
  const Settings& s = settings.get();
  Unit::begin();

  // The Wi-Fi driver must not keep its own copy of the network in flash:
  // after BOOT forgets the network, that copy would quietly rejoin it.
  WiFi.persistent(false);
  Net::begin();
  pinMode(BOOT_PIN, INPUT_PULLUP);

  transport.setTarget(s.printerAddress(), s.printerPort);
#if PRINTER_LINK_W5500
  // Hardware bring-up happens HERE, before the printer task exists. After
  // this point only the printer task may touch the W5500 over SPI.
  if (!transport.begin())
    Log.println("[w5500] no chip on SPI - printing will fail until "
                   "wiring is fixed, or set PRINTER_LINK_W5500 0 in Config.h");
#endif
  printer.setPaper(Papers::byIndex(s.paperIndex));
  printer.setAutoCut(s.autoCut);
  printer.setFeedBeforeCut(s.feedBeforeCut);
  printer.setRasterChunkRows(s.chunkRows);

  printSettingsSummary();
  {
    ThermalProfile prof;
    ThermalProfileStore::load(prof);
    prof.print();
  }
#if BUILD_TRAINER
  Log.println("[build] TRAINER build - flash with BUILD_TRAINER 0 for lend-out");
  if (Trainer::hasSavedProgress())
    Log.println("[train] saved progress found - 'T' resumes it, 'Z' discards it");
#endif

  // Clear storage / factory reset (v0.26): format before anything opens a
  // file. The flag is set by /api/system/storage/clear and /api/system/reset.
  {
    Preferences p;
    bool fmt = false;
    if (p.begin("unit", true)) { fmt = p.getBool("fmt", false); p.end(); }
    if (fmt) {
      Log.println("[storage] formatting (clear storage / factory reset)");
      LittleFS.begin(true);
      LittleFS.format();
      LittleFS.end();
      if (p.begin("unit", false)) { p.remove("fmt"); p.end(); }
    }
  }
  if (!jobStore.begin())
    Log.println("[history] LittleFS mount failed - history will not persist");
  else {
    Log.enableSave();                  // the log is also kept on flash (v0.22)
    Log.save(true);
  }
  if (jobStore.mounted())
    Log.printf("[history] %u of %u records restored, next job #%lu\n",
                  jobStore.count(), JobStore::CAPACITY,
                  (unsigned long)jobStore.highestId() + 1);

  // Logged before the queue starts so it takes an id below the first job,
  // and so a reboot mid-session is visible in the history rather than
  // showing up as an unexplained gap in job numbering.
  {
    char note[40];
    snprintf(note, sizeof(note), "boot v%s", FIRMWARE_VERSION);
    const uint32_t id = jobStore.logEvent(
      static_cast<uint8_t>(JobType::Event), "system", note,
      (uint32_t)(time(nullptr) > 1609459200 ? time(nullptr) : 0));
    Log.printf("[history] logged boot as #%lu\n", (unsigned long)id);
  }

  queue.setDebug(s.debugMode);

  if (!queue.begin(printer, jobStore, 8)) {
    Log.println("[queue] FAILED TO START - nothing will print");
  } else {
    Log.println("[queue] printer task running on core 1");
  }

  desk.begin(queue, jobStore, printer.printableDots());

  setupMode = (s.wifiSsid[0] == 0);
  if (setupMode) {
    Log.println("[setup] no network saved - starting setup");
    portal.begin();
    desk.wantSetup();
  } else {
    startWifi();
    if (jobStore.mounted() && items.begin()) sched.begin(items, queue, jobStore);
    Online::begin(desk, queue);          // online printing, if switched on
    api.attachItems(items, sched);
    api.begin(80);
  }

  showMenu();
}

// ======================================================================
// Running
// ======================================================================

// Watches the saved network in normal mode. It retries for ever and never
// falls back to setup by itself: only BOOT does that (a decision, so a
// router outage can never turn a working unit into an open setup point).
// Keep-awake (v0.22, troubleshooting the printer that stops answering after
// hours of quiet): after KEEPAWAKE_HOURS with no print, send the printer a
// job that prints nothing - ESC @ (reset) over a fresh connection, after the
// usual paper/cover status check - so its network side does real work.
// It shows in the history as a "keep-awake" raw job. 0 hours turns it off.
void serviceKeepAwake() {
#if KEEPAWAKE_HOURS > 0
  static uint32_t last = 0;
  const uint32_t every = (uint32_t)KEEPAWAKE_HOURS * 3600UL * 1000UL;
  const uint32_t now = millis();
  if (now < every || (last && now - last < every)) return;
  const uint32_t idle = queue.idleMs();
  if (idle && idle < every) return;                     // printed recently enough
  if (!queue.printerReachable() || queue.pending() || queue.busy()) return;
  last = now;
  // A phantom print (v0.23.1): one row of white dots, no cut, no tail -
  // the printer goes through a real print job but nothing marks the paper
  // (it feeds 1/8 mm). Tests whether a real print keeps it answering.
  static uint8_t row[72] = {0};                         // 576 dots, all white
  uint16_t w = printer.printableDots() & ~7;
  if (w == 0 || w > 576) w = 576;
  const uint32_t id = queue.submitRaster(row, w / 8, w, 1, JobSource::local("keep-awake"),
                                         false, 0, 0, false);
  Log.printf("[awake] %lu h without a print - phantom print job #%lu sent (1 white row)\n",
             (unsigned long)KEEPAWAKE_HOURS, (unsigned long)id);
#endif
}

void serviceWifi() {
  static bool     online      = false;
  static bool     servicesUp  = false;
  static uint32_t downSince   = 0;
  static uint32_t lastKick    = 0;

  const Settings& s = settings.get();
  const uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (online) return;
    online = true;
    const IPAddress ip = WiFi.localIP();
    Log.printf("[wifi] joined %s, ip %s\n", s.wifiSsid, ip.toString().c_str());
    if (!servicesUp) {
      startClock();
      startMdns();
      servicesUp = true;
      Log.printf("[http] http://%s/ and http://%s.local/\n",
                    ip.toString().c_str(), s.hostname);
    }
    if (!Unit::announced(ip, s.hostname)) desk.wantAddresses(ip, s.hostname);
    return;
  }

  if (online) {
    online    = false;
    downSince = now;
    Log.println("[wifi] lost the network - retrying");
  }

  // The driver's own reconnect gives up after some failures; this does not.
  if (now - lastKick > 30000 && now - downSince > 30000) {
    lastKick = now;
    WiFi.disconnect();
    WiFi.begin(s.wifiSsid, s.wifiPass);
  }
}

// ----------------------------------------------------------------------
// Memory ladder ('M'): how large an image can this unit hold and print?
// ----------------------------------------------------------------------
// One 50% grey image at 96 KB, then 100, 104 ... 128 KB, each only after
// the last has finished printing and its buffer is freed, so exactly one
// image is ever in memory. Stops at the first size that cannot be
// allocated in one piece: that is this unit's ceiling. A web upload's
// ceiling is somewhat lower, because the web server needs memory too.
static uint16_t s_ladderKB   = 0;       // 0 = not running
static uint32_t s_ladderJob  = 0;

void startLadder() {
  s_ladderKB  = 96;
  s_ladderJob = 0;
  Log.printf("[ladder] start: 96 to 128 KB in 4 KB steps, 50%% grey. "
             "Largest free block now %u KB.\n",
             (unsigned)(Mem::block8() / 1024));
}

void serviceLadder() {
  if (!s_ladderKB) return;

  if (s_ladderJob) {                    // wait for the last one to finish
    StoredJob r;
    if (!jobStore.get(s_ladderJob, r)) return;
    if (r.status != (uint8_t)JobStatus::Done &&
        r.status != (uint8_t)JobStatus::Failed) return;
    Log.printf("[ladder] #%lu %s\n", (unsigned long)s_ladderJob,
               r.status == (uint8_t)JobStatus::Done ? "printed" : "FAILED");
    s_ladderJob = 0;
    s_ladderKB += 4;
    if (s_ladderKB > 128) {
      Log.println("[ladder] done - every size up to 128 KB fitted");
      s_ladderKB = 0;
      return;
    }
  }
  if (queue.pending() || queue.busy()) return;

  const uint16_t W        = printer.printableDots();
  const size_t   rowBytes = W / 8;
  const uint16_t rows     = (s_ladderKB * 1024UL) / rowBytes;
  const size_t   len      = rowBytes * rows;
  const size_t   largest  = Mem::block8();

  uint8_t* buf = static_cast<uint8_t*>(malloc(len));
  if (!buf) {
    Log.printf("[ladder] %u KB: CANNOT ALLOCATE - largest free block is %u KB. "
               "That is this unit's ceiling.\n",
               s_ladderKB, (unsigned)(largest / 1024));
    s_ladderKB = 0;
    return;
  }
  for (uint16_t y = 0; y < rows; y++)
    memset(buf + y * rowBytes, (y & 1) ? 0x55 : 0xAA, rowBytes);

  s_ladderJob = queue.submitRasterOwned(buf, len, W, rows, SERIAL_SOURCE,
                                        true, 0, 50);
  if (!s_ladderJob) {
    Log.printf("[ladder] %u KB: queue refused it\n", s_ladderKB);
    s_ladderKB = 0;
    return;
  }
  Log.printf("[ladder] %u KB = %u rows (%u mm) queued as #%lu; largest block "
             "was %u KB\n", s_ladderKB, rows, rows / 8,
             (unsigned long)s_ladderJob, (unsigned)(largest / 1024));
}

// ----------------------------------------------------------------------
// Bench tests. BUILD_BENCH 0 leaves every one of them out of the build.
// ----------------------------------------------------------------------
#if BUILD_BENCH
// A bench test is a short list of steps. Each step is one labelled ruled
// panel, optionally preceded by raw printer commands, and may require a
// cooling gap after the previous step. The runner prints one step at a
// time, logs how long the printer had been idle before the test (the
// starting temperature, near enough), and STOPS at the first failed step
// rather than hammering a printer that has just faulted.
struct BenchStep {
  uint16_t width;          // dots
  uint16_t rows;
  uint8_t  level;          // coverage in 64ths
  uint32_t gapMs;          // cooling wait before this step
  uint8_t  pre[8];         // raw commands sent before the panel
  uint8_t  preLen;
  uint16_t padRows;        // white rows appended inside the same image
  bool     tail;           // production white tail (off for raw panels)
  char     label[48];
};
static BenchStep s_steps[6];
static int8_t    s_count = 0, s_idx = -1;
static uint32_t  s_job = 0, s_doneAt = 0;
static bool      s_gapNoted = false;
static char      s_name[24];

static void benchStart(const char* name) {
  strlcpy(s_name, name, sizeof(s_name));
  s_idx = 0; s_job = 0; s_doneAt = 0; s_gapNoted = false;
  const uint32_t idle = queue.idleMs();
  if (idle) Log.printf("[bench] %s: %d steps. Printer idle for %lu min before "
                       "it.\n", s_name, s_count, (unsigned long)(idle / 60000));
  else      Log.printf("[bench] %s: %d steps. No print since power-up "
                       "(%lu min ago).\n", s_name, s_count,
                       (unsigned long)(millis() / 60000));
}

static void benchStep(uint16_t w, uint16_t rows, uint8_t level, uint32_t gapMs,
                      const char* label, const uint8_t* pre = nullptr,
                      uint8_t preLen = 0, uint16_t padRows = 0,
                      bool tail = false) {
  BenchStep& st = s_steps[s_count++];
  st.width = w; st.rows = rows; st.level = level; st.gapMs = gapMs;
  st.preLen = preLen; st.padRows = padRows; st.tail = tail;
  if (preLen) memcpy(st.pre, pre, preLen);
  strlcpy(st.label, label, sizeof(st.label));
}

void serviceBench() {
  if (s_idx < 0) return;
  if (s_job) {
    StoredJob r;
    if (!jobStore.get(s_job, r)) return;
    if (r.status != (uint8_t)JobStatus::Done &&
        r.status != (uint8_t)JobStatus::Failed) return;
    const bool failed = r.status == (uint8_t)JobStatus::Failed;
    Log.printf("[bench] %s step %d %s\n", s_name, s_idx + 1,
               failed ? "FAILED - stopping here" : "printed");
    s_job = 0; s_doneAt = millis(); s_gapNoted = false;
    if (failed || ++s_idx >= s_count) {
      Log.printf("[bench] %s finished\n", s_name);
      s_idx = -1;
      return;
    }
  }
  const BenchStep& st = s_steps[s_idx];
  if (s_doneAt && millis() - s_doneAt < st.gapMs) {
    if (!s_gapNoted) Log.printf("[bench] cooling %lu s before step %d\n",
                                (unsigned long)(st.gapMs / 1000), s_idx + 1);
    s_gapNoted = true;
    return;
  }
  if (queue.pending() || queue.busy()) return;

  size_t len = 0;
  const uint16_t total = st.rows + st.padRows;
  uint8_t* buf = BandTest::field(st.width, total, st.level, len);
  if (!buf) { Log.println("[bench] out of memory - stopping"); s_idx = -1; return; }
  if (st.padRows)                               // blank the tail, rulers too
    memset(buf + static_cast<size_t>(st.rows) * (st.width / 8), 0,
           static_cast<size_t>(st.padRows) * (st.width / 8));
  queue.submitText(st.label, SERIAL_SOURCE, 0, 1, true, false);
  // Raw commands ride inside the image job, after its ESC @: sent as a job
  // of their own, the next job's reset would undo them.
  s_job = queue.submitRasterOwned(buf, len, st.width, total, SERIAL_SOURCE,
                                  s_idx == s_count - 1, total, 0,
                                  st.preLen ? st.pre : nullptr, st.preLen,
                                  st.tail);
  if (!s_job) { Log.println("[bench] queue refused - stopping"); s_idx = -1; }
}

// H: full vs half width, 128 mm, 31% - is the step time or data?
void startBandTest() {
  s_count = 0;
  benchStep(576, 1024, 20, 0, "H1  FULL width, 128 mm, 31%\n");
  benchStep(288, 1024, 20, 0, "H2  HALF width, 128 mm, 31%\n");
  benchStart("band test H");
}

// J: the same full-width panel three times, back to back, from cold. If the
// step to lighter is the printer's heat compensation it moves up the paper
// on each print; if it stays put, it is not heat.
void startHeatStepTest() {
  s_count = 0;
  for (int i = 1; i <= 3; i++) {
    char l[48]; snprintf(l, sizeof(l), "J%d  FULL width, 128 mm, 31%%\n", i);
    benchStep(576, 1024, 20, 0, l);
  }
  benchStart("heat-step test J");
}

// K: coverage ladder at constant bytes. Five 64 mm strips, identical size
// (36 KB each), 5/25/50/75/100% ink, two minutes' cooling before each so
// every strip starts from a similar head. Only the dots fired change: a
// strip that bands or faults where a lighter one did not points to heat or
// power, never to data.
void startCoverageTest() {
  s_count = 0;
  const uint8_t lv[5] = { 3, 16, 32, 48, 64 };
  const char*   nm[5] = { "5", "25", "50", "75", "100" };
  for (int i = 0; i < 5; i++) {
    char l[48]; snprintf(l, sizeof(l), "K%d  %s%% ink, 64 mm, same bytes\n", i + 1, nm[i]);
    benchStep(576, 512, lv[i], i ? 120000 : 0, l);
  }
  benchStart("coverage test K");
}

// E: the end step. The printer prints at ~213 mm/s, which at full width
// needs ~120 KB/s; it takes data in at only ~111-118 KB/s. So it runs
// slightly short of data for the whole image, and once the last byte has
// arrived it runs free: the tail prints lighter, with a line at the
// switch. The switch happens wherever the paper is when the data ends -
// so if the image ends in white rows, it happens in white, unseen.
// E1 is a plain 64 mm panel with no tail; E2 is the same panel as a normal
// print makes it, with the production tail (tailRowsFor). E2: no step.
void startTailTest() {
  s_count = 0;
  benchStep(576, 512, 20, 0, "E1  64 mm, 31%, no tail\n");
  benchStep(576, 512, 20, 0, "E2  64 mm, 31%, production tail\n",
            nullptr, 0, 0, true);
  benchStart("end-step test E");
}

// U: width. The same shortfall should vanish when the image is narrower:
// 90% needs ~108 KB/s (borderline), 80% ~96 KB/s, 70% ~84 KB/s.
void startWidthTest() {
  s_count = 0;
  const uint16_t w[4]  = { 576, 520, 464, 400 };   // multiples of 8
  const char*    nm[4] = { "100", "90", "80", "70" };
  for (int i = 0; i < 4; i++) {
    char l[48]; snprintf(l, sizeof(l), "U%d  %s%% width, 64 mm, 31%%\n", i + 1, nm[i]);
    benchStep(w[i], 512, 20, 0, l);
  }
  benchStart("width test U");
}

// V: print speed. GS ( K fn 50 selects a print speed on Epson-compatible
// printers (1 = slowest). Epson's own fix for white lines is a lower,
// constant speed. Two full-width panels: slowest, then the printer's
// default. If the slow panel has no step and no lines, the command is
// honoured and a slow photo mode is possible. Switch the printer off and
// on afterwards: the setting is not saved, but that clears it for certain.
void startSpeedTest() {
  s_count = 0;
  static const uint8_t SLOW[7] = { 0x1D, 0x28, 0x4B, 0x02, 0x00, 0x32, 0x01 };
  benchStep(576, 1024, 20, 0,      "V1  speed 1 (slowest), 128 mm, 31%\n", SLOW, 7);
  benchStep(576, 1024, 20, 180000, "V2  default speed, 128 mm, 31%\n");
  benchStart("speed test V");
}
#endif

// BOOT held for 5 s while running: forget the network, restart into setup.
// In setup mode the same hold restarts setup, which reprints the slip.
void serviceBootButton() {
  static uint32_t downAt = 0;
  if (digitalRead(BOOT_PIN) != LOW) { downAt = 0; return; }

  const uint32_t now = millis();
  if (!downAt) {
    downAt = now ? now : 1;
    Log.println("[boot] BOOT pressed - hold 5 s to forget Wi-Fi");
  } else if (now - downAt >= BOOT_HOLD_MS) {
    forgetWifi();
  }
}

void forgetWifi() {
  Settings& st = settings.get();
  st.wifiSsid[0] = 0;
  st.wifiPass[0] = 0;
  settings.save();
  WiFi.disconnect(true, true);
  Log.println("[boot] Wi-Fi forgotten - restarting into setup");
  delay(300);
  ESP.restart();
}

// Probes drive the Printer directly, so they are handed to the printer
// task rather than run here - invariant 1 holds for diagnostics too.
void runProbe(PrintQueue::TaskFn fn, const char* name) {
  if (queue.runOnPrinterTask(fn, nullptr))
    Log.printf("[probe] %s queued - starts when the printer is idle\n", name);
  else
    Log.println("[probe] another probe is still running");
}

void loop() {
  if (setupMode) portal.loop();
  else         { api.loop(); serviceWifi(); sched.loop(); Online::loop(); serviceKeepAwake(); }
  Log.save();
  serviceBootButton();
  desk.loop();
  serviceLadder();
#if BUILD_BENCH
  serviceBench();
#endif

  // The printer task found the printer somewhere new. Settings belong to
  // this task, so the save happens here rather than on the printer task.
  {
    IPAddress moved;
    if (queue.takeNewPrinterAddress(moved)) {
      Settings& st = settings.get();
      for (uint8_t i = 0; i < 4; i++) st.printerIp[i] = moved[i];
      settings.save();
      Log.printf("[find] saved printer address %s\n", moved.toString().c_str());
    }
  }

  if (Serial.available()) {
    switch (Serial.read()) {
      case '1': testBoxGlyph();       break;
      case '2': testCheckerboard();   break;
      case '4': testFullWidth();      break;
      case 'a': testPaperAlignment(); break;
      case 't': testTextJob();        break;
      case 'c': queue.submitText("", SERIAL_SOURCE, 0, 1, false, true); break;
      case '8': selectPaper(0);       break;
      case '9': selectPaper(1);       break;
      case '0': selectPaper(2);       break;
      case 'p': Log.printf("[printer] reachable: %s (probed by print task)\n",
                              api.printerReachable() ? "yes" : "no"); break;
      case 'd': { Settings& st = settings.get();
                  st.debugMode = !st.debugMode; settings.save();
                  queue.setDebug(st.debugMode);
                  Log.printf("[debug] %s\n", st.debugMode ? "on" : "off"); }
                break;
      case 'h': Log.printf("[heap] free %u, largest %u\n",
                              Mem::free8(), Mem::block8()); break;
      case 's': printSettingsSummary(); break;
      case 'g': testGreyField(1000, 2, 0);    break;   // saved chunk
      case 'G': testGreyField(1000, 4, 0);    break;
      case '[': testGreyField(1000, 2, 1000); break;   // ONE command
      case ']': testGreyField(1000, 2, 32);   break;   // many boundaries
      case 'w': testGreyField(1000, 2, 0, 288); break; // 50% width, auto
      case 'l': runProbe(Diagnostics::link,     "link");      break;
      case 'r': runProbe(Diagnostics::readPath, "read path"); break;
      case 'o': runProbe(Diagnostics::paperOut, "paper out"); break;
      case 'y': runProbe(Diagnostics::density,  "density");   break;
      case 'e': runProbe(Diagnostics::resetLink, "link reset"); break;
      case 'P': { ThermalProfile prof; ThermalProfileStore::load(prof); prof.print(); break; }
#if BUILD_TRAINER
      case 'T': runProbe(Trainer::run, "trainer"); break;
      case 'O': Trainer::notePaperOut(); break;
      case 'X': Trainer::requestStop();
                Log.println("[train] stop requested - saving at the next safe point");
                break;
      case 'Z': Trainer::discardProgress();
                Log.println("[train] saved progress discarded");
                break;
#endif
      case 'q': queue.setProbing(!queue.probing());
                Log.printf("[probe] background probing %s\n",
                              queue.probing() ? "ON"
                                : "OFF - nothing will connect to the printer "
                                  "until a job, a probe, or 'q' again");
                break;
      case 'j': printHistory(20);       break;
      case 'W': forgetWifi(); break;
      case 'M': startLadder(); break;
#if BUILD_BENCH
      case 'H': startBandTest(); break;
      case 'J': startHeatStepTest(); break;
      case 'K': startCoverageTest(); break;
      case 'V': startSpeedTest(); break;
      case 'E': startTailTest(); break;
      case 'U': startWidthTest(); break;
#endif
      case 'A': if (!setupMode && WiFi.status() == WL_CONNECTED) {
                  desk.wantAddresses(WiFi.localIP(), settings.get().hostname);
                  Log.println("[slip] welcome slip queued");
                } else Log.println("[slip] not on a network yet");
                break;
      case 'x': jobStore.clear();
                Log.println("[history] cleared"); break;
      case '\r': case '\n': break;
      default:  showMenu();           break;
    }
  }

  delay(2);
}

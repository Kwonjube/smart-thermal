#include "WebApi.h"
#include "Config.h"
#include "WebUI.h"
#include "WebUIImage.h"
#include "WebUITemplate.h"
#include "WebUITheme.h"
#include "WebUIFuture.h"
#include "Online.h"
#include "LogPage.h"
#include "Alerts.h"
#include "KioskPage.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <time.h>

// Text jobs are buffered in RAM before queueing, so cap them. 8 KB is
// roughly 170 full lines of Font A text — far more than any receipt.
static const size_t MAX_TEXT_BYTES = 8192;
static const size_t MAX_RAW_BYTES  = 4096;

WebApi::WebApi(SettingsStore& settings, PrintQueue& queue, JobStore& store,
               Printer& printer, Transport& transport)
  : _settings(settings), _queue(queue), _store(store), _printer(printer),
    _transport(transport), _server(80) {}

void WebApi::begin(uint16_t port) {
  // Content-Type is not retained by default, and the text/plain shortcut
  // in handlePrintText depends on seeing it.
  // User-Agent is needed to label where a job came from; Content-Type for
  // the text/plain shortcut. Neither is retained by default.
  // X-Via-Relay marks a request that came in through the outside link
  // (v0.20): such requests may print and use drafts, but not Settings.
  const char* wanted[] = { "Content-Type", "User-Agent", "X-Via-Relay" };
  _server.collectHeaders(wanted, 3);

  routes();
  _server.begin(port);
}

void WebApi::loop() { _server.handleClient(); }

// ----------------------------------------------------------------------
// Routing
// ----------------------------------------------------------------------

void WebApi::routes() {
  _server.on("/",                    HTTP_GET,  [this]{ handleIndex(); });

  _server.on("/api/printer/status",  HTTP_GET,  [this]{ handleStatus(); });
  // Party kiosk: the page, and its set-up (kept on the unit so the kiosk
  // computer picks up what was configured from a phone).
  _server.on("/kiosk",     HTTP_GET,  [this]{
    _server.sendHeader("Cache-Control", "no-store");
    _server.send_P(200, "text/html", KIOSK_PAGE); });
  _server.on("/api/kiosk", HTTP_GET,  [this]{ handleGetKiosk(); });
  _server.on("/api/kiosk", HTTP_POST, [this]{ if (localOnly()) handlePostKiosk(); });
  // Theme wording (e.g. Parking fine), saved once and seen by every phone.
  itemRoutes();                       // drafts, saved, scheduled (v0.19)
  _server.on("/api/theme", HTTP_GET,  [this]{ handleGetTheme(); });
  _server.on("/api/theme", HTTP_POST, [this]{ handlePostTheme(); });
  // Settings belong to the unit and are changed only from its own network
  // (v0.20). Preferences live in each browser and need nothing here.
  _server.on("/api/online",          HTTP_GET,  [this]{ if (localOnly()) handleOnline(); });
  _server.on("/api/online",          HTTP_POST, [this]{ if (localOnly()) handleOnline(); });
  _server.on("/api/online/newlink",  HTTP_POST, [this]{ if (localOnly()) { Online::newLink(); handleOnline(); } });
  _server.on("/api/online/newowner", HTTP_POST, [this]{ if (localOnly()) { Online::newOwnerCode(); handleOnline(); } });
  _server.on("/api/settings",        HTTP_GET,  [this]{ if (localOnly()) handleGetSettings(); });
  _server.on("/api/settings",        HTTP_POST, [this]{ if (localOnly()) handlePostSettings(); });
  _server.on("/api/print/text",      HTTP_POST, [this]{ handlePrintText(); });
  _server.on("/api/print/raster",    HTTP_POST,
             [this]{ handlePrintRaster(); },
             [this]{ handleRasterUpload(); });
  _server.on("/api/print/raw",       HTTP_POST, [this]{ if (localOnly()) handlePrintRaw(); });
  _server.on("/api/jobs",            HTTP_GET,  [this]{ handleJobs(); });
  _server.on("/api/jobs/clear",      HTTP_POST, [this]{ if (localOnly()) handleJobsClear(); });
  _server.on("/api/system/reset",    HTTP_POST, [this]{ if (localOnly()) handleFactoryReset(); });
  _server.on("/api/system/reboot",   HTTP_POST, [this]{ if (localOnly()) handleReboot(); });
  _server.on("/log",     HTTP_GET, [this]{ if (localOnly()) _server.send_P(200, "text/html", LOG_PAGE); });
  _server.on("/log.txt", HTTP_GET, [this]{
    if (!localOnly()) return;
    if (_server.hasArg("saved")) sendSavedLog(); else sendLogText(_server); });

  // Browsers preflight any JSON POST from a different origin.
  _server.onNotFound([this]{
    if (_server.method() == HTTP_OPTIONS) { cors(); _server.send(204); }
    else handleNotFound();
  });
}

// ----------------------------------------------------------------------
// Handlers
// ----------------------------------------------------------------------

void WebApi::handleIndex() {
  // Sent in parts so the page can live in a few readable PROGMEM blocks
  // rather than one enormous literal. Chunked, because the total length is
  // not worth computing.
  cors();
  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.send(200, "text/html", "");
  _server.sendContent_P(WEB_UI_HEAD);
  _server.sendContent_P(WEB_UI_TEMPLATE);
  _server.sendContent_P(WEB_UI_THEME);
  _server.sendContent_P(WEB_UI_FUTURE);
  _server.sendContent_P(WEB_UI_IMAGE);
  _server.sendContent_P(WEB_UI_TAIL);
  _server.sendContent("");
}

void WebApi::handleStatus() {
  const Settings& s = _settings.get();

  JsonDocument doc;
  doc["connected"]     = printerReachable();
  // Online visitors are not told the home network's addresses (v0.23.2).
  const bool away = viaRelay();
  if (!away) {
    doc["ip"]          = _transport.ip().toString();
    doc["port"]        = _transport.port();
  }
  // Cached text, never a live query: on the W5500 a live query is an SPI
  // transaction, and this runs on the web task.
  doc["link"]          = _transport.linkState();
  // Discovery state, for setup and error messages: searching the cable,
  // or no cable link at all (printer off or unplugged).
  doc["searching"]     = _queue.printerSearching();
  {
    const AlertText a = alertText(_queue.alert());
    JsonObject al = doc["alert"].to<JsonObject>();
    al["code"]    = a.code;
    al["message"] = a.message;
  }
  // Not an error: the head is resting before the next queued print.
  doc["restingMs"]     = _queue.restingMs();
  doc["linkDown"]      = _queue.printerLinkDown();

  JsonObject paper     = doc["paper"].to<JsonObject>();
  paper["name"]        = _printer.paper().name;
  paper["index"]       = s.paperIndex;
  paper["widthMM"]     = _printer.paper().widthMM;
  paper["printableDots"] = _printer.printableDots();
  paper["offsetDots"]  = _printer.originDots();
  paper["charsPerLine"] = _printer.charsPerLine();

  JsonObject q         = doc["queue"].to<JsonObject>();
  q["pending"]         = _queue.pending();
  q["busy"]            = _queue.busy();
  q["completed"]       = _queue.completed();
  q["failed"]          = _queue.failed();
  q["faults"]          = _queue.faults();

  // Thermal pacing. coolingMs is non-zero only while the printer is inside
  // the ~30 s window after a fault, when it answers nothing at all — the UI
  // shows it so a silent queue has a visible reason.
  JsonObject pace      = doc["pacing"].to<JsonObject>();
  pace["thermalLimit"] = _queue.thermalLimit();
  pace["coverage"]     = _queue.lastCoverage();
  pace["printMs"]      = _queue.lastPrintMs();
  pace["gapMs"]        = _queue.lastGapMs();
  pace["coolingMs"]    = _queue.coolingMs();

  char nowStr[24];
  const time_t tnow = time(nullptr);
  formatLocal(tnow > 1609459200 ? (uint32_t)tnow : 0, nowStr, sizeof(nowStr));

  JsonObject sys       = doc["system"].to<JsonObject>();
  sys["version"]       = FIRMWARE_VERSION;
  sys["built"]         = FIRMWARE_DATE;
  sys["localTime"]     = nowStr;
  sys["clockSet"]      = tnow > 1609459200;
  sys["historyStored"] = _store.count();
  itemCounts(doc["items"].to<JsonObject>());     // the Future box and Saved bar
  doc["local"] = !viaRelay();                    // Settings shown only when true
  {
    // Photo defaults, readable through the website too (v0.22): the photo
    // card used to read them from /api/settings, which is home-only.
    JsonObject im = doc["image"].to<JsonObject>();
    im["threshold"]  = s.defaultThreshold;  im["gamma"]    = s.defaultGamma;
    im["brightness"] = s.defaultBrightness; im["contrast"] = s.defaultContrast;
  }
  {
    JsonObject on = doc["online"].to<JsonObject>();
    on["state"] = Online::stateKey();
    on["text"]  = Online::stateText();
    on["paused"] = Online::paused();
    if (!viaRelay()) on["url"] = Online::link();
  }
  sys["hostname"]      = s.hostname;
  if (!away) {
    sys["ip"]          = WiFi.localIP().toString();
    sys["rssi"]        = WiFi.RSSI();
  }
  sys["freeHeap"]      = ESP.getFreeHeap();
  sys["largestBlock"]  = ESP.getMaxAllocHeap();
  // An image needs ONE contiguous allocation, so the largest free block —
  // not total free heap — is the real ceiling. Reported with a margin so
  // the browser can warn before the upload fails.
  {
    const size_t block = ESP.getMaxAllocHeap();
    // Online, the secure link to the website needs room of its own while a
    // photo arrives, so the margin is larger (v0.23.1).
    const size_t margin = (viaRelay() ? 24 : 12) * 1024;
    size_t usable = block > margin ? block - margin : 0;
    if (usable > MAX_RASTER_BYTES) usable = MAX_RASTER_BYTES;
    sys["maxImageBytes"] = usable;
  }
  sys["chunkRows"]     = _printer.rasterChunkRows();
  sys["debug"]         = _queue.debug();
  sys["uptimeSec"]     = millis() / 1000;

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void WebApi::handleGetSettings() {
  const Settings& s = _settings.get();

  JsonDocument doc;
  doc["hostname"]      = s.hostname;
  doc["wifiSsid"]      = s.wifiSsid;
  // Deliberately never returned. The UI shows a blank field; an empty
  // value on POST means "leave unchanged".
  doc["wifiPassSet"]   = strlen(s.wifiPass) > 0;
  doc["printerIp"]     = s.printerAddress().toString();
  doc["printerPort"]   = s.printerPort;
  doc["paperIndex"]    = s.paperIndex;
  doc["autoCut"]       = s.autoCut;
  doc["feedBeforeCut"] = s.feedBeforeCut;
  doc["dither"]        = ditherName(s.defaultDither);
  doc["chunkRows"]     = s.chunkRows;
  doc["debug"]         = s.debugMode;
  doc["threshold"]     = s.defaultThreshold;
  doc["gamma"]         = s.defaultGamma;
  doc["brightness"]    = s.defaultBrightness;
  doc["contrast"]      = s.defaultContrast;

  JsonArray papers = doc["papers"].to<JsonArray>();
  for (uint8_t i = 0; i < Papers::COUNT; i++) {
    JsonObject p = papers.add<JsonObject>();
    p["index"]         = i;
    p["name"]          = Papers::TABLE[i]->name;
    p["widthMM"]       = Papers::TABLE[i]->widthMM;
    p["printableDots"] = Papers::TABLE[i]->printableDots;
  }

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

// Applied by the printer task between jobs — see PrintQueue::requestReconfig.
struct ReconfigCtx {
  Printer*      printer;
  PrintQueue*   queue;
  Transport*    transport;
  uint8_t       paperIndex;
  IPAddress     ip;
  uint16_t      port;
  uint16_t      chunkRows;
  bool          autoCut;
  uint8_t       feedBeforeCut;
};
static ReconfigCtx g_reconfig;

static void applySettingsOnPrinterTask(void* ctx) {
  ReconfigCtx* c = static_cast<ReconfigCtx*>(ctx);
  c->printer->setPaper(Papers::byIndex(c->paperIndex));
  c->printer->setRasterChunkRows(c->chunkRows);
  c->printer->setAutoCut(c->autoCut);
  c->printer->setFeedBeforeCut(c->feedBeforeCut);
  c->transport->setTarget(c->ip, c->port);
}

void WebApi::handlePostSettings() {
  const String raw = body();
  if (raw.isEmpty()) { sendError(400, "empty body"); return; }

  JsonDocument doc;
  if (deserializeJson(doc, raw)) { sendError(400, "invalid json"); return; }

  Settings& s = _settings.get();

  if (doc["hostname"].is<const char*>())
    strlcpy(s.hostname, doc["hostname"], sizeof(s.hostname));
  if (doc["wifiSsid"].is<const char*>())
    strlcpy(s.wifiSsid, doc["wifiSsid"], sizeof(s.wifiSsid));

  // An empty or absent password means "keep the stored one".
  if (doc["wifiPass"].is<const char*>()) {
    const char* p = doc["wifiPass"];
    if (p && *p) strlcpy(s.wifiPass, p, sizeof(s.wifiPass));
  }

  if (doc["printerIp"].is<const char*>()) {
    IPAddress ip;
    if (!ip.fromString(doc["printerIp"].as<const char*>())) {
      sendError(400, "invalid printerIp"); return;
    }
    for (uint8_t i = 0; i < 4; i++) s.printerIp[i] = ip[i];
  }

  if (doc["printerPort"].is<uint16_t>()) s.printerPort = doc["printerPort"];

  if (doc["paperIndex"].is<uint8_t>()) {
    const uint8_t idx = doc["paperIndex"];
    if (idx >= Papers::COUNT) { sendError(400, "invalid paperIndex"); return; }
    s.paperIndex = idx;
  }

  if (doc["autoCut"].is<bool>())          s.autoCut       = doc["autoCut"];
  if (doc["feedBeforeCut"].is<uint8_t>()) s.feedBeforeCut = doc["feedBeforeCut"];
  if (doc["threshold"].is<uint8_t>())     s.defaultThreshold = doc["threshold"];
  if (doc["gamma"].is<uint8_t>())         s.defaultGamma  = doc["gamma"];
  if (doc["brightness"].is<int8_t>())     s.defaultBrightness = doc["brightness"];
  if (doc["contrast"].is<int8_t>())       s.defaultContrast = doc["contrast"];

  // Dither arrives as a NAME, never an index. Indices drifted once already
  // when the ordering changed and a stored 1 silently became a different
  // algorithm; see the DitherMode comment in Settings.h.
  if (doc["dither"].is<const char*>())
    s.defaultDither = ditherIndex(doc["dither"]);

  if (doc["chunkRows"].is<uint16_t>()) {
    const uint16_t cr = doc["chunkRows"];
    // 0 is meaningful: it selects automatic sizing, which is the default.
    if (cr != 0 && (cr < 8 || cr > 4096)) {
      sendError(400, "chunkRows must be 0 (auto) or 8-4096"); return;
    }
    s.chunkRows = cr;
  }

  // thermalLimit is no longer a setting (v0.9.4): pacing comes from the
  // fixed table in PrintQueue.cpp. A value sent by an old page is ignored.

  if (doc["debug"].is<bool>()) {
    s.debugMode = doc["debug"];
    // A plain bool, safe to apply immediately — unlike the paper and target
    // changes, which must wait for a gap between jobs.
    _queue.setDebug(s.debugMode);
  }

  if (!_settings.save()) { sendError(500, "nvs write failed"); return; }

  // Hand the printer-affecting parts to the printer task.
  g_reconfig = { &_printer, &_queue, &_transport, s.paperIndex,
                 s.printerAddress(), s.printerPort,
                 s.chunkRows, s.autoCut, s.feedBeforeCut };
  _queue.requestReconfig(applySettingsOnPrinterTask, &g_reconfig);

  JsonDocument res;
  res["saved"]          = true;
  res["rebootRequired"] = _settings.rebootRequired();

  String out;
  serializeJson(res, out);
  sendJson(200, out);
}

void WebApi::handlePrintText() {
  if (pausedOnline()) return;
  String text;
  uint8_t align = 0, scale = 1, font = 0;
  bool    bold  = false, cut = true;

  const String ctype = _server.hasHeader("Content-Type")
                     ? _server.header("Content-Type") : String();

  if (ctype.startsWith("text/plain")) {
    // Convenience path for shell scripts:
    //   curl -X POST --data-binary @note.txt http://receipt.local/api/print/text
    text = body();
  } else {
    const String raw = body();
    if (raw.isEmpty()) { sendError(400, "empty body"); return; }

    JsonDocument doc;
    if (deserializeJson(doc, raw)) { sendError(400, "invalid json"); return; }

    if (!doc["text"].is<const char*>()) { sendError(400, "missing text"); return; }
    text = doc["text"].as<const char*>();

    if (doc["align"].is<const char*>()) align = parseAlign(doc["align"]);
    else if (doc["align"].is<uint8_t>()) align = doc["align"];

    if (doc["size"].is<uint8_t>()) scale = doc["size"];
    // "b" selects the smaller Font B cell; anything else is Font A.
    if (doc["font"].is<const char*>())
      font = (tolower(doc["font"].as<const char*>()[0]) == 'b') ? 1 : 0;
    if (doc["bold"].is<bool>())    bold  = doc["bold"];
    if (doc["cut"].is<bool>())     cut   = doc["cut"];
  }

  // An empty body with cut=true is a legitimate "feed and cut" request,
  // which is how the UI's cut button works.
  if (text.isEmpty() && !cut)         { sendError(400, "empty text"); return; }
  if (text.length() > MAX_TEXT_BYTES) { sendError(413, "text too long"); return; }

  char label[24];
  const JobSource src = requestSource(label, sizeof(label));

  const uint32_t id = _queue.submitText(text, src, align, scale, bold, cut, font);
  if (!id) { sendError(503, "queue full"); return; }

  sendJobAccepted(id);
}

void WebApi::resetRasterUpload() {
  free(_rasterBuf);
  _rasterBuf   = nullptr;
  _rasterLen   = 0;
  _rasterCap   = 0;
  _rasterWidth = 0;
  _rasterRows  = 0;
  _rasterCoverage = 0;
  _rasterFail  = false;
  _rasterError[0] = 0;
}

// Runs repeatedly as the multipart body streams in. Geometry comes from the
// query string rather than form fields, so it is known before the first
// byte of pixel data arrives and the buffer can be sized exactly once.
void WebApi::handleRasterUpload() {
  HTTPUpload& up = _server.upload();

  if (up.status == UPLOAD_FILE_START) {
    resetRasterUpload();

    _rasterWidth = strtoul(_server.arg("width").c_str(), nullptr, 10);
    _rasterRows  = strtoul(_server.arg("rows").c_str(),  nullptr, 10);

    // Percentage of black dots, counted by the browser while it packed the
    // buffer. It decides how long the printer rests afterwards. Absent or
    // zero is not an error — the printer task counts the bits itself — but
    // sending it saves the ESP32 a pass over 72 KB.
    if (_server.hasArg("coverage")) {
      const unsigned long c = strtoul(_server.arg("coverage").c_str(), nullptr, 10);
      _rasterCoverage = c > 100 ? 100 : static_cast<uint8_t>(c);
    }

    if (!_rasterWidth || !_rasterRows) {
      _rasterFail = true;
      strlcpy(_rasterError, "missing width or rows", sizeof(_rasterError));
      return;
    }
    if (_rasterWidth > _printer.printableDots()) {
      _rasterFail = true;
      strlcpy(_rasterError, "wider than loaded paper", sizeof(_rasterError));
      return;
    }
    if (_rasterWidth % 8) {
      _rasterFail = true;
      strlcpy(_rasterError, "width must be a multiple of 8", sizeof(_rasterError));
      return;
    }

    const size_t need = static_cast<size_t>(_rasterWidth / 8) * _rasterRows;
    if (need > MAX_RASTER_BYTES) {
      _rasterFail = true;
      strlcpy(_rasterError, "image too tall", sizeof(_rasterError));
      return;
    }

    _rasterBuf = static_cast<uint8_t*>(malloc(need));
    if (!_rasterBuf) {
      _rasterFail = true;
      snprintf(_rasterError, sizeof(_rasterError),
               "out of memory (%u free)", (unsigned)ESP.getMaxAllocHeap());
      return;
    }
    _rasterCap = need;

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (_rasterFail || !_rasterBuf) return;

    if (_rasterLen + up.currentSize > _rasterCap) {
      _rasterFail = true;
      strlcpy(_rasterError, "more data than declared", sizeof(_rasterError));
      return;
    }
    memcpy(_rasterBuf + _rasterLen, up.buf, up.currentSize);
    _rasterLen += up.currentSize;
  }
  // UPLOAD_FILE_END needs nothing: handlePrintRaster runs next and submits.
}

void WebApi::handlePrintRaster() {
  if (pausedOnline()) { resetRasterUpload(); return; }
  if (_rasterFail) {
    // Copy the reason first: resetRasterUpload() clears _rasterError, and
    // sending the pointer afterwards gave the page {"error":""} (v0.23.1).
    char msg[sizeof(_rasterError)];
    strlcpy(msg, _rasterError[0] ? _rasterError : "upload failed", sizeof(msg));
    resetRasterUpload();
    sendError(400, msg);
    return;
  }
  if (!_rasterBuf || _rasterLen == 0) {
    resetRasterUpload();
    sendError(400, "no image data");
    return;
  }
  if (_rasterLen != _rasterCap) {
    char msg[48];
    snprintf(msg, sizeof(msg), "expected %u bytes, got %u",
             (unsigned)_rasterCap, (unsigned)_rasterLen);
    resetRasterUpload();
    sendError(400, msg);
    return;
  }

  const bool cut = _server.arg("cut") != "0";

  char label[24];
  const JobSource src = requestSource(label, sizeof(label));

  // Ownership passes to the queue here. Do not free _rasterBuf afterwards,
  // and do not call resetRasterUpload() — just drop our references.
  uint8_t* buf = _rasterBuf;
  const size_t   len   = _rasterLen;
  const uint16_t width = _rasterWidth;
  const uint16_t rows  = _rasterRows;
  const uint8_t  cov   = _rasterCoverage;

  _rasterBuf = nullptr;
  _rasterLen = _rasterCap = 0;
  _rasterCoverage = 0;

  // tail=0: skip the white tail (templates are text-light; see
  // PrintQueue::tailRowsFor). Photos leave it on.
  const bool tail = _server.arg("tail") != "0";
  const uint32_t id = _queue.submitRasterOwned(buf, len, width, rows, src,
                                               cut, 0, cov, nullptr, 0, tail);
  if (!id) { sendError(503, "queue full"); return; }

  sendJobAccepted(id);
}

void WebApi::handlePrintRaw() {
  // Section 17: hidden from the normal interface. Lets arbitrary ESC/POS
  // sequences be tested without reflashing.
  const String raw = body();
  if (raw.isEmpty())                 { sendError(400, "empty body"); return; }
  if (raw.length() > MAX_RAW_BYTES)  { sendError(413, "too long"); return; }

  char label[24];
  const JobSource src = requestSource(label, sizeof(label));

  const uint32_t id = _queue.submitRaw(
    reinterpret_cast<const uint8_t*>(raw.c_str()), raw.length(), src);
  if (!id) { sendError(503, "queue full"); return; }

  sendJobAccepted(id);
}

void WebApi::handleJobs() {
  JsonDocument doc;
  char when[24];

  if (_server.hasArg("id")) {
    const uint32_t id = strtoul(_server.arg("id").c_str(), nullptr, 10);
    StoredJob r;
    if (!_store.get(id, r)) { sendError(404, "unknown job"); return; }

    doc["id"]         = r.id;
    doc["type"]       = jobTypeName(static_cast<JobType>(r.type));
    doc["status"]     = jobStatusName(static_cast<JobStatus>(r.status));
    doc["bytes"]      = r.bytes;
    doc["durationMs"] = r.durationMs;
    doc["source"]     = r.source;
    doc["epoch"]      = r.epoch;
    formatLocal(r.epoch, when, sizeof(when));
    doc["localTime"]  = when;
    doc["ip"] = IPAddress(r.sourceIp[0], r.sourceIp[1],
                          r.sourceIp[2], r.sourceIp[3]).toString();
    if (r.error[0]) doc["error"] = r.error;
    if (viaRelay()) { doc.remove("ip"); doc["source"] = !strncmp(r.source, "online", 6) ? "online" : "home"; }
  } else {
    // Default to a screenful; ?limit=100 returns the whole ring.
    uint16_t limit = 20;
    if (_server.hasArg("limit"))
      limit = strtoul(_server.arg("limit").c_str(), nullptr, 10);
    if (limit == 0 || limit > JobStore::CAPACITY) limit = JobStore::CAPACITY;

    // ~8 KB of records would be a large stack frame on a 6 KB task, so this
    // is heap-allocated and freed before the response is serialised.
    StoredJob* recs = static_cast<StoredJob*>(malloc(sizeof(StoredJob) * limit));
    if (!recs) { sendError(500, "out of memory"); return; }

    const uint16_t n = _store.recent(recs, limit);

    JsonArray arr = doc["jobs"].to<JsonArray>();
    for (uint16_t i = 0; i < n; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["id"]     = recs[i].id;
      o["type"]   = jobTypeName(static_cast<JobType>(recs[i].type));
      o["status"] = jobStatusName(static_cast<JobStatus>(recs[i].status));
      o["bytes"]  = recs[i].bytes;
      if (recs[i].durationMs) o["durationMs"] = recs[i].durationMs;
      o["epoch"]  = recs[i].epoch;
      formatLocal(recs[i].epoch, when, sizeof(when));
      o["localTime"] = when;
      if (viaRelay()) {
        // Through the website: where a print came from, but not which home
        // device or address (v0.22).
        const char* src = recs[i].source;
        o["source"] = !strncmp(src, "online", 6) ? "online" : !strncmp(src, "schedule", 8) ? "schedule"
                    : (!strcmp(src, "system") || !strcmp(src, "unit") || !strcmp(src, "keep-awake")) ? src : "home";
      } else {
        o["source"] = recs[i].source;
        o["ip"] = IPAddress(recs[i].sourceIp[0], recs[i].sourceIp[1],
                            recs[i].sourceIp[2], recs[i].sourceIp[3]).toString();
      }
      if (recs[i].error[0]) o["error"] = recs[i].error;
    }

    free(recs);

    doc["pending"]  = _queue.pending();
    doc["stored"]   = _store.count();
    doc["capacity"] = JobStore::CAPACITY;
  }

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

// Starts the banding calibration run. Returns immediately: the printer task
// does the work, and it will be busy for several minutes.

void WebApi::handleJobsClear() {
  _store.clear();
  sendJson(200, "{\"cleared\":true}");
}

void WebApi::handleFactoryReset() {
  _settings.factoryReset();
  Online::forget();                  // a new identity: approval needed again
  sendJson(200, "{\"reset\":true,\"rebootRequired\":true}");
}

void WebApi::handleReboot() {
  sendJson(200, "{\"rebooting\":true}");
  delay(250);
  ESP.restart();
}

bool WebApi::viaRelay() { return _server.hasHeader("X-Via-Relay"); }

// Online printing paused (v0.23): refuses a print that came through the
// website. Returns true (and has answered) when it refused.
bool WebApi::pausedOnline() {
  if (!viaRelay() || !Online::paused()) return false;
  sendError(503, "Printing is paused by the printer's owner");
  return true;
}

// True for a request from the unit's own network. Otherwise answers 403
// and returns false, so a route reads: if (localOnly()) handler();
bool WebApi::localOnly() {
  if (!viaRelay()) return true;
  sendError(403, "Settings can only be changed on the printer's own network");
  return false;
}

void WebApi::handleNotFound() {
  sendError(404, "not found");
}

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

void WebApi::cors() {
  _server.sendHeader("Access-Control-Allow-Origin",  "*");
  _server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  _server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

String WebApi::body() {
  if (_server.hasArg("plain")) return _server.arg("plain");
  return String();
}

void WebApi::sendJson(int code, const String& payload) {
  cors();
  _server.send(code, "application/json", payload);
}

void WebApi::sendError(int code, const char* message) {
  // Every refusal is logged (v0.22, troubleshooting): what, why, and whether
  // it came through the website. Browsers' favicon misses are not news.
  if (!(code == 404 && _server.uri() == "/favicon.ico"))
    Log.printf("[http] %d %s %s%s: %s\n", code, _server.method() == HTTP_POST ? "POST" : "GET",
               _server.uri().c_str(), viaRelay() ? " (online)" : "", message);
  JsonDocument doc;
  doc["error"] = message;
  String out;
  serializeJson(doc, out);
  sendJson(code, out);
}

void WebApi::sendJobAccepted(uint32_t id) {
  JsonDocument doc;
  doc["jobId"]   = id;
  doc["status"]  = "queued";
  doc["pending"] = _queue.pending();
  String out;
  serializeJson(doc, out);
  sendJson(202, out);          // Accepted, not Created: it has not printed yet
}

// Coarse device identification. A full User-Agent is far too long for a
// receipt log and mostly noise; what matters is "which of my devices".
void WebApi::deviceLabel(const String& ua, char* out, size_t outLen) {
  const char* name = "web";

  if (ua.isEmpty())                       name = "api";
  else if (ua.indexOf("iPhone")    >= 0)  name = "iPhone";
  else if (ua.indexOf("iPad")      >= 0)  name = "iPad";
  else if (ua.indexOf("Android")   >= 0)  name = "Android";
  else if (ua.indexOf("Windows")   >= 0)  name = "Windows";
  else if (ua.indexOf("Macintosh") >= 0)  name = "Mac";
  else if (ua.indexOf("Linux")     >= 0)  name = "Linux";
  else if (ua.indexOf("curl")      >= 0)  name = "curl";
  else if (ua.indexOf("PowerShell")>= 0 ||
           ua.indexOf("WindowsPowerShell") >= 0) name = "PowerShell";
  else if (ua.indexOf("python")    >= 0 ||
           ua.indexOf("Python")    >= 0)  name = "python";
  else if (ua.indexOf("HomeAssistant") >= 0) name = "HomeAssistant";

  strlcpy(out, name, outLen);
}

JobSource WebApi::requestSource(char* buf, size_t bufLen) {
  const String ua = _server.hasHeader("User-Agent")
                  ? _server.header("User-Agent") : String();
  deviceLabel(ua, buf, bufLen);
  // Through the website the connection comes from the unit itself
  // (127.0.0.1): label it, and keep no address.
  if (viaRelay()) {
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "online %s", buf);
    strlcpy(buf, tmp, bufLen);
    return { buf, IPAddress(0, 0, 0, 0) };
  }
  return { buf, _server.client().remoteIP() };
}

void WebApi::formatLocal(uint32_t epoch, char* out, size_t outLen) {
  if (!epoch) { strlcpy(out, "", outLen); return; }

  const time_t t = static_cast<time_t>(epoch);
  struct tm tm_local;
  localtime_r(&t, &tm_local);
  strftime(out, outLen, "%Y-%m-%d %H:%M:%S", &tm_local);
}

uint8_t WebApi::parseAlign(const char* s) {
  if (!s) return 0;
  if (!strcasecmp(s, "center") || !strcasecmp(s, "centre")) return 1;
  if (!strcasecmp(s, "right"))                              return 2;
  return 0;
}



// ----------------------------------------------------------------------
// Kiosk set-up
// ----------------------------------------------------------------------
// Stored as the JSON the page sends, in its own NVS namespace, so it
// survives reflashing and is untouched by Settings. Validated by parsing
// and re-serialising only the known fields.
static const char* KIOSK_DEFAULT =
  "{\"title\":\"Leave a message!\","
  "\"subtitle\":\"Type your message, pick a style, then press Print.\\nPin your print on the board.\","
  "\"printTitle\":\"\",\"footer\":\"Available to hire\",\"date\":false,"
  "\"defTheme\":\"love\",\"defDeco\":\"hearts\"}";

void WebApi::handleGetKiosk() {
  Preferences p;
  String json;
  if (p.begin("kiosk", true)) { json = p.getString("cfg", ""); p.end(); }
  if (!json.length()) json = KIOSK_DEFAULT;
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "application/json", json);
}

void WebApi::handlePostKiosk() {
  JsonDocument in;
  if (deserializeJson(in, _server.arg("plain"))) { sendError(400, "bad JSON"); return; }
  JsonDocument out;
  for (const char* k : { "title", "subtitle", "printTitle", "footer", "defTheme", "defDeco" })
    out[k] = String(in[k] | "").substring(0, 300);
  out["date"] = in["date"] | false;
  for (const char* k : { "themes", "decos" }) {
    JsonArray src = in[k].as<JsonArray>();
    if (src.isNull()) continue;
    JsonArray dst = out[k].to<JsonArray>();
    for (JsonVariant v : src) if (v.is<const char*>() && dst.size() < 20) dst.add(String(v.as<const char*>()).substring(0, 20));
  }
  String json;
  serializeJson(out, json);
  Preferences p;
  if (!p.begin("kiosk", false)) { sendError(500, "storage"); return; }
  p.putString("cfg", json);
  p.end();
  Log.println("[kiosk] set-up saved");
  _server.send(200, "application/json", json);
}

// Theme wording, one JSON object of short strings per theme, in NVS
// namespace "themes" (key = theme name). Not cleared by factory reset, like
// the kiosk set-up. Unknown themes are refused; only string values are kept.
static bool themeName(const String& n) { return n == "fine"; }

void WebApi::handleGetTheme() {
  const String name = _server.arg("name");
  if (!themeName(name)) { sendError(400, "unknown theme"); return; }
  Preferences p;
  String json;
  if (p.begin("themes", true)) { json = p.getString(name.c_str(), ""); p.end(); }
  if (!json.length()) json = "{}";                 // the page has the defaults
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "application/json", json);
}

void WebApi::handlePostTheme() {
  const String name = _server.arg("name");
  if (!themeName(name)) { sendError(400, "unknown theme"); return; }
  JsonDocument in;
  if (deserializeJson(in, _server.arg("plain")) || !in.is<JsonObject>()) { sendError(400, "bad JSON"); return; }
  JsonDocument out;
  size_t n = 0;
  for (JsonPair kv : in.as<JsonObject>()) {
    if (!kv.value().is<const char*>() || ++n > 40) continue;
    out[String(kv.key().c_str()).substring(0, 16)] = String(kv.value().as<const char*>()).substring(0, 400);
  }
  String json;
  serializeJson(out, json);
  if (json.length() > 3800) { sendError(413, "too long"); return; }   // NVS string limit ~4000
  Preferences p;
  if (!p.begin("themes", false)) { sendError(500, "storage"); return; }
  p.putString(name.c_str(), json);
  p.end();
  Log.printf("[theme] %s saved (%u bytes)\n", name.c_str(), (unsigned)json.length());
  _server.send(200, "application/json", json);
}

// Online printing settings (v0.21), home network only. POST saves; both
// answer with the current settings and state, including the owner code.
void WebApi::handleOnline() {
  if (_server.method() == HTTP_POST && _server.uri() == "/api/online") {
    JsonDocument in;
    if (deserializeJson(in, body())) { sendError(400, "invalid json"); return; }
    Online::Config c = Online::config();
    c.en  = in["enabled"] | c.en;
    c.vis = in["visible"] | c.vis;
    c.sec = in["secure"]  | c.sec;
    c.paused = in["paused"] | c.paused;
    if (in["host"].is<const char*>())  strlcpy(c.host,  in["host"],  sizeof(c.host));
    String err;
    if (!Online::save(c, err)) { sendError(400, err.c_str()); return; }
  }
  const Online::Config c = Online::config();
  JsonDocument d;
  d["enabled"] = c.en; d["visible"] = c.vis; d["secure"] = c.sec; d["paused"] = c.paused;
  d["name"] = Online::name(); d["host"] = c.host;
  d["state"] = Online::stateKey(); d["text"] = Online::stateText();
  d["url"] = Online::link(); d["ownerCode"] = Online::ownerCode(); d["mac"] = Online::mac();
  String out;
  serializeJson(d, out);
  _server.sendHeader("Cache-Control", "no-store");
  sendJson(200, out);
}

// The saved log (survives power cuts): /trace.old then /trace.log.
void WebApi::sendSavedLog() {
  Log.save(true);
  _server.sendHeader("Cache-Control", "no-store");
  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.send(200, "text/plain; charset=utf-8", "");
  char buf[1024];
  for (const char* path : { LogTee::SAVED_OLD, LogTee::SAVED }) {
    File f = LittleFS.open(path, "r");
    if (!f) continue;
    size_t n;
    while ((n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf))) > 0) _server.sendContent(buf, n);
    f.close();
  }
  _server.sendContent("");
}

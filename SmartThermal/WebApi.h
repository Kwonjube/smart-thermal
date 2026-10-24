// WebApi.h — Section 17.
//
// Uses the ESP32 core's built-in synchronous WebServer rather than an async
// one. That is safe here precisely because of the queue: every handler
// validates, enqueues and returns in microseconds. Nothing in this file is
// allowed to touch the Printer directly.
//
// CORS is open, because the point of this API is that a PC script, a cron
// job or Home Assistant can hit it from anywhere on the LAN.

#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include "Settings.h"
#include "PrintQueue.h"
#include "JobStore.h"
#include "Printer.h"
#include "Transport.h"
#include "Items.h"
#include "Scheduler.h"

class WebApi {
public:
  WebApi(SettingsStore& settings,
         PrintQueue& queue,
         JobStore& store,
         Printer& printer,
         Transport& transport);

  void begin(uint16_t port = 80);
  void loop();                       // call from the Arduino loop

  // Reachability as decided by the printer task. This handler NEVER probes:
  // the printer accepts one TCP connection at a time, so a status poll
  // every few seconds was competing with prints for the only socket.
  bool printerReachable() const { return _queue.printerReachable(); }

  // Drafts, saved and scheduled prints (v0.19). Call before begin().
  void attachItems(ItemStore& items, Scheduler& sched);

private:
  SettingsStore& _settings;
  PrintQueue&    _queue;
  JobStore&      _store;
  Printer&       _printer;
  Transport&     _transport;      // Wi-Fi or W5500, chosen at build time
  WebServer      _server;

  // --- Raster upload state ---------------------------------------------
  // The body arrives in ~1.4 KB chunks, so it is accumulated here and then
  // handed to the queue by pointer. 96 KB at 576 dots wide is about 1365
  // rows, or 170 mm of paper — far longer than any sane receipt.
  //
  // v0.9.3: raised from 96 KB to 128 KB. The fixed cap was never the real
  // limit - memory is. /api/printer/status reports maxImageBytes from the
  // largest free block (minus a margin), and that is what the page uses.
  static const size_t MAX_RASTER_BYTES = 128 * 1024;

  uint8_t* _rasterBuf   = nullptr;
  size_t   _rasterLen   = 0;
  size_t   _rasterCap   = 0;
  uint16_t _rasterWidth = 0;
  uint16_t _rasterRows  = 0;
  // Black-dot percentage sent with the upload. 0 means the client did not
  // supply one, in which case the printer task counts the bits itself.
  uint8_t  _rasterCoverage = 0;
  bool     _rasterFail  = false;
  char     _rasterError[40] = {0};

  void resetRasterUpload();

  // --- Drafts, saved, scheduled (WebApiItems.cpp) -------------------------
  ItemStore* _items = nullptr;
  Scheduler* _sched = nullptr;
  File       _upFile;
  String     _upPath, _upOwner;
  uint32_t   _upWant = 0, _upLen = 0, _upId = 0, _upOff = 0;
  bool       _upFail = false, _upPic = false;
  char       _upErr[48] = {0};
  void itemRoutes();
  bool itemKind(ItemStore::Kind& k);
  void itemCounts(JsonObject o);
  bool checkSched(JsonDocument& doc, uint32_t& bytes, char* err, size_t errLen);
  void handleItemList();
  void handleItemCreate();
  void handleItemDelete();
  void handleItemOne();
  void handleItemUpdate();
  void handleItemRun();
  void handleFileUpload(bool pic);
  void handleFileDone();
  void handlePicGet();
  void handleClock();
  void handleOnline();
  void sendSavedLog();

  void routes();

  // Handlers
  void handleIndex();
  void handleStatus();
  void handleGetKiosk();
  void handlePostKiosk();
  void handleGetTheme();
  void handlePostTheme();
  void handleGetSettings();
  void handlePostSettings();
  void handlePrintText();
  void handlePrintRaster();        // completion handler
  void handleRasterUpload();       // streaming body handler
  void handlePrintRaw();
  void handleJobs();
  void handleJobsClear();
  void handleFactoryReset();
  void handleReboot();
  void handleNotFound();

  // Helpers
  bool viaRelay();                 // came in through the outside link
  bool localOnly();                // false (and 403 sent) if not local
  bool pausedOnline();             // true (and 503 sent) if online printing is paused
  void sendJson(int code, const String& body);
  void sendError(int code, const char* message);
  void sendJobAccepted(uint32_t id);
  void cors();
  String body();

  static uint8_t parseAlign(const char* s);

  // Builds the provenance label for an incoming request: the client's IP
  // plus a device name derived from the User-Agent.
  JobSource requestSource(char* buf, size_t bufLen);
  static void deviceLabel(const String& userAgent, char* out, size_t outLen);
  static void formatLocal(uint32_t epoch, char* out, size_t outLen);
};

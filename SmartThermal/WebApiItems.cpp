// WebApiItems.cpp - the API behind drafts, saved prints and scheduled
// prints (v0.19). Storage is ItemStore, timing is Scheduler.
//
//   GET  /api/items?kind=d|s|p         the list, newest first (a JSON array)
//   POST /api/items?kind=d|s|p         add one (JSON body) -> {id, next}
//   POST /api/items/delete?kind=&id=
//   POST /api/items/bin?id=&bytes=&off=     a scheduled print's slip (multipart)
//   POST /api/items/pic?owner=s12&bytes=&off=  the one stored picture (multipart)
//        bytes = the whole file, off = where this piece starts. Big files
//        come in pieces (the website relay takes at most 160 KB a request);
//        each piece must follow the last, and only the last one saves.
//   GET  /api/items/pic                that picture
//   POST /api/items/run?id=            print a scheduled item now
//   GET  /api/clock, POST /api/clock {"tz":"AWST-8"}

#include "WebApi.h"
#include "Items.h"
#include "Scheduler.h"
#include "Log.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

void WebApi::attachItems(ItemStore& items, Scheduler& sched) {
  _items = &items;
  _sched = &sched;
}

void WebApi::itemRoutes() {
  _server.on("/api/items",        HTTP_GET,  [this]{ handleItemList(); });
  _server.on("/api/items",        HTTP_POST, [this]{ handleItemCreate(); });
  _server.on("/api/items/delete", HTTP_POST, [this]{ handleItemDelete(); });
  _server.on("/api/items/one",    HTTP_GET,  [this]{ handleItemOne(); });
  _server.on("/api/items/update", HTTP_POST, [this]{ handleItemUpdate(); });
  _server.on("/api/items/run",    HTTP_POST, [this]{ handleItemRun(); });
  _server.on("/api/items/bin",    HTTP_POST, [this]{ handleFileDone(); },
                                             [this]{ handleFileUpload(false); });
  _server.on("/api/items/pic",    HTTP_POST, [this]{ handleFileDone(); },
                                             [this]{ handleFileUpload(true); });
  _server.on("/api/items/pic",    HTTP_GET,  [this]{ handlePicGet(); });
  _server.on("/api/clock",        HTTP_GET,  [this]{ handleClock(); });
  _server.on("/api/clock",        HTTP_POST, [this]{ if (localOnly()) handleClock(); });
}

bool WebApi::itemKind(ItemStore::Kind& k) {
  if (!_items || !_items->ok()) { sendError(503, "storage unavailable"); return false; }
  const String s = _server.arg("kind");
  if (s.length() != 1 || !ItemStore::kindFrom(s[0], k)) { sendError(400, "kind must be d, s or p"); return false; }
  return true;
}

void WebApi::itemCounts(JsonObject o) {
  if (!_items || !_items->ok()) return;
  o["d"] = _items->count(ItemStore::DRAFT);
  o["s"] = _items->count(ItemStore::SAVED);
  o["p"] = _items->count(ItemStore::SCHED);
  JsonObject lim = o["limit"].to<JsonObject>();
  lim["d"] = ItemStore::LIMIT[ItemStore::DRAFT];
  lim["s"] = ItemStore::LIMIT[ItemStore::SAVED];
  lim["p"] = ItemStore::LIMIT[ItemStore::SCHED];
  o["pic"]     = _items->picOwner();
  o["ver"]     = _items->version();
  o["free"]    = (uint32_t)_items->freeBytes();
  o["reserve"] = (uint32_t)ItemStore::RESERVE;
  o["running"] = _sched ? _sched->running() : 0;
  o["waiting"] = _sched && _sched->waiting();
}

void WebApi::handleItemList() {
  ItemStore::Kind k;
  if (!itemKind(k)) return;
  uint32_t ids[20];
  const uint8_t n = _items->ids(k, ids, 20);
  cors();
  _server.sendHeader("Cache-Control", "no-store");
  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.send(200, "application/json", "");
  _server.sendContent("[");
  bool first = true;
  for (uint8_t i = 0; i < n; i++) {
    String s;
    if (!_items->readJson(k, ids[i], s)) continue;
    if (!first) _server.sendContent(",");
    _server.sendContent(s);
    first = false;
  }
  _server.sendContent("]");
  _server.sendContent("");
}

// Validates a scheduled print's description and works out what it needs:
// its first due time and the size of its slip.
bool WebApi::checkSched(JsonDocument& doc, uint32_t& bytes, char* err, size_t errLen) {
  SchedRule r;
  if (!Scheduler::ruleFrom(doc.as<JsonVariantConst>(), r)) { strlcpy(err, "repeat or time not understood", errLen); return false; }
  if (!Scheduler::clockOk()) { strlcpy(err, "the printer does not know the time yet (it needs the internet)", errLen); return false; }
  const uint16_t width = doc["width"] | 0;
  if (!width || width % 8 || width > _printer.printableDots()) { strlcpy(err, "bad width", errLen); return false; }
  JsonArray seq = doc["seq"].as<JsonArray>();
  if (seq.isNull() || !seq.size() || seq.size() > 40) { strlcpy(err, "nothing to print", errLen); return false; }
  bytes = 0;
  for (JsonObject e : seq) {
    if (e["r"].is<uint16_t>()) {
      const uint16_t rows = e["r"];
      if (!rows || rows > 2000) { strlcpy(err, "bad part", errLen); return false; }
      bytes += (uint32_t)rows * (width / 8);
    } else if (!e["d"].is<int>() && !e["x"].is<const char*>()) {
      strlcpy(err, "bad step", errLen); return false;
    }
  }
  const time_t now  = time(nullptr);
  const time_t next = schedNextAfter(r, now);
  if (!next || (r.rep == REP_ONCE && next < now - 60)) { strlcpy(err, "that time has already passed", errLen); return false; }
  doc["next"]  = (uint32_t)next;
  doc["bytes"] = bytes;
  doc["ready"] = bytes == 0;
  doc["tries"] = 0;
  doc["last"]  = 0;
  return true;
}

void WebApi::handleItemCreate() {
  ItemStore::Kind k;
  if (!itemKind(k)) return;
  if (k == ItemStore::SCHED && pausedOnline()) return;   // a schedule would print later
  if (_items->full(k)) {
    char m[64];
    snprintf(m, sizeof(m), "full (%u of %u) - delete one first",
             _items->count(k), ItemStore::LIMIT[k]);
    sendError(409, m);
    return;
  }
  const String raw = body();
  if (raw.length() > ItemStore::MAX_JSON) { sendError(413, "too long to store"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, raw) || !doc.is<JsonObject>()) { sendError(400, "invalid json"); return; }

  if ((doc["pic"] | false) && !_items->picFree()) {
    sendError(409, "only one picture can be stored - delete the item that has one first");
    return;
  }
  uint32_t bytes = 0;
  if (k == ItemStore::SCHED) {
    char err[80];
    if (!checkSched(doc, bytes, err, sizeof(err))) { sendError(400, err); return; }
  }
  if (_items->freeBytes() < ItemStore::RESERVE + bytes + raw.length() + 4096) {
    sendError(507, "the printer's storage is full - delete something first");
    return;
  }

  const uint32_t id = _items->newId();
  doc["id"] = id;
  const time_t now = time(nullptr);
  doc["t"] = (uint32_t)(Scheduler::clockOk() ? now : 0);
  if (k == ItemStore::DRAFT) {           // a shared draft starts at revision 1
    const String sess = String(doc["sess"] | "").substring(0, 16);
    doc.remove("sess");
    doc["rev"]   = 1;
    doc["edits"] = 0;
    doc["by"]    = sess;
    doc["seen"].to<JsonArray>().add(sess);
  }
  String out;
  serializeJson(doc, out);
  if (!_items->writeJson(k, id, out, true)) { sendError(500, "could not write"); return; }
  if (k == ItemStore::SCHED && _sched) _sched->reload();
  Log.printf("[items] %c%lu kept (%u bytes)\n", ItemStore::kindChar(k), (unsigned long)id, (unsigned)out.length());

  JsonDocument res;
  res["id"]   = id;
  res["next"] = doc["next"] | 0UL;
  res["rev"]  = doc["rev"] | 0UL;
  String r;
  serializeJson(res, r);
  sendJson(200, r);
}

void WebApi::handleItemOne() {
  ItemStore::Kind k;
  if (!itemKind(k)) return;
  const uint32_t id = strtoul(_server.arg("id").c_str(), nullptr, 10);
  String s;
  if (!_items->readJson(k, id, s)) { sendError(404, "no such item"); return; }
  _server.sendHeader("Cache-Control", "no-store");
  sendJson(200, s);
}

// Shared drafts (v0.20): a draft open on any phone saves itself as it is
// edited. The latest save wins. Each save carries the revision it was based
// on; if someone else saved in between, the save still goes through but the
// reply says so, and the version it replaced is kept once as "prev" so the
// page can offer "Load theirs". The edit count shown to people (edits) goes
// up once per editing session, not per save.
void WebApi::handleItemUpdate() {
  ItemStore::Kind k;
  if (!itemKind(k)) return;
  if (k != ItemStore::DRAFT) { sendError(400, "only drafts are edited in place"); return; }
  const uint32_t id = strtoul(_server.arg("id").c_str(), nullptr, 10);
  String cur;
  JsonDocument doc;
  if (!_items->readJson(k, id, cur) || deserializeJson(doc, cur)) { sendError(404, "this draft was deleted"); return; }
  const String raw = body();
  if (raw.length() > ItemStore::MAX_JSON / 2) { sendError(413, "too long to store"); return; }
  JsonDocument in;
  if (deserializeJson(in, raw) || !in["src"].is<JsonObject>()) { sendError(400, "invalid json"); return; }
  if (in["src"]["style"] == "image" || doc["pic"] == true) { sendError(400, "picture drafts can't be edited in place"); return; }

  const uint32_t rev  = doc["rev"] | 0UL;
  const uint32_t base = in["base"] | 0UL;
  const String   sess = String(in["sess"] | "").substring(0, 16);
  const bool conflict = base != rev && String(doc["by"] | "") != sess;

  // Seen sessions, newest last, at most 6: a session counts as one edit.
  JsonArray seen = doc["seen"].is<JsonArray>() ? doc["seen"].as<JsonArray>() : doc["seen"].to<JsonArray>();
  bool known = false;
  for (JsonVariant v : seen) if (sess == v.as<const char*>()) known = true;
  if (!known) {
    if (seen.size() >= 6) seen.remove(0);
    seen.add(sess);
    doc["edits"] = (doc["edits"] | 0UL) + 1;
  }
  if (conflict) doc["prev"] = doc["src"]; else doc.remove("prev");
  doc["src"]   = in["src"];
  doc["title"] = String(in["title"] | "Untitled").substring(0, 80);
  doc["style"] = in["src"]["style"];
  doc["rev"]   = rev + 1;
  doc["by"]    = sess;
  doc["t"]     = (uint32_t)(Scheduler::clockOk() ? time(nullptr) : 0);
  String out;
  serializeJson(doc, out);
  if (out.length() > ItemStore::MAX_JSON) { sendError(413, "too long to store"); return; }
  if (!_items->writeJson(k, id, out, false)) { sendError(500, "could not write"); return; }

  JsonDocument res;
  res["rev"]      = rev + 1;
  res["edits"]    = doc["edits"];
  res["conflict"] = conflict;
  String r;
  serializeJson(res, r);
  sendJson(200, r);
}

void WebApi::handleItemDelete() {
  ItemStore::Kind k;
  if (!itemKind(k)) return;
  const uint32_t id = strtoul(_server.arg("id").c_str(), nullptr, 10);
  if (!_items->remove(k, id)) { sendError(404, "no such item"); return; }
  if (k == ItemStore::SCHED && _sched) _sched->reload();
  sendJson(200, "{\"ok\":true}");
}

void WebApi::handleItemRun() {
  if (pausedOnline()) return;
  const uint32_t id = strtoul(_server.arg("id").c_str(), nullptr, 10);
  if (!_sched || !_sched->runNow(id)) { sendError(404, "not a scheduled print that is ready"); return; }
  sendJson(200, "{\"ok\":true}");
}

// ----------------------------------------------------------------------
// File uploads: a scheduled print's slip, or the one picture. Streamed to
// flash in ~1.4 KB pieces, so neither is ever held in memory.
// ----------------------------------------------------------------------

void WebApi::handleFileUpload(bool pic) {
  HTTPUpload& up = _server.upload();
  if (up.status == UPLOAD_FILE_START) {
    _upFail = false; _upErr[0] = 0; _upLen = 0; _upPic = pic; _upId = 0;
    if (_upFile) _upFile.close();
    _upWant = strtoul(_server.arg("bytes").c_str(), nullptr, 10);
    _upOff  = strtoul(_server.arg("off").c_str(), nullptr, 10);
    if (!_items || !_items->ok()) { _upFail = true; strlcpy(_upErr, "storage unavailable", sizeof(_upErr)); return; }
    if (pic) {
      const String owner = _server.arg("owner");
      ItemStore::Kind k;
      const uint32_t id = owner.length() > 1 ? strtoul(owner.c_str() + 1, nullptr, 10) : 0;
      if (!ItemStore::kindFrom(owner[0], k) || !_items->exists(k, id)) {
        _upFail = true; strlcpy(_upErr, "no such item", sizeof(_upErr)); return;
      }
      if (!_items->picFree() && _items->picOwner() != owner) {
        _upFail = true; strlcpy(_upErr, "only one picture can be stored", sizeof(_upErr)); return;
      }
      if (!_upWant || _upWant > ItemStore::MAX_PIC) {
        _upFail = true; strlcpy(_upErr, "picture too big to store", sizeof(_upErr)); return;
      }
      _upPath = "/k/pic.jpg";
      _upOwner = owner;
    } else {
      _upId = strtoul(_server.arg("id").c_str(), nullptr, 10);
      JsonDocument doc;
      String s;
      if (!_items->readJson(ItemStore::SCHED, _upId, s) || deserializeJson(doc, s)) {
        _upFail = true; strlcpy(_upErr, "no such scheduled print", sizeof(_upErr)); return;
      }
      if ((doc["bytes"] | 0UL) != _upWant || !_upWant) {
        _upFail = true; strlcpy(_upErr, "size does not match", sizeof(_upErr)); return;
      }
      _upPath = _items->path(ItemStore::SCHED, _upId, ".bin");
    }
    if (_upOff >= _upWant && _upWant) { _upFail = true; strlcpy(_upErr, "piece out of range", sizeof(_upErr)); return; }
    if (_upOff == 0) {
      if (_items->freeBytes() < ItemStore::RESERVE / 2 + _upWant) {
        _upFail = true; strlcpy(_upErr, "the printer's storage is full", sizeof(_upErr)); return;
      }
      _upFile = LittleFS.open(_upPath + ".tmp", "w");
    } else {
      // A later piece: the file so far must end exactly where this one starts.
      File f = LittleFS.open(_upPath + ".tmp", "r");
      const size_t have = f ? f.size() : 0;
      if (f) f.close();
      if (have != _upOff) { _upFail = true; strlcpy(_upErr, "pieces out of order - start again", sizeof(_upErr)); return; }
      _upFile = LittleFS.open(_upPath + ".tmp", "a");
    }
    if (!_upFile) { _upFail = true; strlcpy(_upErr, "could not open file", sizeof(_upErr)); }

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (_upFail || !_upFile) return;
    if (_upOff + _upLen + up.currentSize > _upWant) {
      _upFail = true; strlcpy(_upErr, "more data than declared", sizeof(_upErr)); return;
    }
    if (_upFile.write(up.buf, up.currentSize) != up.currentSize) {
      _upFail = true; strlcpy(_upErr, "write failed (storage full?)", sizeof(_upErr)); return;
    }
    _upLen += up.currentSize;

  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (_upFile) _upFile.close();
    if (up.status == UPLOAD_FILE_ABORTED) { _upFail = true; strlcpy(_upErr, "upload interrupted", sizeof(_upErr)); }
  }
}

void WebApi::handleFileDone() {
  const String tmp = _upPath + ".tmp";
  if (!_upFail && !_upLen) { _upFail = true; strlcpy(_upErr, "empty piece", sizeof(_upErr)); }
  if (!_upFail && _upOff + _upLen < _upWant) {        // more pieces to come
    char r[48];
    snprintf(r, sizeof(r), "{\"ok\":true,\"next\":%lu}", (unsigned long)(_upOff + _upLen));
    _upPath = "";
    sendJson(200, r);
    return;
  }
  if (_upFail || _upPath.isEmpty()) {
    if (_upPath.length()) LittleFS.remove(tmp);
    sendError(400, _upErr[0] ? _upErr : "upload failed");
    _upPath = "";
    return;
  }
  LittleFS.remove(_upPath);
  if (!LittleFS.rename(tmp, _upPath)) { LittleFS.remove(tmp); sendError(500, "could not save"); _upPath = ""; return; }

  if (_upPic) {
    _items->setPicOwner(_upOwner);
    Log.printf("[items] picture stored for %s (%lu KB)\n", _upOwner.c_str(), (unsigned long)(_upLen / 1024));
  } else {
    // The slip is in: the scheduled print is ready to go.
    String s;
    JsonDocument doc;
    if (_items->readJson(ItemStore::SCHED, _upId, s) && !deserializeJson(doc, s)) {
      doc["ready"] = true;
      String out;
      serializeJson(doc, out);
      _items->writeJson(ItemStore::SCHED, _upId, out, false);
    }
    if (_sched) _sched->reload();
    Log.printf("[items] p%lu slip stored (%lu KB)\n", (unsigned long)_upId, (unsigned long)(_upLen / 1024));
  }
  _upPath = "";
  sendJson(200, "{\"ok\":true}");
}

void WebApi::handlePicGet() {
  File f = LittleFS.open("/k/pic.jpg", "r");
  if (!f || !_items || _items->picFree()) { if (f) f.close(); sendError(404, "no picture"); return; }
  cors();
  _server.sendHeader("Cache-Control", "no-store");
  _server.streamFile(f, "image/jpeg");
  f.close();
}

void WebApi::handleClock() {
  if (_server.method() == HTTP_POST) {
    JsonDocument in;
    if (deserializeJson(in, body())) { sendError(400, "invalid json"); return; }
    if (!Scheduler::setTimeZone(String(in["tz"] | ""))) { sendError(400, "time zone not understood"); return; }
    if (_sched) _sched->reload();
  }
  JsonDocument doc;
  const time_t now = time(nullptr);
  doc["ok"]  = Scheduler::clockOk();
  doc["now"] = (uint32_t)(Scheduler::clockOk() ? now : 0);
  doc["tz"]  = Scheduler::timeZone();
  char local[24];
  formatLocal(Scheduler::clockOk() ? (uint32_t)now : 0, local, sizeof(local));
  doc["local"] = local;
  String out;
  serializeJson(doc, out);
  _server.sendHeader("Cache-Control", "no-store");
  sendJson(200, out);
}

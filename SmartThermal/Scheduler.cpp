#include "Scheduler.h"
#include "Config.h"
#include "Log.h"
#include <LittleFS.h>
#include <Preferences.h>

static const uint8_t  MAX_TRIES     = 3;
static const uint32_t RETRY_S       = 300;        // 5 minutes between tries
static const uint32_t STEP_LIMIT_MS = 20 * 60000; // one step may take this long
static const uint32_t QUIET_MS      = 10000;      // queue idle this long first

// ----------------------------------------------------------------------
// Clock and time zone
// ----------------------------------------------------------------------

bool Scheduler::clockOk() { return time(nullptr) > 1735689600; }   // after 1 Jan 2025

String Scheduler::timeZone() {
  Preferences p;
  String tz;
  if (p.begin("unit", true)) { tz = p.getString("tz", ""); p.end(); }
  return tz.length() ? tz : String(TIMEZONE);
}

void Scheduler::startClock() {
  const String tz = timeZone();
  configTzTime(tz.c_str(), NTP_SERVER_1, NTP_SERVER_2);
  Log.printf("[time] syncing in the background (%s)\n", tz.c_str());
}

bool Scheduler::setTimeZone(const String& tz) {
  if (tz.isEmpty() || tz.length() > 60) return false;
  for (char c : tz)
    if (!isalnum((unsigned char)c) && !strchr(",.:/+-<>", c)) return false;
  Preferences p;
  if (!p.begin("unit", false)) return false;
  p.putString("tz", tz);
  p.end();
  setenv("TZ", tz.c_str(), 1);
  tzset();
  Log.printf("[time] time zone now %s\n", tz.c_str());
  return true;
}

// ----------------------------------------------------------------------
// Descriptions
// ----------------------------------------------------------------------

bool Scheduler::ruleFrom(JsonVariantConst m, SchedRule& r) {
  r = SchedRule();
  r.rep = schedRepFrom(m["rep"] | "");
  if (!schedParseTime(m["at"] | "", r.hh, r.mm)) return false;
  if (r.rep == REP_ONCE && !schedParseDate(m["date"] | "", r.year, r.mon, r.day)) return false;
  r.days = (uint8_t)(m["days"] | 0);
  r.mday = (uint8_t)(m["mday"] | 1);
  return schedValid(r);
}

bool Scheduler::loadMeta(uint32_t id, JsonDocument& doc) {
  String s;
  if (!_items->readJson(ItemStore::SCHED, id, s)) return false;
  return !deserializeJson(doc, s) && doc.is<JsonObject>();
}

bool Scheduler::saveMeta(uint32_t id, JsonDocument& doc) {
  String s;
  serializeJson(doc, s);
  return _items->writeJson(ItemStore::SCHED, id, s, false);
}

void Scheduler::begin(ItemStore& items, PrintQueue& queue, JobStore& jobs) {
  _items = &items; _queue = &queue; _jobs = &jobs;
  reload();
}

Scheduler::Slot* Scheduler::slot(uint32_t id) {
  for (uint8_t i = 0; i < _n; i++) if (_slots[i].id == id) return &_slots[i];
  return nullptr;
}

// Rebuilds the table from the files. Due items stay due (that is how a
// print missed while the unit was off still comes out); everything else
// gets its next time worked out again, which also follows a time-zone
// change.
void Scheduler::reload() {
  if (!_items || !_items->ok()) return;
  uint32_t ids[8];
  const uint8_t n = _items->ids(ItemStore::SCHED, ids, 8);
  const bool clock = clockOk();
  const time_t now = time(nullptr);
  Slot old[8]; const uint8_t oldN = _n;
  memcpy(old, _slots, sizeof(old));
  _n = 0;
  for (uint8_t i = 0; i < n; i++) {
    JsonDocument doc;
    if (!loadMeta(ids[i], doc)) continue;
    Slot s = { ids[i], (time_t)(doc["next"] | 0UL), 0, (uint8_t)(doc["tries"] | 0), doc["ready"] | false };
    for (uint8_t j = 0; j < oldN; j++) if (old[j].id == s.id) s.retryAt = old[j].retryAt;
    SchedRule r;
    if (!ruleFrom(doc.as<JsonVariantConst>(), r)) { s.next = 0; }
    else if (clock) {
      time_t want = s.next;
      if (r.rep == REP_ONCE) { if (s.next) want = schedNextAfter(r, now); }
      else if (!s.next || s.next > now) want = schedNextAfter(r, now);
      if (want != s.next) { s.next = want; doc["next"] = (uint32_t)want; saveMeta(s.id, doc); }
    }
    _slots[_n++] = s;
  }
  if (_run.id && !slot(_run.id)) { Log.println("[sched] running item was deleted - stopped"); _run = Run(); }
}

bool Scheduler::runNow(uint32_t id) {
  Slot* s = slot(id);
  if (!s || !s->ready) return false;
  s->next = time(nullptr) - 1; s->retryAt = 0; s->tries = 0;
  JsonDocument doc;
  if (loadMeta(id, doc)) { doc["next"] = (uint32_t)s->next; doc["tries"] = 0; saveMeta(id, doc); }
  return true;
}

// ----------------------------------------------------------------------
// Running
// ----------------------------------------------------------------------

void Scheduler::loop() {
  if (!_items || !_items->ok() || !clockOk()) return;
  if (!_clockSeen) { _clockSeen = true; reload(); }      // first time with a clock

  const uint32_t ms = millis();
  if (ms - _tickMs < 250) return;
  _tickMs = ms;

  if (_run.id) { step(); return; }

  const time_t now = time(nullptr);
  Slot* due = nullptr;
  for (uint8_t i = 0; i < _n; i++) {
    Slot& s = _slots[i];
    if (!s.ready || !s.next || s.next > now || (s.retryAt && s.retryAt > now)) continue;
    if (!due || s.next < due->next) due = &s;
  }
  _waiting = due != nullptr;
  if (!due) return;

  // Hold it while the printer is away (off, unplugged, not answering) and
  // while anything else is printing - including the short gaps between the
  // parts of a long print sent from a phone.
  if (!_queue->printerReachable() || _queue->pending() || _queue->busy()) return;
  const uint32_t idle = _queue->idleMs();
  if (idle && idle < QUIET_MS) return;
  start(*due);
}

void Scheduler::start(Slot& s) {
  _run = Run();
  _run.id = s.id;
  _run.sinceMs = millis();
  _waiting = false;
  Log.printf("[sched] printing scheduled item %lu (try %u)\n", (unsigned long)s.id, s.tries + 1);
  step();
}

void Scheduler::step() {
  // Waiting on the step already handed to the queue.
  if (_run.jobId) {
    StoredJob j;
    if (_jobs->get(_run.jobId, j)) {
      if (j.status == (uint8_t)JobStatus::Failed) { finish(false, "the printer did not finish it"); return; }
      if (j.status == (uint8_t)JobStatus::Done)   { _run.jobId = 0; _run.step++; _run.sinceMs = millis(); }
    }
    if (_run.jobId) {
      if (millis() - _run.sinceMs > STEP_LIMIT_MS) finish(false, "timed out");
      return;
    }
  }

  JsonDocument doc;
  if (!loadMeta(_run.id, doc)) { finish(false, "description missing"); return; }
  JsonArrayConst seq = doc["seq"].as<JsonArrayConst>();
  if (_run.step >= seq.size()) { finish(true, nullptr); return; }

  const uint16_t width = doc["width"] | 576;
  const uint32_t rb    = width / 8;
  const bool     last  = _run.step + 1 >= seq.size();
  const bool     cut   = last && (doc["cut"] | true);
  JsonObjectConst e    = seq[_run.step].as<JsonObjectConst>();
  const JobSource src  = JobSource::local("schedule");
  uint32_t id = 0;

  if (e["r"].is<uint16_t>()) {
    // Where this part starts in the stored slip.
    uint32_t off = 0;
    for (uint16_t i = 0; i < _run.step; i++) off += (uint32_t)(seq[i]["r"] | 0) * rb;
    const uint16_t rows = e["r"];
    const size_t   len  = (size_t)rows * rb;
    uint8_t* buf = static_cast<uint8_t*>(malloc(len));
    if (!buf) {                                  // memory busy (an upload?): try again
      if (millis() - _run.sinceMs > 60000) finish(false, "out of memory");
      return;
    }
    File f = LittleFS.open(_items->path(ItemStore::SCHED, _run.id, ".bin"), "r");
    const bool got = f && f.seek(off) && f.read(buf, len) == len;
    if (f) f.close();
    if (!got) { free(buf); finish(false, "stored slip unreadable"); return; }
    id = _queue->submitRasterOwned(buf, len, width, rows, src, cut, 0,
                                   (uint8_t)(e["c"] | 0), nullptr, 0, false);
  } else if (e["d"].is<int>()) {
    // Today's date, the way the page writes it: "Sunday 18 October".
    char d[40]; const time_t now = time(nullptr); struct tm t; localtime_r(&now, &t);
    strftime(d, sizeof(d), "%A %d %B", &t);
    char* z = strchr(d, ' ');
    if (z && z[1] == '0') memmove(z + 1, z + 2, strlen(z + 2) + 1);
    id = _queue->submitText(String(d), src, 1, 1, false, cut, (uint8_t)(e["f"] | 1));
  } else if (e["x"].is<const char*>()) {
    id = _queue->submitText(String(e["x"].as<const char*>()), src, (uint8_t)(e["a"] | 0),
                            (uint8_t)(e["z"] | 1), false, cut, (uint8_t)(e["f"] | 0));
  } else {
    finish(false, "unknown step");
    return;
  }

  if (!id) {                                     // queue full or no memory for text
    if (millis() - _run.sinceMs > 60000) finish(false, "queue would not take it");
    return;
  }
  _run.jobId   = id;
  _run.sinceMs = millis();
}

void Scheduler::finish(bool ok, const char* why) {
  const uint32_t id = _run.id;
  _run = Run();
  Slot* s = slot(id);
  JsonDocument doc;
  if (!s || !loadMeta(id, doc)) return;

  const time_t now = time(nullptr);
  SchedRule r;
  const bool rule = ruleFrom(doc.as<JsonVariantConst>(), r);
  doc["last"]   = (uint32_t)now;
  doc["lastOk"] = ok;

  if (ok) {
    Log.printf("[sched] item %lu printed\n", (unsigned long)id);
    if (rule && r.rep == REP_ONCE) {
      _items->remove(ItemStore::SCHED, id);
      reload();
      return;
    }
    s->tries = 0; s->retryAt = 0;
    s->next = rule ? schedNextAfter(r, now > s->next ? now : s->next) : 0;
  } else if (++s->tries < MAX_TRIES) {
    Log.printf("[sched] item %lu failed (%s) - trying again in 5 min\n", (unsigned long)id, why);
    s->retryAt = now + RETRY_S;
  } else {
    Log.printf("[sched] item %lu failed %u times (%s) - skipped this time\n",
               (unsigned long)id, s->tries, why);
    s->tries = 0; s->retryAt = 0;
    // A one-off stays on the list, not due, so "Print now" can retry it.
    s->next = (rule && r.rep != REP_ONCE) ? schedNextAfter(r, now) : 0;
  }
  if (!ok && why) doc["why"] = why; else doc.remove("why");
  doc["tries"] = s->tries;
  doc["next"]  = (uint32_t)s->next;
  saveMeta(id, doc);
}

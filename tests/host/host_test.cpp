#include "Arduino.h"
#include "LittleFS.h"
#include "Preferences.h"
#include "Log.h"
#include "Items.h"
#include "Scheduler.h"
#include <map>
uint32_t fake_ms = 1000; time_t fake_now = 0;
std::string fake_root; FakeFS LittleFS; FakeLog Log; std::map<std::string, std::string> fake_nvs;
static int fails = 0;
#define CHECK(c, ...) do { bool _ok = (c); if (!_ok) fails++; printf("%s ", _ok ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); } while (0)

ItemStore items; PrintQueue q; JobStore js; Scheduler sched;
static std::string fmt(time_t t) { char b[40]; struct tm l; localtime_r(&t, &l); strftime(b, 40, "%a %d %b %H:%M", &l); return b; }
static void tick(int n = 1) { for (int i = 0; i < n; i++) { fake_ms += 300; sched.loop(); } }
static void finishAll(uint8_t st = (uint8_t)JobStatus::Done) { for (auto& s : q.subs) if (js.st[s.id] == (uint8_t)JobStatus::Queued) js.st[s.id] = st; }
// What the API does on create (checkSched + write) and on slip upload.
static uint32_t add(const char* extra, size_t binRows) {
  JsonDocument d; deserializeJson(d, extra);
  SchedRule r; Scheduler::ruleFrom(d.as<JsonVariantConst>(), r);
  d["next"] = (uint32_t)schedNextAfter(r, time(nullptr)); d["width"] = 576; d["tries"] = 0;
  d["ready"] = true;
  uint32_t id = items.newId(); d["id"] = id;
  String s; serializeJson(d, s); items.writeJson(ItemStore::SCHED, id, s, true);
  if (binRows) { File f = LittleFS.open(items.path(ItemStore::SCHED, id, ".bin"), "w"); std::vector<uint8_t> b(binRows * 72);
    for (size_t i = 0; i < b.size(); i++) b[i] = (uint8_t)(i / 72 < 60 ? 0xAA : 0x55); f.write(b.data(), b.size()); f.close(); }
  sched.reload(); return id;
}
static JsonDocument meta(uint32_t id) { String s; JsonDocument d; items.readJson(ItemStore::SCHED, id, s); deserializeJson(d, s); return d; }

int main() {
  setenv("TZ", "AWST-8", 1); tzset();
  fake_root = (fs_::temp_directory_path() / "st_host_fs").string(); fs_::remove_all(fake_root); fs_::create_directories(fake_root);
  q.js = &js;
  fake_now = schedLocal(2026, 10, 18, 16, 30);              // Sun 16:30
  items.begin(); sched.begin(items, q, js);

  // 1. Weekdays 07:00, slip of 60 + 181 rows with the date between
  uint32_t a = add(R"({"rep":"days","days":62,"at":"07:00","cut":true,"seq":[{"r":60,"c":6},{"d":1,"f":1},{"r":181,"c":4}]})", 241);
  CHECK(fmt(meta(a)["next"].as<uint32_t>()) == "Mon 19 Oct 07:00", "first due %s", fmt(meta(a)["next"].as<uint32_t>()).c_str());
  tick(5); CHECK(q.subs.empty(), "nothing before it is due");
  fake_now = schedLocal(2026, 10, 19, 7, 0); q.reach = false; tick(5);
  CHECK(q.subs.empty() && sched.waiting(), "due but printer away: held, waiting=%d", sched.waiting());
  fake_now += 3 * 3600; q.reach = true; q.idle = 4000; tick(5);
  CHECK(q.subs.empty(), "printer back but another print just ended: still held");
  q.idle = 60000; tick(1);
  CHECK(q.subs.size() == 1 && q.subs[0].raster && q.subs[0].rows == 60 && !q.subs[0].cut && !q.subs[0].tail && q.subs[0].first == 0xAA, "part 1: 60 rows, no cut, no tail");
  tick(3); CHECK(q.subs.size() == 1, "waits for part 1 to print");
  finishAll(); tick(1);
  CHECK(q.subs.size() == 2 && !q.subs[1].raster && q.subs[1].text == "Monday 19 October" && q.subs[1].font == 1 && q.subs[1].align == 1, "date line by the printer: '%s'", (q.subs.size() > 1 ? q.subs[1].text.c_str() : "none"));
  finishAll(); tick(1);
  CHECK(q.subs.size() == 3 && q.subs[2].rows == 181 && q.subs[2].cut && q.subs[2].first == 0x55, "part 2 from the right offset, cut at the end");
  finishAll(); tick(2);
  JsonDocument m = meta(a);
  CHECK(m["lastOk"] == true && fmt(m["next"].as<uint32_t>()) == "Tue 20 Oct 07:00", "printed late once, next %s", fmt(m["next"].as<uint32_t>()).c_str());

  // 2. One-off is removed after printing
  q.subs.clear();
  uint32_t b = add(R"({"rep":"once","date":"2026-10-19","at":"12:00","cut":true,"seq":[{"x":"Bins out!","z":2,"f":1,"a":0}]})", 0);
  fake_now = schedLocal(2026, 10, 19, 12, 1); tick(1);
  CHECK(q.subs.size() == 1 && q.subs[0].text == "Bins out!" && q.subs[0].scale == 2 && q.subs[0].cut, "plain text step");
  finishAll(); tick(2);
  CHECK(!items.exists(ItemStore::SCHED, b) && items.count(ItemStore::SCHED) == 1, "one-off gone after printing");

  // 3. Failures: retried after 5 min, given up after 3
  q.subs.clear();
  uint32_t c = add(R"({"rep":"daily","at":"13:00","cut":true,"seq":[{"x":"Lunch"}]})", 0);
  fake_now = schedLocal(2026, 10, 19, 13, 0); tick(1); finishAll((uint8_t)JobStatus::Failed); tick(1);
  CHECK(meta(c)["tries"] == 1, "failed once: tries=1 (%s)", meta(c)["why"] | "");
  fake_now += 120; tick(3); CHECK(q.subs.size() == 1, "no retry before 5 min");
  fake_now += 200; tick(1); CHECK(q.subs.size() == 2, "retried after 5 min");
  finishAll((uint8_t)JobStatus::Failed); tick(1); fake_now += 301; tick(1); finishAll((uint8_t)JobStatus::Failed); tick(1);
  m = meta(c);
  CHECK(q.subs.size() == 3 && m["tries"] == 0 && m["lastOk"] == false && fmt(m["next"].as<uint32_t>()) == "Tue 20 Oct 13:00", "3 failures: skipped to %s", fmt(m["next"].as<uint32_t>()).c_str());

  // 4. Unit off for two days: prints once on return, then normal timetable
  q.subs.clear();
  fake_now = schedLocal(2026, 10, 22, 9, 0);   // Thu 09:00; c was due Tue 13:00 and Wed 13:00
  { Scheduler s2; ItemStore i2; i2.begin(); s2.begin(i2, q, js); fake_ms += 1000; s2.loop();
    CHECK(q.subs.size() == 1 || q.subs.size() == 2, "after restart: missed items print (%zu started)", q.subs.size()); }
  // 5. Run now, delete while running
  q.subs.clear(); fake_now = schedLocal(2026, 10, 22, 10, 0);
  sched.reload();
  CHECK(sched.runNow(c), "print now accepted"); tick(1);
  size_t n = q.subs.size();
  uint32_t r0 = sched.running();
  items.remove(ItemStore::SCHED, r0); sched.reload(); finishAll(); tick(1);
  CHECK(r0 && sched.running() != r0 && !items.exists(ItemStore::SCHED, r0), "deleted while running: stops cleanly (%zu->%zu steps)", n, q.subs.size());
  // 6. Time zone change follows wall clock
  uint32_t d = add(R"({"rep":"daily","at":"07:00","seq":[{"x":"x"}]})", 0);
  Scheduler::setTimeZone("AEST-10AEDT,M10.1.0,M4.1.0/3"); sched.reload();
  CHECK(fmt(meta(d)["next"].as<uint32_t>()) == "Fri 23 Oct 07:00", "Sydney: next %s local", fmt(meta(d)["next"].as<uint32_t>()).c_str());
  CHECK(Scheduler::timeZone() == String("AEST-10AEDT,M10.1.0,M4.1.0/3") && !Scheduler::setTimeZone("bad tz;rm"), "zone saved, junk refused");
  // 7. Limits and picture slot
  ItemStore::Kind k; CHECK(ItemStore::kindFrom('d', k) && k == ItemStore::DRAFT && !ItemStore::kindFrom('x', k), "kind letters");
  for (int i = 0; i < 10; i++) items.writeJson(ItemStore::DRAFT, items.newId(), "{}", true);
  CHECK(items.full(ItemStore::DRAFT) && items.count(ItemStore::DRAFT) == 10, "draft limit 10");
  uint32_t ids[20]; uint8_t got = items.ids(ItemStore::DRAFT, ids, 20);
  CHECK(got == 10 && ids[0] > ids[9], "newest first");
  { File f = LittleFS.open("/k/pic.jpg", "w"); f.print("jpg"); f.close(); } items.setPicOwner(String("d") + String(ids[3]));
  items.remove(ItemStore::DRAFT, ids[3]);
  CHECK(items.picFree() && !LittleFS.exists("/k/pic.jpg"), "picture goes with its item");
  printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails); return fails;
}

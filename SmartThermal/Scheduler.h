// Scheduler.h - prints scheduled items when they are due (v0.19).
//
// Runs on loop(). Each scheduled item is a description (/k/p<id>.json: the
// repeat rule and a print sequence) plus the slip the phone drew when it
// was scheduled (/k/p<id>.bin). The sequence is a list of steps:
//   {"r":rows,"c":coverage}        the next rows of the stored slip
//   {"d":1,"f":font}               today's date, printed by the printer
//                                  itself so a repeating list is never
//                                  stamped with the day it was scheduled
//   {"x":"text","z":scale,"f":font,"a":align}   printer text (Plain style)
// Steps go to the queue one at a time, each after the last has printed, so
// only one image is ever in memory (the same rule as the page's parts).
//
// Owner's rules: a print that falls due while the printer is off, out of
// paper or unreachable is held and printed when it comes back. A unit that
// was itself off prints a missed item once on return, then carries on with
// the normal timetable. A failed print is retried twice, 5 minutes apart.
// Nothing prints until the clock has been set from the internet.

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "Items.h"
#include "PrintQueue.h"
#include "JobStore.h"
#include "SchedRule.h"

class Scheduler {
public:
  void begin(ItemStore& items, PrintQueue& queue, JobStore& jobs);
  void loop();
  void reload();                       // after any change to scheduled items

  bool runNow(uint32_t id);            // "Print now" on the Future page
  uint32_t running() const { return _run.id; }
  bool     waiting() const { return _waiting; }   // due, printer not ready

  // Reads repeat fields from a description. False if they make no sense.
  static bool ruleFrom(JsonVariantConst meta, SchedRule& r);

  // The clock and its time zone (POSIX TZ string, kept in NVS "unit").
  static bool   clockOk();
  static String timeZone();
  static bool   setTimeZone(const String& tz);   // validates, saves, applies
  static void   startClock();

private:
  struct Slot {
    uint32_t  id;
    time_t    next;       // 0 = not due (finished one-off, or bad rule)
    time_t    retryAt;
    uint8_t   tries;
    bool      ready;      // slip uploaded
  };
  struct Run {
    uint32_t id = 0;
    uint16_t step = 0;
    uint32_t jobId = 0;
    uint32_t sinceMs = 0;  // when the current step started
  };

  ItemStore*  _items = nullptr;
  PrintQueue* _queue = nullptr;
  JobStore*   _jobs  = nullptr;
  Slot     _slots[8];
  uint8_t  _n = 0;
  Run      _run;
  bool     _waiting = false;
  bool     _clockSeen = false;
  uint32_t _tickMs = 0;

  Slot* slot(uint32_t id);
  void  start(Slot& s);
  void  step();
  void  finish(bool ok, const char* why);
  bool  loadMeta(uint32_t id, JsonDocument& doc);
  bool  saveMeta(uint32_t id, JsonDocument& doc);
};

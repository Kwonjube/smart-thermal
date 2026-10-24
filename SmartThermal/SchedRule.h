// SchedRule.h - when a scheduled print is next due. Pure C, no Arduino, so
// the same file is tested on a PC (test/sched_test.cpp).
//
// Times are local wall-clock times in the unit's time zone (TZ / tzset), so
// "every weekday at 07:00" stays at 07:00 across daylight-saving changes.

#pragma once
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

enum SchedRep : uint8_t { REP_ONCE = 0, REP_DAILY, REP_DAYS, REP_MONTH, REP_BAD };

struct SchedRule {
  SchedRep rep  = REP_BAD;
  uint8_t  hh   = 0, mm = 0;
  int      year = 0, mon = 0, day = 0;   // once: the date (mon 1-12)
  uint8_t  days = 0;                     // REP_DAYS: bit 0 = Sunday ... bit 6 = Saturday
  uint8_t  mday = 1;                     // REP_MONTH: 1-31, clamped to the month's last day
};

inline SchedRep schedRepFrom(const char* s) {
  if (!s) return REP_BAD;
  if (!strcmp(s, "once"))  return REP_ONCE;
  if (!strcmp(s, "daily")) return REP_DAILY;
  if (!strcmp(s, "days"))  return REP_DAYS;
  if (!strcmp(s, "month")) return REP_MONTH;
  return REP_BAD;
}

// "HH:MM" -> hh, mm. False if malformed.
inline bool schedParseTime(const char* s, uint8_t& hh, uint8_t& mm) {
  if (!s || strlen(s) != 5 || s[2] != ':') return false;
  const int h = atoi(s), m = atoi(s + 3);
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  hh = (uint8_t)h; mm = (uint8_t)m;
  return true;
}

// "YYYY-MM-DD". False if malformed.
inline bool schedParseDate(const char* s, int& y, int& mo, int& d) {
  if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
  y = atoi(s); mo = atoi(s + 5); d = atoi(s + 8);
  return y >= 2024 && y <= 2099 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
}

inline bool schedValid(const SchedRule& r) {
  if (r.rep == REP_BAD || r.hh > 23 || r.mm > 59) return false;
  if (r.rep == REP_DAYS  && !(r.days & 0x7F)) return false;
  if (r.rep == REP_MONTH && (r.mday < 1 || r.mday > 31)) return false;
  if (r.rep == REP_ONCE  && !r.year) return false;
  return true;
}

inline int schedDaysIn(int year, int mon /*1-12*/) {
  static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return d[mon - 1];
}

// Local date/time -> epoch. mktime normalises day overflow (e.g. day 32).
inline time_t schedLocal(int y, int mo, int d, int hh, int mm) {
  struct tm t; memset(&t, 0, sizeof(t));
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = hh; t.tm_min = mm; t.tm_sec = 0; t.tm_isdst = -1;
  return mktime(&t);
}

// The first time strictly after `after` that the rule fires. For a one-off
// it is simply the stated time (which may already have passed). 0 = never.
inline time_t schedNextAfter(const SchedRule& r, time_t after) {
  if (!schedValid(r)) return 0;
  if (r.rep == REP_ONCE) return schedLocal(r.year, r.mon, r.day, r.hh, r.mm);

  struct tm a; localtime_r(&after, &a);
  for (int off = 0; off < 400; off++) {
    // Walk calendar days from the local date of `after`.
    struct tm d; memset(&d, 0, sizeof(d));
    d.tm_year = a.tm_year; d.tm_mon = a.tm_mon; d.tm_mday = a.tm_mday + off;
    d.tm_hour = 12; d.tm_isdst = -1;
    mktime(&d);                                     // normalise the date
    const int y = d.tm_year + 1900, mo = d.tm_mon + 1, dd = d.tm_mday;
    bool match = false;
    switch (r.rep) {
      case REP_DAILY: match = true; break;
      case REP_DAYS:  match = (r.days >> d.tm_wday) & 1; break;
      case REP_MONTH: {
        const int last = schedDaysIn(y, mo);
        match = dd == (r.mday > last ? last : r.mday);
        break;
      }
      default: break;
    }
    if (!match) continue;
    const time_t t = schedLocal(y, mo, dd, r.hh, r.mm);
    if (t > after) return t;
  }
  return 0;
}

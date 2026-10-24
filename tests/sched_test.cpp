#include "../SmartThermal/SchedRule.h"
#include <stdio.h>
#include <stdlib.h>
static int fails = 0;
static void show(const char* name, time_t t, const char* want){
  char b[40]; struct tm l; localtime_r(&t,&l); strftime(b,sizeof b,"%a %Y-%m-%d %H:%M",&l);
  const bool ok = !strcmp(b,want); if(!ok) fails++;
  printf("%-34s %s %s%s\n", name, b, ok?"ok":"WANT ", ok?"":want);
}
int main(){
  setenv("TZ","AWST-8",1); tzset();
  time_t now = schedLocal(2026,10,18,16,30);           // Sun 18 Oct 2026 16:30 Perth
  SchedRule r; schedParseTime("07:00", r.hh, r.mm);
  r.rep = REP_DAILY;  show("daily 07:00 after Sun 16:30", schedNextAfter(r, now), "Mon 2026-10-19 07:00");
  schedParseTime("17:00", r.hh, r.mm);
  show("daily 17:00 same day", schedNextAfter(r, now), "Sun 2026-10-18 17:00");
  show("daily 17:00 at exactly 17:00", schedNextAfter(r, schedLocal(2026,10,18,17,0)), "Mon 2026-10-19 17:00");
  r.rep = REP_DAYS; r.days = 0x3E; schedParseTime("07:00", r.hh, r.mm);  // Mon-Fri
  show("weekdays after Fri 08:00", schedNextAfter(r, schedLocal(2026,10,23,8,0)), "Mon 2026-10-26 07:00");
  show("weekdays after Sun", schedNextAfter(r, now), "Mon 2026-10-19 07:00");
  r.rep = REP_MONTH; r.mday = 31;
  show("month 31st after 31 Oct", schedNextAfter(r, schedLocal(2026,10,31,9,0)), "Mon 2026-11-30 07:00");
  r.mday = 29; show("month 29th Feb 2027 -> 28th", schedNextAfter(r, schedLocal(2027,2,1,0,0)), "Sun 2027-02-28 07:00");
  show("month 29th leap 2028", schedNextAfter(r, schedLocal(2028,2,1,0,0)), "Tue 2028-02-29 07:00");
  r.rep = REP_ONCE; schedParseDate("2026-12-25", r.year, r.mon, r.day);
  show("once xmas", schedNextAfter(r, now), "Fri 2026-12-25 07:00");
  // Sydney DST: starts Sun 4 Oct 2026 02:00 -> 03:00
  setenv("TZ","AEST-10AEDT,M10.1.0,M4.1.0/3",1); tzset();
  r.rep = REP_DAILY; schedParseTime("07:00", r.hh, r.mm);
  show("Sydney daily across DST start", schedNextAfter(r, schedLocal(2026,10,3,8,0)), "Sun 2026-10-04 07:00");
  time_t a = schedNextAfter(r, schedLocal(2026,10,3,8,0));
  time_t pre = schedNextAfter(r, schedLocal(2026,10,2,8,0));
  if ((a-pre)/3600 != 23) { fails++; printf("DST gap WRONG %ld\n",(long)(a-pre)/3600); } else printf("DST gap ok (23 h)\n");
  printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails); return fails;
}

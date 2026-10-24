// LogPage.h - /log: the recent log with a Copy button.
//
// The Copy button selects a hidden textarea and uses execCommand, because
// the modern clipboard API is refused on plain http:// pages.

#pragma once
#include <Arduino.h>

static const char LOG_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Log</title>
<style>
body{font:15px -apple-system,system-ui,sans-serif;margin:0;padding:14px;background:#fafafa;color:#222}
.bar{display:flex;gap:8px;position:sticky;top:0;background:#fafafa;padding-bottom:10px}
button{font-size:16px;padding:10px 16px;border:0;border-radius:8px;background:#222;color:#fff}
button.alt{background:#ddd;color:#222}
#msg{align-self:center;color:#2a7a3a}
pre{font:12px ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-all;margin:0}
textarea{position:absolute;left:-9999px}
</style></head><body>
<div class=bar><button id=copy>Copy all</button><button id=ref class=alt>Refresh</button><button id=sv class=alt>Saved log</button><span id=msg></span></div>
<pre id=log>Loading&hellip;</pre><textarea id=ta readonly></textarea>
<script>
const $=i=>document.getElementById(i);
// "Saved log": the longer copy kept on the printer's storage, which also
// survives a power cut (v0.22). The button toggles between the two.
let saved=false;
$('sv').onclick=()=>{saved=!saved;$('sv').textContent=saved?'Recent log':'Saved log';load()};
async function load(){try{const t=await(await fetch(saved?'/log.txt?saved=1':'/log.txt',{cache:'no-store'})).text();$('log').textContent=t;$('ta').value=t;scrollTo(0,document.body.scrollHeight)}catch(e){$('log').textContent='Could not load the log.'}}
$('ref').onclick=load;
$('copy').onclick=()=>{const ta=$('ta');ta.focus();ta.select();ta.setSelectionRange(0,ta.value.length);
 let ok=false;try{ok=document.execCommand('copy')}catch(e){}
 $('msg').textContent=ok?'Copied':'Select and copy by hand';setTimeout(()=>$('msg').textContent='',2500)};
load();
</script></body></html>)HTML";

// Shared by the web API and the setup portal. A header line first, so a
// pasted log always says which build and how long it had been running.
#include "Log.h"
#include "Config.h"
#include <time.h>
template <class Server>
inline void sendLogText(Server& server) {
  // Memory is measured before the snapshot buffer is taken, or the header
  // reports the log's own 16 KB as missing from the largest block.
  const unsigned freeHeap = ESP.getFreeHeap(), largest = ESP.getMaxAllocHeap();
  char* buf = static_cast<char*>(malloc(LogTee::LOG_BYTES + 1));
  if (!buf) { server.send(503, "text/plain", "no memory"); return; }
  const size_t n = Log.snapshot(buf, LogTee::LOG_BYTES);
  buf[n] = 0;

  char when[24] = "clock not synced";
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &t);
  }
  char head[128];
  snprintf(head, sizeof(head),
           "SmartThermal v%s, %s, up %lu s, heap %u, largest %u\n\n",
           FIRMWARE_VERSION, when, (unsigned long)(millis() / 1000),
           freeHeap, largest);

  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(strlen(head) + n);
  server.send(200, "text/plain; charset=utf-8", "");
  server.sendContent(head);
  server.sendContent(buf, n);
  free(buf);
}

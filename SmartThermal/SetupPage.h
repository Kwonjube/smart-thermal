// SetupPage.h - the page the setup hotspot opens by itself.
//
// Self-contained: the phone has no internet while on the hotspot, so no
// fonts, scripts or images from anywhere else. Network names from the scan
// are untrusted (anyone nearby can name a network anything), so they are
// only ever inserted with textContent, never as HTML.

#pragma once
#include <Arduino.h>

static const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Printer setup</title>
<style>
body{font:17px -apple-system,system-ui,sans-serif;margin:0 auto;padding:20px;max-width:420px;color:#222;background:#fafafa}
h1{font-size:23px;margin:0}
.st{padding:11px 13px;border-radius:9px;background:#eee;margin:14px 0;font-size:15px;line-height:1.4}
.ok{background:#e2f3e5}.bad{background:#fbe5e2}
label{display:block;margin:16px 0 5px;font-weight:600}
select,input,button{width:100%;box-sizing:border-box;font-size:17px;padding:12px;border:1px solid #bbb;border-radius:9px;background:#fff}
button{background:#222;color:#fff;border:0;margin-top:20px}
button:disabled{opacity:.45}
small{color:#666;display:block;margin-top:5px}
details{margin-top:26px}summary{color:#555}
.show{display:flex;align-items:center;gap:8px;font-weight:400;margin:8px 0 0;font-size:15px;color:#444}
.show input{width:auto;margin:0;padding:0;transform:scale(1.25)}
</style></head><body>
<h1>Printer setup</h1><small id=unit></small>
<div id=ps class=st>Checking the printer&hellip;</div>
<div id=form>
<label for=net>Your Wi-Fi</label>
<select id=net><option value="">Searching&hellip;</option></select>
<input id=other placeholder="Network name" autocapitalize=none style="display:none;margin-top:8px">
<small>2.4 GHz networks only.</small>
<label for=pw>Wi-Fi password</label>
<input id=pw type=password autocomplete=off autocapitalize=none autocorrect=off spellcheck=false>
<label class=show><input type=checkbox id=show> Show password</label>
<label for=nm>Name this printer</label>
<input id=nm placeholder="e.g. kitchen" maxlength=24 autocapitalize=none autocorrect=off>
<small id=addr></small>
<label class=show style="margin-top:18px;font-weight:600"><input type=checkbox id=on> Allow online printing</label>
<small>Print from anywhere through the website, once approved.</small>
<div id=onf style="display:none">
<small>Shown online by the name above.</small>
<label class=show><input type=checkbox id=ovis> Visible to other printers</label>
<label class=show><input type=checkbox id=osec> Extra secure: approve each device first</label>
</div>
<button id=go>Connect</button>
</div>
<div id=js class=st style="display:none"></div>
<details><summary>Printer not found?</summary>
<small>Check the printer is switched on and its cable is plugged in. If it is still not found, print its self-test slip (hold FEED while switching it on) and enter the IP address it shows.</small>
<input id=pip placeholder="e.g. 192.168.1.100" inputmode=decimal style="margin-top:10px">
<button id=pgo>Use this address</button>
</details>
<script>
const $=i=>document.getElementById(i);let DEF='',busy=false;
function slug(s){return s.toLowerCase().replace(/[^a-z0-9]+/g,'-').replace(/^-+|-+$/g,'').slice(0,24)}
function showAddr(){$('addr').textContent='It will be at http://'+(slug($('nm').value)||DEF)+'.local'}
$('nm').oninput=showAddr;
$('show').onchange=()=>{$('pw').type=$('show').checked?'text':'password'};
$('on').onchange=()=>{$('onf').style.display=$('on').checked?'block':'none'};
$('net').onchange=()=>{$('other').style.display=$('net').value==='*'?'block':'none'};
function line(el,parts){el.textContent='';parts.forEach(p=>{if(p===0){el.appendChild(document.createElement('br'));return}
 if(typeof p==='string'){el.appendChild(document.createTextNode(p));return}
 const b=document.createElement('b');b.textContent=p.b;el.appendChild(b)})}
async function nets(){
 try{const r=await(await fetch('/setup/networks')).json();
  if(r.scanning){setTimeout(nets,1500);return}
  const sel=$('net'),cur=sel.value;sel.textContent='';
  const add=(v,t)=>{const o=document.createElement('option');o.value=v;o.textContent=t;sel.appendChild(o)};
  if(!r.list.length)add('','No networks found');
  r.list.forEach(n=>add(n.s,n.s+(n.lock?'':' (open)')));
  add('*','Other / hidden network\u2026');
  if(cur&&[...sel.options].some(o=>o.value===cur))sel.value=cur;sel.onchange();
 }catch(e){setTimeout(nets,3000)}}
async function status(){
 try{const r=await(await fetch('/setup/status')).json();
  $('unit').textContent=r.unit;if(!DEF){DEF=r.def;showAddr()}
  const ps=$('ps');ps.className='st '+(r.printer==='found'?'ok':r.printer==='searching'?'':'bad');
  ps.textContent={found:'Printer found.',
   searching:'Looking for the printer\u2026',
   nolink:'Printer not detected. Check it is switched on and its cable is plugged in.',
   notfound:'The printer is plugged in but not answering. See \u201cPrinter not found?\u201d below.'}[r.printer];
  const js=$('js');js.style.display=r.join==='idle'?'none':'block';
  if(r.join==='connecting'){js.className='st';line(js,['Joining ',{b:r.ssid},'\u2026 this takes up to 20 seconds.']);$('go').disabled=true}
  else if(r.join==='ok'){js.className='st ok';$('form').style.display='none';
   line(js,['Connected to ',{b:r.ssid},'.',0,'The printer restarts now and prints its address in about a minute.',0,0,
    'Reconnect this device to ',{b:r.ssid},', then open ',{b:'http://'+r.host+'.local'}].concat($('on').checked?
    [0,0,'Online printing: a \u201cWelcome to the internet\u201d slip prints once it is approved.']:[]));return}
  else if(r.join==='failed'){js.className='st bad';line(js,['Could not join ',{b:r.ssid},': '+r.reason+'. Check the password and try again.']);$('go').disabled=false}
 }catch(e){}
 setTimeout(status,1500)}
$('go').onclick=async()=>{
 let ssid=$('net').value;if(ssid==='*')ssid=$('other').value.trim();
 if(!ssid){alert('Choose your Wi-Fi network first.');return}
 const on=$('on').checked;
 $('go').disabled=true;
 try{await fetch('/setup/join',{method:'POST',body:new URLSearchParams({ssid,pass:$('pw').value,name:slug($('nm').value),
  online:on?'1':'0',ovis:$('ovis').checked?'1':'0',osec:$('osec').checked?'1':'0'})})}catch(e){}};
$('pgo').onclick=async()=>{
 const r=await fetch('/setup/printer',{method:'POST',body:new URLSearchParams({ip:$('pip').value.trim()})}).catch(()=>null);
 alert(r&&r.ok?'Trying that address. The status above updates in a few seconds.':'That is not a valid address.')};
nets();status();
</script></body></html>)HTML";

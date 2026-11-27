// Dashboard single-page app, served from flash by web.cpp. No external assets
// (the access point has no internet).
#pragma once
#include <pgmspace.h>

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ATDS Door</title>
<style>
:root{--bg:#0f1419;--panel:#171d24;--line:#26303b;--text:#e6edf3;--mute:#8b98a5;--ok:#2ea043;--warn:#d29922;--bad:#f85149;--info:#388bfd;--acc:#388bfd}
*{box-sizing:border-box}body{margin:0;font:15px/1.45 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:var(--bg);color:var(--text)}
header{display:flex;align-items:center;gap:12px;padding:14px 20px;border-bottom:1px solid var(--line);background:#11171d;position:sticky;top:0;z-index:2}
header h1{font-size:16px;margin:0;font-weight:600;letter-spacing:.2px}header .sp{flex:1}
.pill{display:inline-flex;align-items:center;gap:6px;padding:3px 10px;border-radius:999px;font-size:12px;font-weight:600;border:1px solid var(--line);color:var(--mute);white-space:nowrap}
.pill i{width:8px;height:8px;border-radius:50%;background:currentColor;display:inline-block}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.info{color:var(--info)}.mute{color:var(--mute)}
main{max-width:1180px;margin:0 auto;padding:20px;display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(320px,1fr))}
.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:16px 18px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.8px;color:var(--mute);margin:0 0 12px;font-weight:600}
.wide{grid-column:1/-1}
.big{font-size:28px;font-weight:700;margin:2px 0 6px}
dl{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;margin:0}dt{color:var(--mute)}dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}
button{font:inherit;font-weight:600;border-radius:7px;border:1px solid var(--line);background:#21262d;color:var(--text);padding:8px 14px;cursor:pointer}
button:hover{border-color:#3d4752}button.pri{background:var(--acc);border-color:var(--acc);color:#fff}button.dan{color:var(--bad)}
button:disabled{opacity:.45;cursor:default}.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}
input{font:inherit;background:#0d1117;border:1px solid var(--line);color:var(--text);border-radius:7px;padding:9px 11px;width:100%}
table{width:100%;border-collapse:collapse;font-size:13px}th,td{text-align:left;padding:7px 8px;border-bottom:1px solid var(--line)}th{color:var(--mute);font-weight:600}
td.n{font-variant-numeric:tabular-nums;color:var(--mute);white-space:nowrap}.tbl{max-height:420px;overflow:auto}
.center{min-height:calc(100vh - 60px);display:flex;align-items:center;justify-content:center;padding:20px}
.box{width:100%;max-width:360px}.box .card{padding:22px}.note{color:var(--mute);font-size:13px;margin:10px 0 0}
.pad{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:14px}.pad button{font-size:22px;padding:16px 0}
.dots{font-size:30px;letter-spacing:10px;text-align:center;min-height:44px;margin-top:6px}
#toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);background:#21262d;border:1px solid var(--line);padding:10px 16px;border-radius:8px;display:none;z-index:5}
a{color:var(--acc);cursor:pointer;text-decoration:none}.hide{display:none!important}
</style></head><body>
<header><h1>ATDS Door</h1><span class="sp"></span>
<span id="hLink" class="pill mute"><i></i>link</span><span id="hLock" class="pill mute"><i></i>—</span>
<button id="logout" class="hide" onclick="logout()">Log out</button></header>

<section id="vLogin" class="center hide"><div class="box"><div class="card">
<h2>Administrator</h2><input id="pw" type="password" placeholder="Dashboard password" autocomplete="current-password" onkeydown="if(event.key=='Enter')login()">
<div class="row"><button class="pri" onclick="login()" style="flex:1">Log in</button></div>
<p class="note">Residents: <a onclick="showPin()">enter your PIN on this phone</a> instead of the door keypad.</p>
</div></div></section>

<section id="vPin" class="center hide"><div class="box"><div class="card">
<h2>Phone PIN</h2><div class="dots" id="dots"></div>
<div class="pad" id="pad"></div>
<div class="row"><button class="pri" style="flex:1" onclick="sendPin()">Send PIN</button></div>
<p class="note" id="pinMsg">Enter the shared PIN. Then tap your card at the door and press <b>#</b> without typing digits, within 60 s. The PIN works once.</p>
<p class="note"><a onclick="route()">Back</a></p>
</div></div></section>

<main id="vDash" class="hide">
<div class="card"><h2>Door</h2><div class="big" id="dLock">—</div>
<dl><dt>Bolt position</dt><dd id="dPos">—</dd><dt>Door</dt><dd id="dDoor">—</dd><dt>Locked switch (SW1)</dt><dd id="dSw1">—</dd>
<dt>Latch switch (SW2)</dt><dd id="dSw2">—</dd><dt>Motor</dt><dd id="dMotor">—</dd></dl>
<div class="row"><button class="pri" onclick="act('lock')">Lock door</button><button onclick="showPin()">Phone PIN</button></div>
<p class="note">Unlocking always needs a card at the door.</p></div>

<div class="card"><h2>Security</h2><div class="big" id="dAlarm">—</div>
<dl><dt>Armed</dt><dd id="dArmed">—</dd><dt>Failed attempts</dt><dd id="dFails">—</dd><dt>Lockout</dt><dd id="dLockout">—</dd>
<dt>Vibration / motion</dt><dd id="dSens">—</dd><dt>Phone PIN waiting</dt><dd id="dPhone">—</dd></dl>
<div class="row"><button onclick="act('clear')">Clear alarm</button><button onclick="act('resetlockout')">Reset lockout</button></div></div>

<div class="card"><h2>System</h2>
<dl><dt>Exterior link</dt><dd id="dLink">—</dd><dt>Clock</dt><dd id="dTime">—</dd><dt>Uptime</dt><dd id="dUp">—</dd>
<dt>Log records</dt><dd id="dLog">—</dd><dt>Firmware</dt><dd id="dBuild">—</dd></dl>
<div class="row"><button onclick="syncTime()">Set clock from this device</button><button onclick="chpw()">Change password</button></div></div>

<div class="card"><h2>Cards <span class="mute" id="cCount"></span></h2>
<div class="tbl"><table><thead><tr><th>Slot</th><th>Card UID</th><th></th></tr></thead><tbody id="cards"></tbody></table></div>
<div class="row"><button class="pri" onclick="act('enroll')">Enroll card</button><button onclick="act('setpin')">Change shared PIN</button><button onclick="act('cancel')">Cancel</button></div>
<p class="note" id="adminNote">Enroll and PIN changes are finished at the door with the card and keypad.</p></div>

<div class="card wide"><h2>Event log</h2>
<div class="row" style="margin:0 0 10px"><button onclick="verify()">Verify integrity</button><span id="vres" class="pill mute"><i></i>not checked</span></div>
<div class="tbl"><table><thead><tr><th>#</th><th>Time</th><th>Event</th><th>Details</th></tr></thead><tbody id="events"></tbody></table></div></div>
</main>
<div id="toast"></div>

<script>
const $=s=>document.getElementById(s);let pin='',timer=null,isAdmin=false;
function toast(m,bad){const t=$('toast');t.textContent=m;t.style.color=bad?'var(--bad)':'var(--text)';t.style.display='block';clearTimeout(t.h);t.h=setTimeout(()=>t.style.display='none',3500)}
async function get(u){const r=await fetch(u,{credentials:'same-origin'});if(r.status==401){route();throw 0}return r.json()}
async function post(u,d){const r=await fetch(u,{method:'POST',credentials:'same-origin',headers:{'X-ATDS':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d||{})});
 let j;try{j=await r.json()}catch(e){j={ok:false,msg:'no response'}}if(r.status==401){route()}return j}
function view(v){for(const id of['vLogin','vPin','vDash'])$(id).classList.toggle('hide',id!=v);$('logout').classList.toggle('hide',!isAdmin)}
async function route(){clearInterval(timer);try{isAdmin=(await (await fetch('/api/me')).json()).admin}catch(e){isAdmin=false}
 if(isAdmin){view('vDash');refresh();loadCards();loadEvents();timer=setInterval(()=>{refresh();if(Date.now()%10000<2100)loadEvents()},2000)}
 else{view('vLogin');pubPoll();timer=setInterval(pubPoll,3000)}}
async function login(){const j=await post('/api/login',{password:$('pw').value});$('pw').value='';if(j.ok)route();else toast(j.msg,1)}
async function logout(){await post('/api/logout');isAdmin=false;route()}
const LOCK={LOCKED:['Locked','ok'],UNLOCKED:['Unlocked','warn'],HOLDING:['Latch open — push door','warn'],UNLOCKING:['Unlocking…','info'],
 LOCKING:['Locking…','info'],RELEASING:['Releasing latch…','info'],FAULT:['Fault','bad'],UNKNOWN:['Position unknown','warn']};
function pill(el,txt,cls){el.className='pill '+cls;el.innerHTML='<i></i>'+txt}
function hdr(link,lock){pill($('hLink'),link?'link up':'link down',link?'ok':'bad');const L=LOCK[lock]||[lock,'mute'];pill($('hLock'),L[0],L[1])}
function dur(s){s=Math.floor(s);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+'d ':'')+(h||d?h+'h ':'')+m+'m '+s%60+'s'}
async function refresh(){let s;try{s=await get('/api/status')}catch(e){return}hdr(s.link,s.lock);const L=LOCK[s.lock]||[s.lock,'mute'];
 $('dLock').textContent=L[0];$('dLock').className='big '+L[1];$('dPos').textContent=s.position.toFixed(2)+' turns';
 $('dDoor').innerHTML=s.door=='closed'?'<span class=ok>closed</span>':'<span class=warn>open</span>';
 $('dSw1').textContent=s.sw1?'pressed':'released';$('dSw2').textContent=s.sw2?'pressed':'released';$('dMotor').textContent=s.motor;
 const al=s.alarm=='NONE';$('dAlarm').textContent=al?'No alarm':s.alarm.replace('_',' ');$('dAlarm').className='big '+(al?'ok':'bad');
 $('dArmed').textContent=s.disarmed?'disarmed':(s.armed?'yes':'no');$('dFails').textContent=s.fails;
 $('dLockout').innerHTML=s.lockout?'<span class=bad>'+s.lockout+' s</span>':'none';$('dSens').textContent=(s.vibration?'active':'quiet')+' / '+(s.motion?'active':'quiet');
 $('dPhone').textContent=s.phonePin?s.phonePin+' s left':'no';$('dLink').innerHTML=s.link?'<span class=ok>up</span>, last frame '+s.lastRx+' s ago':'<span class=bad>down</span>';
 $('dTime').textContent=s.rtc==1?new Date(s.time*1000).toLocaleString():(s.rtc==0?'no RTC fitted':(s.rtc==2?'RTC lost power':'RTC error'));
 $('dUp').textContent=dur(s.uptime);$('dLog').textContent=s.logCount+' (head #'+s.logHead+')';$('dBuild').textContent=s.build;
 $('cCount').textContent=s.cards+' / '+s.maxCards+(s.pinSet?'':' — shared PIN not set');
 $('adminNote').textContent=s.adminMode=='none'?'Enroll and PIN changes are finished at the door with the card and keypad.':
  (s.adminMode=='enroll'?'Enroll mode: tap the new card at the door and type the shared PIN, then #.':'PIN change: tap an enrolled card, type the new PIN, #, then repeat.')}
async function loadCards(){let c;try{c=await get('/api/cards')}catch(e){return}const tb=$('cards');tb.innerHTML='';
 for(const x of c.cards){const tr=document.createElement('tr');tr.innerHTML='<td class=n>'+x.slot+'</td><td><code>'+x.uid+'</code></td><td style="text-align:right"><button class=dan>Remove</button></td>';
  tr.querySelector('button').onclick=()=>{if(confirm('Remove card in slot '+x.slot+'?'))act('delete',{slot:x.slot})};tb.appendChild(tr)}
 if(!c.cards.length)tb.innerHTML='<tr><td colspan=3 class=mute>No cards enrolled</td></tr>'}
const EV={BOOT:'Interior started',GRANTED:'Access granted',DENIED_PIN:'Wrong PIN',DENIED_CARD:'Unknown card',DENIED_NO_PIN:'Denied: no PIN',LOCKOUT:'Keypad locked out',
 LOCK_REQUEST:'Lock request',LOCK_DENIED_CARD:'Lock: unknown card',LOCKED:'Bolt locked',UNLOCKED:'Bolt unlocked',DOOR_OPEN:'Door opened',DOOR_CLOSED:'Door closed',
 ALARM:'ALARM',ALARM_CLEARED:'Alarm cleared',FAULT:'Motor fault',CARD_ADDED:'Card enrolled',CARD_REMOVED:'Card removed',PIN_CHANGED:'Shared PIN changed',WIPE:'All cards wiped',
 TIME_SET:'Clock set',CLOCK_JUMP:'Clock jumped',CLOCK_BACKWARDS:'Clock went backwards',TIME_INVALID:'Clock lost power',LOG_ROLLBACK:'Log rollback detected',LOG_REPAIRED:'Log repaired',
 LOCKOUT_RESET:'Lockout reset',LATCH_RELEASED:'Latch released',WEB_LOGIN:'Dashboard login',WEB_LOGIN_FAIL:'Dashboard login failed',PHONE_PIN:'Phone PIN entered',WEB_ACTION:'Dashboard action'};
const BAD=['ALARM','FAULT','DENIED_PIN','DENIED_CARD','LOCKOUT','LOCK_DENIED_CARD','WEB_LOGIN_FAIL','CLOCK_JUMP','CLOCK_BACKWARDS','LOG_ROLLBACK','TIME_INVALID'];
const ALARMS=['','Forced door','Vibration','Exterior link lost','Radio tampering','Bolt tampering','Motion'];
const WA=['','lock','clear alarm','reset lockout','enroll','change PIN','remove card','set clock','change password','cancel'];
function details(e){let s=[];if(e.slot>=0)s.push('card '+e.slot);
 if(e.ev=='GRANTED'&&e.d==1)s.push('phone PIN');if(e.ev=='DENIED_PIN'&&e.d==1)s.push('phone PIN');if(e.ev=='ALARM')s.push(ALARMS[e.d]||e.d);
 if(e.ev=='LOCKOUT')s.push(e.a1+' s');if(e.ev=='LATCH_RELEASED')s.push(e.d?'door opened':'hold timed out');
 if(e.ev=='LOCK_REQUEST')s.push(e.d?'accepted':'refused');if(e.ev=='WEB_ACTION')s.push((WA[e.d]||e.d)+(e.a1?'':' (failed)'));
 if(e.ev=='TIME_SET')s.push(new Date(e.a1*1000).toLocaleString()+' → '+new Date(e.a2*1000).toLocaleString());return s.join(', ')}
async function loadEvents(){let j;try{j=await get('/api/events?n=100')}catch(e){return}const tb=$('events');tb.innerHTML='';
 for(const e of j.events.reverse()){const t=e.st==1?new Date(e.t*1000).toLocaleString():'boot '+e.boot+' +'+dur(e.up);
  const tr=document.createElement('tr');tr.innerHTML='<td class=n>'+e.seq+'</td><td class=n>'+t+'</td><td class="'+(BAD.includes(e.ev)?'bad':'')+'">'+(EV[e.ev]||e.ev)+'</td><td>'+details(e)+'</td>';tb.appendChild(tr)}}
async function verify(){pill($('vres'),'checking…','info');const j=await get('/api/verify');
 pill($('vres'),j.ok?'intact: '+j.checked+' records'+(j.backwards?', '+j.backwards+' time steps back':''):'TAMPERED at #'+j.bad,j.ok?'ok':'bad')}
async function act(a,extra){const j=await post('/api/action',Object.assign({a},extra||{}));toast(j.msg,!j.ok);refresh();loadCards();loadEvents()}
function syncTime(){act('time',{t:Math.floor(Date.now()/1000)})}
async function chpw(){const o=prompt('Current password');if(o==null)return;const n=prompt('New password (8+ characters)');if(!n)return;act('password',{old:o,new:n})}
function showPin(){pin='';drawPin();view('vPin');clearInterval(timer);pubPoll();timer=setInterval(pubPoll,1000)}
function drawPin(){$('dots').textContent='•'.repeat(pin.length)||'·'}
(function(){const p=$('pad');for(const k of['1','2','3','4','5','6','7','8','9','⌫','0','C']){const b=document.createElement('button');b.textContent=k;
 b.onclick=()=>{if(k=='⌫')pin=pin.slice(0,-1);else if(k=='C')pin='';else if(pin.length<8)pin+=k;drawPin()};p.appendChild(b)}})();
async function sendPin(){if(pin.length<4){toast('PIN needs 4-8 digits',1);return}const j=await post('/api/phonepin',{pin});pin='';drawPin();toast(j.msg,!j.ok)}
async function pubPoll(){let s;try{s=await (await fetch('/api/public')).json()}catch(e){return}hdr(s.link,s.lock);
 if(!$('vPin').classList.contains('hide')){$('pinMsg').innerHTML=s.lockout?'<span class=bad>Keypad locked out for '+s.lockout+' s.</span>':
  (s.phonePin?'<span class=ok>PIN ready.</span> Tap your card at the door and press <b>#</b> without digits ('+s.phonePin+' s left).':
  'Enter the shared PIN. Then tap your card at the door and press <b>#</b> without typing digits, within 60 s. The PIN works once.')}}
route();
</script></body></html>)HTML";

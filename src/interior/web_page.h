// Dashboard single-page app, served from flash by web.cpp. No external assets
// (the access point has no internet). "Hazel" is a rule-based helper that only
// rephrases what the door reports (status + event log): it cannot invent facts
// and cannot unlock. See docs/DASHBOARD.md.
#pragma once
#include <pgmspace.h>

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en" data-size="m" data-theme="light"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#f5f6f8">
<title>Front Door</title>
<style>
:root{--bg:#f5f6f8;--card:#fff;--text:#13171c;--mute:#5d6672;--line:#e6e8ec;--soft:#eef1f5;--acc:#2f6bff;--acc2:#1f55e0;
--ok:#0f7a55;--okb:#e5f5ee;--warn:#9a5806;--warnb:#fdf2e1;--bad:#bf2530;--badb:#fdebec;--info:#2f6bff;--infob:#e9efff;
--sh:0 1px 2px rgba(16,24,40,.04),0 6px 24px rgba(16,24,40,.06);--r:20px}
html[data-theme=dark]{--bg:#0c0e11;--card:#15181d;--text:#eef1f4;--mute:#9aa4af;--line:#242930;--soft:#1d2127;--acc:#5b8cff;--acc2:#7aa2ff;
--ok:#4ade9a;--okb:#11281f;--warn:#fbbf4a;--warnb:#2b2112;--bad:#ff7a7f;--badb:#2e1517;--info:#7aa2ff;--infob:#16203a;--sh:none}
html{font-size:17px}html[data-size=s]{font-size:15px}html[data-size=l]{font-size:20px}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--text);font:400 1rem/1.5 system-ui,-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;-webkit-font-smoothing:antialiased}
h1{font-size:1.9rem;line-height:1.15;margin:0;font-weight:700;letter-spacing:-.02em}h2{font-size:1.05rem;margin:0;font-weight:650}
p{margin:.25rem 0}.mute{color:var(--mute)}.small{font-size:.88rem}.hide{display:none!important}.grow{flex:1;min-width:0}
a{color:var(--acc);cursor:pointer;font-weight:600;text-decoration:none}
.ic{width:1.35em;height:1.35em;fill:none;stroke:currentColor;stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round;flex:none}
button{font:inherit;color:inherit}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:.5rem;min-height:2.9rem;padding:.6rem 1.15rem;border-radius:14px;border:0;background:var(--soft);color:var(--text);font-weight:600;cursor:pointer;transition:transform .08s,background .15s}
.btn:active{transform:scale(.98)}.btn:disabled{opacity:.5;cursor:default}.btn:focus-visible,input:focus-visible,.tab-btn:focus-visible{outline:3px solid var(--acc);outline-offset:2px}
.btn.pri{background:var(--acc);color:#fff}.btn.big{min-height:3.5rem;font-size:1.05rem;width:100%}.btn.sm{min-height:2.4rem;padding:.35rem .9rem;font-size:.92rem}
.btn.ghost{background:transparent;box-shadow:inset 0 0 0 1.5px var(--line)}.btn.danger{color:var(--bad)}
input[type=text],input[type=password]{font:inherit;width:100%;padding:.8rem 1rem;border-radius:14px;border:1.5px solid var(--line);background:var(--card);color:var(--text);margin:.35rem 0 .9rem}
label{font-weight:600;font-size:.95rem}
.card{background:var(--card);border-radius:var(--r);box-shadow:var(--sh);padding:1.2rem 1.3rem;border:1px solid var(--line)}
.card-h{display:flex;align-items:center;justify-content:space-between;margin-bottom:.6rem}
.stack{display:flex;flex-direction:column;gap:1rem}
/* app shell */
.app{min-height:100vh}.side{display:none}
.main{padding:1.2rem 1.1rem calc(6.2rem + env(safe-area-inset-bottom));max-width:1160px;margin:0 auto}
.bar{position:fixed;left:0;right:0;bottom:0;display:flex;background:color-mix(in srgb,var(--card) 88%,transparent);backdrop-filter:saturate(1.4) blur(14px);-webkit-backdrop-filter:saturate(1.4) blur(14px);border-top:1px solid var(--line);padding:.35rem .4rem calc(.35rem + env(safe-area-inset-bottom));z-index:5}
.tab-btn{flex:1;display:flex;flex-direction:column;align-items:center;gap:.15rem;padding:.4rem 0;border:0;background:none;color:var(--mute);font-size:.72rem;font-weight:600;cursor:pointer;border-radius:12px}
.tab-btn .ic{width:1.55rem;height:1.55rem}.tab-btn.on{color:var(--acc)}
.top{display:flex;align-items:flex-end;justify-content:space-between;gap:1rem;margin:.6rem 0 1.2rem}
.eyebrow{color:var(--mute);font-size:.92rem;font-weight:600;margin:0 0 .2rem}
.icon-btn{width:2.9rem;height:2.9rem;border-radius:50%;border:0;background:var(--card);box-shadow:var(--sh);display:grid;place-items:center;cursor:pointer;color:var(--text);border:1px solid var(--line)}
.grid2{display:grid;gap:1rem}
@media(min-width:960px){
 .app{display:grid;grid-template-columns:268px 1fr}
 .side{display:flex;flex-direction:column;gap:.3rem;position:sticky;top:0;height:100vh;padding:1.6rem 1rem;border-right:1px solid var(--line);background:var(--card)}
 .side .tab-btn{flex:none;flex-direction:row;gap:.8rem;font-size:1rem;padding:.75rem .9rem;color:var(--text);justify-content:flex-start}
 .side .tab-btn.on{background:var(--infob);color:var(--acc)}
 .brand{display:flex;align-items:center;gap:.6rem;font-weight:700;font-size:1.15rem;padding:0 .6rem 1.4rem}
 .brand .logo-s{width:2.3rem;height:2.3rem;border-radius:12px;background:var(--acc);color:#fff;display:grid;place-items:center}
 .side-foot{margin-top:auto;padding:.8rem;color:var(--mute);font-size:.88rem}
 .bar{display:none}.main{padding:2.4rem 2.8rem 3rem}
 .grid2{grid-template-columns:minmax(0,1.15fr) minmax(0,.85fr);gap:1.4rem;align-items:start}
 h1{font-size:2.2rem}
}
/* hero */
.hero{display:flex;align-items:center;gap:1.1rem;padding:1.5rem;border-radius:24px;border:1px solid transparent}
.hero-ic{width:4.2rem;height:4.2rem;border-radius:50%;display:grid;place-items:center;color:#fff;flex:none}
.hero-ic .ic{width:2.1rem;height:2.1rem;stroke-width:2.1}
.hero-t{font-size:1.75rem;font-weight:750;letter-spacing:-.02em;line-height:1.15}.hero-s{font-size:1.02rem;margin-top:.2rem}
.tone-ok{background:var(--okb);color:var(--ok)}.tone-ok .hero-ic{background:var(--ok)}
.tone-warn{background:var(--warnb);color:var(--warn)}.tone-warn .hero-ic{background:var(--warn)}
.tone-bad{background:var(--badb);color:var(--bad)}.tone-bad .hero-ic{background:var(--bad)}
.tone-info{background:var(--infob);color:var(--info)}.tone-info .hero-ic{background:var(--info)}
.hero .hero-s{color:var(--text);opacity:.85}
html[data-theme=dark] .hero-ic{color:#0c0e11}
.alert{display:flex;gap:.9rem;padding:1rem 1.1rem;border-radius:18px;align-items:flex-start}
.alert p{color:var(--text);opacity:.9;margin:.15rem 0 .55rem}.alert b{display:block}
/* helper */
.helper{display:flex;gap:.9rem;align-items:flex-start}
.avatar{width:2.6rem;height:2.6rem;border-radius:50%;background:linear-gradient(135deg,#ffb36b,#ff6f91);color:#fff;font-weight:750;display:grid;place-items:center;flex:none;font-size:1.05rem}
.avatar.sm{width:2rem;height:2rem;font-size:.85rem}
.say{font-size:1.08rem;line-height:1.55;margin:.2rem 0 .8rem}
.row{display:flex;gap:.6rem;flex-wrap:wrap}
.actions{display:grid;gap:.7rem;grid-template-columns:1fr 1fr}
/* lists */
.feed{list-style:none;margin:0;padding:0}.feed li{display:flex;gap:.85rem;align-items:center;padding:.75rem 0;border-top:1px solid var(--line)}
.feed li:first-child{border-top:0}.when{color:var(--mute);font-size:.88rem}
.fi{width:2.5rem;height:2.5rem;border-radius:50%;display:grid;place-items:center;flex:none}
.fi.k-in{background:var(--okb);color:var(--ok)}.fi.k-lock{background:var(--infob);color:var(--info)}.fi.k-warn{background:var(--warnb);color:var(--warn)}
.fi.k-bad{background:var(--badb);color:var(--bad)}.fi.k-info{background:var(--soft);color:var(--mute)}
.empty{color:var(--mute);padding:.9rem 0;list-style:none}
.grp{font-size:.82rem;text-transform:uppercase;letter-spacing:.08em;color:var(--mute);margin-bottom:.2rem}
.glance{display:grid;grid-template-columns:1fr 1fr;gap:.6rem}
.gl{background:var(--soft);border-radius:14px;padding:.75rem .9rem}.gl b{display:block;font-size:1.02rem}.gl span{color:var(--mute);font-size:.85rem}
.dot{display:inline-block;width:.55rem;height:.55rem;border-radius:50%;margin-right:.4rem;vertical-align:middle}
.d-ok{background:var(--ok)}.d-warn{background:var(--warn)}.d-bad{background:var(--bad)}
.people{list-style:none;margin:0;padding:0}.people li{display:flex;align-items:center;gap:.8rem;padding:.8rem 0;border-top:1px solid var(--line);flex-wrap:wrap}
.people li:first-child{border-top:0}
.av{width:2.7rem;height:2.7rem;border-radius:50%;background:var(--infob);color:var(--acc);display:grid;place-items:center;font-weight:700;flex:none}
/* segmented, toggles */
.seg{display:inline-flex;background:var(--soft);border-radius:14px;padding:.25rem;gap:.2rem;flex-wrap:wrap}
.seg button{border:0;background:none;padding:.5rem .95rem;border-radius:11px;font-weight:600;cursor:pointer;color:var(--mute);min-height:2.4rem}
.seg button.on{background:var(--card);color:var(--text);box-shadow:var(--sh)}
.li{display:flex;align-items:center;justify-content:space-between;gap:1rem;padding:.9rem 0;border-top:1px solid var(--line);flex-wrap:wrap}.li:first-child{border-top:0}
.tog{position:relative;width:3.3rem;height:2rem;flex:none}.tog input{opacity:0;width:0;height:0}
.tog span{position:absolute;inset:0;background:var(--line);border-radius:999px;transition:.2s;cursor:pointer}
.tog span:before{content:"";position:absolute;width:1.6rem;height:1.6rem;left:.2rem;top:.2rem;background:#fff;border-radius:50%;transition:.2s;box-shadow:0 1px 3px rgba(0,0,0,.2)}
.tog input:checked+span{background:var(--acc)}.tog input:checked+span:before{transform:translateX(1.3rem)}
.check{display:flex;align-items:center;gap:.6rem;margin:.8rem 0;color:var(--mute);font-weight:600}
.check input{width:1.2rem;height:1.2rem}
details.tech summary{cursor:pointer;font-weight:650}dl.kv{display:grid;grid-template-columns:auto 1fr;gap:.35rem 1rem;margin:.9rem 0 0;font-size:.92rem}
dl.kv dt{color:var(--mute)}dl.kv dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}
/* chat */
.chat{display:flex;flex-direction:column;min-height:60vh}
.msgs{flex:1;display:flex;flex-direction:column;gap:.8rem;padding:.2rem 0 1rem}
.msg{display:flex;gap:.6rem;align-items:flex-end;flex-wrap:wrap}.msg.me{justify-content:flex-end}
.bubble{max-width:min(85%,560px);padding:.8rem 1rem;border-radius:20px;background:var(--soft);line-height:1.5;font-size:1.04rem;position:relative}
.msg.bot .bubble{border-bottom-left-radius:6px}.msg.me .bubble{background:var(--acc);color:#fff;border-bottom-right-radius:6px}
.say-btn{border:0;background:none;color:var(--mute);cursor:pointer;padding:.3rem 0 0;display:flex;align-items:center;gap:.3rem;font-size:.82rem;font-weight:600}
.msg-acts{flex-basis:100%;display:flex;gap:.5rem;flex-wrap:wrap;padding-left:2.6rem}
.chips{display:flex;gap:.5rem;flex-wrap:wrap;margin:.2rem 0 .9rem}
.chip{border:1.5px solid var(--line);background:var(--card);border-radius:999px;padding:.5rem .95rem;font-weight:600;cursor:pointer;font-size:.93rem;color:var(--text)}
.ask{display:flex;gap:.5rem}.ask input{margin:0}.ask .btn{flex:none;width:3.2rem;padding:0}
/* solo pages (login, phone PIN) */
.solo{min-height:100vh;display:flex;align-items:center;justify-content:center;padding:1.5rem}
.solo-box{width:100%;max-width:390px;text-align:center}.solo-box form{text-align:left;margin-top:1.4rem}
.logo{width:4.2rem;height:4.2rem;border-radius:22px;background:var(--acc);color:#fff;display:grid;place-items:center;margin:0 auto 1.1rem}
.logo .ic{width:2.2rem;height:2.2rem}
.dots{display:flex;justify-content:center;gap:.8rem;margin:1.4rem 0 1.1rem;min-height:1.2rem}
.dots i{width:1rem;height:1rem;border-radius:50%;background:var(--line)}.dots i.on{background:var(--text)}
.pad{display:grid;grid-template-columns:repeat(3,1fr);gap:.8rem;margin:0 auto 1.2rem;max-width:320px}
.pad button{aspect-ratio:1.25;border-radius:22px;border:0;background:var(--card);box-shadow:var(--sh);border:1px solid var(--line);font-size:1.6rem;font-weight:600;cursor:pointer;color:var(--text)}
.pad button:active{background:var(--soft)}
.back{display:inline-flex;align-items:center;gap:.3rem;border:0;background:none;color:var(--acc);font-weight:600;cursor:pointer;padding:.4rem 0;margin-bottom:1rem}
/* sheet */
.sheet{position:fixed;inset:0;z-index:20;display:flex;align-items:flex-end;justify-content:center}
.sheet-bg{position:absolute;inset:0;background:rgba(10,12,16,.45)}
.sheet-box{position:relative;background:var(--card);width:100%;max-width:520px;border-radius:26px 26px 0 0;padding:1.6rem 1.4rem calc(1.4rem + env(safe-area-inset-bottom))}
.sheet-box h3{margin:0 0 .5rem;font-size:1.35rem}.sheet-box .btns{display:grid;gap:.6rem;margin-top:1.2rem}
@media(min-width:640px){.sheet{align-items:center}.sheet-box{border-radius:26px}}
#toast{position:fixed;left:50%;bottom:calc(6.5rem + env(safe-area-inset-bottom));transform:translateX(-50%);background:var(--text);color:var(--bg);padding:.75rem 1.2rem;border-radius:14px;font-weight:600;z-index:30;display:none;max-width:90vw;text-align:center}
@media(min-width:960px){#toast{bottom:2rem}}
.banner{background:var(--badb);color:var(--bad);padding:.8rem 1rem;border-radius:14px;margin-bottom:1rem;font-weight:600}
@media(prefers-reduced-motion:reduce){*{transition:none!important;scroll-behavior:auto!important}}
</style></head><body>

<div id="vLogin" class="solo hide"><div class="solo-box">
 <div class="logo" id="logo1"></div>
 <h1>Welcome home</h1><p class="mute">Sign in to look after your front door.</p>
 <form onsubmit="login(event)"><label for="pw">Password</label>
  <input id="pw" type="password" autocomplete="current-password" required>
  <button class="btn pri big" type="submit">Sign in</button></form>
 <p style="margin:1rem 0"><button class="btn ghost big" onclick="showPin()" id="usePhone1"></button></p>
 <p class="mute small" id="loginStatus" aria-live="polite"></p>
</div></div>

<div id="vPin" class="solo hide"><div class="solo-box">
 <button class="back" onclick="route()" id="backBtn"></button>
 <h1>Your door PIN</h1>
 <p class="mute" id="pinMsg" aria-live="polite"></p>
 <div class="dots" id="dots" aria-label="PIN digits entered"></div>
 <div class="pad" id="pad"></div>
 <button class="btn pri big" onclick="sendPin()">Send PIN</button>
</div></div>

<div id="vApp" class="app hide">
 <aside class="side"><div class="brand"><span class="logo-s" id="logo2"></span>Front Door</div><nav id="sideNav"></nav><div class="side-foot" id="sideFoot"></div></aside>
 <div class="main">
  <div id="offline" class="banner hide">I can't reach the door right now. Please check this device is on the "ATDS-Door" WiFi.</div>

  <section id="t-home">
   <header class="top"><div><p class="eyebrow" id="hDate"></p><h1 id="hHello">Hello</h1></div>
    <button class="icon-btn" id="hSpeak" aria-label="Read the summary aloud" onclick="speak(greeting()+'. '+summary())"></button></header>
   <div class="grid2"><div class="stack">
    <div class="hero tone-info" id="hero" aria-live="polite"><div class="hero-ic" id="heroIc"></div><div><div class="hero-t" id="heroT">Connecting…</div><div class="hero-s" id="heroS"></div></div></div>
    <div class="stack" id="alerts"></div>
    <div class="card helper"><div class="avatar">H</div><div class="grow"><b>Hazel</b> <span class="mute small">your door helper</span>
     <p class="say" id="sumText">Let me check on your door…</p>
     <div class="row"><button class="btn sm ghost" onclick="speak($('sumText').textContent)" id="readBtn"></button><button class="btn sm ghost" onclick="go('helper')">Ask Hazel</button></div></div></div>
    <div class="actions"><button class="btn big pri" id="bLock" onclick="confirmLock()"></button><button class="btn big" onclick="showPin()" id="usePhone2"></button></div>
   </div><div class="stack">
    <div class="card"><div class="card-h"><h2>Recently</h2><a onclick="go('activity')">See all</a></div><ul class="feed" id="recent"></ul></div>
    <div class="card"><div class="card-h"><h2>At a glance</h2></div><div class="glance" id="glance"></div></div>
   </div></div>
  </section>

  <section id="t-activity" class="hide">
   <header class="top"><div><h1>Activity</h1><p class="mute">Everything your door has noticed, newest first.</p></div></header>
   <div class="seg" id="fil"><button data-f="all" class="on">Everything</button><button data-f="in">Comings &amp; goings</button><button data-f="warn">Worth a look</button></div>
   <label class="check"><input type="checkbox" id="tech" onchange="renderFeed()"> Show technical details</label>
   <div class="stack" id="feedAll"></div>
   <div class="card" style="margin-top:1rem"><div class="helper"><div class="fi k-lock" id="shieldIc"></div><div class="grow"><b>Is this history genuine?</b>
    <p class="mute">Every entry is sealed, so nobody can change it without it showing.</p><p class="say" id="vres"></p>
    <button class="btn sm" onclick="verify()">Check now</button></div></div></div>
  </section>

  <section id="t-helper" class="hide">
   <header class="top"><div><h1>Ask Hazel</h1><p class="mute">Hazel explains what your door has recorded, in plain words.</p></div></header>
   <div class="card chat"><div id="msgs" class="msgs" aria-live="polite"></div>
    <div class="chips" id="chips"></div>
    <form class="ask" onsubmit="ask(event)"><input type="text" id="q" placeholder="Type a question…" autocomplete="off" aria-label="Your question"><button class="btn pri" type="submit" aria-label="Ask" id="sendBtn"></button></form>
    <p class="mute small" style="margin-top:.7rem">Hazel only tells you what the door has recorded. She can lock the door for you, but never unlock it.</p></div>
  </section>

  <section id="t-people" class="hide">
   <header class="top"><div><h1>People &amp; cards</h1><p class="mute">Everyone with a card uses the same door PIN.</p></div></header>
   <div class="stack"><div id="modeBanner"></div>
    <div class="card"><ul class="people" id="people"></ul></div>
    <div class="actions"><button class="btn pri big" onclick="addCard()">Add a card</button><button class="btn big" onclick="changePin()">Change the door PIN</button></div></div>
  </section>

  <section id="t-settings" class="hide">
   <header class="top"><div><h1>Settings</h1></div></header>
   <div class="stack">
    <div class="card"><div class="li"><div><b>Text size</b><p class="mute small">Make everything easier to read</p></div>
      <div class="seg" id="sizeSeg"><button data-v="s" style="font-size:.85rem">A</button><button data-v="m" style="font-size:1rem">A</button><button data-v="l" style="font-size:1.25rem">A</button></div></div>
     <div class="li"><div><b>Appearance</b></div><div class="seg" id="themeSeg"><button data-v="auto">Auto</button><button data-v="light">Light</button><button data-v="dark">Dark</button></div></div>
     <div class="li"><div><b>Read answers aloud</b><p class="mute small">Hazel speaks her answers and the summary</p></div><label class="tog"><input type="checkbox" id="autoSpeak" onchange="setPref('speak',this.checked)"><span></span></label></div></div>
    <div class="card">
     <div class="li"><div><b>Alarm</b><p class="mute small" id="alarmTxt"></p></div><button class="btn sm" onclick="confirmClear()" id="clearBtn">Switch off</button></div>
     <div class="li"><div><b>Keypad</b><p class="mute small" id="lockoutTxt"></p></div><button class="btn sm" onclick="act('resetlockout')" id="lockoutBtn">End the pause</button></div>
     <div class="li"><div><b>Clock</b><p class="mute small" id="clockTxt"></p></div><button class="btn sm" onclick="syncTime()">Set from this device</button></div>
     <div class="li"><div><b>App password</b><p class="mute small">Used to sign in to this app</p></div><button class="btn sm" onclick="chpw()">Change</button></div></div>
    <details class="card tech"><summary>Technical details</summary><dl class="kv" id="techDl"></dl></details>
    <button class="btn big ghost danger" onclick="logout()">Sign out</button>
   </div>
  </section>
 </div>
 <nav class="bar" id="barNav"></nav>
</div>

<div id="sheet" class="sheet hide"><div class="sheet-bg" onclick="closeSheet()"></div>
 <div class="sheet-box" role="dialog" aria-modal="true" aria-labelledby="shT"><h3 id="shT"></h3><div id="shB"></div><div class="btns" id="shBtns"></div></div></div>
<div id="toast" role="status" aria-live="polite"></div>

<script>
'use strict';
const $=id=>document.getElementById(id);
const esc=s=>String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const store={get(k,d){try{const v=localStorage.getItem('atds.'+k);return v==null?d:JSON.parse(v)}catch(e){return d}},set(k,v){try{localStorage.setItem('atds.'+k,JSON.stringify(v))}catch(e){}}};
const HELPER='Hazel';
let S=null,EV=[],CARDS=[],NAMES={},isAdmin=false,tab='home',timer=null,tick=0,pin='',filter='all',spoke=false,lastVerify=null;

const IC={lock:'<rect x="5" y="11" width="14" height="10" rx="2.5"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
unlock:'<rect x="5" y="11" width="14" height="10" rx="2.5"/><path d="M8 11V8a4 4 0 0 1 7.6-1.8"/>',
alert:'<path d="M10.3 3.9 2.4 18a2 2 0 0 0 1.7 3h15.8a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/><path d="M12 9v4M12 17h.01"/>',
home:'<path d="M3 10.5 12 3l9 7.5"/><path d="M5 9.5V20h14V9.5"/><path d="M10 20v-5h4v5"/>',
list:'<path d="M9 6h11M9 12h11M9 18h11"/><path d="M4 6h.01M4 12h.01M4 18h.01"/>',
chat:'<path d="M21 12a8 8 0 0 1-11.6 7.1L4 20.5l1.4-5A8 8 0 1 1 21 12z"/>',
people:'<circle cx="9" cy="8" r="3.5"/><path d="M2.5 20a6.5 6.5 0 0 1 13 0"/><path d="M16 4.6a3.5 3.5 0 0 1 0 6.8M21.5 20a6.5 6.5 0 0 0-4-6"/>',
gear:'<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/>',
in:'<path d="M10 17l5-5-5-5M15 12H3"/><path d="M14 4h4a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-4"/>',
speak:'<path d="M11 5 6 9H3v6h3l5 4V5z"/><path d="M15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13"/>',
phone:'<rect x="7" y="2.5" width="10" height="19" rx="2.5"/><path d="M11 18h2"/>',
dot:'<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5M12 8h.01"/>',
shield:'<path d="M12 3 4.5 6v6c0 4.6 3.2 8 7.5 9 4.3-1 7.5-4.4 7.5-9V6L12 3z"/><path d="m9 12 2 2 4-4"/>',
send:'<path d="M5 12h14M13 6l6 6-6 6"/>',back:'<path d="M15 18l-6-6 6-6"/>'};
const ic=(n,c)=>`<svg viewBox="0 0 24 24" class="ic ${c||''}" aria-hidden="true">${IC[n]||''}</svg>`;

/* ---------------------------------------------------------------- prefs */
function applyPrefs(){const r=document.documentElement,t=store.get('theme','auto'),sz=store.get('size','m');r.dataset.size=sz;
 r.dataset.theme=t=='auto'?(window.matchMedia&&matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light'):t;
 const m=document.querySelector('meta[name=theme-color]');if(m)m.content=r.dataset.theme=='dark'?'#0c0e11':'#f5f6f8';
 document.querySelectorAll('#sizeSeg button').forEach(b=>b.classList.toggle('on',b.dataset.v==sz));
 document.querySelectorAll('#themeSeg button').forEach(b=>b.classList.toggle('on',b.dataset.v==t));
 $('autoSpeak').checked=!!store.get('speak',false)}
function setPref(k,v){store.set(k,v);applyPrefs()}

/* ------------------------------------------------------------------ api */
async function get(u){const r=await fetch(u,{credentials:'same-origin',cache:'no-store'});if(r.status==401){isAdmin=false;route();throw new Error('auth')}return r.json()}
async function post(u,d){let r;try{r=await fetch(u,{method:'POST',credentials:'same-origin',headers:{'X-ATDS':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d||{})})}
 catch(e){return{ok:false,msg:"I couldn't reach the door. Are you still on its WiFi?"}}
 let j;try{j=await r.json()}catch(e){j={ok:false,msg:'The door did not answer'}}if(r.status==401){isAdmin=false;route()}return j}
function toast(m){const t=$('toast');t.textContent=m;t.style.display='block';clearTimeout(t.h);t.h=setTimeout(()=>t.style.display='none',4000)}
const FRIENDLY={'locking':'Locking your door now.','already locked':'Your door is already locked.','close the door first':'Please close the door first.',
 'alarm cleared':'The alarm is off.','no alarm':'There is no alarm right now.','lockout reset':'The keypad is ready again.','card removed':'The card was removed.',
 'cancelled':'Cancelled.','clock set (logged)':'The clock is set.','name saved':'Name saved.','name removed':'Name removed.','password changed':'Your password has been changed.',
 'wrong password':'That password is not right. Please try again.','no DS3231 clock fitted':'This door has no clock fitted yet.'};
function friendly(m){m=String(m||'');return FRIENDLY[m]||(m.charAt(0).toUpperCase()+m.slice(1)+(/[.!?]$/.test(m)?'':'.'))}

/* ------------------------------------------------------------ navigation */
const TABS=[['home','Home','home'],['activity','Activity','list'],['helper','Hazel','chat'],['people','People','people'],['settings','Settings','gear']];
function buildNav(){const h=TABS.map(([id,l,i])=>`<button class="tab-btn" data-t="${id}" onclick="go('${id}')">${ic(i)}<span>${l}</span></button>`).join('');
 $('sideNav').innerHTML=h;$('barNav').innerHTML=h}
function go(t){tab=t;for(const[id]of TABS)$('t-'+id).classList.toggle('hide',id!=t);
 document.querySelectorAll('[data-t]').forEach(b=>{b.classList.toggle('on',b.dataset.t==t);if(b.dataset.t==t)b.setAttribute('aria-current','page');else b.removeAttribute('aria-current')});
 window.scrollTo(0,0);render();if(t=='helper'&&!$('msgs').children.length)greetChat()}
function view(v){for(const id of['vLogin','vPin','vApp'])$(id).classList.toggle('hide',id!=v)}
async function route(){clearInterval(timer);timer=null;let me={admin:false};
 try{me=await (await fetch('/api/me',{cache:'no-store'})).json()}catch(e){}
 isAdmin=!!me.admin;
 if(isAdmin){view('vApp');go(tab);await refresh(true);timer=setInterval(()=>refresh(false),2500)}
 else{view('vLogin');pubPoll();timer=setInterval(pubPoll,4000)}}
async function refresh(full){try{S=await get('/api/status')}catch(e){$('offline').classList.remove('hide');return}
 $('offline').classList.add('hide');
 if(full||++tick%4==0){try{const[e,c]=await Promise.all([get('/api/events?n=120'),get('/api/cards')]);EV=e.events||[];CARDS=c.cards||[];
  NAMES={};for(const x of CARDS)if(x.name)NAMES[x.slot]=x.name}catch(e){}}
 render();if(full&&!spoke&&store.get('speak',false)){spoke=true;speak(greeting()+'. '+summary())}}
async function login(ev){ev.preventDefault();const j=await post('/api/login',{password:$('pw').value});$('pw').value='';if(j.ok){tab='home';route()}else toast(friendly(j.msg))}
async function logout(){await post('/api/logout');isAdmin=false;S=null;route()}

/* ------------------------------------------------------- understanding */
const ALARMS={FORCED_DOOR:'the door was opened while it was locked',VIBRATION:'the door was knocked or shaken hard',TAMPER_LINK:'the keypad outside stopped answering',
 TAMPER_RF:'someone sent fake signals to the lock',BOLT_TAMPER:'the bolt was moved by hand',MOTION:'someone moved near the door while it was locked'};
const ALARM_BY_D=['','FORCED_DOOR','VIBRATION','TAMPER_LINK','TAMPER_RF','BOLT_TAMPER','MOTION'];
const WA_TXT=['','The door was locked','The alarm was switched off','The keypad pause was ended','Adding a card was started','Changing the PIN was started',
 'A card was removed','The clock was set','The app password was changed','A change was cancelled','A card was renamed'];
const who=s=>NAMES[s]||('Card '+(s+1));
const lastOf=f=>{for(let i=EV.length-1;i>=0;i--)if(f(EV[i]))return EV[i];return null};
const uniq=a=>a.filter((x,i)=>a.indexOf(x)==i);
const listJoin=a=>a.length<2?(a[0]||''):a.slice(0,-1).join(', ')+' and '+a[a.length-1];
const cap=s=>s.charAt(0).toUpperCase()+s.slice(1);
function curBoot(){return S&&S.boot?S.boot:EV.reduce((m,e)=>Math.max(m,e.boot),0)}
function ageSec(e){if(!S)return null;if(e.st==1&&S.rtc==1)return Math.max(0,S.time-e.t);if(e.boot==curBoot())return Math.max(0,S.uptime-e.up);return null}
function clock(d){return d.toLocaleTimeString([],{hour:'numeric',minute:'2-digit'})}
function when(e){const a=ageSec(e);if(a==null)return'before the door last restarted';if(a<60)return'just now';
 if(a<3600){const m=Math.round(a/60);return m+(m==1?' minute':' minutes')+' ago'}
 if(e.st==1&&S.rtc==1){const d=new Date(e.t*1000),n=new Date(S.time*1000),y=new Date(n);y.setDate(n.getDate()-1);
  if(d.toDateString()==n.toDateString())return'today at '+clock(d);if(d.toDateString()==y.toDateString())return'yesterday at '+clock(d);
  return d.toLocaleDateString([],{weekday:'long',day:'numeric',month:'long'})+' at '+clock(d)}
 const h=Math.round(a/3600);return h<48?h+(h==1?' hour':' hours')+' ago':Math.round(h/24)+' days ago'}
function isToday(e){const a=ageSec(e);if(a==null)return false;if(e.st==1&&S.rtc==1)return new Date(e.t*1000).toDateString()==new Date(S.time*1000).toDateString();return a<86400}
const scope=()=>S&&S.rtc==1?'today':'over the last day';
function dayGroup(e){const a=ageSec(e);if(a==null)return'Before the last restart';
 if(e.st==1&&S.rtc==1){const d=new Date(e.t*1000),n=new Date(S.time*1000),y=new Date(n);y.setDate(n.getDate()-1);
  if(d.toDateString()==n.toDateString())return'Today';if(d.toDateString()==y.toDateString())return'Yesterday';return d.toLocaleDateString([],{weekday:'long',day:'numeric',month:'long'})}
 return a<86400?'Last 24 hours':'Earlier'}
// k: in | lock | warn | bad | info. s: shown in the simple view.
function describe(e){const w=e.slot>=0?who(e.slot):'Someone';
 switch(e.ev){
 case'GRANTED':return{k:'in',s:1,t:`${w} came in`+(e.d==1?' using the phone PIN':'')};
 case'DENIED_PIN':return{k:'warn',s:1,t:`Someone with ${w} typed the wrong PIN`+(e.d==1?' on a phone':'')};
 case'DENIED_CARD':return{k:'warn',s:1,t:"A card I don't know was tried at the door"};
 case'DENIED_NO_PIN':return{k:'info',s:1,t:e.d==1?'A card was tapped for a phone PIN, but none was waiting':'Someone tried to come in before a PIN was set up'};
 case'LOCKOUT':return{k:'warn',s:1,t:`After several wrong PINs, the keypad took a ${e.a1}-second break`};
 case'LOCK_REQUEST':return e.d?{k:'lock',s:1,t:`${w} locked the door`}:{k:'info',s:1,t:`${w} tried to lock the door, but it was still open`};
 case'LOCK_DENIED_CARD':return{k:'warn',s:1,t:"A card I don't know tried to lock the door"};
 case'LOCKED':return{k:'lock',s:0,t:'The bolt closed'};
 case'UNLOCKED':return{k:'info',s:0,t:'The bolt opened'};
 case'LATCH_RELEASED':return{k:'info',s:0,t:e.d?'The latch let go after the door opened':'Nobody pushed the door, so the latch let go'};
 case'DOOR_OPEN':return{k:'info',s:1,t:'The door was opened'};
 case'DOOR_CLOSED':return{k:'info',s:0,t:'The door was closed'};
 case'ALARM':return{k:'bad',s:1,t:'The alarm went off because '+(ALARMS[ALARM_BY_D[e.d]]||'something unusual happened')};
 case'ALARM_CLEARED':return{k:'info',s:1,t:'The alarm was switched off'};
 case'FAULT':return{k:'bad',s:1,t:'The lock got stuck while it was moving'};
 case'CARD_ADDED':return{k:'info',s:1,t:`A new card was added (${w})`};
 case'CARD_REMOVED':return{k:'info',s:1,t:`Card ${e.slot+1} was removed`};
 case'PIN_CHANGED':return{k:'info',s:1,t:'The door PIN was changed'};
 case'WIPE':return{k:'warn',s:1,t:'All cards and the PIN were cleared'};
 case'TIME_SET':return{k:'info',s:0,t:'The clock was set'};
 case'CLOCK_JUMP':case'CLOCK_BACKWARDS':return{k:'warn',s:1,t:'The clock changed unexpectedly'};
 case'TIME_INVALID':return{k:'warn',s:1,t:'The clock lost power and needs setting'};
 case'LOG_ROLLBACK':return{k:'bad',s:1,t:'Part of the history may have been tampered with'};
 case'LOG_REPAIRED':return{k:'info',s:0,t:'The history was tidied up after a power cut'};
 case'LOCKOUT_RESET':return{k:'info',s:0,t:'The keypad break was ended early'};
 case'BOOT':return{k:'info',s:0,t:'The door system started up'};
 case'WEB_LOGIN':return{k:'info',s:0,t:'Someone signed in to this app'};
 case'WEB_LOGIN_FAIL':return{k:'warn',s:1,t:'Someone typed the wrong app password'};
 case'PHONE_PIN':return{k:'info',s:0,t:'A PIN was sent from a phone'};
 case'WEB_ACTION':return{k:e.d==1?'lock':'info',s:e.d==1?1:0,t:(WA_TXT[e.d]||'A change was made')+' from the app'+(e.a1?'':", but it didn't work")}}
 return{k:'info',s:0,t:e.ev}}
const KI={in:'in',lock:'lock',warn:'alert',bad:'alert',info:'dot'};
function situation(){if(!S)return{tone:'info',icon:'dot',title:'Connecting…',line:"I'm checking on your door."};
 if(S.alarm!='NONE')return{tone:'bad',icon:'alert',title:'Alarm',line:`The alarm went off because ${ALARMS[S.alarm]||'something unusual happened'}.`};
 if(S.lock=='FAULT')return{tone:'bad',icon:'alert',title:'Lock needs a look',line:'The lock got stuck while moving. Please check nothing is blocking the bolt, then try locking again.'};
 const m={LOCKED:['ok','lock','Locked','Your door is locked and safe.'],
  UNLOCKED:['warn','unlock','Not locked',S.door=='open'?'Your door is open and not locked.':'Your door is closed but not locked.'],
  HOLDING:['info','unlock','Ready to open','The door is unlocked and ready to be pushed open.'],
  UNLOCKING:['info','unlock','Unlocking…','The lock is opening for someone.'],RELEASING:['info','unlock','Unlocked','The lock is settling after the door opened.'],
  LOCKING:['info','lock','Locking…','The lock is closing now.'],
  UNKNOWN:['warn','unlock','Not sure',"I'm not sure whether the door is locked. Locking it once will sort that out."]}[S.lock]||['info','dot',S.lock,''];
 return{tone:m[0],icon:m[1],title:m[2],line:m[3]}}
function greeting(){const h=new Date().getHours();return h>=5&&h<12?'Good morning':h>=12&&h<18?'Good afternoon':'Good evening'}
function summary(){if(!S)return'Let me check on your door…';const p=[situation().line];
 if(!S.link)p.push("I can't reach the keypad outside the door. It may have lost power.");
 if(S.lockout)p.push(`Someone typed the wrong PIN a few times, so the keypad is taking a short break (${S.lockout} seconds). That's normal and keeps you safe.`);
 const ins=EV.filter(e=>e.ev=='GRANTED'&&isToday(e));
 if(ins.length){const ppl=uniq(ins.map(e=>who(e.slot))),l=ins[ins.length-1];p.push(`${listJoin(ppl.slice(0,3))}${ppl.length>3?' and others':''} came in ${scope()}. The last time was ${l.slot>=0?who(l.slot):'someone'}, ${when(l)}.`)}
 else p.push(`Nobody has come in ${scope()}.`);
 const w=worrying();if(w.length)p.push(`${w.length==1?'One thing':w.length+' things'} might be worth a look. Just ask me "Anything unusual?"`);
 else if(S.alarm=='NONE'&&S.lock!='FAULT')p.push('Everything else looks calm.');
 return p.join(' ')}
function worrying(){return EV.filter(e=>{const k=describe(e).k;return(k=='warn'||k=='bad')&&isToday(e)})}

/* -------------------------------------------------------------- render */
function render(){if(!S)return;renderHome();if(tab=='activity')renderFeed();if(tab=='people')renderPeople();if(tab=='settings')renderSettings();
 $('sideFoot').innerHTML=`<span class="dot ${S.link?'d-ok':'d-bad'}"></span>${S.link?'Keypad outside connected':'Keypad outside not answering'}`}
function feedItems(l){return l.map(e=>{const d=describe(e);return`<li><span class="fi k-${d.k}">${ic(KI[d.k])}</span><div class="grow"><div>${esc(d.t)}</div><div class="when">${esc(cap(when(e)))}</div></div></li>`}).join('')}
function alerts(){const a=[];
 if(S.alarm!='NONE')a.push({tone:'bad',icon:'alert',title:'The alarm is on',text:"If this wasn't you or someone you know, please call someone you trust. When everything is fine, you can switch it off.",btn:['Switch off the alarm','confirmClear()']});
 if(!S.link)a.push({tone:'warn',icon:'alert',title:'Keypad outside not answering',text:'It may have lost power or its batteries may be flat. Your door stays locked.'});
 if(S.lockout)a.push({tone:'info',icon:'dot',title:'The keypad is taking a short break',text:`After several wrong PINs it will be ready again in ${S.lockout} seconds.`,btn:['End the break now',"act('resetlockout')"]});
 if(S.phonePin)a.push({tone:'ok',icon:'phone',title:'Your phone PIN is ready',text:`Tap your card at the door and press # (${S.phonePin} seconds left).`});
 if(S.adminMode!='none')a.push({tone:'info',icon:'people',title:S.adminMode=='enroll'?'Adding a card':'Changing the PIN',text:adminSteps(),btn:['Cancel',"act('cancel')"]});
 if(!S.pinSet&&S.adminMode=='none')a.push({tone:'warn',icon:'people',title:'No door PIN yet',text:'Add the first card to choose the PIN everyone will use.',btn:['Add a card','addCard()']});
 if(S.rtc==2)a.push({tone:'warn',icon:'dot',title:'The clock needs setting',text:'It lost power, so times may be wrong.',btn:['Set it from this device','syncTime()']});
 return a}
function renderHome(){const s=situation();$('hHello').textContent=greeting();
 $('hDate').textContent=new Date().toLocaleDateString([],{weekday:'long',day:'numeric',month:'long'});
 $('hero').className='hero tone-'+s.tone;$('heroIc').innerHTML=ic(s.icon);$('heroT').textContent=s.title;$('heroS').textContent=s.line;
 $('sumText').textContent=summary();
 const locked=S.lock=='LOCKED',moving=S.lock=='LOCKING';$('bLock').disabled=locked||moving;
 $('bLock').innerHTML=ic('lock')+(locked?'Locked':moving?'Locking…':'Lock the door');
 $('alerts').innerHTML=alerts().map(a=>`<div class="alert tone-${a.tone}">${ic(a.icon)}<div class="grow"><b>${esc(a.title)}</b><p>${esc(a.text)}</p>${a.btn?`<button class="btn sm" onclick="${a.btn[1]}">${esc(a.btn[0])}</button>`:''}</div></div>`).join('');
 $('recent').innerHTML=feedItems(EV.filter(e=>describe(e).s).slice(-5).reverse())||'<li class="empty">Nothing yet. Visits will show up here.</li>';
 const wrong=EV.filter(e=>(e.ev=='DENIED_PIN'||e.ev=='DENIED_CARD')&&isToday(e)).length;
 $('glance').innerHTML=[['Door',S.door=='closed'?'Closed':'Open',S.door=='closed'?'ok':'warn'],['Keypad outside',S.link?'Connected':'Not answering',S.link?'ok':'bad'],
  ['Wrong tries '+scope(),wrong?String(wrong):'None',wrong?'warn':'ok'],['Cards',String(S.cards),S.cards?'ok':'warn']]
  .map(([l,v,t])=>`<div class="gl"><span>${esc(l)}</span><b><span class="dot d-${t}"></span>${esc(v)}</b></div>`).join('')}
function renderFeed(){const tech=$('tech').checked;
 const l=EV.slice().reverse().filter(e=>{const d=describe(e);if(!tech&&!d.s)return false;
  if(filter=='in')return e.ev=='GRANTED'||e.ev=='DOOR_OPEN'||(e.ev=='LOCK_REQUEST'&&e.d)||(e.ev=='WEB_ACTION'&&e.d==1);
  if(filter=='warn')return d.k=='warn'||d.k=='bad';return true});
 if(!l.length){$('feedAll').innerHTML=`<div class="card empty">${filter=='warn'?'Nothing to worry about. All calm.':'Nothing to show yet.'}</div>`;return}
 let h='',g=null;for(const e of l){const gg=dayGroup(e);if(gg!==g){if(g!==null)h+='</ul></div>';g=gg;h+=`<div class="card"><h2 class="grp">${esc(gg)}</h2><ul class="feed">`}h+=feedItems([e])}
 $('feedAll').innerHTML=h+'</ul></div>'}
const initials=n=>/^Card \d+$/.test(n)?n.slice(5):n.split(/\s+/).map(w=>w[0]||'').join('').slice(0,2).toUpperCase();
function adminSteps(){if(!S)return'';if(S.adminMode=='enroll')return S.pinSet?'Go to the door, tap the new card, type the door PIN and press #.':
 'Go to the door, tap the card, choose a PIN (4 to 8 numbers) and press #. Then tap the same card and type the same PIN once more.';
 return'Go to the door, tap a card that already works, type the new PIN and press #. Then do the same once more to confirm.'}
function renderPeople(){$('modeBanner').innerHTML=S.adminMode=='none'?'':`<div class="alert tone-info">${ic('people')}<div class="grow"><b>${S.adminMode=='enroll'?'Adding a card':'Changing the PIN'}</b><p>${esc(adminSteps())}</p><button class="btn sm" onclick="act('cancel')">Cancel</button></div></div>`;
 $('people').innerHTML=CARDS.length?CARDS.map(c=>`<li><span class="av">${esc(initials(who(c.slot)))}</span><div class="grow"><b>${esc(who(c.slot))}</b><div class="mute small">${c.name?'Card '+(c.slot+1):'No name yet'}</div></div><button class="btn sm ghost" onclick="rename(${c.slot})">Rename</button><button class="btn sm ghost danger" onclick="removeCard(${c.slot})">Remove</button></li>`).join(''):
  '<li class="empty">No cards yet. Add the first one below. It also sets the door PIN.</li>'}
function renderSettings(){$('alarmTxt').textContent=S.alarm=='NONE'?'No alarm right now':'On: '+(ALARMS[S.alarm]||S.alarm);$('clearBtn').disabled=S.alarm=='NONE';
 $('lockoutTxt').textContent=S.lockout?`Taking a break for ${S.lockout} more seconds`:(S.fails?`${S.fails} wrong ${S.fails==1?'try':'tries'} in a row`:'Ready');
 $('lockoutBtn').disabled=!S.lockout&&!S.fails;
 $('clockTxt').textContent=S.rtc==1?'Shows '+new Date(S.time*1000).toLocaleString():S.rtc==2?'Lost power, please set it':S.rtc==0?'No clock fitted, so times show as "minutes ago"':'Not readable';
 $('techDl').innerHTML=[['Lock state',S.lock],['Bolt position',S.position.toFixed(2)+' turns'],['Locked switch (SW1)',S.sw1?'pressed':'released'],['Latch switch (SW2)',S.sw2?'pressed':'released'],
  ['Motor',S.motor],['Armed',S.disarmed?'disarmed':S.armed?'yes':'no'],['Vibration / motion',(S.vibration?'active':'quiet')+' / '+(S.motion?'active':'quiet')],
  ['Exterior link',S.link?'up, '+S.lastRx+' s ago':'down'],['Log',S.logCount+' records, head #'+S.logHead],['Boot',S.boot],['Uptime',Math.floor(S.uptime/60)+' min'],['Firmware',S.build]]
  .map(([k,v])=>`<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`).join('')}

/* ------------------------------------------------------------- actions */
async function act(a,extra){const j=await post('/api/action',Object.assign({a},extra||{}));toast(friendly(j.msg));await refresh(true);return j}
function sheet(title,body,btns){$('shT').textContent=title;$('shB').innerHTML=body;const b=$('shBtns');b.innerHTML='';
 for(const[l,cls,fn]of btns){const x=document.createElement('button');x.className='btn big '+(cls||'');x.textContent=l;x.onclick=async()=>{if(fn&&(await fn())===false)return;closeSheet()};b.appendChild(x)}
 $('sheet').classList.remove('hide');const f=$('shB').querySelector('input')||b.firstChild;setTimeout(()=>f&&f.focus(),60)}
function closeSheet(){$('sheet').classList.add('hide')}
document.addEventListener('keydown',e=>{if(e.key=='Escape')closeSheet()});
function confirmLock(){if(!S)return;if(S.door=='open'){toast('Please close the door first, then I can lock it.');return}
 sheet('Lock the door?','<p class="mute">The bolt will turn and lock your door. It takes a few seconds.</p>',[['Yes, lock it','pri',()=>{act('lock')}],['Not now','ghost']])}
function confirmClear(){sheet('Switch off the alarm?','<p class="mute">Only do this if you know why it went off and everything is fine.</p>',[['Yes, switch it off','pri',()=>{act('clear')}],['Not now','ghost']])}
function addCard(){sheet('Add a card',`<p class="mute">${S&&S.pinSet?'Once you press Start, go to the door, tap the new card, type the door PIN and press #. You have two minutes.':'This first card also sets the door PIN. Once you press Start, go to the door, tap the card, choose a PIN (4 to 8 numbers) and press #, then do it once more.'}</p>`,[['Start','pri',()=>{act('enroll')}],['Not now','ghost']])}
function changePin(){if(!S||!S.cards){toast('Add a card first.');return}
 sheet('Change the door PIN','<p class="mute">The new PIN will work for every card. Once you press Start, go to the door, tap a card that already works, type the new PIN and press #, then do it once more.</p>',[['Start','pri',()=>{act('setpin')}],['Not now','ghost']])}
function rename(slot){sheet('Name this card',`<label for="nm">Name (for example "Grandma")</label><input type="text" id="nm" maxlength="20" value="${esc(NAMES[slot]||'')}">`,
 [['Save','pri',async()=>{const n=$('nm').value.trim();if(n&&!/^[A-Za-z0-9 '.\-]{1,20}$/.test(n)){toast('Please use letters, numbers and spaces only.');return false}await act('name',{slot,name:n})}],['Cancel','ghost']])}
function removeCard(slot){sheet('Remove '+who(slot)+'?','<p class="mute">This card will no longer open the door.</p>',[['Yes, remove it','pri danger',()=>{act('delete',{slot})}],['Keep it','ghost']])}
function syncTime(){act('time',{t:Math.floor(Date.now()/1000)})}
function chpw(){sheet('Change the app password','<label for="o1">Current password</label><input type="password" id="o1" autocomplete="current-password"><label for="o2">New password (8 or more characters)</label><input type="password" id="o2" autocomplete="new-password">',
 [['Change password','pri',async()=>{if($('o2').value.length<8){toast('The new password needs at least 8 characters.');return false}await act('password',{old:$('o1').value,new:$('o2').value})}],['Cancel','ghost']])}
async function verify(){$('vres').textContent='Checking every entry…';let j;try{j=await get('/api/verify')}catch(e){$('vres').textContent="I couldn't reach the door to check.";return}
 lastVerify=j.ok;$('vres').textContent=j.ok?`All ${j.checked} entries are genuine. Nothing has been changed.`:`Entry ${j.bad} doesn't match its seal. Please ask someone you trust to take a look.`}

/* --------------------------------------------------------------- Hazel */
function speak(t){if(!('speechSynthesis' in window)){toast("Reading aloud isn't available on this device.");return}
 speechSynthesis.cancel();const u=new SpeechSynthesisUtterance(String(t).replace(/[•…]/g,' '));u.rate=.93;u.lang=navigator.language||'en-GB';speechSynthesis.speak(u)}
function addMsg(from,text,acts){const m=document.createElement('div');m.className='msg '+from;
 m.innerHTML=(from=='bot'?'<span class="avatar sm" aria-hidden="true">H</span>':'')+`<div class="bubble">${esc(text).replace(/\n/g,'<br>')}${from=='bot'?`<button class="say-btn">${ic('speak')} Read aloud</button>`:''}</div>`;
 if(from=='bot')m.querySelector('.say-btn').onclick=()=>speak(text);
 if(acts&&acts.length){const r=document.createElement('div');r.className='msg-acts';for(const[l,fn]of acts){const b=document.createElement('button');b.className='btn sm';b.textContent=l;b.onclick=()=>{r.remove();fn()};r.appendChild(b)}m.appendChild(r)}
 $('msgs').appendChild(m);if(m.scrollIntoView)m.scrollIntoView({behavior:'smooth',block:'end'});if(from=='bot'&&store.get('speak',false))speak(text)}
function greetChat(){addMsg('bot',`${greeting()}! I'm ${HELPER}, your door helper. ${summary()}\nTap a question below, or type your own.`)}
function ask(ev){if(ev)ev.preventDefault();const q=$('q').value.trim();if(!q)return;$('q').value='';reply(q)}
function reply(q){addMsg('me',q);setTimeout(()=>{const r=answer(q);addMsg('bot',r.t,r.a)},300)}
const CHIPS=['Is my door locked?','Who came in today?','Anything unusual?','Is everything working?','How do I lock the door?','How do I use my phone?'];
const chat=t=>addMsg('bot',t);
async function lockFromChat(){const j=await post('/api/action',{a:'lock'});chat(j.ok?"Done! I've asked the lock to close. It takes a few seconds.":"I couldn't lock it: "+friendly(j.msg));refresh(true)}
async function verifyChat(){chat('Checking every entry now, one moment…');let j;try{j=await get('/api/verify')}catch(e){chat("I couldn't reach the door to check.");return}
 lastVerify=j.ok;chat(j.ok?`All ${j.checked} entries are genuine. Nobody has changed your history.`:`Something isn't right: entry ${j.bad} doesn't match its seal. Please ask someone you trust to take a look.`)}
function lockStatus(){let t=situation().line;
 if(S.lock!='LOCKED'&&S.lock!='LOCKING'&&S.door=='closed'&&S.alarm=='NONE')return{t:t+' Would you like me to lock it?',a:[['Yes, lock it',lockFromChat],['No, thanks',()=>chat('Okay! Just ask if you change your mind.')]]};
 if(S.door=='open'&&S.lock!='LOCKED')t+=' Once the door is closed, I can lock it for you.';
 const l=lastOf(e=>(e.ev=='LOCK_REQUEST'&&e.d)||(e.ev=='WEB_ACTION'&&e.d==1&&e.a1));
 if(S.lock=='LOCKED'&&l)t+=` It was locked ${when(l)}${e2who(l)}.`;return{t}}
const e2who=e=>e.slot>=0?' by '+who(e.slot):(e.ev=='WEB_ACTION'?' from this app':'');
function lockAction(){if(S.lock=='LOCKED')return{t:'Good news, your door is already locked.'};
 if(S.door=='open')return{t:"The door is open at the moment. Please close it first, then I'll lock it for you."};
 return{t:'Shall I lock the door now?',a:[['Yes, lock it',lockFromChat],['No, thanks',()=>chat('No problem.')]]}}
function whoAnswer(){const ins=EV.filter(e=>e.ev=='GRANTED'&&isToday(e));
 if(!ins.length){const l=lastOf(e=>e.ev=='GRANTED');return{t:`Nobody has come in ${scope()}.`+(l?` The last visit was ${who(l.slot)}, ${when(l)}.`:'')}}
 const ppl=uniq(ins.map(e=>who(e.slot))),l=ins[ins.length-1];
 return{t:`${listJoin(ppl)} came in ${scope()}${ins.length>ppl.length?` (${ins.length} visits in all)`:''}. The most recent was ${who(l.slot)}, ${when(l)}.`+(Object.keys(NAMES).length?'':'\nTip: give the cards names in People, so I can tell you who they belong to.')}}
function unusualAnswer(){const w=worrying(),now=[];if(S.alarm!='NONE')now.push(situation().line);if(!S.link)now.push("Right now I can't reach the keypad outside the door.");
 if(S.lock=='FAULT')now.push('The lock got stuck the last time it moved.');
 if(!w.length&&!now.length)return{t:`Good news: nothing unusual ${scope()}. Everything has been calm.`};
 let t=now.join(' ');if(w.length){t+=(t?'\n\n':'')+`Here's what I noticed ${scope()}:\n`+w.slice(-4).reverse().map(e=>'• '+describe(e).t+' ('+when(e)+')').join('\n');if(w.length>4)t+=`\n…and ${w.length-4} more in Activity.`}
 t+="\n\nIf any of this wasn't you or someone you know, it's a good idea to tell someone you trust.";
 return{t,a:[['Show me',()=>{filter='warn';setSeg();go('activity')}]]}}
function healthAnswer(){const ok=[],bad=[];
 S.link?ok.push('the keypad outside is connected'):bad.push("I can't reach the keypad outside, it may need power");
 S.lock=='FAULT'?bad.push('the lock got stuck the last time it moved'):ok.push('the lock is moving normally');
 if(S.rtc==1)ok.push('the clock is right');else if(S.rtc==2)bad.push('the clock lost power and needs setting');
 if(!S.pinSet)bad.push('no door PIN is set up yet');
 let t=bad.length?`Mostly fine, but ${listJoin(bad)}.`+(ok.length?` The good news: ${listJoin(ok)}.`:''):`Everything is working nicely: ${listJoin(ok)}.`;
 if(lastVerify===true)t+=' I also checked your history, and nothing has been changed.';if(lastVerify===false)t+=' Your history check found a problem, please have someone look at it.';
 return{t,a:lastVerify==null?[['Check the history too',verifyChat]]:[]}}
function cardsAnswer(){const n=CARDS.length;return n?{t:`There ${n==1?'is 1 card':'are '+n+' cards'}: ${listJoin(CARDS.map(c=>who(c.slot)))}. You can give cards names in People, so I can tell you who came in.`,a:[['Go to People',()=>go('people')]]}
 :{t:'There are no cards yet. Go to People and press "Add a card" to set one up.',a:[['Go to People',()=>go('people')]]}}
const INTENTS=[
 [/\b(hi|hello|hey|good (morning|afternoon|evening))\b/i,()=>({t:`Hello! ${situation().line} What would you like to know?`})],
 [/\bthank/i,()=>({t:"You're very welcome! I'm right here whenever you need me."})],
 [/\bhow\b.*\b(lock|close)/i,()=>({t:'To lock the door, close it, tap your card on the reader outside and press the * key straight away. Or press "Lock the door" in this app.'})],
 [/\bunlock|\bopen (the |my )?door|let me in/i,()=>({t:"I can't open the door from here, and that's on purpose to keep you safe. To come in, tap your card at the door, type your PIN and press #. If you sent your PIN from this phone, just tap your card and press #."})],
 [/\block (the |my |it|up)|please lock|can you lock/i,lockAction],
 [/\block|safe|secure|closed/i,lockStatus],
 [/\bcards?\b|\bkeys?\b|who has/i,cardsAnswer],
 [/\bwho\b|came|come in|visit|anyone|anybody|someone|arriv/i,whoAnswer],
 [/unusual|wrong|problem|alarm|strange|worr|alert|tamper|break|suspicious/i,unusualAnswer],
 [/work|okay|\bok\b|fine|broken|batter|connect|signal|health/i,healthAnswer],
 [/guest|visitor|carer|family|friend|let .* in/i,()=>({t:'Everyone who comes in needs their own card and the door PIN. To give someone a card, go to People and press "Add a card". For a one-off visit, someone who already has a card can let them in.',a:[['Go to People',()=>go('people')]]})],
 [/phone|\bpin\b|code/i,()=>({t:'You can use this phone instead of the keypad outside. Press "Use phone as keypad", type your PIN and press Send. Then, within one minute, tap your card at the door and press # without typing any numbers. The PIN only works once.',a:[['Open the phone keypad',showPin]]})],
 [/help|what can you|how do you/i,()=>({t:"I can tell you whether your door is locked, who came in, whether anything unusual happened, and whether everything is working. I can also lock the door for you. I can't unlock it, that always needs a card at the door."})]];
function answer(q){if(!S)return{t:"I'm still getting in touch with your door. Give me a moment and ask again."};
 for(const[re,fn]of INTENTS)if(re.test(q))return fn(q);
 return{t:'Sorry, I didn\'t quite understand that. You can ask me things like "Is my door locked?" or "Who came in today?"',a:[['Is my door locked?',()=>reply('Is my door locked?')],['Who came in today?',()=>reply('Who came in today?')]]}}

/* ----------------------------------------------------------- phone PIN */
function showPin(){pin='';drawPin();view('vPin');clearInterval(timer);pubPoll();timer=setInterval(pubPoll,1500)}
function drawPin(){$('dots').innerHTML=Array.from({length:Math.max(4,pin.length)},(_,i)=>`<i class="${i<pin.length?'on':''}"></i>`).join('')}
async function sendPin(){if(pin.length<4){toast('Your PIN has 4 to 8 numbers.');return}const j=await post('/api/phonepin',{pin});pin='';drawPin();toast(j.ok?'Done! Now tap your card at the door and press #.':friendly(j.msg));pubPoll()}
async function pubPoll(){let s;try{s=await (await fetch('/api/public',{cache:'no-store'})).json()}catch(e){$('loginStatus').textContent="Can't reach the door. Please join the ATDS-Door WiFi.";return}
 const L={LOCKED:'Your door is locked.',UNLOCKED:'Your door is not locked.',HOLDING:'Your door is ready to open.',FAULT:'The lock needs attention.',UNKNOWN:''}[s.lock]||'';
 $('loginStatus').textContent=(s.link?'':"The keypad outside isn't answering. ")+L;
 $('pinMsg').innerHTML=s.lockout?`The keypad is taking a short break. Please try again in ${s.lockout} seconds.`:
  s.phonePin?`<b style="color:var(--ok)">Your PIN is ready.</b> Tap your card at the door and press <b>#</b> without typing numbers (${s.phonePin} seconds left).`:
  'Type your door PIN, then press Send. Afterwards, tap your card at the door and press <b>#</b>.'}

/* ---------------------------------------------------------------- init */
function setSeg(){document.querySelectorAll('#fil button').forEach(b=>b.classList.toggle('on',b.dataset.f==filter))}
function init(){buildNav();$('logo1').innerHTML=ic('lock');$('logo2').innerHTML=ic('lock');$('hSpeak').innerHTML=ic('speak');$('readBtn').innerHTML=ic('speak')+'Read aloud';
 $('usePhone1').innerHTML=ic('phone')+'Use my phone as the keypad';$('usePhone2').innerHTML=ic('phone')+'Use phone as keypad';$('backBtn').innerHTML=ic('back')+'Back';
 $('sendBtn').innerHTML=ic('send');$('shieldIc').innerHTML=ic('shield');
 document.querySelectorAll('#fil button').forEach(b=>b.onclick=()=>{filter=b.dataset.f;setSeg();renderFeed()});
 document.querySelectorAll('#sizeSeg button').forEach(b=>b.onclick=()=>setPref('size',b.dataset.v));
 document.querySelectorAll('#themeSeg button').forEach(b=>b.onclick=()=>setPref('theme',b.dataset.v));
 $('chips').innerHTML='';for(const c of CHIPS){const b=document.createElement('button');b.className='chip';b.textContent=c;b.onclick=()=>reply(c);$('chips').appendChild(b)}
 const pad=$('pad');for(const k of['1','2','3','4','5','6','7','8','9','C','0','⌫']){const b=document.createElement('button');b.textContent=k;
  b.setAttribute('aria-label',k=='C'?'Clear':k=='⌫'?'Delete':k);b.onclick=()=>{if(k=='⌫')pin=pin.slice(0,-1);else if(k=='C')pin='';else if(pin.length<8)pin+=k;drawPin()};pad.appendChild(b)}
 if(window.matchMedia){const mq=matchMedia('(prefers-color-scheme: dark)');if(mq.addEventListener)mq.addEventListener('change',applyPrefs)}
 applyPrefs();drawPin();route()}
init();
</script></body></html>)HTML";

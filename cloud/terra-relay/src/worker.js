// Terra relay — the plant pushes status here every few seconds and gets any
// pending owner command back in the same response. The owner opens the dashboard
// from anywhere with ?k=<OWNER_TOKEN>. No inbound connection to the device needed,
// so it works behind any WiFi / phone hotspot / NAT.

const CORS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Headers': '*',
  'Access-Control-Allow-Methods': 'GET,POST,OPTIONS',
};
const J = (o, status = 200) =>
  new Response(JSON.stringify(o), { status, headers: { ...CORS, 'Content-Type': 'application/json' } });

// merge "k=v k=v" command strings; later value wins per key
function mergeCmd(prev, cur) {
  const m = {};
  for (const part of ((prev || '') + ' ' + (cur || '')).trim().split(/\s+/)) {
    const i = part.indexOf('=');
    if (i > 0) m[part.slice(0, i)] = part.slice(i + 1);
  }
  return Object.entries(m).map(([k, v]) => k + '=' + v).join(' ');
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const p = url.pathname;
    const id = url.searchParams.get('id') || 'terra';
    if (req.method === 'OPTIONS') return new Response(null, { headers: CORS });

    // --- device pushes status, receives any pending command in the response ---
    if (p === '/ingest' && req.method === 'POST') {
      if (req.headers.get('X-Terra-Key') !== env.DEVICE_KEY) return new Response('unauthorized', { status: 401 });
      const body = await req.text();
      await env.TERRA.put('status:' + id, JSON.stringify({ t: Date.now(), data: body }), { expirationTtl: 86400 });
      const cmd = await env.TERRA.get('cmd:' + id);
      if (cmd) await env.TERRA.delete('cmd:' + id);
      return J({ cmd: cmd || '' });
    }

    // --- owner dashboard: latest status (+ online/age) ---
    if (p === '/api') {
      if (url.searchParams.get('k') !== env.OWNER_TOKEN) return J({ error: 'unauthorized' }, 401);
      const s = await env.TERRA.get('status:' + id);
      if (!s) return J({ online: false, age_s: null });
      const o = JSON.parse(s);
      const age = Math.floor((Date.now() - o.t) / 1000);
      let data = {};
      try { data = JSON.parse(o.data); } catch (e) {}
      return J({ online: age < 30, age_s: age, ...data });
    }

    // --- owner sends a command (queued for the device's next push) ---
    if (p === '/cmd' && req.method === 'POST') {
      if (url.searchParams.get('k') !== env.OWNER_TOKEN) return J({ error: 'unauthorized' }, 401);
      const c = await req.text();
      const prev = await env.TERRA.get('cmd:' + id);
      await env.TERRA.put('cmd:' + id, mergeCmd(prev, c), { expirationTtl: 600 });
      return J({ ok: true });
    }

    // --- the dashboard page ---
    if (p === '/' || p === '/d') return new Response(DASH, { headers: { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' } });

    return new Response('Terra relay is running 🌵', { status: 200 });
  },
};

const DASH = `<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Terra 🌵</title>
<style>
  :root{--sand:#efe6d6;--sand2:#e5d8c3;--ink:#3b2f24;--muted:#8a7a64;--terra:#c06a44;
        --sage:#7f9a72;--card:#faf5ec;--line:#e0d3bd;--good:#6f9a5e;--warn:#c9973f;--bad:#c05a48;}
  *{box-sizing:border-box}
  body{margin:0;font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
       background:radial-gradient(120% 100% at 50% -10%,#f6efe1,#e9dcc6);color:var(--ink);
       min-height:100vh;padding:22px 16px 60px}
  .wrap{max-width:520px;margin:0 auto}
  header{display:flex;align-items:center;gap:12px;margin-bottom:4px}
  header .logo{font-size:34px}
  h1{font-size:26px;margin:0;letter-spacing:.5px}
  .status{display:flex;align-items:center;gap:7px;color:var(--muted);font-size:14px;margin:2px 0 20px}
  .dot{width:9px;height:9px;border-radius:50%;background:#bbb;box-shadow:0 0 0 3px rgba(0,0,0,.04)}
  .dot.on{background:var(--good)}.dot.off{background:var(--bad)}
  .screen{width:min(240px,66vw);height:min(240px,66vw);margin:8px auto 2px;border-radius:50%;
        overflow:hidden;background:#050a08;box-shadow:0 10px 34px rgba(0,0,0,.28),
        0 0 0 7px #2a2320,0 0 0 9px #40372f,0 0 0 10px #241d18}
  .screen canvas{display:block;width:100%;height:100%;border-radius:50%}
  .moodrow{text-align:center;margin:10px 0 20px;font-size:17px;font-weight:600;text-transform:capitalize}
  .moodrow .say{color:var(--muted);font-size:13px;font-weight:400;text-transform:none}
  .caption{max-width:320px;margin:14px auto 0;text-align:center;font-size:15px;line-height:1.4;
        color:var(--ink);background:var(--card);border:1px solid var(--line);border-radius:16px;
        padding:11px 18px;opacity:0;transform:translateY(-6px);transition:opacity .3s,transform .3s;
        pointer-events:none;position:relative}
  .caption::before{content:'';position:absolute;top:-7px;left:50%;margin-left:-7px;width:12px;height:12px;
        background:var(--card);border-left:1px solid var(--line);border-top:1px solid var(--line);transform:rotate(45deg)}
  .caption.show{opacity:1;transform:none}
  .wxhead{display:flex;align-items:center;gap:14px;padding:6px 2px 14px;border-bottom:1px solid var(--line)}
  .wxicon{font-size:38px;line-height:1}
  .wxcond{font-size:18px;font-weight:600}.wxsub{font-size:13px;color:var(--muted)}
  .wxgrid{display:grid;grid-template-columns:1fr 1fr;gap:0 20px;padding:10px 2px 2px}
  .wxgrid>div{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--line)}
  .wxgrid>div:last-child,.wxgrid>div:nth-last-child(2){border-bottom:none}
  .wxgrid span{color:var(--muted);font-size:14px}.wxgrid b{font-weight:600}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:16px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px 16px}
  .card .k{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.6px}
  .card .v{font-size:24px;font-weight:650;margin-top:3px}
  .card .v small{font-size:14px;font-weight:500;color:var(--muted)}
  .sect{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.8px;margin:22px 4px 10px}
  .panel{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:6px 16px}
  .row{display:flex;align-items:center;justify-content:space-between;padding:14px 0;border-bottom:1px solid var(--line)}
  .row:last-child{border-bottom:none}
  .row label{font-weight:550}
  input[type=range]{width:150px;accent-color:var(--terra)}
  .seg{display:flex;background:var(--sand2);border-radius:11px;padding:3px}
  .seg button{border:none;background:none;padding:7px 12px;border-radius:8px;font:inherit;font-size:13px;color:var(--muted);cursor:pointer}
  .seg button.on{background:#fff;color:var(--ink);font-weight:600;box-shadow:0 1px 3px rgba(0,0,0,.08)}
  .tog{width:52px;height:30px;border-radius:16px;background:#cdbfa6;position:relative;cursor:pointer;transition:.2s}
  .tog.on{background:var(--terra)}.tog b{position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:.2s}
  .tog.on b{left:25px}
  input[type=time]{font:inherit;border:1px solid var(--line);border-radius:9px;padding:6px 8px;background:#fff;color:var(--ink)}
  .foot{text-align:center;color:var(--muted);font-size:12px;margin-top:26px}
  .offline{opacity:.55;pointer-events:none}
  .wbtn{background:var(--terra);color:#fff;border:none;border-radius:10px;padding:9px 15px;font:inherit;font-size:14px;font-weight:600;cursor:pointer}
  .wmsg{color:var(--sage);font-size:13px;line-height:1.4;margin:10px 4px 0;min-height:1em}
</style></head><body><div class="wrap">
<header><div class="logo">🌵</div><h1>Terra</h1></header>
<div class="status"><span class="dot" id="dot"></span><span id="stat">connecting…</span></div>

<div class="screen"><canvas id="face" width="240" height="240"></canvas></div>
<div class="caption" id="cap"></div>
<div class="moodrow"><span class="m" id="mood">—</span> <span class="say" id="seen"></span></div>

<div class="sect">Her spot</div>
<div class="grid">
  <div class="card"><div class="k">Soil</div><div class="v" id="soil">–<small id="soilst"></small></div></div>
  <div class="card"><div class="k">Air</div><div class="v" id="air">–<small>°F</small></div></div>
  <div class="card"><div class="k">Humidity</div><div class="v" id="hum">–<small>%</small></div></div>
  <div class="card"><div class="k">Light</div><div class="v" id="light">–</div></div>
</div>

<div class="sect">Weather</div>
<div class="panel">
  <div class="wxhead"><span class="wxicon" id="wxicon">🌡️</span>
    <div><div class="wxcond" id="wxcond">—</div><div class="wxsub" id="wxsub">waiting for data…</div></div></div>
  <div class="wxgrid">
    <div><span>Feels like</span><b id="wxfeels">–</b></div>
    <div><span>Wind</span><b id="wxwind">–</b></div>
    <div><span>High / Low</span><b id="wxhilo">–</b></div>
    <div><span>Humidity</span><b id="wxhum">–</b></div>
    <div><span>UV index</span><b id="wxuv">–</b></div>
    <div><span>Rain today</span><b id="wxrain">–</b></div>
    <div><span>Sunrise</span><b id="wxsr">–</b></div>
    <div><span>Sunset</span><b id="wxss">–</b></div>
  </div>
</div>

<div class="sect">Controls</div>
<div class="panel" id="controls">
  <div class="row"><label>Volume</label><input type="range" id="vol" min="0" max="100" step="5"></div>
  <div class="row"><label>Mute</label><div class="tog" id="mute"><b></b></div></div>
  <div class="row"><label>Chattiness</label>
    <div class="seg" id="chat"><button data-v="0">Quiet</button><button data-v="1">Normal</button><button data-v="2">Chatty</button></div></div>
  <div class="row"><label>Quiet from</label><input type="time" id="qs"></div>
  <div class="row"><label>Quiet until</label><input type="time" id="qe"></div>
</div>

<div class="sect">Network</div>
<div class="panel">
  <div class="row"><label>WiFi setup</label><button class="wbtn" id="wifi">Change network…</button></div>
</div>
<div class="wmsg" id="wmsg"></div>

<div class="foot">Terra checks in every few seconds · access from anywhere 🌵</div>
</div>
<script>
const K=new URLSearchParams(location.search).get('k')||'';
const $=id=>document.getElementById(id);
const PI=Math.PI;
let D={}, busy=0, curEmo='neutral', netOffline=false;
const WMO={0:['Clear','☀️'],1:['Mostly clear','🌤️'],2:['Partly cloudy','⛅'],3:['Overcast','☁️'],
 45:['Fog','🌫️'],48:['Rime fog','🌫️'],51:['Light drizzle','🌦️'],53:['Drizzle','🌦️'],55:['Heavy drizzle','🌧️'],
 56:['Freezing drizzle','🌧️'],57:['Freezing drizzle','🌧️'],61:['Light rain','🌦️'],63:['Rain','🌧️'],65:['Heavy rain','🌧️'],
 66:['Freezing rain','🌧️'],67:['Freezing rain','🌧️'],71:['Light snow','🌨️'],73:['Snow','🌨️'],75:['Heavy snow','❄️'],
 77:['Snow grains','🌨️'],80:['Showers','🌦️'],81:['Showers','🌧️'],82:['Heavy showers','⛈️'],85:['Snow showers','🌨️'],
 86:['Snow showers','❄️'],95:['Thunderstorm','⛈️'],96:['Thunderstorm','⛈️'],99:['Thunderstorm','⛈️']};
const m2t=m=>String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0');
const t2m=s=>{const[a,b]=s.split(':').map(Number);return a*60+b;};
async function cmd(kv){ busy=Date.now();
  await fetch('/cmd?k='+K,{method:'POST',body:kv}).catch(()=>{}); }
const S=(id,html)=>{const e=$(id);if(e)e.innerHTML=html;};
const T=(id,t)=>{const e=$(id);if(e)e.textContent=t;};
const toF=v=>Math.round(v*9/5+32);          // °C -> °F
function paint(d){
  const on=!!d.online;
  curEmo=d.mood||'neutral'; netOffline=!on;  // drive the animated face
  const dot=$('dot'); if(dot)dot.className='dot '+(on?'on':'off');
  T('stat', on?'online':(d.age_s==null?'never connected':'offline'));
  const ctl=$('controls'); if(ctl)ctl.className='panel'+(on?'':' offline');
  T('seen', d.age_s==null?'· waiting for first check-in…':(on?('· '+d.age_s+'s ago'):('· last seen '+Math.floor(d.age_s/60)+'m ago')));
  T('mood', d.mood||'—');
  if(d.soil_pct!=null){S('soil',Math.round(d.soil_pct)+'<small>%</small>');T('soilst',' '+(d.soil_status||''));}
  if(d.temp_c!=null)  S('air', toF(d.temp_c)+'<small>°F</small>');
  if(d.humidity!=null)S('hum', Math.round(d.humidity)+'<small>%</small>');
  if(d.light!=null)   T('light', d.light?'☀️ bright':'🌙 dark');
  // speaking subtitle — only while she's recently talking
  const cap=$('cap');
  if(cap){ const talking = d.saying && d.say_age!=null && d.say_age<12;
    if(talking){ cap.textContent='“'+d.saying+'”'; cap.classList.add('show'); } else cap.classList.remove('show'); }
  // live weather detail
  if(d.wx_valid){
    const w=WMO[d.wx_code]||['Unknown','🌡️']; let ic=w[1];
    if((d.wx_code===0||d.wx_code===1)&&d.wx_isday===0)ic='🌙';
    T('wxicon',ic); T('wxcond',w[0]);
    T('wxsub', toF(d.out_temp_c)+'°F · '+(d.wx_isday?'daytime':'night'));
    T('wxfeels', toF(d.wx_feels_c)+'°F'); T('wxwind', Math.round(d.wx_wind)+' mph');
    T('wxhilo', toF(d.wx_hi_c)+'° / '+toF(d.wx_lo_c)+'°'); T('wxhum', Math.round(d.out_hum)+'%');
    T('wxuv', Number(d.uv||0).toFixed(1)); T('wxrain', Number(d.wx_precip||0).toFixed(2)+' in');
    T('wxsr', d.wx_sr||'–'); T('wxss', d.wx_ss||'–');
  } else { T('wxcond','Not available'); T('wxsub','needs internet'); }
  if(Date.now()-busy>2500){                 // don't fight the user mid-drag
    const v=$('vol'); if(v&&d.volume!=null)v.value=Math.round(d.volume/4*100);
    const mu=$('mute'); if(mu)mu.className='tog'+(d.mute?' on':'');
    const ch=$('chat'); if(ch)[...ch.children].forEach(b=>b.classList.toggle('on',+b.dataset.v===d.chattiness));
    const a=$('qs'); if(a&&d.quietStart!=null)a.value=m2t(d.quietStart);
    const b=$('qe'); if(b&&d.quietEnd!=null)b.value=m2t(d.quietEnd);
  }
}
let fails=0;
async function poll(){
  try{
    const r=await fetch('/api?k='+K,{cache:'no-store'});
    if(!r.ok) throw 0;
    D=await r.json(); fails=0; paint(D);
  }catch(e){                                  // tolerate the odd flaky-network blip
    if(++fails>=3){ $('dot').className='dot off'; $('stat').textContent='reconnecting…'; }
  }
}
$('vol').oninput=e=>cmd('vol='+(e.target.value/100*4).toFixed(2));
$('mute').onclick=()=>{const n=!D.mute;D.mute=n;$('mute').className='tog'+(n?' on':'');cmd('mute='+(n?1:0));};
[...$('chat').children].forEach(b=>b.onclick=()=>{D.chattiness=+b.dataset.v;[...$('chat').children].forEach(x=>x.classList.toggle('on',x===b));cmd('chat='+b.dataset.v);});
$('qs').onchange=e=>cmd('qs='+t2m(e.target.value));
$('qe').onchange=e=>cmd('qe='+t2m(e.target.value));
$('wifi').onclick=()=>{
  if(!confirm("Put Terra into WiFi setup mode?\\n\\nShe'll reboot and open a 'Terra-Setup' hotspot for ~3 minutes. Connect your phone to it to choose a new network. She'll go offline here until she's back on WiFi.")) return;
  cmd('wifireset=1');
  $('wmsg').innerHTML="✅ Sent. In ~10 seconds Terra opens the <b>Terra-Setup</b> hotspot.<br>On your phone: WiFi settings → join <b>Terra-Setup</b> → pick your new network → Save.";
};
// ===== animated Terra face — the very renderer ported to the device's round display =====
const FACE={
 neutral:{body:'#57c98a',glow:'#173a28',edge:'#081711',eye:'sparkle',mouth:'smile', leaf:'up'},
 happy:  {body:'#5fe39a',glow:'#1e5234',edge:'#0a2417',eye:'happy',  mouth:'grin',  leaf:'up',  blush:1,fx:'sparkle'},
 love:   {body:'#74dba2',glow:'#3c1626',edge:'#180810',eye:'heart',  mouth:'smile', leaf:'up',  blush:2,fx:'hearts'},
 sleepy: {body:'#4d9c87',glow:'#13243d',edge:'#060c18',eye:'sleepy', mouth:'tiny',  leaf:'droop',fx:'stars',dim:.8,zzz:1},
 thirsty:{body:'#c4cf73',glow:'#2c2510',edge:'#161203',eye:'worry',  mouth:'pant',  leaf:'wilt', fx:'sweat'},
 cold:   {body:'#82c6db',glow:'#132f49',edge:'#07182a',eye:'wide',   mouth:'chatter',leaf:'shiver',blush:1,fx:'snow',shiver:1},
 hot:    {body:'#e8a06f',glow:'#3c1808',edge:'#1c0a02',eye:'tired',  mouth:'pant',  leaf:'droop',fx:'heat',sweat:1},
 sad:    {body:'#90a7b9',glow:'#1b1828',edge:'#0c0a14',eye:'sad',    mouth:'frown', leaf:'wilt', fx:'rain',tear:1},
 offline:{body:'#8a9bad',glow:'#1a2230',edge:'#090d15',eye:'worry', mouth:'tiny', leaf:'droop'},
};
let caption='', captionUntil=0, drinkStart=-9999; const DRINK_MS=5200;
const cv=$("face"), g=cv.getContext('2d');
const rnd=i=>{const x=Math.sin(i*127.1+311.7)*43758.5;return x-Math.floor(x);};
let blinkAt=0,nextBlink=900,dbl=false,lookCur=[0,0],lookTgt=[0,0],nextLook=1500,shownEmo='neutral',changeAt=-9999;
function leaf(x,y,ang,len,col){ g.save();g.translate(x,y);g.rotate(ang);
  g.fillStyle=col;g.beginPath();g.moveTo(0,0);
  g.bezierCurveTo(len*0.42,-len*0.45,len*0.16,-len,0,-len);
  g.bezierCurveTo(-len*0.16,-len,-len*0.42,-len*0.45,0,0);g.fill();
  g.strokeStyle='rgba(255,255,255,.22)';g.lineWidth=1.4;
  g.beginPath();g.moveTo(0,-3);g.lineTo(0,-len*0.82);g.stroke();g.restore(); }
function heart(x,y,s,col){ g.fillStyle=col;g.beginPath();
  g.moveTo(x,y+s*0.75);g.bezierCurveTo(x+s,y-s*0.35,x+s*0.5,y-s,x,y-s*0.25);
  g.bezierCurveTo(x-s*0.5,y-s,x-s,y-s*0.35,x,y+s*0.75);g.fill(); }
function teardrop(x,y,s,col){ g.fillStyle=col;g.beginPath();
  g.moveTo(x,y-s);g.bezierCurveTo(x+s*0.9,y+s*0.1,x+s*0.75,y+s,x,y+s);
  g.bezierCurveTo(x-s*0.75,y+s,x-s*0.9,y+s*0.1,x,y-s);g.fill();
  g.fillStyle='rgba(255,255,255,.5)';g.beginPath();g.arc(x-s*0.25,y,s*0.2,0,7);g.fill(); }
function particles(t,f){
  const k=f.fx; if(!k)return;
  if(k==='stars'){ for(let i=0;i<20;i++){const x=rnd(i)*240,y=rnd(i+7)*150+12,
    a=0.25+0.6*(0.5+0.5*Math.sin(t/500+i*2)),s=1+2*rnd(i+3);
    g.fillStyle='rgba(207,227,255,'+a+')';g.beginPath();g.arc(x,y,s,0,7);g.fill();} }
  else if(k==='snow'){ for(let i=0;i<26;i++){const x=(rnd(i)*240+Math.sin(t/700+i)*12+240)%240,
    y=((t*0.03*(0.5+rnd(i+2))+rnd(i+5)*260))%260,r=1+2.2*rnd(i+1);
    g.fillStyle='rgba(255,255,255,.7)';g.beginPath();g.arc(x,y,r,0,7);g.fill();} }
  else if(k==='hearts'){ for(let i=0;i<11;i++){const prog=((t*0.045*(0.5+rnd(i+1))+rnd(i+4)*260))%270,
    y=240-prog,x=rnd(i)*200+20+Math.sin(t/450+i)*9,a=Math.min(1,prog/40)*Math.min(1,(270-prog)/60);
    g.globalAlpha=0.8*a;heart(x,y,4+4*rnd(i+2),'#ff86ad');g.globalAlpha=1;} }
  else if(k==='sparkle'){ for(let i=0;i<13;i++){const ph=(t/650+rnd(i)*7)%4,sc=Math.max(0,Math.sin(ph*PI/2));
    if(sc<=0)continue;const x=rnd(i+1)*220+10,y=rnd(i+6)*200+15,r=3+5*sc;
    g.fillStyle='rgba(255,243,180,'+(0.9*sc)+')';g.save();g.translate(x,y);
    g.beginPath();for(let j=0;j<4;j++){const a=j*PI/2;g.lineTo(Math.cos(a)*r,Math.sin(a)*r);
      g.lineTo(Math.cos(a+PI/4)*r*0.32,Math.sin(a+PI/4)*r*0.32);}g.closePath();g.fill();g.restore();} }
  else if(k==='rain'){ for(let i=0;i<22;i++){const x=rnd(i)*240,
    y=((t*0.2*(0.6+rnd(i+1))+rnd(i+3)*240))%260;
    g.strokeStyle='rgba(159,200,230,.55)';g.lineWidth=2;g.lineCap='round';
    g.beginPath();g.moveTo(x,y);g.lineTo(x-2,y+9);g.stroke();} }
  else if(k==='heat'){ g.save();g.translate(196,46);g.rotate(t/2600);g.fillStyle='rgba(255,196,90,.5)';
    for(let j=0;j<8;j++){g.rotate(PI/4);g.beginPath();g.moveTo(0,-14);g.lineTo(4,-22);g.lineTo(-4,-22);g.closePath();g.fill();}
    g.restore();g.fillStyle='rgba(255,210,120,.7)';g.beginPath();g.arc(196,46,11,0,7);g.fill();
    for(let i=0;i<5;i++){const x=70+i*25,off=Math.sin(t/300+i)*4;
      g.strokeStyle='rgba(255,170,120,.25)';g.lineWidth=3;g.beginPath();
      g.moveTo(x+off,210);g.quadraticCurveTo(x+off+6,196,x+off,182);g.stroke();} }
}
function drawFace(t){
  const drinking=(t-drinkStart)<DRINK_MS, dprog=drinking?(t-drinkStart)/DRINK_MS:0;
  const offline=netOffline&&!drinking;
  const effEmo=drinking?'happy':(offline?'offline':curEmo);
  const f=FACE[effEmo]||FACE.neutral;
  if(effEmo!==shownEmo){shownEmo=effEmo;changeAt=t;}
  if(t>nextBlink&&blinkAt===0){blinkAt=t;dbl=rnd(t)<0.28;nextBlink=t+1800+rnd(t+1)*3200;}
  if(t>nextLook){nextLook=t+1100+rnd(t+2)*2600;
    lookTgt=(rnd(t+3)<0.45)?[0,0]:[(rnd(t+4)*2-1)*6,(rnd(t+5)*2-1)*4];}
  lookCur[0]+=(lookTgt[0]-lookCur[0])*0.12;lookCur[1]+=(lookTgt[1]-lookCur[1])*0.12;
  if(offline){lookCur[0]*=0.5;lookCur[1]+=(-5-lookCur[1])*0.2;}
  let open=1;
  if(blinkAt>0){const d=t-blinkAt,bw=x=>x<90?1-x/90:(x<180?(x-90)/90:1);
    if(d<180)open=bw(d);else if(dbl&&d>=240&&d<420)open=bw(d-240);
    if(d>(dbl?420:180))blinkAt=0;}
  if(f.eye==='sleepy')open=Math.min(open,0.32);
  if(f.eye==='tired') open=Math.min(open,0.55);
  if(f.eye==='wide')  open=Math.max(open,1);
  const bg=g.createRadialGradient(120,108,12,120,120,128);
  bg.addColorStop(0,f.glow);bg.addColorStop(1,f.edge);
  g.fillStyle=bg;g.fillRect(0,0,240,240);
  particles(t,f);
  const breath=Math.sin(t/850),sy=1+0.04*breath,sx=1-0.03*breath;
  const dt=t-changeAt,pop=1+0.17*Math.exp(-dt/210)*Math.cos(dt/72);
  const shv=f.shiver?Math.sin(t/40)*1.7:0;
  const gulp=(drinking&&dprog<0.82)?Math.max(0,Math.sin(t/150))*0.08:0;
  const BX=120,BY=142,bw=74,bh=66;
  g.save();g.translate(BX+shv,BY);g.scale((sx-gulp*0.5)*pop,(sy+gulp)*pop);
  g.fillStyle='rgba(0,0,0,.18)';g.beginPath();g.ellipse(0,bh-2,bw*0.7,9,0,0,7);g.fill();
  let bend=0,droop=0,jit=0;
  if(f.leaf==='droop'){bend=0.5;} else if(f.leaf==='wilt'){bend=1.0;droop=8;}
  else if(f.leaf==='shiver'){jit=Math.sin(t/45)*0.12;}
  const sway=Math.sin(t/700)*0.1+jit+(drinking?Math.sin(t/110)*0.12:0);
  g.strokeStyle='#3f8f5e';g.lineWidth=5;g.lineCap='round';
  g.beginPath();g.moveTo(0,-bh+8);g.lineTo(0,-bh-6+droop);g.stroke();
  leaf(0,-bh-4+droop,-0.6+bend+sway,26,'#52b878');
  leaf(0,-bh-4+droop, 0.6-bend+sway,26,'#46a86b');
  g.fillStyle=f.body;g.beginPath();g.ellipse(0,0,bw,bh,0,0,7);g.fill();
  g.fillStyle='rgba(255,255,255,.14)';g.beginPath();g.ellipse(-18,-22,bw*0.55,bh*0.45,-0.3,0,7);g.fill();
  g.fillStyle='rgba(0,0,0,.12)';g.beginPath();g.ellipse(14,26,bw*0.6,bh*0.4,0.2,0,7);g.fill();
  g.strokeStyle='rgba(255,255,255,.25)';g.lineWidth=2;g.beginPath();
  g.ellipse(0,0,bw-1,bh-1,0,PI*1.15,PI*1.85);g.stroke();
  if(f.blush){const col=f.blush>1?'rgba(255,120,150,.6)':'rgba(255,130,150,.42)';
    [-40,40].forEach(cx=>{const cg=g.createRadialGradient(cx,12,0,cx,12,15);
      cg.addColorStop(0,col);cg.addColorStop(1,'rgba(255,130,150,0)');
      g.fillStyle=cg;g.beginPath();g.arc(cx,12,15,0,7);g.fill();});}
  const EYX=27,EYY=-12,lx=lookCur[0],ly=lookCur[1],DK='#1e2b25';
  function ball(ex,ey,kx){const rx=15*kx,ry=Math.max(2.5,18*kx*open);
    if(open<0.12){g.strokeStyle=DK;g.lineWidth=4;g.lineCap='round';
      g.beginPath();g.arc(ex,ey-3,9,0.18*PI,0.82*PI);g.stroke();return;}
    g.fillStyle=DK;g.beginPath();g.ellipse(ex+lx*0.4,ey+ly*0.4,rx,ry,0,0,7);g.fill();
    if(open>0.45){g.fillStyle='rgba(255,255,255,.96)';
      g.beginPath();g.arc(ex-rx*0.32+lx*0.4,ey-ry*0.34+ly*0.4,rx*0.36,0,7);g.fill();
      g.beginPath();g.arc(ex+rx*0.34+lx*0.4,ey+ry*0.28+ly*0.4,rx*0.16,0,7);g.fill();}}
  function arcUp(ex,ey){g.strokeStyle=DK;g.lineWidth=5;g.lineCap='round';
    g.beginPath();g.arc(ex,ey+4,12,PI*1.18,PI*1.82);g.stroke();}
  function brow(ex,ey,dir){g.strokeStyle=DK;g.lineWidth=4;g.lineCap='round';
    g.beginPath();g.moveTo(ex-10*dir,ey-16);g.lineTo(ex+9*dir,ey-9);g.stroke();}
  const s=f.eye;
  if(s==='happy'){arcUp(-EYX,EYY);arcUp(EYX,EYY);}
  else if(s==='heart'){const p=1+0.08*Math.sin(t/200);heart(-EYX+lx*0.4,EYY+ly*0.4,13*p,'#ff5d86');heart(EYX+lx*0.4,EYY+ly*0.4,13*p,'#ff5d86');}
  else{ball(-EYX,EYY,s==='wide'?1.12:1);ball(EYX,EYY,s==='wide'?1.12:1);
    if(s==='sad'){brow(-EYX,EYY-3,-1);brow(EYX,EYY-3,1);}
    if(s==='worry'){brow(-EYX,EYY,-1);brow(EYX,EYY,1);}}
  const MY=22;g.strokeStyle=DK;g.lineWidth=4.5;g.lineCap='round';g.lineJoin='round';g.fillStyle=DK;
  const mo=f.mouth;
  if(mo==='smile'){g.beginPath();g.arc(0,MY-6,15,PI*0.18,PI*0.82);g.stroke();}
  else if(mo==='tiny'){g.beginPath();g.arc(0,MY-4,7,PI*0.2,PI*0.8);g.stroke();}
  else if(mo==='frown'){g.beginPath();g.arc(0,MY+14,14,PI*1.2,PI*1.8);g.stroke();}
  else if(mo==='chatter'){g.beginPath();g.moveTo(-13,MY);for(let i=0;i<=4;i++)g.lineTo(-13+i*6.5,MY+(i%2?5:-5));g.stroke();}
  else if(mo==='grin'){g.beginPath();g.moveTo(-15,MY-6);g.quadraticCurveTo(0,MY+14,15,MY-6);g.closePath();g.fill();
    g.fillStyle='#ff7c98';g.beginPath();g.ellipse(0,MY+3,7,4,0,0,PI);g.fill();}
  else if(mo==='pant'){g.beginPath();g.ellipse(0,MY,9,8,0,0,7);g.fill();
    g.fillStyle='#ff8fa3';g.beginPath();g.ellipse(0,MY+5,6,5,0,0,7);g.fill();}
  g.restore();
  const fx=BY+EYY;
  if(f.sweat)teardrop(BX+46,fx-8+Math.sin(t/200)*1,5,'#bfe6ff');
  if(f.tear){const fall=(t/9)%26;teardrop(BX-30,fx+14+fall,5,'#9fd8ff');
    if(fall>20)teardrop(BX+30,fx+14+(fall-20),4,'#9fd8ff');}
  if(curEmo==='thirsty'&&!offline)teardrop(BX-46,fx-4+Math.sin(t/240)*1,5,'#8fd0ff');
  if(f.zzz){g.fillStyle='rgba(207,227,255,.9)';const zb=Math.sin(t/450)*2;
    g.font='bold 15px monospace';g.fillText('z',150,72+zb);
    g.font='bold 21px monospace';g.fillText('Z',166,56-zb);}
  if(f.dim){g.fillStyle='rgba(4,6,20,'+(1-f.dim)+')';g.fillRect(0,0,240,240);}
  if(offline){
    const cx=120, cy=60, cyc=(t/300)%4;
    for(let k=0;k<3;k++){ const on=cyc>(k+1);
      g.strokeStyle=on?'#9fd9ff':'rgba(180,200,220,.15)'; g.lineWidth=4; g.lineCap='round';
      g.beginPath(); g.arc(cx,cy,8+k*9,PI*1.25,PI*1.75); g.stroke(); }
    g.fillStyle=cyc>1?'#9fd9ff':'rgba(180,200,220,.3)'; g.beginPath(); g.arc(cx,cy,3.2,0,7); g.fill();
    if(cyc>3){ g.strokeStyle='#ff6b6b'; g.lineWidth=4.5; g.lineCap='round';
      g.beginPath(); g.moveTo(cx-20,cy-22); g.lineTo(cx+20,cy+6); g.stroke(); }
    g.fillStyle='rgba(207,227,255,.8)'; g.font='600 12px ui-monospace,monospace'; g.textAlign='center';
    g.fillText('looking for internet…', 120, 214); g.textAlign='left';
  }
}
function faceLoop(t){ drawFace(t); requestAnimationFrame(faceLoop); }
requestAnimationFrame(faceLoop);
poll(); setInterval(poll,3000);
</script></body></html>`;

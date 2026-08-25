const CFG = window.ATE_CLOUD_CONFIG;
const $ = s => document.querySelector(s);
const stations = new Map(CFG.stations.map(id => [id, { id, available: false, status: null, lastSeen: 0 }]));
const pending = new Map();
let client = null;
let toastTimer = null;

$('#brokerHost').textContent = CFG.brokerUrl;
$('#brokerShort').textContent = new URL(CFG.brokerUrl).hostname;

function esc(v='') { return String(v).replace(/[&<>'"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c])); }
function fmtUptime(sec=0){ sec=Number(sec)||0; const d=Math.floor(sec/86400),h=Math.floor(sec%86400/3600),m=Math.floor(sec%3600/60); return d?`${d}d ${h}h`:`${h}h ${m}m`; }
function nowTime(){ return new Date().toLocaleTimeString([], {hour12:false}); }
function toast(msg,error=false){ const t=$('#toast'); t.textContent=msg; t.className=`toast show${error?' error':''}`; clearTimeout(toastTimer); toastTimer=setTimeout(()=>t.className='toast',3200); }
function addLog(station,kind,msg){ const log=$('#eventLog'); if(log.querySelector('.empty')) log.innerHTML=''; const row=document.createElement('div'); row.className='event-row'; row.innerHTML=`<span class="time">${nowTime()}</span><span class="station">${esc(station)}</span><span class="kind">${esc(kind)}</span><span>${esc(msg)}</span>`; log.prepend(row); while(log.children.length>60) log.lastElementChild.remove(); $('#lastEvent').textContent=`${station} · ${kind}`; }

function stationTemplate(id){ return `
<article class="station-card" id="card-${id}">
  <div class="station-head">
    <div><p class="eyebrow">ATE STATION</p><h2>${id}</h2><p class="local-ip">Local IP: --</p></div>
    <div class="availability"><i class="dot"></i><span>OFFLINE</span></div>
  </div>
  <div class="mode-zone">
    <div class="mode-top"><strong class="mode">--</strong><span class="tool-pill">TOOL -</span></div>
    <div class="step">Waiting for MQTT status…</div>
    <div class="metrics">
      <div class="metric"><span>Firmware</span><strong class="fw">--</strong></div>
      <div class="metric"><span>Filesystem</span><strong class="fs">--</strong></div>
      <div class="metric"><span>Wi-Fi RSSI</span><strong class="rssi">--</strong></div>
      <div class="metric"><span>Uptime</span><strong class="uptime">--</strong></div>
    </div>
  </div>
  <div class="controls">
    <div class="controls-title"><strong>Remote Control</strong><span class="last-seen">No status yet</span></div>
    <div class="button-grid">
      <button class="btn ghost cmd" data-station="${id}" data-command="REQUEST_STATUS">REFRESH</button>
      <button class="btn secondary cmd start" data-station="${id}" data-command="START_PC">PC MODE</button>
      <button class="btn primary cmd start" data-station="${id}" data-command="START_AUTO">AUTO</button>
      <button class="btn primary cmd start" data-station="${id}" data-command="START_AUTO2">AUTO2</button>
      <button class="btn danger cmd stop" data-station="${id}" data-command="STOP">STOP</button>
    </div>
  </div>
</article>`; }

$('#stationGrid').innerHTML = CFG.stations.map(stationTemplate).join('');

function renderStation(id){
  const m=stations.get(id); if(!m) return;
  const c=$(`#card-${id}`), s=m.status||{};
  const stale=m.lastSeen && (Date.now()-m.lastSeen>8000);
  const online=m.available && !stale;
  c.querySelector('.availability .dot').className=`dot ${online?'good':'bad'}`;
  c.querySelector('.availability span').textContent=online?'ONLINE':'OFFLINE';
  c.querySelector('.mode').textContent=s.mode||'--';
  c.querySelector('.tool-pill').textContent=`TOOL ${s.tool||'-'}`;
  c.querySelector('.step').textContent=s.step||(online?'Waiting for status…':'Station not connected');
  c.querySelector('.fw').textContent=s.fw_version||'--';
  c.querySelector('.fs').textContent=s.fs_version||'--';
  c.querySelector('.rssi').textContent=Number.isFinite(s.rssi)?`${s.rssi} dBm`:'--';
  c.querySelector('.uptime').textContent=s.uptime_s!=null?fmtUptime(s.uptime_s):'--';
  c.querySelector('.local-ip').textContent=`Local IP: ${s.ip||'--'}`;
  c.querySelector('.last-seen').textContent=m.lastSeen?`Status ${Math.max(0,Math.floor((Date.now()-m.lastSeen)/1000))}s ago`:'No status yet';
  const running=!!s.running;
  c.querySelectorAll('.cmd').forEach(b=>b.disabled=!online||!client?.connected);
  c.querySelectorAll('.start').forEach(b=>b.disabled=!online||!client?.connected||running);
  c.querySelector('.stop').disabled=!online||!client?.connected||!running;
}

function renderSummary(){
  const arr=[...stations.values()];
  $('#onlineCount').textContent=`${arr.filter(x=>x.available).length} / ${arr.length}`;
  $('#runningCount').textContent=arr.filter(x=>x.available && x.status?.running).length;
  arr.forEach(x=>renderStation(x.id));
}

function setBrokerState(state){
  const dot=$('#brokerDot'), label=$('#brokerLabel');
  if(state==='connected'){dot.className='dot good';label.textContent='CONNECTED';}
  else if(state==='connecting'){dot.className='dot';label.textContent='CONNECTING';}
  else {dot.className='dot bad';label.textContent='DISCONNECTED';}
  $('#connectBtn').disabled=state==='connected'||state==='connecting';
  $('#disconnectBtn').disabled=state!=='connected';
  renderSummary();
}

function subscribeTopics(){
  ['ate/+/availability','ate/+/status','ate/+/ack','ate/+/event'].forEach(t=>client.subscribe(t,{qos:1}));
}

function handleMessage(topic,payload){
  const parts=topic.split('/'); if(parts.length<3||parts[0]!=='ate') return;
  const id=parts[1], kind=parts[2], m=stations.get(id); if(!m) return;
  const text=payload.toString();
  if(kind==='availability'){
    m.available=text.trim().toUpperCase()==='ONLINE';
    addLog(id,'AVAILABILITY',m.available?'ONLINE':'OFFLINE');
  } else if(kind==='status'){
    try { m.status=JSON.parse(text); m.lastSeen=Date.now(); }
    catch(e){ addLog(id,'ERROR','Invalid status JSON'); }
  } else if(kind==='ack'){
    try {
      const ack=JSON.parse(text); const p=pending.get(ack.id);
      if(p){ clearTimeout(p.timer); pending.delete(ack.id); }
      const ok=ack.result==='OK'; toast(`${id}: ${ack.message||ack.result}`,!ok); addLog(id,`ACK ${ack.result}`,`${ack.command||''} · ${ack.message||''}`);
    } catch(e){ addLog(id,'ERROR','Invalid ACK JSON'); }
  } else if(kind==='event'){
    try { const ev=JSON.parse(text); addLog(id,ev.type||'EVENT',ev.message||text); }
    catch(e){ addLog(id,'EVENT',text); }
  }
  renderSummary();
}

function connectBroker(){
  const username=$('#mqttUser').value.trim(), password=$('#mqttPass').value;
  if(!username||!password){toast('Enter MQTT username and password',true);return;}
  if(typeof mqtt==='undefined'){toast('MQTT.js failed to load',true);return;}
  if(client){ try{client.end(true);}catch(e){} client=null; }
  setBrokerState('connecting');
  const clientId=`ate-web-${Date.now().toString(36)}-${Math.random().toString(16).slice(2,8)}`;
  client=mqtt.connect(CFG.brokerUrl,{
    clientId,username,password,protocolVersion:4,clean:true,keepalive:30,
    reconnectPeriod:CFG.reconnectPeriodMs,connectTimeout:CFG.connectTimeoutMs,
    resubscribe:true
  });
  client.on('connect',()=>{setBrokerState('connected');subscribeTopics();addLog('CLOUD','BROKER','Dashboard connected');toast('MQTT dashboard connected');});
  client.on('reconnect',()=>setBrokerState('connecting'));
  client.on('offline',()=>setBrokerState('disconnected'));
  client.on('close',()=>setBrokerState('disconnected'));
  client.on('error',err=>{toast(`MQTT: ${err.message}`,true);addLog('CLOUD','ERROR',err.message);});
  client.on('message',handleMessage);
}

function disconnectBroker(){ if(client){client.end(true);client=null;} setBrokerState('disconnected'); CFG.stations.forEach(id=>{stations.get(id).available=false;renderStation(id)}); addLog('CLOUD','BROKER','Dashboard disconnected'); }

function sendCommand(station,command){
  if(!client?.connected){toast('MQTT broker is not connected',true);return;}
  const m=stations.get(station); if(!m?.available){toast(`${station} is offline`,true);return;}
  if(command==='STOP' && !confirm(`Stop Automation on ${station}?`)) return;
  const id=`WEB-${Date.now()}-${Math.random().toString(16).slice(2,6)}`;
  const body=JSON.stringify({id,command,source:'cloud-dashboard'});
  const topic=`ate/${station}/command`;
  client.publish(topic,body,{qos:1,retain:false},err=>{if(err){toast(`${station}: publish failed`,true);return;} addLog(station,'COMMAND',command);});
  const timer=setTimeout(()=>{pending.delete(id);toast(`${station}: command ACK timeout`,true);addLog(station,'TIMEOUT',command);},CFG.commandTimeoutMs);
  pending.set(id,{station,command,timer});
}

$('#stationGrid').addEventListener('click',e=>{const b=e.target.closest('.cmd');if(b)sendCommand(b.dataset.station,b.dataset.command);});
$('#connectBtn').addEventListener('click',connectBroker);
$('#disconnectBtn').addEventListener('click',disconnectBroker);
$('#mqttPass').addEventListener('keydown',e=>{if(e.key==='Enter')connectBroker();});
$('#clearLog').addEventListener('click',()=>$('#eventLog').innerHTML='<div class="empty">Log cleared.</div>');
setInterval(renderSummary,1000);
setBrokerState('disconnected');
renderSummary();

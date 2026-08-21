const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];

let latestStatus = null;
let lastSnapshot = null;
let config = null;
let toastTimer = null;
let pollingBusy = false;

const pageNames = {
  dashboard: 'Dashboard', run: 'Run Control', manual: 'Manual Control',
  calibration: 'Calibration', wifi: 'Wi-Fi', update: 'Update'
};

function toast(message, error = false) {
  const el = $('#toast');
  el.textContent = message;
  el.className = `toast show${error ? ' error' : ''}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.className = 'toast', 3200);
}

async function api(path, options = {}) {
  const opts = { ...options, headers: { ...(options.headers || {}) } };
  if (opts.body && typeof opts.body !== 'string') {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(opts.body);
  }
  const res = await fetch(path, opts);
  const type = res.headers.get('content-type') || '';
  const data = type.includes('application/json') ? await res.json() : { message: await res.text() };
  if (!res.ok) throw new Error(data.message || `HTTP ${res.status}`);
  return data;
}

function setPage(name) {
  $$('.nav-item').forEach(b => b.classList.toggle('active', b.dataset.page === name));
  $$('.page').forEach(p => p.classList.toggle('active', p.id === `page-${name}`));
  $('#pageTitle').textContent = pageNames[name] || name;
  if (name === 'calibration' && !config) loadConfig();
}

$$('.nav-item').forEach(btn => btn.addEventListener('click', () => setPage(btn.dataset.page)));
$$('[data-go]').forEach(btn => btn.addEventListener('click', () => setPage(btn.dataset.go)));

function fmtUptime(sec) {
  sec = Number(sec || 0);
  const d = Math.floor(sec / 86400); sec %= 86400;
  const h = Math.floor(sec / 3600); sec %= 3600;
  const m = Math.floor(sec / 60); const s = sec % 60;
  return `${d ? d + 'd ' : ''}${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`;
}

function pill(el, text, cls = 'neutral') {
  if (!el) return;
  el.textContent = text;
  el.className = `pill ${cls}`;
}

function health(el, ok) {
  el.textContent = ok ? 'READY' : 'NOT READY';
  el.className = `health ${ok ? 'good' : 'bad'}`;
}

function addActivity(text) {
  const log = $('#activityLog');
  const empty = log.querySelector('.empty');
  if (empty) empty.remove();
  const row = document.createElement('div');
  row.className = 'activity-item';
  row.innerHTML = `<time>${new Date().toLocaleTimeString([], {hour12:false})}</time><span></span>`;
  row.querySelector('span').textContent = text;
  log.prepend(row);
  while (log.children.length > 18) log.lastElementChild.remove();
}

function trackChanges(s) {
  if (!lastSnapshot) {
    addActivity(`Web connected · ${s.mode} · Tool ${s.tool}`);
  } else {
    if (s.mode !== lastSnapshot.mode) addActivity(`Mode: ${lastSnapshot.mode} → ${s.mode}`);
    if (s.tool !== lastSnapshot.tool) addActivity(`Changed to Tool ${s.tool}`);
    if (s.step !== lastSnapshot.step) addActivity(s.step);
    if (s.wifi_connected !== lastSnapshot.wifi_connected) addActivity(s.wifi_connected ? 'Wi-Fi connected' : 'Wi-Fi disconnected');
  }
  lastSnapshot = { mode: s.mode, tool: s.tool, step: s.step, wifi_connected: s.wifi_connected };
}

function renderStatus(s) {
  latestStatus = s;
  trackChanges(s);
  $('#stationName').textContent = s.station || 'ATE';
  $('#modeValue').textContent = s.mode;
  $('#toolValue').textContent = s.tool;
  $('#stepValue').textContent = s.step;
  $('#fwValue').textContent = s.fw_version;
  $('#fsValue').textContent = s.fs_version;
  $('#rssiValue').textContent = s.wifi_connected ? `${s.rssi} dBm` : '--';
  $('#ssidValue').textContent = s.ssid || 'Not connected';
  $('#uptimeValue').textContent = fmtUptime(s.uptime_s);
  $('#topIp').textContent = s.ip || '--';

  const online = s.wifi_connected;
  pill($('#onlinePill'), online ? 'ONLINE' : 'OFFLINE', online ? 'good' : 'bad');
  pill($('#runBadge'), s.running ? 'RUNNING' : 'STOPPED', s.running ? 'good' : 'neutral');
  $('#sidebarDot').classList.toggle('online', online);
  $('#sidebarConnection').textContent = online ? 'ATE Online' : 'ATE Offline';

  health($('#healthWeb'), online);
  health($('#healthFs'), !!s.littlefs_ready);
  health($('#healthAudio'), !!s.audio_ready);
  health($('#healthWifi'), online);

  $('#wifiRssiBig').textContent = online ? s.rssi : '--';
  $('#wifiSsidBig').textContent = s.ssid || '--';
  $('#wifiIpBig').textContent = s.ip || '--';
  $('#wifiStatusBig').textContent = online ? 'Connected' : 'Disconnected';

  $('#updateCurrentFw').textContent = s.fw_version;
  $('#updateCurrentFs').textContent = s.fs_version;

  const locked = !!s.running;
  pill($('#manualLock'), locked ? 'LOCKED · STOP ATE' : 'MANUAL READY', locked ? 'warn' : 'good');
  $$('#page-manual button, #page-calibration button, #page-calibration input').forEach(el => el.disabled = locked || s.local_ota || s.wifi_portal_active);
  $$('.run-mode').forEach(el => el.disabled = !!s.running);
  $('#startWifiSetup').disabled = !!s.running || s.local_ota;
  $('#startUpdate').disabled = !!s.running || s.local_ota;
  $('#dashboardStop').disabled = !s.running;
  $('#runStop').disabled = !s.running;
}

async function pollStatus() {
  if (pollingBusy) return;
  pollingBusy = true;
  try {
    const s = await api('/api/status');
    renderStatus(s);
  } catch (e) {
    pill($('#onlinePill'), 'OFFLINE', 'bad');
    $('#sidebarDot').classList.remove('online');
    $('#sidebarConnection').textContent = 'Connection lost';
  } finally {
    pollingBusy = false;
  }
}

async function postRun(mode) {
  try {
    await api('/api/run', { method: 'POST', body: { mode } });
    toast(`${mode.toUpperCase()} started`);
    await pollStatus();
  } catch (e) { toast(e.message, true); }
}

async function stopRun() {
  try {
    await api('/api/stop', { method: 'POST' });
    toast('Automation stopped');
    await pollStatus();
  } catch (e) { toast(e.message, true); }
}

$$('.run-mode').forEach(btn => btn.addEventListener('click', () => postRun(btn.dataset.mode)));
$('#dashboardStop').addEventListener('click', stopRun);
$('#runStop').addEventListener('click', stopRun);

function buildManualControls() {
  const host = $('#toolControls');
  host.innerHTML = [1,2,3].map(tool => `
    <article class="tool-card">
      <h3>Tool ${tool}</h3>
      <div class="tool-section"><span>USB Actuator</span><div class="button-row">
        <button class="btn secondary hw" data-type="usb" data-tool="${tool}" data-action="disconnect">DISCONNECT</button>
        <button class="btn primary hw" data-type="usb" data-tool="${tool}" data-action="connect">CONNECT</button>
      </div></div>
      <div class="tool-section"><span>DLC Relay</span><div class="button-row">
        <button class="btn secondary hw" data-type="dlc" data-tool="${tool}" data-action="disconnect">DISCONNECT</button>
        <button class="btn primary hw" data-type="dlc" data-tool="${tool}" data-action="connect">CONNECT</button>
      </div></div>
    </article>`).join('');
  $$('.hw').forEach(btn => btn.addEventListener('click', async () => {
    btn.disabled = true;
    try {
      await api(`/api/manual/${btn.dataset.type}`, { method: 'POST', body: { tool: Number(btn.dataset.tool), action: btn.dataset.action } });
      toast(`Tool ${btn.dataset.tool} ${btn.dataset.type.toUpperCase()} ${btn.dataset.action}`);
      addActivity(`Manual: Tool ${btn.dataset.tool} ${btn.dataset.type.toUpperCase()} ${btn.dataset.action}`);
    } catch (e) { toast(e.message, true); }
    finally { btn.disabled = latestStatus?.running || false; }
  }));
}

function buildKeys() {
  const host = $('#keyGrid');
  host.innerHTML = Array.from({length:11}, (_,i) => `<button class="key-btn" data-key="${i+1}">KEY ${i+1}</button>`).join('');
  $$('.key-btn').forEach(btn => btn.addEventListener('click', async () => {
    try {
      await api('/api/manual/key', { method: 'POST', body: { key: Number(btn.dataset.key) } });
      toast(`Key ${btn.dataset.key} tested`);
    } catch (e) { toast(e.message, true); }
  }));
}

$('#testAllKeys').addEventListener('click', async () => {
  try { await api('/api/manual/key', { method: 'POST', body: { all: true } }); toast('All 11 keys tested'); }
  catch (e) { toast(e.message, true); }
});
$('#testVoice').addEventListener('click', async () => {
  try { await api('/api/audio/test', { method: 'POST' }); toast('Voice test queued'); }
  catch (e) { toast(e.message, true); }
});

function buildCalibration() {
  if (!config) return;
  const host = $('#calibrationTools');
  host.innerHTML = [0,1,2].map(i => `
    <article class="cal-card" data-tool="${i+1}">
      <h3>Tool ${i+1}</h3>
      <div class="angle-block">
        <div class="angle-head"><label>Angle A · Disconnect</label><output id="out-a-${i}">${config.usbAngleA[i]}°</output></div>
        <input class="angle-range" id="angle-a-${i}" type="range" min="0" max="180" value="${config.usbAngleA[i]}">
        <button class="btn secondary move-angle" data-tool="${i+1}" data-input="angle-a-${i}">MOVE A</button>
      </div>
      <div class="angle-block">
        <div class="angle-head"><label>Angle B · Connect</label><output id="out-b-${i}">${config.usbAngleB[i]}°</output></div>
        <input class="angle-range" id="angle-b-${i}" type="range" min="0" max="180" value="${config.usbAngleB[i]}">
        <button class="btn secondary move-angle" data-tool="${i+1}" data-input="angle-b-${i}">MOVE B</button>
      </div>
    </article>`).join('');

  [0,1,2].forEach(i => {
    $(`#angle-a-${i}`).addEventListener('input', e => $(`#out-a-${i}`).textContent = `${e.target.value}°`);
    $(`#angle-b-${i}`).addEventListener('input', e => $(`#out-b-${i}`).textContent = `${e.target.value}°`);
  });
  $$('.move-angle').forEach(btn => btn.addEventListener('click', async () => {
    const angle = Number($(`#${btn.dataset.input}`).value);
    try { await api('/api/calibration/move', { method: 'POST', body: { tool: Number(btn.dataset.tool), angle } }); toast(`Tool ${btn.dataset.tool} moved to ${angle}°`); }
    catch (e) { toast(e.message, true); }
  }));
  $('#delayDisconnect').value = config.delayDisconnect;
  $('#delayConnect').value = config.delayConnect;
}

async function loadConfig() {
  try { config = await api('/api/config'); buildCalibration(); }
  catch (e) { toast(`Config: ${e.message}`, true); }
}

$('#saveConfig').addEventListener('click', async () => {
  const body = {
    usbAngleA: [0,1,2].map(i => Number($(`#angle-a-${i}`).value)),
    usbAngleB: [0,1,2].map(i => Number($(`#angle-b-${i}`).value)),
    delayDisconnect: Number($('#delayDisconnect').value),
    delayConnect: Number($('#delayConnect').value)
  };
  try { await api('/api/config', { method: 'POST', body }); config = body; toast('Configuration saved to EEPROM'); addActivity('Calibration settings saved'); }
  catch (e) { toast(e.message, true); }
});

$('#startWifiSetup').addEventListener('click', async () => {
  if (!confirm('Start ATE_Setup_WiFi? The current web connection will close.')) return;
  try {
    await api('/api/wifi/setup', { method: 'POST' });
    toast('Wi-Fi Setup AP is starting…');
    addActivity('Wi-Fi Setup AP requested');
  } catch (e) { toast(e.message, true); }
});

$('#checkUpdate').addEventListener('click', async () => {
  const btn = $('#checkUpdate'); btn.disabled = true; btn.textContent = 'CHECKING…';
  try {
    const u = await api('/api/update/check');
    $('#updateServerFw').textContent = u.server_fw;
    $('#updateServerFs').textContent = u.server_fs;
    pill($('#fwUpdateState'), u.fw_update_available ? 'UPDATE AVAILABLE' : 'UP TO DATE', u.fw_update_available ? 'warn' : 'good');
    pill($('#fsUpdateState'), u.fs_update_available ? 'UPDATE AVAILABLE' : 'UP TO DATE', u.fs_update_available ? 'warn' : 'good');
    toast('Server versions checked');
  } catch (e) { toast(e.message, true); }
  finally { btn.disabled = false; btn.textContent = 'CHECK SERVER'; }
});

$('#startUpdate').addEventListener('click', async () => {
  if (!confirm('Start HTTPS OTA? The ATE may reboot.')) return;
  try {
    await api('/api/update/start', { method: 'POST' });
    toast('HTTPS OTA starting. Device may reboot…');
    addActivity('HTTPS OTA requested');
  } catch (e) { toast(e.message, true); }
});

$('#clearActivity').addEventListener('click', () => $('#activityLog').innerHTML = '<div class="empty">Activity cleared.</div>');

buildManualControls();
buildKeys();
loadConfig();
pollStatus();
setInterval(pollStatus, 900);

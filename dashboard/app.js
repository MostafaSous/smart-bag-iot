const REST_BASE = '';

// ── Map setup ─────────────────────────────────────────────────────────────────
const map = L.map('map', { zoomControl: true }).setView([0, 0], 2);

L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
  attribution: '© OpenStreetMap contributors',
  maxZoom: 19,
}).addTo(map);

const bagIcon = L.divIcon({
  className: '',
  html: `<div style="
    width:18px;height:18px;border-radius:50%;
    background:#6366f1;border:3px solid #fff;
    box-shadow:0 0 8px #6366f1aa;"></div>`,
  iconSize: [18, 18],
  iconAnchor: [9, 9],
});

let marker   = null;
let pathLine = null;
const pathCoords = [];

// ── DOM refs ──────────────────────────────────────────────────────────────────
const sseDot          = document.getElementById('mqtt-dot');
const sseLabel        = document.getElementById('mqtt-label');
const relayBanner     = document.getElementById('relay-banner');
const tamperBanner    = document.getElementById('tamper-banner');
const tamperConfEl    = document.getElementById('tamper-confidence');

const elLat       = document.getElementById('gps-lat');
const elLng       = document.getElementById('gps-lng');
const elGpsBadge  = document.getElementById('gps-valid-badge');
const elWeight    = document.getElementById('weight-value');

const pillOpen    = document.getElementById('pill-open');
const pillMagnet  = document.getElementById('pill-magnet');
const pillFlat    = document.getElementById('pill-flat');
const pillBagB    = document.getElementById('pill-bagb');
const bagbDot     = document.getElementById('bagb-dot');
const bagbLast    = document.getElementById('bagb-last');

const barPitch    = document.getElementById('bar-pitch');
const barRoll     = document.getElementById('bar-roll');
const valPitch    = document.getElementById('val-pitch');
const valRoll     = document.getElementById('val-roll');

// ── Helpers ───────────────────────────────────────────────────────────────────
function angleToBarPct(deg) {
  return Math.min(100, Math.max(0, 50 + (deg / 90) * 50));
}

function setSseStatus(s) {
  sseDot.className = s;
  sseLabel.textContent =
    s === 'connected'   ? 'Live' :
    s === 'connecting'  ? 'Connecting…' : 'Disconnected';
}

let relayTimer  = null;
let tamperTimer = null;

function showRelay(on) {
  relayBanner.classList.toggle('visible', on);
}

function updateTamper(payload) {
  if (!payload || !payload.alert) return;
  const pct = (payload.confidence * 100).toFixed(1);
  tamperConfEl.textContent = `(${pct}% confidence)`;
  tamperBanner.classList.add('visible');
  clearTimeout(tamperTimer);
  tamperTimer = setTimeout(() => tamperBanner.classList.remove('visible'), 30000);
}

// ── State updaters ────────────────────────────────────────────────────────────
function updateGps(payload) {
  if (!payload.valid) {
    elGpsBadge.textContent = 'No Fix';
    elGpsBadge.className   = '';
    return;
  }
  const { lat, lng } = payload;
  elLat.textContent      = lat.toFixed(6);
  elLng.textContent      = lng.toFixed(6);
  elGpsBadge.textContent = 'Fix OK';
  elGpsBadge.className   = 'valid';

  const latlng = [lat, lng];
  if (!marker) {
    marker = L.marker(latlng, { icon: bagIcon }).addTo(map);
    map.setView(latlng, 15);
  } else {
    marker.setLatLng(latlng);
  }
  pathCoords.push(latlng);
  if (pathLine) {
    pathLine.setLatLngs(pathCoords);
  } else {
    pathLine = L.polyline(pathCoords, { color: '#6366f1', weight: 2, opacity: 0.6 }).addTo(map);
  }
}

function updateWeight(payload) {
  elWeight.textContent = Number(payload.grams).toFixed(0);
}

function updateTilt(payload) {
  const { pitch, roll } = payload;
  valPitch.textContent = pitch.toFixed(1) + '°';
  valRoll.textContent  = roll.toFixed(1) + '°';
  barPitch.style.width = angleToBarPct(pitch) + '%';
  barRoll.style.width  = angleToBarPct(roll) + '%';
  pillFlat.textContent = payload.flat ? 'FLAT' : 'NOT FLAT';
  pillFlat.className   = 'status-pill ' + (payload.flat ? 'active-good' : '');
}

function updateStatus(payload) {
  pillOpen.textContent = payload.open ? 'OPEN' : 'CLOSED';
  pillOpen.className   = 'status-pill ' + (payload.open ? 'active-warn' : 'active-good');
  pillMagnet.textContent = payload.magnetDetected ? 'MAGNET ON' : 'NO MAGNET';
  pillMagnet.className   = 'status-pill ' + (payload.magnetDetected ? 'active-good' : 'active-warn');
}

function updateRelay() {
  showRelay(true);
  clearTimeout(relayTimer);
  relayTimer = setTimeout(() => showRelay(false), 15000);
}

function updateBagBHeartbeat() {
  bagbDot.className    = 'alive';
  pillBagB.className   = 'status-pill active-info';
  bagbLast.textContent = 'Last seen: ' + new Date().toLocaleTimeString();
  clearTimeout(updateBagBHeartbeat._timer);
  updateBagBHeartbeat._timer = setTimeout(() => {
    bagbDot.className    = '';
    pillBagB.className   = 'status-pill';
    bagbLast.textContent = 'Last seen: >20s ago';
  }, 20000);
}

function routeMessage(topic, payload) {
  if      (topic === '/bags/bagA/gps')      updateGps(payload);
  else if (topic === '/bags/bagA/weight')   updateWeight(payload);
  else if (topic === '/bags/bagA/tilt')     updateTilt(payload);
  else if (topic === '/bags/bagA/status')   updateStatus(payload);
  else if (topic === '/bags/bagA/relay')    updateRelay();
  else if (topic === '/bags/bagA/tamper')   updateTamper(payload);
  else if (topic === '/bags/bagB/heartbeat') updateBagBHeartbeat();
}

// ── Auth ──────────────────────────────────────────────────────────────────────
function authHeaders() {
  return { Authorization: 'Bearer ' + (localStorage.getItem('sb_token') || '') };
}

function logout() {
  localStorage.removeItem('sb_token');
  localStorage.removeItem('sb_user');
  window.location.href = '/login.html';
}

// ── GPS history trail ─────────────────────────────────────────────────────────
async function loadHistory() {
  try {
    const res  = await fetch(REST_BASE + '/api/history/gps', { headers: authHeaders() });
    const rows = await res.json();
    rows.forEach(r => { if (r.valid) pathCoords.push([r.lat, r.lng]); });
    if (pathCoords.length > 1) {
      pathLine = L.polyline(pathCoords, { color: '#6366f1', weight: 2, opacity: 0.4 }).addTo(map);
      map.fitBounds(pathLine.getBounds(), { padding: [40, 40] });
    }
  } catch { /* backend not ready yet */ }
}

// ── SSE connection ────────────────────────────────────────────────────────────
function connectSSE(token) {
  setSseStatus('connecting');

  const es = new EventSource(`/api/events?token=${encodeURIComponent(token)}`);

  es.onopen = () => setSseStatus('connected');

  es.onmessage = (e) => {
    try {
      const { topic, payload } = JSON.parse(e.data);
      routeMessage(topic, payload);
    } catch { /* ignore malformed */ }
  };

  es.onerror = () => {
    setSseStatus('disconnected');
    // EventSource auto-reconnects — just update the dot while it retries
  };
}

// ── Boot ──────────────────────────────────────────────────────────────────────
const token = localStorage.getItem('sb_token');
if (!token) {
  window.location.href = '/login.html';
} else {
  document.getElementById('username-display').textContent =
    localStorage.getItem('sb_user') || '?';
  loadHistory();
  connectSSE(token);
}

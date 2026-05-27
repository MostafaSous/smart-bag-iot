const express        = require('express');
const mqtt           = require('mqtt');
const jwt            = require('jsonwebtoken');
const { pool, waitForDb } = require('./db');
const { router: authRouter, requireAuth } = require('./auth');
const TamperDetector = require('./detector');

const detector = new TamperDetector();

const MQTT_URL   = process.env.MQTT_URL  || 'mqtts://YOUR_CLUSTER.s1.eu.hivemq.cloud:8883';
const MQTT_USER  = process.env.MQTT_USER || 'YOUR_HIVEMQ_USERNAME';
const MQTT_PASS  = process.env.MQTT_PASS || 'YOUR_HIVEMQ_PASSWORD';
const HTTP_PORT  = process.env.PORT      || 3000;
const JWT_SECRET = process.env.JWT_SECRET || 'smartbag-secret-change-me';

// ── In-memory latest state ────────────────────────────────────────────────────
const state = {
  bagA: {
    gps:    { lat: 0, lng: 0, valid: false },
    weight: { grams: 0 },
    tilt:   { pitch: 0, roll: 0, flat: false },
    status: { open: false, magnetDetected: false },
    relay:  { relayedBy: null },
    tamper: { alert: false, confidence: 0 },
    lastSeen: null,
  },
  bagB: {
    heartbeat: { alive: false },
    lastSeen: null,
  },
};

let isRelayed = false;

// ── SSE client registry ───────────────────────────────────────────────────────
const sseClients = new Set();

function broadcast(topic, payload) {
  const data = JSON.stringify({ topic, payload });
  for (const res of sseClients) {
    res.write(`data: ${data}\n\n`);
  }
}

// ── MQTT ──────────────────────────────────────────────────────────────────────
function startMqtt() {
  const client = mqtt.connect(MQTT_URL, {
    username:           MQTT_USER,
    password:           MQTT_PASS,
    rejectUnauthorized: false,
  });

  client.on('connect', () => {
    console.log('[mqtt] Connected to HiveMQ.');
    client.subscribe('/bags/bagA/#');
    client.subscribe('/bags/bagB/#');
  });

  client.on('error', err => console.error('[mqtt] Error:', err.message));

  client.on('message', async (topic, raw) => {
    let payload;
    try {
      payload = JSON.parse(raw.toString());
    } catch {
      return;
    }

    const now = new Date();

    if (topic === '/bags/bagA/gps') {
      state.bagA.gps      = payload;
      state.bagA.lastSeen = now;
      await pool.execute(
        'INSERT INTO gps_logs (device_id, lat, lng, valid, relayed) VALUES (1,?,?,?,?)',
        [payload.lat, payload.lng, payload.valid ? 1 : 0, isRelayed ? 1 : 0]
      ).catch(e => console.error('[db] gps_logs:', e.message));
      await pool.execute('UPDATE devices SET last_seen=? WHERE id=1', [now]).catch(() => {});

    } else if (topic === '/bags/bagA/weight') {
      state.bagA.weight = payload;
      await pool.execute(
        'INSERT INTO weight_logs (device_id, grams) VALUES (1,?)',
        [payload.grams]
      ).catch(e => console.error('[db] weight_logs:', e.message));

    } else if (topic === '/bags/bagA/tilt') {
      state.bagA.tilt = payload;
      await pool.execute(
        'INSERT INTO tilt_logs (device_id, pitch, roll, flat) VALUES (1,?,?,?)',
        [payload.pitch, payload.roll, payload.flat ? 1 : 0]
      ).catch(e => console.error('[db] tilt_logs:', e.message));

      // ── Tamper detection ──
      detector.observe({
        pitch: payload.pitch,
        roll:  payload.roll,
        flat:  payload.flat,
        open:  state.bagA.status.open,
        grams: state.bagA.weight.grams,
      });
      const tamperResult = await detector.predict();
      if (tamperResult && tamperResult.alert) {
        state.bagA.tamper = { alert: true, confidence: tamperResult.confidence };
        broadcast('/bags/bagA/tamper', state.bagA.tamper);
        await pool.execute(
          'INSERT INTO tamper_alerts (device_id, confidence) VALUES (1,?)',
          [tamperResult.confidence]
        ).catch(e => console.error('[db] tamper_alerts:', e.message));
      } else if (tamperResult && !tamperResult.alert) {
        state.bagA.tamper = { alert: false, confidence: tamperResult.confidence };
      }

    } else if (topic === '/bags/bagA/status') {
      state.bagA.status = payload;
      await pool.execute(
        'INSERT INTO status_logs (device_id, open, magnet_detected) VALUES (1,?,?)',
        [payload.open ? 1 : 0, payload.magnetDetected ? 1 : 0]
      ).catch(e => console.error('[db] status_logs:', e.message));

    } else if (topic === '/bags/bagA/relay') {
      state.bagA.relay = payload;
      isRelayed = true;
      setTimeout(() => { isRelayed = false; }, 10000);

    } else if (topic === '/bags/bagB/heartbeat') {
      state.bagB.heartbeat = payload;
      state.bagB.lastSeen  = now;
      await pool.execute('UPDATE devices SET last_seen=? WHERE id=2', [now]).catch(() => {});
    }

    // Push to all connected dashboards
    broadcast(topic, payload);
  });
}

// ── HTTP ──────────────────────────────────────────────────────────────────────
function startHttp() {
  const app = express();

  app.use(express.static('/app/dashboard'));
  app.use('/api/auth', authRouter);

  // SSE — token passed as query param because EventSource doesn't support headers
  app.get('/api/events', (req, res) => {
    const token = req.query.token;
    if (!token) return res.status(401).end();
    try {
      jwt.verify(token, JWT_SECRET);
    } catch {
      return res.status(401).end();
    }

    res.setHeader('Content-Type',  'text/event-stream');
    res.setHeader('Cache-Control', 'no-cache');
    res.setHeader('Connection',    'keep-alive');
    res.flushHeaders();

    // Send current state immediately so the page isn't blank on load
    const init = [
      { topic: '/bags/bagA/gps',       payload: state.bagA.gps },
      { topic: '/bags/bagA/weight',     payload: state.bagA.weight },
      { topic: '/bags/bagA/tilt',       payload: state.bagA.tilt },
      { topic: '/bags/bagA/status',     payload: state.bagA.status },
      { topic: '/bags/bagB/heartbeat',  payload: state.bagB.heartbeat },
      { topic: '/bags/bagA/tamper',     payload: state.bagA.tamper },
    ];
    for (const { topic, payload } of init) {
      res.write(`data: ${JSON.stringify({ topic, payload })}\n\n`);
    }
    if (isRelayed) {
      res.write(`data: ${JSON.stringify({ topic: '/bags/bagA/relay', payload: { relayedBy: 'bagB' } })}\n\n`);
    }

    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
  });

  // REST endpoints (protected)
  app.get('/api/state', requireAuth, (_req, res) => {
    res.json({ ...state, relayed: isRelayed });
  });

  app.get('/api/history/gps', requireAuth, async (_req, res) => {
    try {
      const [rows] = await pool.execute(
        `SELECT lat, lng, valid, relayed, recorded_at
         FROM gps_logs WHERE device_id=1
         ORDER BY recorded_at DESC LIMIT 200`
      );
      res.json(rows.reverse());
    } catch (e) {
      res.status(500).json({ error: e.message });
    }
  });

  app.get('/api/history/weight', requireAuth, async (_req, res) => {
    try {
      const [rows] = await pool.execute(
        `SELECT grams, recorded_at
         FROM weight_logs WHERE device_id=1
         ORDER BY recorded_at DESC LIMIT 200`
      );
      res.json(rows.reverse());
    } catch (e) {
      res.status(500).json({ error: e.message });
    }
  });

  app.listen(HTTP_PORT, '0.0.0.0', () => {
    console.log(`[http] Listening on port ${HTTP_PORT}`);
  });
}

// ── Entry point ───────────────────────────────────────────────────────────────
(async () => {
  await waitForDb();
  await detector.load().catch(err => {
    console.warn('[detector] Model not loaded (run train_model.js first):', err.message);
  });
  startMqtt();
  startHttp();
})();

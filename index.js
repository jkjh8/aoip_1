import { setPriority } from 'os';
try { setPriority(-20); } catch { /* CAP_SYS_NICE 없으면 무시 */ }

import express from 'express';
import { createServer } from 'http';
import { Server as SocketIO } from 'socket.io';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

import jackRoutes    from './routes/jack.js';
import bridgesRoutes from './routes/bridges.js';
import streamsRoutes from './routes/streams.js';
import gainerRoutes  from './routes/gainer.js';

import { startJack, waitForJack, isJackRunning, checkJackAlive, setJackReady,
         getPorts, getConnections, connect, disconnect, stopJack } from './lib/jack.js';
import { startBridges, stopBridges, getBridgeStatus }              from './lib/bridges.js';
import { startRxPipeline, stopRxPipeline, isRxRunning, setRxPort, getRxPort,
         startTxPipeline, stopTxPipeline, isTxRunning,
         addTxTarget, removeTxTarget, getGstStatus }               from './lib/gstreamer.js';
import { getChannels, setGain, setMute, setLabel,
         setHpf, setEqBand, setLimiter, startMeters,
         getInputSrcPorts, getOutputSinkPorts }                     from './lib/channels.js';
import { startGainer, stopGainer, isGainerRunning,
         sendGain, sendMute, sendHpf, sendEqCoeffs, sendLimiter,
         sendAllDsp, getLimiterMeters }                             from './lib/gainer.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const config = JSON.parse(readFileSync(join(__dirname, './config/audio.json'), 'utf8'));

const PORT            = process.env.PORT ?? 3000;
const STATUS_INTERVAL = 2000;
const LEVEL_INTERVAL  = 80;   // ~12 fps

// ── Express ───────────────────────────────────────────

const app = express();
app.use(express.json());
app.use((_req, res, next) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (_req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

app.use('/jack',    jackRoutes);
app.use('/bridges', bridgesRoutes);
app.use('/streams', streamsRoutes);
app.use('/gainer',  gainerRoutes);

// ── HTTP + Socket.IO ──────────────────────────────────

const httpServer = createServer(app);
const io = new SocketIO(httpServer, {
  cors: { origin: '*', methods: ['GET', 'POST'] }
});

// ── 상태 스냅샷 ───────────────────────────────────────

// 커넥션 캐시 — snapshot()이 2초마다 갱신, 레벨 브로드캐스트가 재사용
let cachedConnections = [];


async function snapshot() {
  const [ports, connections] = isJackRunning()
    ? await Promise.all([getPorts(), getConnections()]).catch(() => [[], []])
    : [[], []];

  cachedConnections = connections;

  return {
    jack:     { running: isJackRunning(), ports, connections },
    bridges:  getBridgeStatus(),
    streams:  getGstStatus(),
    channels: getChannels(connections)
  };
}

async function broadcastStatus() {
  if (io.engine.clientsCount === 0) return;
  try { io.emit('status', await snapshot()); } catch { /* jack not ready */ }
}

// 리미터 미터를 구독 중인 소켓 ID → 채널 id Set
const limiterWatchers = new Map();   // socketId → Set<chId>

function watchedChannels() {
  const ids = new Set();
  for (const set of limiterWatchers.values()) for (const id of set) ids.add(id);
  return ids;
}

// 레벨 미터 — 빠른 주기로 별도 emit (커넥션 캐시 재사용)
setInterval(() => {
  if (io.engine.clientsCount === 0) return;
  const ch = getChannels(cachedConnections);
  const limMeters = getLimiterMeters();
  const watched   = watchedChannels();
  io.emit('levels', {
    inputs:  ch.inputs.map(c  => ({ id: c.id, level: c.level })),
    outputs: ch.outputs.map(c => ({
      id:      c.id,
      level:   c.level,
      limiter: watched.has(c.id) ? (limMeters.get(`out ${c.id}`) ?? null) : undefined
    }))
  });
}, LEVEL_INTERVAL);

// 전체 상태 — 느린 주기
setInterval(broadcastStatus, STATUS_INTERVAL);

// ── Socket.IO 이벤트 ──────────────────────────────────

io.on('connection', async (socket) => {
  console.log('[io] client connected:', socket.id);
  try { socket.emit('status', await snapshot()); } catch { /* ignore */ }

  socket.on('disconnect', () => {
    console.log('[io] disconnected:', socket.id);
    limiterWatchers.delete(socket.id);
  });

  // ── JACK ──
  socket.on('jack:start', async (cb) => {
    try {
      if (isJackRunning()) return cb?.({ ok: false, error: 'already running' });
      startJack();
      await waitForJack();
      startMeters();
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('jack:stop', async (cb) => {
    try {
      stopJack();
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('jack:connect', async ({ src, dst } = {}, cb) => {
    try { await connect(src, dst); await broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('jack:disconnect', async ({ src, dst } = {}, cb) => {
    try { await disconnect(src, dst); await broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── Bridges ──
  socket.on('bridges:start', async (cb) => {
    try { await startBridges(config.bridges); await broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });
  socket.on('bridges:stop', async (cb) => {
    try { stopBridges(); await broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── Streams ──
  socket.on('rx:start', (cb) => {
    try {
      if (isRxRunning()) return cb?.({ ok: false, error: 'already running' });
      startRxPipeline(config.rtp.input); broadcastStatus(); cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
  socket.on('rx:stop',  (cb) => { try { stopRxPipeline();  broadcastStatus(); cb?.({ ok: true }); } catch (e) { cb?.({ ok: false, error: e.message }); } });
  socket.on('rx:port', ({ port } = {}, cb) => {
    try {
      const was = isRxRunning();
      if (was) stopRxPipeline();
      setRxPort(port);
      if (was) startRxPipeline({});
      broadcastStatus();
      cb?.({ ok: true, port: getRxPort(), restarted: was });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
  socket.on('tx:start', (cb) => {
    try {
      if (isTxRunning()) return cb?.({ ok: false, error: 'already running' });
      startTxPipeline(getGstStatus().tx.targets); broadcastStatus(); cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
  socket.on('tx:stop',          (cb)               => { try { stopTxPipeline(); broadcastStatus(); cb?.({ ok: true }); } catch (e) { cb?.({ ok: false, error: e.message }); } });
  socket.on('tx:target:add',    ({ host, port } = {}, cb) => { try { addTxTarget({ host, port: Number(port) }); broadcastStatus(); cb?.({ ok: true, targets: getGstStatus().tx.targets }); } catch (e) { cb?.({ ok: false, error: e.message }); } });
  socket.on('tx:target:remove', ({ host, port } = {}, cb) => { try { removeTxTarget({ host, port: Number(port) }); broadcastStatus(); cb?.({ ok: true, targets: getGstStatus().tx.targets }); } catch (e) { cb?.({ ok: false, error: e.message }); } });

  // ── Channel gain / mute / label ──
  socket.on('ch:gain', async ({ type, id, gain } = {}, cb) => {
    try {
      setGain(type, id, gain);
      if (isGainerRunning()) sendGain(type === 'input' ? 'in' : 'out', id, gain);
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('ch:mute', async ({ type, id, muted } = {}, cb) => {
    try {
      setMute(type, id, muted);
      if (isGainerRunning()) sendMute(type === 'input' ? 'in' : 'out', id, muted);
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
  socket.on('ch:label', ({ type, id, label } = {}, cb) => { try { setLabel(type, id, label); broadcastStatus(); cb?.({ ok: true }); } catch (e) { cb?.({ ok: false, error: e.message }); } });

  // ── Gainer bypass (테스트용) ──
  socket.on('gainer:bypass', async (cb) => {
    try {
      stopGainer();
      await new Promise(r => setTimeout(r, 500)); // 포트 사라질 때까지 대기
      const srcPorts  = getInputSrcPorts();
      const sinkPorts = getOutputSinkPorts();
      const n = Math.min(srcPorts.length, sinkPorts.length);
      for (let i = 0; i < n; i++) {
        try { await connect(srcPorts[i], sinkPorts[i]); } catch { /* ignore */ }
      }
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('gainer:restore', async (cb) => {
    try {
      const srcPorts  = getInputSrcPorts();
      const sinkPorts = getOutputSinkPorts();
      // 바이패스 연결 해제
      const n = Math.min(srcPorts.length, sinkPorts.length);
      for (let i = 0; i < n; i++) {
        try { await disconnect(srcPorts[i], sinkPorts[i]); } catch { /* ignore */ }
      }
      // gainer 재시작 및 연결 복원
      startGainer(srcPorts.length, sinkPorts.length);
      await new Promise(r => setTimeout(r, 1000));
      for (let i = 0; i < srcPorts.length; i++)
        try { await connect(srcPorts[i], `gainer:in_${i + 1}`); } catch { /* ignore */ }
      for (let i = 0; i < sinkPorts.length; i++)
        try { await connect(`gainer:sout_${i + 1}`, sinkPorts[i]); } catch { /* ignore */ }
      // DSP 상태 복원
      const { inputs, outputs } = getChannels([]);
      for (const ch of inputs)  { sendGain('in',  ch.id, ch.gain); if (ch.muted) sendMute('in',  ch.id, true); }
      for (const ch of outputs) { sendGain('out', ch.id, ch.gain); if (ch.muted) sendMute('out', ch.id, true); }
      sendAllDsp({ inputs, outputs });
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── DSP ──
  const hpfTimers  = new Map();
  const hpfPending = new Map();

  socket.on('dsp:hpf', ({ id, ...params } = {}, cb) => {
    try {
      setHpf(id, params);
      io.emit('channels', getChannels(cachedConnections));
      cb?.({ ok: true });

      if (isGainerRunning()) {
        hpfPending.set(id, { id, params });
        clearTimeout(hpfTimers.get(id));
        hpfTimers.set(id, setTimeout(() => {
          hpfTimers.delete(id);
          const p = hpfPending.get(id);
          hpfPending.delete(id);
          if (p && isGainerRunning()) sendHpf(p.id, p.params);
        }, 60));
      }
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* EQ 디바운스: 슬라이더를 빠르게 움직일 때 DSP 전송은 마지막 값만 보냄 (60ms) */
  const eqTimers  = new Map();
  const eqPending = new Map();

  socket.on('dsp:eq', ({ type, id, band, ...params } = {}, cb) => {
    try {
      setEqBand(type, id, band, params);   /* 상태 저장은 즉시 */
      io.emit('channels', getChannels(cachedConnections));
      cb?.({ ok: true });

      if (isGainerRunning()) {
        const key = `${type}:${id}:${band}`;
        eqPending.set(key, { type, id, band, params });
        clearTimeout(eqTimers.get(key));
        eqTimers.set(key, setTimeout(() => {
          eqTimers.delete(key);
          const p = eqPending.get(key);
          eqPending.delete(key);
          if (p && isGainerRunning())
            sendEqCoeffs(p.type === 'input' ? 'in' : 'out', p.id, p.band, p.params);
        }, 60));
      }
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* limiter:watch { id, watch: true|false } — 리미터 창 열림/닫힘 */
  socket.on('limiter:watch', ({ id, watch } = {}) => {
    const chId = Number(id);
    if (!limiterWatchers.has(socket.id)) limiterWatchers.set(socket.id, new Set());
    const set = limiterWatchers.get(socket.id);
    watch ? set.add(chId) : set.delete(chId);
  });

  socket.on('dsp:limiter', ({ id, ...params } = {}, cb) => {
    try {
      setLimiter(id, params);
      if (isGainerRunning()) sendLimiter(id, params);
      io.emit('channels', getChannels(cachedConnections)); cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

});

// ── Startup ───────────────────────────────────────────

async function startup() {
  // JACK: 이미 외부에서 실행 중이면 jackd 재시작 없이 그대로 사용
  if (await checkJackAlive()) {
    console.log('[startup] JACK already running — skipping jackd start');
    setJackReady(true);
  } else {
    console.log('[startup] Starting jackd...');
    startJack();
    try {
      await waitForJack();
    } catch (e) {
      console.warn('[startup] waitForJack failed:', e.message, '— continuing without JACK');
    }
  }

  const srcPorts  = getInputSrcPorts();
  const sinkPorts = getOutputSinkPorts();

  console.log('[startup] Starting gainer (in=%d out=%d)...', srcPorts.length, sinkPorts.length);
  try { startGainer(srcPorts.length, sinkPorts.length); } catch (e) { console.warn('[startup] gainer:', e.message); }

  console.log('[startup] Starting ALSA bridges...');
  try { await startBridges(config.bridges); } catch (e) { console.warn('[startup] bridges:', e.message); }

  // gainer + 브릿지 포트 등록 대기 후 연결 (재시도 포함)
  await new Promise(r => setTimeout(r, 2000));

  async function connectWithRetry(src, dst, retries = 5) {
    for (let i = 0; i < retries; i++) {
      try { await connect(src, dst); return; } catch (e) {
        if (i < retries - 1) await new Promise(r => setTimeout(r, 1000));
        else console.warn('[startup] connect %s→%s failed: %s', src, dst, e.message);
      }
    }
  }

  console.log('[startup] Connecting src → gainer inputs...');
  for (let i = 0; i < srcPorts.length; i++)
    await connectWithRetry(srcPorts[i], `gainer:in_${i + 1}`);

  console.log('[startup] Connecting gainer outputs → sinks...');
  for (let i = 0; i < sinkPorts.length; i++)
    await connectWithRetry(`gainer:sout_${i + 1}`, sinkPorts[i]);

  // 저장된 gain/mute/DSP 상태를 dsp_engine에 복원
  const { inputs, outputs } = getChannels([]);
  for (const ch of inputs)  { sendGain('in',  ch.id, ch.gain); if (ch.muted) sendMute('in',  ch.id, true); }
  for (const ch of outputs) { sendGain('out', ch.id, ch.gain); if (ch.muted) sendMute('out', ch.id, true); }
  sendAllDsp({ inputs, outputs });

  console.log('[startup] Starting GStreamer RTP input...');
  try { startRxPipeline(config.rtp.input); } catch (e) { console.warn('[startup] gst rx:', e.message); }

  console.log('[startup] Starting channel meters...');
  try { startMeters(); } catch (e) { console.warn('[startup] meters:', e.message); }

  httpServer.listen(PORT, () => console.log(`[server] http://localhost:${PORT}`));
}

startup().catch((err) => {
  console.error('[startup] Fatal:', err.message);
});

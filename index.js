import { setPriority } from 'os';
try { setPriority(-20); } catch { /* CAP_SYS_NICE 없으면 무시 */ }

import express from 'express';
import { createServer } from 'http';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

import jackRoutes    from './routes/jack.js';
import bridgesRoutes from './routes/bridges.js';
import streamsRoutes from './routes/streams.js';
import gainerRoutes  from './routes/gainer.js';
import systemRoutes  from './routes/system.js';

import { setupSocket } from './socket/index.js';
import logger from './lib/logger.js';

import { startJack, waitForJack, checkJackAlive, killExistingJack,
         setJackReady, connect } from './lib/jack.js';
import { startBridges, startUsbGadgetWatcher }       from './lib/bridges.js';
import { startRxPipeline, startTxClient,
         waitForRxReady, waitForTxReady,
         startRtpStreams, waitForRtpStreamsReady,
         getRtpStreamStatus }                         from './lib/gstreamer.js';
import { getChannels, startMeters,
         getInputSrcPorts, getOutputSinkPorts,
         getTotalInputCount, getTotalOutputCount,
         getSavedRoutes }                            from './lib/channels.js';
import { startGainer, sendGain, sendMute,
         sendBypass, sendAllDsp }                    from './lib/gainer.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const config    = JSON.parse(readFileSync(join(__dirname, './config/audio.json'), 'utf8'));

const PORT = process.env.PORT ?? 3000;

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
app.use('/system',  systemRoutes);

// ── SPA 정적 파일 서빙 ────────────────────────────────
const SPA_DIR = join(__dirname, 'public/spa');
app.use(express.static(SPA_DIR));
app.get('*', (_req, res) => res.sendFile(join(SPA_DIR, 'index.html')));

// ── HTTP + Socket.IO ──────────────────────────────────

const httpServer = createServer(app);
setupSocket(httpServer, config);

// ── Startup ───────────────────────────────────────────

async function startup() {
  if (await checkJackAlive()) {
    logger.info('[startup] JACK already running — reusing existing instance');
    setJackReady(true);
  } else {
    await killExistingJack();
    logger.info('[startup] Starting jackd...');
    startJack();
    try {
      await waitForJack();
    } catch (e) {
      logger.error('[startup] waitForJack failed: %s — aborting startup', e.message);
      httpServer.listen(PORT, () => logger.info(`[server] http://localhost:${PORT}`));
      return;
    }
  }

  const srcPorts  = getInputSrcPorts();
  const sinkPorts = getOutputSinkPorts();

  const totalIn  = getTotalInputCount();
  const totalOut = getTotalOutputCount();
  logger.info('[startup] Starting gainer (in=%d out=%d, active in=%d out=%d)...', totalIn, totalOut, srcPorts.length, sinkPorts.length);
  try { startGainer(totalIn, totalOut); } catch (e) { logger.warn('[startup] gainer:', e.message); }

  logger.info('[startup] Starting ALSA bridges...');
  try { await startBridges(config.bridges); } catch (e) { logger.warn('[startup] bridges:', e.message); }
  startUsbGadgetWatcher();

  // rtp_recv / rtp_send 를 포트 연결 전에 먼저 기동
  if (config.rtp_streams?.length) {
    logger.info('[startup] Starting RTP streams (%d)...', config.rtp_streams.length);
    try { startRtpStreams(config.rtp_streams); } catch (e) { logger.warn('[startup] rtp_streams:', e.message); }

    // 포트 등록 대기
    await new Promise(r => setTimeout(r, 2000));
    try { await waitForRtpStreamsReady(6000); }
    catch (e) { logger.warn('[startup] rtp_streams ready timeout: %s', e.message); }
  } else if (config.rtp) {
    // 레거시 단일 rtp 설정
    logger.info('[startup] Starting GStreamer RTP input...');
    try { startRxPipeline(config.rtp.input); } catch (e) { logger.warn('[startup] gst rx:', e.message); }
    logger.info('[startup] Starting rtp_send JACK client...');
    try { startTxClient({ channels: 2 }); } catch (e) { logger.warn('[startup] rtp_send:', e.message); }

    await new Promise(r => setTimeout(r, 2000));
    try { await Promise.all([waitForRxReady(6000), waitForTxReady(6000)]); }
    catch (e) { logger.warn('[startup] rtp ready timeout: %s', e.message); }
  } else {
    await new Promise(r => setTimeout(r, 2000));
  }

  async function connectWithRetry(src, dst, retries = 5) {
    for (let i = 0; i < retries; i++) {
      try { await connect(src, dst); return; } catch (e) {
        if (i < retries - 1) await new Promise(r => setTimeout(r, 1000));
        else logger.warn('[startup] connect %s→%s failed: %s', src, dst, e.message);
      }
    }
  }

  // rtp_in 실제 JACK 포트명 맵 (config client명 → 실제 등록 포트 목록)
  const rtpPortMap = new Map();
  for (const s of getRtpStreamStatus()) {
    if (s.type === 'rtp_in' && s.ports.length)
      rtpPortMap.set(s.client, s.ports);
  }

  logger.info('[startup] Connecting src → gainer inputs...');
  for (const { id, srcPort } of srcPorts) {
    let src = srcPort;
    // rtp_in 포트: config의 `client:out_N` 대신 실제 등록된 포트명 사용
    const m = src.match(/^([^:]+):out_(\d+)$/);
    if (m && rtpPortMap.has(m[1])) {
      const actual = rtpPortMap.get(m[1])[Number(m[2]) - 1];
      if (actual) src = actual;
    }
    await connectWithRetry(src, `gainer:in_${id}`);
  }

  logger.info('[startup] Connecting gainer outputs → sinks...');
  for (const { id, sinkPort } of sinkPorts)
    await connectWithRetry(`gainer:sout_${id}`, sinkPort);

  // 저장된 라우팅 매트릭스 복원 — 현재 활성 채널의 포트만 연결
  const savedRoutes = getSavedRoutes();
  if (savedRoutes.length > 0) {
    const { inputs: activeIn, outputs: activeOut } = getChannels([]);
    const validSrcs = new Set(activeIn.map(ch => ch.jackPort));   // gainer:out_N
    const validDsts = new Set(activeOut.map(ch => ch.jackPort));  // gainer:sin_N

    logger.info('[startup] Restoring %d saved routes...', savedRoutes.length);
    for (const { src, dst } of savedRoutes) {
      if (!validSrcs.has(src) || !validDsts.has(dst)) {
        logger.info('[startup] skipping route %s→%s (port not active)', src, dst);
        continue;
      }
      await connectWithRetry(src, dst);
    }
  }

  // 저장된 gain/mute/DSP 상태를 dsp_engine에 복원
  const { inputs, outputs } = getChannels([]);
  for (const ch of inputs) {
    if (ch.bypassDsp) sendBypass('in', ch.id, true);
    sendGain('in', ch.id, ch.gain);
    if (ch.muted) sendMute('in', ch.id, true);
  }
  for (const ch of outputs) {
    if (ch.bypassDsp) sendBypass('out', ch.id, true);
    sendGain('out', ch.id, ch.gain);
    if (ch.muted) sendMute('out', ch.id, true);
  }
  sendAllDsp({ inputs, outputs });

  logger.info('[startup] Starting channel meters...');
  try { startMeters(); } catch (e) { logger.warn('[startup] meters:', e.message); }

  httpServer.listen(PORT, () => logger.info(`[server] http://localhost:${PORT}`));
}

startup().catch((err) => {
  logger.error('[startup] Fatal:', err.message);
});

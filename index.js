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

import { setupSocket } from './socket/index.js';
import logger from './lib/logger.js';

import { startJack, waitForJack, checkJackAlive, killExistingJack,
         setJackReady, connect } from './lib/jack.js';
import { startBridges }                              from './lib/bridges.js';
import { startRxPipeline, startTxClient,
         waitForRxReady, waitForTxReady }             from './lib/gstreamer.js';
import { getChannels, startMeters,
         getInputSrcPorts, getOutputSinkPorts,
         getSavedRoutes }                            from './lib/channels.js';
import { startGainer, sendGain, sendMute,
         sendAllDsp }                                from './lib/gainer.js';

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

  logger.info('[startup] Starting gainer (in=%d out=%d)...', srcPorts.length, sinkPorts.length);
  try { startGainer(srcPorts.length, sinkPorts.length); } catch (e) { logger.warn('[startup] gainer:', e.message); }

  logger.info('[startup] Starting ALSA bridges...');
  try { await startBridges(config.bridges); } catch (e) { logger.warn('[startup] bridges:', e.message); }

  // rtp_recv / rtp_send 를 포트 연결 전에 먼저 기동
  logger.info('[startup] Starting GStreamer RTP input...');
  try { startRxPipeline(config.rtp.input); } catch (e) { logger.warn('[startup] gst rx:', e.message); }
  logger.info('[startup] Starting rtp_send JACK client...');
  try { startTxClient({ channels: 2 }); } catch (e) { logger.warn('[startup] rtp_send:', e.message); }

  // gainer + 브릿지 + rtp 포트 등록 대기
  await new Promise(r => setTimeout(r, 2000));
  try { await Promise.all([waitForRxReady(6000), waitForTxReady(6000)]); }
  catch (e) { logger.warn('[startup] rtp ready timeout: %s', e.message); }

  async function connectWithRetry(src, dst, retries = 5) {
    for (let i = 0; i < retries; i++) {
      try { await connect(src, dst); return; } catch (e) {
        if (i < retries - 1) await new Promise(r => setTimeout(r, 1000));
        else logger.warn('[startup] connect %s→%s failed: %s', src, dst, e.message);
      }
    }
  }

  logger.info('[startup] Connecting src → gainer inputs...');
  for (let i = 0; i < srcPorts.length; i++)
    await connectWithRetry(srcPorts[i], `gainer:in_${i + 1}`);

  logger.info('[startup] Connecting gainer outputs → sinks...');
  for (let i = 0; i < sinkPorts.length; i++)
    await connectWithRetry(`gainer:sout_${i + 1}`, sinkPorts[i]);

  // 저장된 라우팅 매트릭스 복원
  const savedRoutes = getSavedRoutes();
  if (savedRoutes.length > 0) {
    logger.info('[startup] Restoring %d saved routes...', savedRoutes.length);
    for (const { src, dst } of savedRoutes) {
      await connectWithRetry(src, dst);
    }
  }

  // 저장된 gain/mute/DSP 상태를 dsp_engine에 복원
  const { inputs, outputs } = getChannels([]);
  for (const ch of inputs)  { sendGain('in',  ch.id, ch.gain); if (ch.muted) sendMute('in',  ch.id, true); }
  for (const ch of outputs) { sendGain('out', ch.id, ch.gain); if (ch.muted) sendMute('out', ch.id, true); }
  sendAllDsp({ inputs, outputs });

  logger.info('[startup] Starting channel meters...');
  try { startMeters(); } catch (e) { logger.warn('[startup] meters:', e.message); }

  httpServer.listen(PORT, () => logger.info(`[server] http://localhost:${PORT}`));
}

startup().catch((err) => {
  logger.error('[startup] Fatal:', err.message);
});

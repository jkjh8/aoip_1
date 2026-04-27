import { setPriority } from 'os';
try { setPriority(-20); } catch { /* CAP_SYS_NICE 없으면 무시 */ }

import express from 'express';
import { createServer } from 'http';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import httpLogger from 'morgan';
import cookieParser from 'cookie-parser'

import bridgesRoutes from './routes/bridges.js';
import streamsRoutes from './routes/streams.js';
import dspRoutes     from './routes/dsp.js';
import systemRoutes  from './routes/system.js';
import aes67Routes   from './routes/aes67.js';

import { setupSocket } from './socket/index.js';
import logger from './lib/logger.js';

import { connect }                                                                from './lib/jack.js';
import { startBridges, stopBridges, startUsbGadgetWatcher, killOrphanBridges,
         notifyBridgeStartupComplete }                                            from './lib/bridges.js';
import { startRxPipeline, startTxClient,
         waitForRxReady, waitForTxReady,
         startRtpStreams, waitForRtpStreamsReady,
         getRtpStreamStatus,
         notifyRtpStartupComplete }                   from './lib/gstreamer.js';
import { getChannels,
         getSavedRoutes,
         getDspChannelCounts }                       from './lib/channels.js';
import { startDsp, sendGain, sendMute,
         sendBypass, sendAllDsp,
         sendToEngine, waitForDspReady }             from './lib/dsp.js';
import { getConfig } from './lib/config.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const config    = getConfig();

const PORT = process.env.PORT ?? 3000;

// ── Express ───────────────────────────────────────────

const app = express();
app.use(express.json());
app.use(express.urlencoded({ extended: false }))
app.use(cookieParser())
app.use((_req, res, next) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (_req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

app.use(httpLogger('dev'))

app.use('/bridges', bridgesRoutes);
app.use('/streams', streamsRoutes);
app.use('/dsp',     dspRoutes);
app.use('/system',  systemRoutes);
app.use('/aes67',   aes67Routes);

// ── SPA 정적 파일 서빙 ────────────────────────────────
const SPA_DIR = join(__dirname, 'public/spa');
app.use(express.static(SPA_DIR));
app.get('*', (_req, res) => res.sendFile(join(SPA_DIR, 'index.html')));

// ── HTTP + Socket.IO ──────────────────────────────────

const httpServer = createServer(app);
setupSocket(httpServer, config);

// ── Startup ───────────────────────────────────────────

async function connectWithRetry(src, dst, retries = 5) {
  for (let i = 0; i < retries; i++) {
    try { await connect(src, dst); return true; } catch (e) {
      if (i < retries - 1) await new Promise(r => setTimeout(r, 1000));
      else logger.warn('[startup] connect %s→%s failed: %s', src, dst, e.message);
    }
  }
  return false;
}


async function startup() {
  /* ── aoip_engine 시작 (JACK 대체 단일 C 데몬) ── */
  const dspCounts = getDspChannelCounts();
  logger.info('[startup] Starting aoip_engine: %s',
    [...dspCounts.entries()].map(([n, c]) => `${n}(${c.n_in}in/${c.n_out}out)`).join(', '));
  for (const [name, { n_in, n_out }] of dspCounts) {
    try { startDsp(name, n_in, n_out); } catch (e) { logger.warn('[startup] dsp %s:', name, e.message); }
  }

  /* ── aoip_engine ready 대기 ── */
  try { await Promise.all([...dspCounts.keys()].map(name => waitForDspReady(name))); }
  catch (e) { logger.warn('[startup] aoip_engine ready timeout: %s', e.message); }

  /* ── 메인 아날로그 장치 bridge 등록 (config.jack) ── */
  const jackCfg = config.jack;
  if (jackCfg?.device) {
    const ch = jackCfg.channels ?? 2;
    const cmd = `bridge add analog ${jackCfg.device} ${jackCfg.rate ?? 48000} ${jackCfg.period ?? 512} ${jackCfg.periods ?? 3} ${ch} 0`;
    logger.info('[startup] Registering analog bridge: %s', cmd);
    sendToEngine(cmd);
  }

  /* ── ALSA 브릿지 등록 ── */
  logger.info('[startup] Registering ALSA bridges...');
  await killOrphanBridges();
  try { await startBridges(config.bridges); } catch (e) { logger.warn('[startup] bridges:', e.message); }
  startUsbGadgetWatcher();

  /* ── RTP 스트림 기동 ── */
  if (config.rtp_streams?.length) {
    logger.info('[startup] Starting RTP streams (%d)...', config.rtp_streams.length);
    try { startRtpStreams(config.rtp_streams); } catch (e) { logger.warn('[startup] rtp_streams:', e.message); }
    try { await waitForRtpStreamsReady(6000); }
    catch (e) { logger.warn('[startup] rtp_streams ready timeout: %s', e.message); }
  } else if (config.rtp) {
    logger.info('[startup] Starting GStreamer RTP (legacy)...');
    try { startRxPipeline(config.rtp.input); } catch (e) { logger.warn('[startup] gst rx:', e.message); }
    try { startTxClient({ channels: 2 }); } catch (e) { logger.warn('[startup] rtp_send:', e.message); }
    try { await Promise.all([waitForRxReady(6000), waitForTxReady(6000)]); }
    catch (e) { logger.warn('[startup] rtp ready timeout: %s', e.message); }
  }

  /* ── 저장된 라우팅 매트릭스 복원 ── */
  const savedRoutes = getSavedRoutes();
  if (savedRoutes.length > 0) {
    const { inputs: activeIn, outputs: activeOut } = getChannels([]);
    const validSrcs = new Set(activeIn.map(ch => ch.jackPort));
    const validDsts = new Set(activeOut.map(ch => ch.jackPort));
    logger.info('[startup] Restoring %d saved routes...', savedRoutes.length);
    for (const { src, dst } of savedRoutes) {
      if (!validSrcs.has(src) || !validDsts.has(dst)) continue;
      await connectWithRetry(src, dst);  /* → jack.js stub → dsp.js route add */
    }
  }

  /* ── 저장된 gain/mute/DSP 상태 복원 ── */
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

  notifyRtpStartupComplete();
  notifyBridgeStartupComplete();

  httpServer.listen(PORT, () => logger.info(`[server] http://localhost:${PORT}`));
}

startup().catch((err) => {
  logger.error('[startup] Fatal:', err.message);
});

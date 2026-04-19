import { setPriority } from 'os';
try { setPriority(-20); } catch { /* CAP_SYS_NICE 없으면 무시 */ }

import express from 'express';
import { createServer } from 'http';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import httpLogger from 'morgan';
import cookieParser from 'cookie-parser'

import jackRoutes    from './routes/jack.js';
import bridgesRoutes from './routes/bridges.js';
import streamsRoutes from './routes/streams.js';
import dspRoutes     from './routes/dsp.js';
import systemRoutes  from './routes/system.js';
import aes67Routes   from './routes/aes67.js';

import { setupSocket } from './socket/index.js';
import logger from './lib/logger.js';

import { startJack, waitForJack, checkJackAlive, killExistingJack,
         setJackReady, connect,
         setJackCrashHandler, setJackRecoverHandler } from './lib/jack.js';
import { startBridges, stopBridges, startUsbGadgetWatcher, killOrphanBridges,
         notifyBridgeStartupComplete, setOnBridgeReady,
         restartBridge }                                                         from './lib/bridges.js';
import { startRxPipeline, startTxClient,
         waitForRxReady, waitForTxReady,
         startRtpStreams, waitForRtpStreamsReady,
         getRtpStreamStatus,
         notifyRtpStartupComplete }                   from './lib/gstreamer.js';
import { getChannels, startMeters,
         getInputSrcPorts, getOutputSinkPorts,
         getSavedRoutes, getBridgeChannelDef,
         getDspClientOf, getDspLocalId,
         getDspChannelCounts }                       from './lib/channels.js';
import { startDsp, stopDsp, sendGain, sendMute,
         sendBypass, sendAllDsp,
         waitForDspReady }                           from './lib/dsp.js';
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

app.use('/jack',    jackRoutes);
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

async function connectBridgePorts(bridgeName) {
  const def = getBridgeChannelDef(bridgeName);
  if (!def) return;
  for (const ch of def.inputs)
    await connectWithRetry(ch.srcPort, `${getDspClientOf(ch.id,'in')}:in_${getDspLocalId(ch.id,'in')}`, 3);
  for (const ch of def.outputs)
    await connectWithRetry(`${getDspClientOf(ch.id,'out')}:sout_${getDspLocalId(ch.id,'out')}`, ch.sinkPort, 3);
}

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

  const dspCounts = getDspChannelCounts();
  logger.info('[startup] Starting DSP engines: %s',
    [...dspCounts.entries()].map(([n, c]) => `${n}(${c.n_in}in/${c.n_out}out)`).join(', '));
  for (const [name, { n_in, n_out }] of dspCounts) {
    try { startDsp(name, n_in, n_out); } catch (e) { logger.warn('[startup] dsp %s:', name, e.message); }
  }
  try { await Promise.all([...dspCounts.keys()].map(name => waitForDspReady(name))); }
  catch (e) { logger.warn('[startup] dsp ready timeout: %s', e.message); }

  logger.info('[startup] Starting ALSA bridges...');
  await killOrphanBridges();
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

  const connectedSrcIds  = new Set();
  const connectedSinkIds = new Set();

  // rtp_in 실제 JACK 포트명 맵 (config client명 → 실제 등록 포트 목록)
  const rtpPortMap = new Map();
  for (const s of getRtpStreamStatus()) {
    if (s.type === 'rtp_in' && s.ports.length)
      rtpPortMap.set(s.client, s.ports);
  }

  logger.info('[startup] Connecting src → DSP inputs...');
  for (const { id, srcPort, noRetry, usbGadget } of srcPorts) {
    let src = srcPort;
    // rtp_in 포트: config의 `client:out_N` 대신 실제 등록된 포트명 사용
    const m = src.match(/^([^:]+):out_(\d+)$/);
    if (m && rtpPortMap.has(m[1])) {
      const actual = rtpPortMap.get(m[1])[Number(m[2]) - 1];
      if (actual) src = actual;
    }
    const ok = await connectWithRetry(src, `${getDspClientOf(id,'in')}:in_${getDspLocalId(id,'in')}`, noRetry ? 1 : 5);
    if (ok && noRetry && !usbGadget) connectedSrcIds.add(id);
  }

  logger.info('[startup] Connecting DSP outputs → sinks...');
  for (const { id, sinkPort, noRetry, usbGadget } of sinkPorts) {
    const ok = await connectWithRetry(`${getDspClientOf(id,'out')}:sout_${getDspLocalId(id,'out')}`, sinkPort, noRetry ? 1 : 5);
    if (ok && noRetry && !usbGadget) connectedSinkIds.add(id);
  }

  // 저장된 라우팅 매트릭스 복원 — 현재 활성 채널의 포트만 연결
  const savedRoutes = getSavedRoutes();
  if (savedRoutes.length > 0) {
    const { inputs: activeIn, outputs: activeOut } = getChannels([]);
    const validSrcs = new Set(activeIn.map(ch => ch.jackPort));
    const validDsts = new Set(activeOut.map(ch => ch.jackPort));

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

  notifyRtpStartupComplete();
  notifyBridgeStartupComplete();

  // zita 브릿지가 재시작 후 안정화되면 포트 자동 재연결
  setOnBridgeReady(async (bridgeName) => {
    logger.info('[startup] bridge %s ready — reconnecting JACK ports', bridgeName);
    await connectBridgePorts(bridgeName);
  });

  // AES67 브리지 포트 연결 재시도 + 스턱 감지 시 브리지 재시작
  const aes67Bridges = (config.bridges ?? []).filter(b => b.aes67 && b.enabled !== false);
  const bridgeFailCount = Object.fromEntries(aes67Bridges.map(b => [b.name, 0]));

  const retryNoRetryPorts = async () => {
    const pendingSrc  = srcPorts.filter(p => p.noRetry && !p.usbGadget && !connectedSrcIds.has(p.id));
    const pendingSink = sinkPorts.filter(p => p.noRetry && !p.usbGadget && !connectedSinkIds.has(p.id));
    if (!pendingSrc.length && !pendingSink.length) return;
    logger.info('[startup] delayed reconnect: checking %d noRetry ports...', pendingSrc.length + pendingSink.length);
    for (const { id, srcPort } of pendingSrc) {
      const ok = await connectWithRetry(srcPort, `${getDspClientOf(id,'in')}:in_${getDspLocalId(id,'in')}`, 1);
      if (ok) connectedSrcIds.add(id);
    }
    for (const { id, sinkPort } of pendingSink) {
      const ok = await connectWithRetry(`${getDspClientOf(id,'out')}:sout_${getDspLocalId(id,'out')}`, sinkPort, 1);
      if (ok) connectedSinkIds.add(id);
    }

    // AES67 브리지가 45s 이상 포트 등록을 못하면 강제 재시작
    for (const b of aes67Bridges) {
      const def = getBridgeChannelDef(b.name);
      if (!def) continue;
      const allDone = def.inputs.every(ch => connectedSrcIds.has(ch.id)) &&
                      def.outputs.every(ch => connectedSinkIds.has(ch.id));
      if (allDone) { bridgeFailCount[b.name] = 0; continue; }
      bridgeFailCount[b.name]++;
      if (bridgeFailCount[b.name] >= 3) {
        logger.warn('[startup] %s stuck (%d retries) — force restart', b.name, bridgeFailCount[b.name]);
        restartBridge(b.name);
        bridgeFailCount[b.name] = 0;
      }
    }

    setTimeout(retryNoRetryPorts, 15000);
  };
  setTimeout(retryNoRetryPorts, 15000);

  httpServer.listen(PORT, () => logger.info(`[server] http://localhost:${PORT}`));

  // ── JACK 크래시/복구 핸들러 — startup 완료 후 등록 ──────────
  setJackCrashHandler(() => {
    logger.warn('[startup] JACK crashed — stopping DSP and bridges');
    stopDsp();   // 인자 없으면 전체 종료
    stopBridges();
  });

  setJackRecoverHandler(async () => {
    logger.info('[startup] JACK recovered — restarting DSP and bridges');
    const { inputs, outputs } = getChannels([]);
    for (const [name, { n_in, n_out }] of getDspChannelCounts())
      startDsp(name, n_in, n_out);
    await new Promise(r => setTimeout(r, 1500)); // DSP 안정화 대기
    await startBridges(getConfig().bridges);
    // 포트 재연결
    for (const { id, srcPort, noRetry } of getInputSrcPorts())
      await connectWithRetry(srcPort, `${getDspClientOf(id,'in')}:in_${getDspLocalId(id,'in')}`, noRetry ? 1 : 5);
    for (const { id, sinkPort, noRetry } of getOutputSinkPorts())
      await connectWithRetry(`${getDspClientOf(id,'out')}:sout_${getDspLocalId(id,'out')}`, sinkPort, noRetry ? 1 : 5);
    // DSP 상태 복원
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
    logger.info('[startup] JACK recovery complete');
  });
}

startup().catch((err) => {
  logger.error('[startup] Fatal:', err.message);
});

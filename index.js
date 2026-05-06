import express from 'express';
import { createServer } from 'http';
import { execFile } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import httpLogger from 'morgan';
import cookieParser from 'cookie-parser';

// systemd watchdog: notify every 10s so WatchdogSec=30 kills us if event loop hangs
function _sdNotify(msg) {
  if (!process.env.NOTIFY_SOCKET) return;
  execFile('systemd-notify', [msg], () => {});
}
setInterval(() => _sdNotify('WATCHDOG=1'), 10_000).unref();

import apiRoutes from './routes/index.js';
import { setupSocket } from './socket/index.js';
import logger from './lib/logger.js';
import { getConfig, reloadConfig } from './lib/config.js';
import { getDspChannelCounts, restoreRoutes, restoreDspState, syncAes67Active } from './lib/channels/index.js';
import { startupDsp, registerAnalogBridge, waitForDspReady, addEngineRestartListener, sendToEngine, markStartupDone } from './lib/dsp/index.js';
import { startupBridges, reregisterBridges } from './lib/bridges.js';
import { startupRtp } from './lib/rtp/index.js';
import { getDaemonStatus, getSinks, getSources, startDaemonLogForwarder, startStatusFileWatcher, refreshDaemonNetworkConf } from './lib/aes67daemon.js'
import { startSerial } from './lib/serial/index.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const config    = getConfig();

const PORT = process.env.PORT ?? 3000;

// ── Express ───────────────────────────────────────────

const app = express();
app.use(express.json());
app.use(express.urlencoded({ extended: false }));
app.use(cookieParser());
app.use((_req, res, next) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (_req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});
app.use(httpLogger('dev'));
app.use('/api', apiRoutes);

// ── SPA 정적 파일 서빙 ────────────────────────────────
const SPA_DIR = join(__dirname, 'public/spa');
app.use(express.static(SPA_DIR));
app.get('*', (_req, res) => res.sendFile(join(SPA_DIR, 'index.html')));

// ── HTTP + Socket.IO ──────────────────────────────────

const httpServer = createServer(app);
const { broadcastChannels } = setupSocket(httpServer, config);
httpServer.listen(PORT, () => logger.info(`[server] http://localhost:${PORT}`));

// ── Startup ───────────────────────────────────────────

async function startup() {
  await refreshDaemonNetworkConf();
  const dspCounts = getDspChannelCounts();
  // Reserve spare DSP channels for dynamic stream creation (8 streams × 2ch)
  const sc = dspCounts.get('stream') ?? { n_in: 0, n_out: 0 };
  dspCounts.set('stream', { n_in: sc.n_in + 16, n_out: sc.n_out + 16 });
  await startupDsp(dspCounts, config);

  const startupDelay = config.engine?.startupDelay ?? 0;
  if (startupDelay > 0) {
    logger.info('[startup] stabilization delay %dms before audio start...', startupDelay);
    await new Promise(r => setTimeout(r, startupDelay));
  }

  startupBridges();
  await startupRtp(config);
  await restoreRoutes();
  restoreDspState();
  getDaemonStatus();
  startStatusFileWatcher();
  startDaemonLogForwarder();

  // 초기 동기화 — status.json에 기존 sinks/sources가 있으면 active 상태 반영
  await Promise.allSettled([
    getSinks().then(sinks     => syncAes67Active('input',  sinks)),
    getSources().then(sources => syncAes67Active('output', sources)),
  ]);
  broadcastChannels();

  startSerial(config)
  markStartupDone();

  addEngineRestartListener(async () => {
    logger.info('[startup] aoip_engine restarted — re-applying config...');
    try { await waitForDspReady('_engine'); }
    catch (e) { logger.warn('[startup] restart ready timeout: %s', e.message); return; }
    const freshConfig = reloadConfig();
    if (freshConfig.engine?.periodFrames != null) {
      const pf = Math.trunc(freshConfig.engine.periodFrames)
      logger.info('[startup] Setting engine period_frames to %d (restart)', pf)
      sendToEngine(`set period ${pf}`)
    }
    registerAnalogBridge(config);
    await reregisterBridges();
    await restoreRoutes();
    restoreDspState();
    logger.info('[startup] config re-applied after engine restart');
  });
}

startup().catch((err) => {
  logger.error('[startup] Fatal:', err.message);
});

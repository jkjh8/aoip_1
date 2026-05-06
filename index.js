import express from 'express';
import { createServer } from 'http';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import httpLogger from 'morgan';
import cookieParser from 'cookie-parser';

import apiRoutes from './routes/index.js';
import { setupSocket } from './socket/index.js';
import logger from './lib/logger.js';
import { getConfig, reloadConfig } from './lib/config.js';
import { getDspChannelCounts, restoreRoutes, restoreDspState } from './lib/channels/index.js';
import { startupDsp, registerAnalogBridge, waitForDspReady, addEngineRestartListener, sendToEngine, markStartupDone } from './lib/dsp/index.js';
import { startupBridges, reregisterBridges } from './lib/bridges.js';
import { startupRtp } from './lib/rtp/index.js';
import { getDaemonStatus, startDaemonLogForwarder, startStatusFileWatcher, refreshDaemonNetworkConf } from './lib/aes67daemon.js';

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
setupSocket(httpServer, config);
httpServer.listen(PORT, () => logger.info(`[server] http://localhost:${PORT}`));

// ── Startup ───────────────────────────────────────────

async function startup() {
  await refreshDaemonNetworkConf();
  const dspCounts = getDspChannelCounts();
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

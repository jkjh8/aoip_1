import { setPriority } from 'os';
try { setPriority(-20); } catch { /* CAP_SYS_NICE 없으면 무시 */ }

import express from 'express';
import { createServer } from 'http';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import httpLogger from 'morgan';
import cookieParser from 'cookie-parser';

import apiRoutes from './routes/index.js';
import { setupSocket } from './socket/index.js';
import logger from './lib/logger.js';
import { getConfig } from './lib/config.js';
import { getDspChannelCounts, restoreRoutes, restoreDspState } from './lib/channels/index.js';
import { startupDsp, registerAnalogBridge, waitForDspReady, addEngineRestartListener } from './lib/dsp/index.js';
import { startupBridges, reregisterBridges } from './lib/bridges.js';
import { startupRtp } from './lib/rtp/index.js';
import { getDaemonStatus } from './lib/aes67daemon.js';

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

// ── Startup ───────────────────────────────────────────

async function startup() {
  const dspCounts = getDspChannelCounts();
  await startupDsp(dspCounts, config);
  await startupBridges(config);
  await startupRtp(config);
  await restoreRoutes();
  restoreDspState();
  getDaemonStatus();

  addEngineRestartListener(async () => {
    logger.info('[startup] aoip_engine restarted — re-applying config...');
    try { await waitForDspReady('_engine'); }
    catch (e) { logger.warn('[startup] restart ready timeout: %s', e.message); return; }
    registerAnalogBridge(config);
    await reregisterBridges(config);
    await restoreRoutes();
    restoreDspState();
    logger.info('[startup] config re-applied after engine restart');
  });

  httpServer.listen(PORT, () => logger.info(`[server] http://localhost:${PORT}`));
}

startup().catch((err) => {
  logger.error('[startup] Fatal:', err.message);
});

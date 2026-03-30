import { Router } from 'express';
import { getConfig } from '../lib/config.js';
import {
  startRxPipeline,
  stopRxPipeline,
  isRxRunning,
  setRxPort,
  getRxPort,
  startTxPipeline,
  stopTxPipeline,
  isTxRunning,
  addTxTarget,
  removeTxTarget,
  getGstStatus
} from '../lib/gstreamer.js';


const router = Router();

// GET /streams — overall GStreamer status
router.get('/', (_req, res) => {
  res.json(getGstStatus());
});

// ── RX ──────────────────────────────────────

// POST /streams/rx/start
router.post('/rx/start', (_req, res) => {
  if (isRxRunning()) {
    return res.status(409).json({ error: 'rx pipeline already running' });
  }
  startRxPipeline(getConfig().rtp.input);
  res.json({ ok: true });
});

// POST /streams/rx/stop
router.post('/rx/stop', (_req, res) => {
  stopRxPipeline();
  res.json({ ok: true });
});

// GET /streams/rx/port
router.get('/rx/port', (_req, res) => {
  res.json({ port: getRxPort() });
});

// PUT /streams/rx/port  { port }  — change port (restarts pipeline if running)
router.put('/rx/port', (req, res) => {
  const { port } = req.body ?? {};
  if (!port || isNaN(Number(port))) {
    return res.status(400).json({ error: 'port is required' });
  }
  const wasRunning = isRxRunning();
  if (wasRunning) stopRxPipeline();
  setRxPort(port);
  if (wasRunning) startRxPipeline({});
  res.json({ ok: true, port: getRxPort(), restarted: wasRunning });
});

// ── TX ──────────────────────────────────────

// POST /streams/tx/start  — start with current configured targets
router.post('/tx/start', (_req, res) => {
  if (isTxRunning()) {
    return res.status(409).json({ error: 'tx pipeline already running' });
  }
  startTxPipeline(getConfig().rtp.outputs);
  res.json({ ok: true });
});

// POST /streams/tx/stop
router.post('/tx/stop', (_req, res) => {
  stopTxPipeline();
  res.json({ ok: true });
});

// POST /streams/tx/targets  { host, port }  — add a target (rebuilds pipeline)
router.post('/tx/targets', (req, res) => {
  const { host, port } = req.body ?? {};
  if (!host || !port) {
    return res.status(400).json({ error: 'host and port are required' });
  }
  addTxTarget({ host, port: Number(port) });
  res.json({ ok: true, targets: getGstStatus().tx.targets });
});

// DELETE /streams/tx/targets  { host, port }  — remove a target (rebuilds pipeline)
router.delete('/tx/targets', (req, res) => {
  const { host, port } = req.body ?? {};
  if (!host || !port) {
    return res.status(400).json({ error: 'host and port are required' });
  }
  removeTxTarget({ host, port: Number(port) });
  res.json({ ok: true, targets: getGstStatus().tx.targets });
});

export default router;

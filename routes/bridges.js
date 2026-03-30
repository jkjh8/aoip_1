import { Router } from 'express';
import { startBridges, stopBridges, getBridgeStatus } from '../lib/bridges.js';
import { getConfig } from '../lib/config.js';

const router = Router();

// GET /bridges
router.get('/', (_req, res) => {
  res.json(getBridgeStatus());
});

// POST /bridges/start
router.post('/start', (_req, res) => {
  try {
    startBridges(getConfig().bridges);
    res.json({ ok: true });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// POST /bridges/stop
router.post('/stop', (_req, res) => {
  stopBridges();
  res.json({ ok: true });
});

export default router;

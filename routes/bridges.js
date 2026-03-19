import { Router } from 'express';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { startBridges, stopBridges, getBridgeStatus } from '../lib/bridges.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const config = JSON.parse(readFileSync(join(__dirname, '../config/audio.json'), 'utf8'));

const router = Router();

// GET /bridges
router.get('/', (_req, res) => {
  res.json(getBridgeStatus());
});

// POST /bridges/start
router.post('/start', (_req, res) => {
  try {
    startBridges(config.bridges);
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

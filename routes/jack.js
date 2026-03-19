import { Router } from 'express';
import {
  startJack,
  stopJack,
  waitForJack,
  getPorts,
  getConnections,
  connect,
  disconnect,
  isJackRunning
} from '../lib/jack.js';

const router = Router();

// GET /jack/status
router.get('/status', (_req, res) => {
  res.json({ running: isJackRunning() });
});

// POST /jack/start
router.post('/start', async (_req, res) => {
  if (isJackRunning()) {
    return res.status(409).json({ error: 'jackd is already running' });
  }
  try {
    startJack();
    await waitForJack();
    res.json({ ok: true });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// POST /jack/stop
router.post('/stop', (_req, res) => {
  if (!isJackRunning()) {
    return res.status(409).json({ error: 'jackd is not running' });
  }
  stopJack();
  res.json({ ok: true });
});

// GET /jack/ports
router.get('/ports', async (_req, res) => {
  try {
    const ports = await getPorts();
    res.json(ports);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /jack/connections
router.get('/connections', async (_req, res) => {
  try {
    const connections = await getConnections();
    res.json(connections);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// POST /jack/connect  { src, dst }
router.post('/connect', async (req, res) => {
  const { src, dst } = req.body ?? {};
  if (!src || !dst) {
    return res.status(400).json({ error: 'src and dst are required' });
  }
  try {
    await connect(src, dst);
    res.json({ ok: true });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// POST /jack/disconnect  { src, dst }
router.post('/disconnect', async (req, res) => {
  const { src, dst } = req.body ?? {};
  if (!src || !dst) {
    return res.status(400).json({ error: 'src and dst are required' });
  }
  try {
    await disconnect(src, dst);
    res.json({ ok: true });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

export default router;

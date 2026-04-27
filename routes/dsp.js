import { Router } from 'express';
import { isDspRunning } from '../lib/dsp.js';
import { getDspChannelCounts } from '../lib/channels.js';

const router = Router();

// GET /dsp/status
router.get('/status', (_req, res) => {
  const counts = getDspChannelCounts();
  const status = Object.fromEntries(
    [...counts.keys()].map(name => [name, isDspRunning(name)])
  );
  res.json({ running: Object.values(status).some(Boolean), engines: status });
});

export default router;

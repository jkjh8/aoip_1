import { Router } from 'express';
import { isDspRunning } from '../../lib/dsp/index.js';
import { getDspChannelCounts, getI2sMode, setI2sMode } from '../../lib/channels/index.js';

const router = Router();

// GET /dsp/status
router.get('/status', (_req, res) => {
  const counts = getDspChannelCounts();
  const status = Object.fromEntries(
    [...counts.keys()].map(name => [name, isDspRunning(name)])
  );
  res.json({ running: Object.values(status).some(Boolean), engines: status });
});

// GET /dsp/mode — I2S Analog 1/2 mono/stereo 처리 모드
router.get('/mode', (_req, res) => {
  res.json({ ok: true, mode: getI2sMode() });
});

// PUT /dsp/mode  body: { input?: 'mono'|'stereo', output?: 'mono'|'stereo' }
router.put('/mode', (req, res) => {
  const { input, output } = req.body || {};
  if (input == null && output == null) {
    return res.status(400).json({ ok: false, error: 'input or output required' });
  }
  try {
    if (input  != null) setI2sMode('input',  input);
    if (output != null) setI2sMode('output', output);
    res.json({ ok: true, mode: getI2sMode() });
  } catch (e) {
    res.status(400).json({ ok: false, error: e.message });
  }
});

export default router;

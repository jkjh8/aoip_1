import { Router } from 'express';
import { getBridgeStatus } from '../../lib/bridges.js';

const router = Router();

// GET /bridges
router.get('/', (_req, res) => {
  res.json(getBridgeStatus());
});

export default router;

import { Router } from 'express';

import bridgesRoutes from './api/bridges.js';
import streamsRoutes from './api/streams.js';
import dspRoutes     from './api/dsp.js';
import systemRoutes  from './api/system.js';
import aes67Routes   from './api/aes67.js';
import serialRoutes  from './api/serial.js';

const router = Router();

router.use('/bridges', bridgesRoutes);
router.use('/streams', streamsRoutes);
router.use('/dsp',     dspRoutes);
router.use('/system',  systemRoutes);
router.use('/aes67',   aes67Routes);
router.use('/serial',  serialRoutes);

export default router;

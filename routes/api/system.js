import { Router } from 'express';
import { getNetworkInfo, setStaticIp, setDhcp, rebootSystem } from '../../lib/system.js';
import { getSystemConfig, saveSystemConfig } from '../../lib/config.js';
import { setLogEnabled, setLogDebug } from '../../lib/logger.js';

const router = Router();

// GET /system/log → { enabled, debug }
router.get('/log', (_req, res) => {
  const { log = {} } = getSystemConfig();
  res.json({ enabled: log.enabled ?? true, debug: log.debug ?? false });
});

// POST /system/log  body: { enabled?, debug? }
router.post('/log', (req, res) => {
  const { enabled, debug } = req.body ?? {};
  if (enabled !== undefined && typeof enabled !== 'boolean') return res.status(400).json({ ok: false, error: 'enabled must be boolean' });
  if (debug   !== undefined && typeof debug   !== 'boolean') return res.status(400).json({ ok: false, error: 'debug must be boolean' });

  const sys = getSystemConfig();
  sys.log = { ...(sys.log ?? {}), ...(enabled !== undefined && { enabled }), ...(debug !== undefined && { debug }) };
  saveSystemConfig();

  if (enabled !== undefined) setLogEnabled(enabled);
  if (debug   !== undefined) setLogDebug(debug);
  res.json({ ok: true, log: sys.log });
});

// GET /system/network?iface=eth0
router.get('/network', (req, res) => {
  try {
    res.json(getNetworkInfo(req.query.iface ?? 'eth0'));
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// POST /system/network
// body: { iface?, mode: 'dhcp' }
//    or { iface?, mode: 'static', ip, subnet?, gateway, dns? }
router.post('/network', async (req, res) => {
  try {
    const { iface = 'eth0', mode = 'static', ip, subnet, gateway, dns } = req.body ?? {};
    console.log('Setting network config', { iface, mode, ip, subnet, gateway, dns });
    if (mode === 'dhcp') {
      await setDhcp(iface);
    } else {
      await setStaticIp({ iface, ip, subnet, gateway, dns });
    }
    res.json({ ok: true });
  } catch (e) {
    res.status(400).json({ ok: false, error: e });
  }
});

// POST /system/reboot
router.post('/reboot', (req, res) => {
  res.json({ ok: true });
  rebootSystem();
});

export default router;

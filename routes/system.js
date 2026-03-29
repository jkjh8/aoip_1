import { Router } from 'express';
import { getNetworkInfo, setStaticIp, setDhcp, rebootSystem } from '../lib/system.js';
import { getUsbGadgetEnabled, setUsbGadgetEnabled } from '../lib/bridges.js';

const router = Router();

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
//    or { iface?, mode: 'static', ip, prefix?, gateway, dns? }
router.post('/network', async (req, res) => {
  try {
    const { iface = 'eth0', mode = 'static', ip, prefix, gateway, dns } = req.body ?? {};
    if (mode === 'dhcp') {
      await setDhcp(iface);
    } else {
      await setStaticIp({ iface, ip, prefix, gateway, dns });
    }
    res.json({ ok: true });
  } catch (e) {
    res.status(400).json({ ok: false, error: e.message });
  }
});

// GET /system/usb
router.get('/usb', (_req, res) => {
  res.json({ enabled: getUsbGadgetEnabled() });
});

// POST /system/usb
// body: { enabled: boolean }
router.post('/usb', async (req, res) => {
  try {
    const { enabled } = req.body ?? {};
    await setUsbGadgetEnabled(Boolean(enabled));
    res.json({ ok: true, enabled: Boolean(enabled) });
  } catch (e) {
    res.status(500).json({ ok: false, error: e.message });
  }
});

// POST /system/reboot
router.post('/reboot', (req, res) => {
  res.json({ ok: true });
  rebootSystem();
});

export default router;

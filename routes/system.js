import { Router } from 'express';
import { getNetworkInfo, setStaticIp, setDhcp, rebootSystem } from '../lib/system.js';
import { getConfig, saveConfig } from '../lib/config.js';
import { setUsbGadgetEnabled, getUsbGadgetEnabled, isUdcConnected, restartUsbBridges } from '../lib/bridges.js';

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

// GET /system/usb
router.get('/usb', (_req, res) => {
  const config = getConfig();
  const usbCfgs = (config.bridges ?? []).filter(b => b.usb_gadget);
  if (!usbCfgs.length) {
    return res.status(404).json({ error: 'USB gadget bridge config not found' });
  }
  const period = usbCfgs[0].period ?? config.jack?.period ?? 256;
  res.json({ period, enabled: getUsbGadgetEnabled() });
});

// GET /system/usb/enabled
router.get('/usb/enabled', (_req, res) => {
  res.json({ enabled: getUsbGadgetEnabled(), connected: isUdcConnected() });
});

// POST /system/usb/enabled  { enabled: boolean }
router.post('/usb/enabled', async (req, res) => {
  try {
    const { enabled } = req.body ?? {};
    await setUsbGadgetEnabled(Boolean(enabled));
    res.json({ ok: true, enabled: Boolean(enabled) });
  } catch (e) {
    res.status(400).json({ ok: false, error: e.message });
  }
});

// POST /system/usb/period — 즉시 적용 (브릿지 재시작)
router.post('/usb/period', async (req, res) => {
  try {
    const { period } = req.body ?? {};
    const config = getConfig();
    let updated = false;
    for (const b of (config.bridges ?? [])) {
      if (b.usb_gadget) {
        b.period = Number(period);
        updated = true;
      }
    }
    if (!updated) {
      return res.status(404).json({ error: 'USB gadget bridge config not found' });
    }
    saveConfig();
    restartUsbBridges();
    res.json({ ok: true, period: Number(period) });
  } catch (e) {
    res.status(400).json({ ok: false, error: e.message });
  }
});

// POST /system/reboot
router.post('/reboot', (req, res) => {
  res.json({ ok: true });
  rebootSystem();
});

export default router;

import { setUsbGadgetEnabled, getUsbGadgetEnabled } from '../lib/bridges.js';

/**
 * socket events:
 *   usb:state  (client→server)  — enabled 상태 요청
 *   usb:set    (client→server)  { enabled: boolean }
 *   usb:state  (server→client)  { enabled, running }
 */
export default function register(socket, { broadcastStatus }) {
  /* 현재 상태 반환 */
  socket.on('usb:state', (cb) => {
    cb?.({ enabled: getUsbGadgetEnabled() });
  });

  /* 활성화/비활성화 */
  socket.on('usb:set', async ({ enabled } = {}, cb) => {
    try {
      await setUsbGadgetEnabled(Boolean(enabled));
      await broadcastStatus();
      cb?.({ ok: true, enabled: Boolean(enabled) });
    } catch (e) {
      cb?.({ ok: false, error: e.message });
    }
  });
}

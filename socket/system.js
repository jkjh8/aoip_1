import { getNetworkInfo, setStaticIp, setDhcp, rebootSystem } from '../lib/system.js';

/**
 * socket events:
 *   system:network:get  (client→server)  { iface? }
 *                       (server→client)  { iface, ip, subnet, gateway, dns, mode, mac }
 *
 *   system:network:set  (client→server)  { iface?, mode: 'dhcp' }
 *                     OR                 { iface?, mode: 'static', ip, subnet?, gateway, dns? }
 *                       (server→client)  { ok, error? }
 *
 *   system:reboot       (client→server)  —
 *                       (server→client)  { ok }
 */
export default function register(socket) {
  socket.on('system:network:get', ({ iface = 'eth0' } = {}, cb) => {
    try {
      cb?.(getNetworkInfo(iface));
    } catch (e) {
      cb?.({ error: e.message });
    }
  });

  socket.on('system:network:set', async (opts = {}, cb) => {
    try {
      const { iface = 'eth0', mode = 'static', ip, subnet, gateway, dns } = opts;
      if (mode === 'dhcp') {
        await setDhcp(iface);
      } else {
        await setStaticIp({ iface, ip, subnet, gateway, dns });
      }
      cb?.({ ok: true });
    } catch (e) {
      cb?.({ ok: false, error: e.message });
    }
  });

  socket.on('system:reboot', (cb) => {
    cb?.({ ok: true });
    rebootSystem();
  });
}

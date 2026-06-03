import { getNetworkInfo, setStaticIp, setDhcp, rebootSystem, getUptime } from '../lib/system.js';
import { getSystemConfig, saveSystemConfig } from '../lib/config.js';
import { setLogEnabled, setLogDebug } from '../lib/logger.js';

/**
 * socket events:
 *   system:network      (server→client)  { iface, ip, subnet, gateway, dns, mode, mac }  — on connect
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
  // 연결 즉시 네트워크 정보 + uptime push
  try { socket.emit('system:network', { ...getNetworkInfo(), uptime: getUptime() }); } catch { /* ignore */ }

  socket.on('system:network:get', ({ iface = 'eth0' } = {}, cb) => {
    try {
      cb?.({ ...getNetworkInfo(iface), uptime: getUptime() });
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

  socket.on('system:log:get', (_arg, cb) => {
    const { log = {} } = getSystemConfig();
    cb?.({ enabled: log.enabled ?? true, debug: log.debug ?? false });
  });

  socket.on('system:log:set', ({ enabled, debug } = {}, cb) => {
    try {
      if (enabled !== undefined && typeof enabled !== 'boolean') throw new Error('enabled must be boolean');
      if (debug   !== undefined && typeof debug   !== 'boolean') throw new Error('debug must be boolean');
      const sys = getSystemConfig();
      sys.log = { ...(sys.log ?? {}), ...(enabled !== undefined && { enabled }), ...(debug !== undefined && { debug }) };
      saveSystemConfig();
      if (enabled !== undefined) setLogEnabled(enabled);
      if (debug   !== undefined) setLogDebug(debug);
      cb?.({ ok: true, log: sys.log });
    } catch (e) {
      cb?.({ ok: false, error: e.message });
    }
  });
}

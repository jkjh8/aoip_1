import { startBridges, stopBridges } from '../lib/bridges.js';

export default function register(socket, { broadcastStatus, config }) {
  socket.on('bridges:start', async (cb) => {
    try { await startBridges(config.bridges); await broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('bridges:stop', async (cb) => {
    try { stopBridges(); await broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

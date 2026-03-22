import { startJack, stopJack, waitForJack, connect, disconnect, isJackRunning } from '../lib/jack.js';
import { startMeters, addRoute, removeRoute } from '../lib/channels.js';

export default function register(socket, { broadcastStatus }) {
  socket.on('jack:start', async (cb) => {
    try {
      if (isJackRunning()) return cb?.({ ok: false, error: 'already running' });
      startJack();
      await waitForJack();
      startMeters();
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('jack:stop', async (cb) => {
    try {
      stopJack();
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('jack:connect', async ({ src, dst } = {}, cb) => {
    try { await connect(src, dst); addRoute(src, dst); await broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('jack:disconnect', async ({ src, dst } = {}, cb) => {
    try { await disconnect(src, dst); removeRoute(src, dst); await broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

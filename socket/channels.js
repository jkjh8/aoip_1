import { getChannels, setGain, setMute, setLabel, addRoute, removeRoute } from '../lib/channels/index.js';
import { sendGain, sendMute, sendBypass, connect, disconnect } from '../lib/dsp/index.js';

export default function register(socket, { broadcastStatus }) {
  socket.on('route:add', async ({ src, dst } = {}, cb) => {
    try {
      if (!src || !dst) return cb?.({ ok: false, error: 'src and dst required' });
      await connect(src, dst);
      addRoute(src, dst);
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('route:remove', async ({ src, dst } = {}, cb) => {
    try {
      if (!src || !dst) return cb?.({ ok: false, error: 'src and dst required' });
      await disconnect(src, dst);
      removeRoute(src, dst);
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('ch:gain', async ({ type, id, gain } = {}, cb) => {
    try {
      setGain(type, id, gain);
      sendGain(type === 'input' ? 'in' : 'out', id, gain);
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('ch:mute', async ({ type, id, muted } = {}, cb) => {
    try {
      setMute(type, id, muted);
      sendMute(type === 'input' ? 'in' : 'out', id, muted);
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('ch:label', ({ type, id, label } = {}, cb) => {
    try { setLabel(type, id, label); broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('dsp:bypass', async (cb) => {
    try {
      const { inputs, outputs } = getChannels();
      for (const ch of inputs)  sendBypass('in',  ch.id, true);
      for (const ch of outputs) sendBypass('out', ch.id, true);
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('dsp:restore', async (cb) => {
    try {
      const { inputs, outputs } = getChannels();
      for (const ch of inputs)  { sendBypass('in',  ch.id, false); sendGain('in',  ch.id, ch.gain); if (ch.muted) sendMute('in',  ch.id, true); }
      for (const ch of outputs) { sendBypass('out', ch.id, false); sendGain('out', ch.id, ch.gain); if (ch.muted) sendMute('out', ch.id, true); }
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

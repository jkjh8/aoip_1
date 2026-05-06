import { getChannels, getAllChannelDefs, setChannelActive, setGain, setMute, setLabel, addRoute, removeRoute, getSavedRoutes } from '../lib/channels/index.js';
import { sendGain, sendMute, sendBypass, connect, disconnect } from '../lib/dsp/index.js';
import { startRtpStream, stopRtpStream, startRtpStreams } from '../lib/rtp/index.js';

export default function register(socket, { broadcastStatus, config }) {
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

  socket.on('dsp:bypass', (cb) => {
    try {
      const { inputs, outputs } = getChannels();
      for (const ch of inputs)  sendBypass('in',  ch.id, true);
      for (const ch of outputs) sendBypass('out', ch.id, true);
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('channels:defs', (cb) => {
    try { cb?.({ ok: true, ...getAllChannelDefs() }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('ch:activate', async ({ type, id } = {}, cb) => {
    try {
      const ch = setChannelActive(type, id, true)
      if (ch.source?.type === 'rtp') {
        const clientName = ch.source.name
        try {
          await startRtpStream(clientName)
        } catch (e) {
          if (e.message.includes('not found')) {
            // First activation — instance never created (was inactive at boot)
            const streamCfg = config?.rtp_streams?.find(s => s.client === clientName)
            if (streamCfg) startRtpStreams([{ ...streamCfg, enabled: true }])
          }
        }
      }
      await broadcastStatus()
      cb?.({ ok: true })
    } catch (e) { cb?.({ ok: false, error: e.message }) }
  });

  socket.on('ch:deactivate', async ({ type, id } = {}, cb) => {
    try {
      const defs = getAllChannelDefs()
      const list = type === 'input' ? defs.inputs : defs.outputs
      const ch = list.find(c => c.id === Number(id))
      if (!ch) return cb?.({ ok: false, error: 'channel not found' })

      // Remove DSP routes for all channels being deactivated
      const affected = ch.source?.type === 'rtp'
        ? list.filter(c => c.source?.name === ch.source.name)
        : [ch]
      const routes = [...getSavedRoutes()]
      for (const { port } of affected) {
        for (const { src, dst } of routes) {
          if (type === 'input' ? src === port : dst === port) {
            try { disconnect(src, dst) } catch { }
            removeRoute(src, dst)
          }
        }
      }

      setChannelActive(type, id, false)
      if (ch.source?.type === 'rtp') {
        try { stopRtpStream(ch.source.name) } catch { }
      }
      await broadcastStatus()
      cb?.({ ok: true })
    } catch (e) { cb?.({ ok: false, error: e.message }) }
  });

  socket.on('dsp:restore', (cb) => {
    try {
      const { inputs, outputs } = getChannels();
      for (const ch of inputs)  { sendBypass('in',  ch.id, false); sendGain('in',  ch.id, ch.gain); if (ch.muted) sendMute('in',  ch.id, true); }
      for (const ch of outputs) { sendBypass('out', ch.id, false); sendGain('out', ch.id, ch.gain); if (ch.muted) sendMute('out', ch.id, true); }
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

import { getChannels, setGain, setMute, setLabel,
         getInputSrcPorts, getOutputSinkPorts } from '../lib/channels.js';
import { startGainer, stopGainer, isGainerRunning,
         sendGain, sendMute, sendAllDsp } from '../lib/gainer.js';
import { connect, disconnect } from '../lib/jack.js';

export default function register(socket, { broadcastStatus }) {
  socket.on('ch:gain', async ({ type, id, gain } = {}, cb) => {
    try {
      setGain(type, id, gain);
      if (isGainerRunning()) sendGain(type === 'input' ? 'in' : 'out', id, gain);
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('ch:mute', async ({ type, id, muted } = {}, cb) => {
    try {
      setMute(type, id, muted);
      if (isGainerRunning()) sendMute(type === 'input' ? 'in' : 'out', id, muted);
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('ch:label', ({ type, id, label } = {}, cb) => {
    try { setLabel(type, id, label); broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('gainer:bypass', async (cb) => {
    try {
      stopGainer();
      await new Promise(r => setTimeout(r, 500));
      const srcPorts  = getInputSrcPorts();
      const sinkPorts = getOutputSinkPorts();
      const n = Math.min(srcPorts.length, sinkPorts.length);
      for (let i = 0; i < n; i++) {
        try { await connect(srcPorts[i], sinkPorts[i]); } catch { /* ignore */ }
      }
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('gainer:restore', async (cb) => {
    try {
      const srcPorts  = getInputSrcPorts();
      const sinkPorts = getOutputSinkPorts();
      const n = Math.min(srcPorts.length, sinkPorts.length);
      for (let i = 0; i < n; i++) {
        try { await disconnect(srcPorts[i], sinkPorts[i]); } catch { /* ignore */ }
      }
      startGainer(srcPorts.length, sinkPorts.length);
      await new Promise(r => setTimeout(r, 1000));
      for (let i = 0; i < srcPorts.length; i++)
        try { await connect(srcPorts[i], `gainer:in_${i + 1}`); } catch { /* ignore */ }
      for (let i = 0; i < sinkPorts.length; i++)
        try { await connect(`gainer:sout_${i + 1}`, sinkPorts[i]); } catch { /* ignore */ }
      const { inputs, outputs } = getChannels([]);
      for (const ch of inputs)  { sendGain('in',  ch.id, ch.gain); if (ch.muted) sendMute('in',  ch.id, true); }
      for (const ch of outputs) { sendGain('out', ch.id, ch.gain); if (ch.muted) sendMute('out', ch.id, true); }
      sendAllDsp({ inputs, outputs });
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

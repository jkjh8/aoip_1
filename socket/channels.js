import { getChannels, setGain, setMute, setLabel,
         getInputSrcPorts, getOutputSinkPorts,
         getTotalInputCount, getTotalOutputCount,
         getDspClientOf, getDspLocalId } from '../lib/channels.js';
import { startDsp, stopDsp,
         sendGain, sendMute, sendAllDsp } from '../lib/dsp.js';
import { getConfig } from '../lib/config.js';
import { connect, disconnect } from '../lib/jack.js';

export default function register(socket, { broadcastStatus }) {
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
      stopDsp();
      await new Promise(r => setTimeout(r, 500));
      const srcPorts  = getInputSrcPorts();
      const sinkPorts = getOutputSinkPorts();
      const sinkById  = new Map(sinkPorts.map(({ id, sinkPort }) => [id, sinkPort]));
      for (const { id, srcPort } of srcPorts) {
        const dst = sinkById.get(id);
        if (dst) try { await connect(srcPort, dst); } catch { /* ignore */ }
      }
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('dsp:restore', async (cb) => {
    try {
      const srcPorts  = getInputSrcPorts();
      const sinkPorts = getOutputSinkPorts();
      const sinkById  = new Map(sinkPorts.map(({ id, sinkPort }) => [id, sinkPort]));
      for (const { id, srcPort } of srcPorts) {
        const dst = sinkById.get(id);
        if (dst) try { await disconnect(srcPort, dst); } catch { /* ignore */ }
      }
      const cfg = getConfig();
      const GAINER_CH = cfg.jack?.channels ?? 2;
      const totalIn = getTotalInputCount(), totalOut = getTotalOutputCount();
      startDsp('gainer', GAINER_CH, GAINER_CH);
      if (totalIn - GAINER_CH > 0 || totalOut - GAINER_CH > 0)
        startDsp('mixer', totalIn - GAINER_CH, totalOut - GAINER_CH);
      await new Promise(r => setTimeout(r, 1000));
      for (const { id, srcPort } of srcPorts)
        try { await connect(srcPort, `${getDspClientOf(id)}:in_${getDspLocalId(id)}`); } catch { /* ignore */ }
      for (const { id, sinkPort } of sinkPorts)
        try { await connect(`${getDspClientOf(id)}:sout_${getDspLocalId(id)}`, sinkPort); } catch { /* ignore */ }
      const { inputs, outputs } = getChannels([]);
      for (const ch of inputs)  { sendGain('in',  ch.id, ch.gain); if (ch.muted) sendMute('in',  ch.id, true); }
      for (const ch of outputs) { sendGain('out', ch.id, ch.gain); if (ch.muted) sendMute('out', ch.id, true); }
      sendAllDsp({ inputs, outputs });
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

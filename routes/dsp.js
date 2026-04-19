import { Router } from 'express';
import { startDsp, stopDsp, isDspRunning,
         sendGain, sendMute, sendAllDsp } from '../lib/dsp.js';
import { connect, disconnect } from '../lib/jack.js';
import { getInputSrcPorts, getOutputSinkPorts, getChannels,
         getDspClientOf, getDspLocalId,
         getDspChannelCounts } from '../lib/channels.js';

const router = Router();

// GET /dsp/bypass  — 현재 매트릭스 라우팅을 읽어 DSP 없이 직결
router.get('/bypass', async (_req, res) => {
  try {
    const { getConnections } = await import('../lib/jack.js');
    const srcPorts  = getInputSrcPorts();
    const sinkPorts = getOutputSinkPorts();

    const conns = await getConnections();
    const portMap = new Map(conns.map(({ port, connections }) => [port, connections]));
    const sinkByJackPort = new Map(
      sinkPorts.map(({ id, sinkPort }) => [`${getDspClientOf(id,'out')}:sin_${getDspLocalId(id,'out')}`, sinkPort])
    );

    const pairs = [];
    for (const { id, srcPort } of srcPorts) {
      const dspOut = `${getDspClientOf(id,'in')}:out_${getDspLocalId(id,'in')}`;
      const dsts = portMap.get(dspOut) ?? [];
      for (const dspSin of dsts) {
        const dst = sinkByJackPort.get(dspSin);
        if (dst) pairs.push({ src: srcPort, dst });
      }
    }

    stopDsp();
    await new Promise(r => setTimeout(r, 500));

    const results = [];
    for (const { src, dst } of pairs) {
      try {
        await connect(src, dst);
        results.push({ src, dst, ok: true });
      } catch (e) {
        results.push({ src, dst, ok: false, error: e.message });
      }
    }

    res.json({ ok: true, connections: results });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// GET /dsp/restore  — 직결 해제 후 DSP 재시작 및 연결 복원
router.get('/restore', async (_req, res) => {
  try {
    const srcPorts  = getInputSrcPorts();
    const sinkPorts = getOutputSinkPorts();
    for (const { srcPort } of srcPorts)
      try { await disconnect(srcPort, srcPort); } catch { /* ignore */ }
    for (const [name, { n_in, n_out }] of getDspChannelCounts())
      startDsp(name, n_in, n_out);
    await new Promise(r => setTimeout(r, 1000));
    for (const { id, srcPort } of srcPorts)
      try { await connect(srcPort, `${getDspClientOf(id,'in')}:in_${getDspLocalId(id,'in')}`); } catch { /* ignore */ }
    for (const { id, sinkPort } of sinkPorts)
      try { await connect(`${getDspClientOf(id,'out')}:sout_${getDspLocalId(id,'out')}`, sinkPort); } catch { /* ignore */ }
    const { inputs, outputs } = getChannels([]);
    for (const ch of inputs)  { sendGain('in',  ch.id, ch.gain); if (ch.muted) sendMute('in',  ch.id, true); }
    for (const ch of outputs) { sendGain('out', ch.id, ch.gain); if (ch.muted) sendMute('out', ch.id, true); }
    sendAllDsp({ inputs, outputs });
    res.json({ ok: true });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// GET /dsp/status
router.get('/status', (_req, res) => {
  const counts = getDspChannelCounts();
  const status = Object.fromEntries(
    [...counts.keys()].map(name => [name, isDspRunning(name)])
  );
  res.json({ running: Object.values(status).some(Boolean), engines: status });
});

export default router;

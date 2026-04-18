import { Router } from 'express';
import { startDsp, stopDsp, isDspRunning,
         sendGain, sendMute, sendAllDsp } from '../lib/dsp.js';
import { connect, disconnect } from '../lib/jack.js';
import { getInputSrcPorts, getOutputSinkPorts,
         getTotalInputCount, getTotalOutputCount, getChannels,
         getDspClientOf, getDspLocalId } from '../lib/channels.js';
import { getConfig } from '../lib/config.js';

const router = Router();

// GET /gainer/bypass  — 현재 매트릭스 라우팅을 읽어 gainer 없이 직결
router.get('/bypass', async (_req, res) => {
  try {
    const { getConnections } = await import('../lib/jack.js');
    const srcPorts  = getInputSrcPorts();
    const sinkPorts = getOutputSinkPorts();

    // 현재 연결 스냅샷: gainer 체인 매핑 파악
    const conns = await getConnections();
    const portMap = new Map(conns.map(({ port, connections }) => [port, connections]));
    const sinkByJackPort = new Map(
      sinkPorts.map(({ id, sinkPort }) => [`${getDspClientOf(id)}:sin_${getDspLocalId(id)}`, sinkPort])
    );

    const pairs = [];
    for (const { id, srcPort } of srcPorts) {
      const dspOut = `${getDspClientOf(id)}:out_${getDspLocalId(id)}`;
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

// POST /gainer/restore  — 직결 해제 후 gainer 재시작 및 연결 복원
router.get('/restore', async (_req, res) => {
  try {
    const srcPorts  = getInputSrcPorts();
    const sinkPorts = getOutputSinkPorts();
    for (const { srcPort } of srcPorts)
      try { await disconnect(srcPort, srcPort); } catch { /* ignore */ }
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
    res.json({ ok: true });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// GET /gainer/status
router.get('/status', (_req, res) => {
  res.json({ running: isDspRunning('gainer') || isDspRunning('mixer') });
});

export default router;

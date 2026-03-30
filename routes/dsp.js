import { Router } from 'express';
import { startDsp, stopDsp, isDspRunning,
         sendGain, sendMute, sendAllDsp } from '../lib/dsp.js';
import { connect, disconnect } from '../lib/jack.js';
import { getInputSrcPorts, getOutputSinkPorts,
         getTotalInputCount, getTotalOutputCount, getChannels } from '../lib/channels.js';

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
    const sinkById = new Map(sinkPorts.map(({ id, sinkPort }) => [id, sinkPort]));

    const pairs = [];
    for (const { id, srcPort } of srcPorts) {
      const gainerOut = `gainer:out_${id}`;
      const dsts = portMap.get(gainerOut) ?? [];
      for (const gainerSin of dsts) {
        const m = gainerSin.match(/^gainer:sin_(\d+)$/);
        if (m) {
          const dst = sinkById.get(parseInt(m[1], 10));
          if (dst) pairs.push({ src: srcPort, dst });
        }
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
    startDsp(getTotalInputCount(), getTotalOutputCount());
    await new Promise(r => setTimeout(r, 1000));
    for (const { id, srcPort } of srcPorts)
      try { await connect(srcPort, `gainer:in_${id}`); } catch { /* ignore */ }
    for (const { id, sinkPort } of sinkPorts)
      try { await connect(`gainer:sout_${id}`, sinkPort); } catch { /* ignore */ }
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
  res.json({ running: isDspRunning() });
});

export default router;

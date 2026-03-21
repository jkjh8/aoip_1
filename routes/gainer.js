import { Router } from 'express';
import { startGainer, stopGainer, isGainerRunning,
         sendGain, sendMute, sendAllDsp } from '../lib/gainer.js';
import { connect, disconnect } from '../lib/jack.js';
import { getInputSrcPorts, getOutputSinkPorts, getChannels } from '../lib/channels.js';

const router = Router();

// GET /gainer/bypass  — 현재 매트릭스 라우팅을 읽어 gainer 없이 직결
router.get('/bypass', async (_req, res) => {
  try {
    const { getConnections } = await import('../lib/jack.js');
    const srcPorts  = getInputSrcPorts();
    const sinkPorts = getOutputSinkPorts();5

    // 현재 연결 스냅샷: gainer 체인 매핑 파악
    // srcPorts[i] → gainer:in_{i+1} → gainer:out_{i+1} → gainer:sin_{j+1} → sinkPorts[j]
    const conns = await getConnections();
    const portMap = new Map(conns.map(({ port, connections }) => [port, connections]));

    const pairs = [];
    for (let i = 0; i < srcPorts.length; i++) {
      const gainerOut = `gainer:out_${i + 1}`;
      const dsts = portMap.get(gainerOut) ?? [];
      for (const gainerSin of dsts) {
        // gainer:sin_N → sinkPorts[N-1]
        const m = gainerSin.match(/^gainer:sin_(\d+)$/);
        if (m) {
          const j = parseInt(m[1], 10) - 1;
          if (sinkPorts[j]) pairs.push({ src: srcPorts[i], dst: sinkPorts[j] });
        }
      }
    }

    stopGainer();
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
    res.json({ ok: true });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// GET /gainer/status
router.get('/status', (_req, res) => {
  res.json({ running: isGainerRunning() });
});

export default router;

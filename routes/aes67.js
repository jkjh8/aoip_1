import { Router } from 'express';
import {
  getDaemonStatus,
  getConfig, setConfig,
  getPtpConfig, setPtpConfig, getPtpStatus,
  getSources, addSource, removeSource, getSourceSdp,
  getSinks, addSink, removeSink, getSinkStatus,
  browseAll, browseMdns, browseSap,
} from '../lib/aes67daemon.js';

const router = Router();

// ── 데몬 상태 ────────────────────────────────────────────

// GET /aes67/status
router.get('/status', async (_req, res) => {
  try { res.json(await getDaemonStatus()); }
  catch (e) { res.status(500).json({ ok: false, error: e.message }); }
});

// ── 데몬 설정 ────────────────────────────────────────────

// GET /aes67/config
router.get('/config', async (_req, res) => {
  try { res.json(await getConfig()); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// POST /aes67/config
router.post('/config', async (req, res) => {
  try { res.json(await setConfig(req.body)); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// ── PTP ──────────────────────────────────────────────────

// GET /aes67/ptp/config
router.get('/ptp/config', async (_req, res) => {
  try { res.json(await getPtpConfig()); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// POST /aes67/ptp/config
router.post('/ptp/config', async (req, res) => {
  try { res.json(await setPtpConfig(req.body)); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// GET /aes67/ptp/status
router.get('/ptp/status', async (_req, res) => {
  try { res.json(await getPtpStatus()); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// ── Sources ──────────────────────────────────────────────

// GET /aes67/sources
router.get('/sources', async (_req, res) => {
  try { res.json(await getSources()); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// PUT /aes67/sources/:id
router.put('/sources/:id', async (req, res) => {
  try { res.json(await addSource(req.params.id, req.body)); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// DELETE /aes67/sources/:id
router.delete('/sources/:id', async (req, res) => {
  try { res.json(await removeSource(req.params.id)); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// GET /aes67/sources/:id/sdp
router.get('/sources/:id/sdp', async (req, res) => {
  try {
    const sdp = await getSourceSdp(req.params.id);
    res.type('text/plain').send(sdp);
  } catch (e) { res.status(502).json({ error: e.message }); }
});

// ── Sinks ────────────────────────────────────────────────

// GET /aes67/sinks
router.get('/sinks', async (_req, res) => {
  try { res.json(await getSinks()); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// PUT /aes67/sinks/:id
router.put('/sinks/:id', async (req, res) => {
  try { res.json(await addSink(req.params.id, req.body)); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// DELETE /aes67/sinks/:id
router.delete('/sinks/:id', async (req, res) => {
  try { res.json(await removeSink(req.params.id)); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// GET /aes67/sinks/:id/status
router.get('/sinks/:id/status', async (req, res) => {
  try { res.json(await getSinkStatus(req.params.id)); }
  catch (e) { res.status(502).json({ error: e.message }); }
});

// ── Browse (원격 AES67 소스 탐색) ────────────────────────

// GET /aes67/browse          → SAP + mDNS 모두
// GET /aes67/browse?type=mdns
// GET /aes67/browse?type=sap
router.get('/browse', async (req, res) => {
  try {
    const type = req.query.type;
    let result;
    if (type === 'mdns')     result = await browseMdns();
    else if (type === 'sap') result = await browseSap();
    else                     result = await browseAll();
    res.json(result);
  } catch (e) { res.status(502).json({ error: e.message }); }
});

export default router;

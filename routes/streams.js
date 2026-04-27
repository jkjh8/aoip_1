import { Router } from 'express';
import { getConfig } from '../lib/config.js';
import {
  startRxPipeline, stopRxPipeline, isRxRunning,
  startTxClient, stopTxClient, isTxRunning,
  getGstStatus,
  getRtpStreamStatus, getRtpStreamDetail,
  startRtpStream, stopRtpStream,
  updateRtpInConfig, updateRtpOutConfig,
  addRtpOutTarget, removeRtpOutTarget, setRtpOutCodec,
} from '../lib/gstreamer.js';

function parseBody(body = {}) {
  const updates = {};
  const { port, protocol, address, sampleRate, codec, bitrate, bufferMs, channels, targets } = body;
  if (port       != null) updates.port       = Number(port);
  if (protocol   != null) updates.protocol   = protocol;
  if (address    != null) updates.address    = address;
  if (sampleRate != null) updates.sampleRate = Number(sampleRate);
  if (codec      != null) updates.codec      = codec;
  if (bitrate    != null) updates.bitrate    = Number(bitrate);
  if (bufferMs   != null) updates.bufferMs   = Number(bufferMs);
  if (channels   != null) updates.channels   = Number(channels);
  if (targets    != null) updates.targets    = targets;
  return updates;
}

const router = Router();

// GET /streams
router.get('/', (_req, res) => {
  res.json(getGstStatus());
});

// ── RX (legacy) ──────────────────────────────────────

router.post('/rx/start', (_req, res) => {
  if (isRxRunning()) return res.status(409).json({ error: 'rx pipeline already running' });
  startRxPipeline(getConfig().rtp?.input ?? {});
  res.json({ ok: true });
});

router.post('/rx/stop', (_req, res) => {
  stopRxPipeline();
  res.json({ ok: true });
});

// ── TX (legacy) ──────────────────────────────────────

router.post('/tx/start', (_req, res) => {
  if (isTxRunning()) return res.status(409).json({ error: 'tx pipeline already running' });
  startTxClient();
  res.json({ ok: true });
});

router.post('/tx/stop', (_req, res) => {
  stopTxClient();
  res.json({ ok: true });
});

// ── rtp_streams ──────────────────────────────────────

// GET /streams/rtp — 전체 목록
router.get('/rtp', (_req, res) => {
  res.json({ ok: true, streams: getRtpStreamStatus() });
});

// GET /streams/rtp/:client — 스트림 상세
router.get('/rtp/:client', (req, res) => {
  const detail = getRtpStreamDetail(req.params.client);
  if (!detail) return res.status(404).json({ error: `stream ${req.params.client} not found` });
  res.json({ ok: true, stream: detail });
});

router.post('/rtp/:client/start', (req, res) => {
  const { client } = req.params;
  try {
    const updates = parseBody(req.body);
    const detail  = getRtpStreamDetail(client);
    if (!detail) return res.status(404).json({ error: `stream ${client} not found` });
    if (Object.keys(updates).length > 0 && detail.type === 'rtp_in')
      updateRtpInConfig(client, updates);
    startRtpStream(client);
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

// PUT /streams/rtp/:client/config — rtp_in 설정 변경 (저장만, 적용은 재시작 필요)
router.put('/rtp/:client/config', (req, res) => {
  const { client } = req.params;
  try {
    const detail = getRtpStreamDetail(client);
    if (!detail) return res.status(404).json({ error: `stream ${client} not found` });
    const updates = parseBody(req.body);
    if (Object.keys(updates).length === 0) return res.status(400).json({ error: 'no fields to update' });
    if (detail.type === 'rtp_in')       updateRtpInConfig(client, updates);
    else if (detail.type === 'rtp_out') updateRtpOutConfig(client, updates);
    else return res.status(400).json({ error: 'unsupported stream type' });
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

// POST /streams/rtp/:client/stop
router.post('/rtp/:client/stop', (req, res) => {
  try {
    stopRtpStream(req.params.client);
    res.json({ ok: true });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

// POST /streams/rtp/:client/targets  { host, port } — rtp_out 전송 대상 추가
router.post('/rtp/:client/targets', (req, res) => {
  const { client } = req.params;
  const { host, port } = req.body ?? {};
  if (!host || !port) return res.status(400).json({ error: 'host and port required' });
  try {
    addRtpOutTarget(client, host, Number(port));
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

// DELETE /streams/rtp/:client/targets  { host, port } — rtp_out 전송 대상 제거
router.delete('/rtp/:client/targets', (req, res) => {
  const { client } = req.params;
  const { host, port } = req.body ?? {};
  if (!host || !port) return res.status(400).json({ error: 'host and port required' });
  try {
    removeRtpOutTarget(client, host, Number(port));
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

// PUT /streams/rtp/:client/codec  { codec, bitrate } — rtp_out 코덱 변경
router.put('/rtp/:client/codec', (req, res) => {
  const { client } = req.params;
  const { codec, bitrate } = req.body ?? {};
  if (!codec) return res.status(400).json({ error: 'codec required' });
  try {
    setRtpOutCodec(client, codec, bitrate ? Number(bitrate) : undefined);
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

export default router;

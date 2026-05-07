import { Router } from 'express';
import {
  getGstStatus,
  getRtpStreamStatus, getRtpStreamDetail,
  startRtpStream, stopRtpStream,
  updateRtpInConfig, updateRtpOutConfig,
  addRtpOutTarget, removeRtpOutTarget, setRtpOutCodec,
  setRtpInFormat, setRtpOutRate,
} from '../../lib/rtp/index.js';

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

router.post('/rtp/:client/start', async (req, res) => {
  const { client } = req.params;
  try {
    const updates    = parseBody(req.body);
    const detail     = getRtpStreamDetail(client);
    if (!detail) return res.status(404).json({ error: `stream ${client} not found` });
    const hasUpdates = Object.keys(updates).length > 0;
    if (hasUpdates) {
      stopRtpStream(client);
      if (detail.type === 'rtp_in')  updateRtpInConfig(client, updates);
      if (detail.type === 'rtp_out') updateRtpOutConfig(client, updates);
    }
    await startRtpStream(client);
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

// PUT /streams/rtp/:client/config
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

// POST /streams/rtp/:client/targets
router.post('/rtp/:client/targets', (req, res) => {
  const { client } = req.params;
  const { host, port } = req.body ?? {};
  if (!host || !port) return res.status(400).json({ error: 'host and port required' });
  try {
    addRtpOutTarget(client, host, Number(port));
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

// DELETE /streams/rtp/:client/targets
router.delete('/rtp/:client/targets', (req, res) => {
  const { client } = req.params;
  const { host, port } = req.body ?? {};
  if (!host || !port) return res.status(400).json({ error: 'host and port required' });
  try {
    removeRtpOutTarget(client, host, Number(port));
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

// PUT /streams/rtp/:client/codec  — rtp_out 런타임 codec 변경
router.put('/rtp/:client/codec', (req, res) => {
  const { client } = req.params;
  const { codec, bitrate } = req.body ?? {};
  if (!codec) return res.status(400).json({ error: 'codec required' });
  if (!['mp3', 'raw', 'opus'].includes(codec)) return res.status(400).json({ error: 'codec must be mp3, raw, or opus' });
  try {
    setRtpOutCodec(client, codec, bitrate ? Number(bitrate) : undefined);
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

// PUT /streams/rtp/:client/format  — 포맷/샘플레이트 설정 + 자동 재시작
// rtp_in: { codec: 'mp3'|'mpa'|'wav'|'l16'|'l24'|'opus'|null|'' → null이면 자동감지, sampleRate: number }
// rtp_out: { sampleRate: number }
router.put('/rtp/:client/format', async (req, res) => {
  const { client } = req.params;
  const { codec, sampleRate } = req.body ?? {};
  try {
    const detail = getRtpStreamDetail(client);
    if (!detail) return res.status(404).json({ error: `stream ${client} not found` });
    if (detail.type === 'rtp_in') {
      if (codec === undefined && sampleRate === undefined)
        return res.status(400).json({ error: 'codec or sampleRate required' });
      await setRtpInFormat(client, codec, sampleRate);
    } else if (detail.type === 'rtp_out') {
      if (sampleRate == null) return res.status(400).json({ error: 'sampleRate required' });
      await setRtpOutRate(client, sampleRate);
    } else {
      return res.status(400).json({ error: 'unsupported stream type' });
    }
    res.json({ ok: true, stream: getRtpStreamDetail(client) });
  } catch (e) { res.status(400).json({ error: e.message }); }
});

export default router;

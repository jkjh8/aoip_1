import {
  startRxPipeline, stopRxPipeline, isRxRunning, setRxPort, getRxPort, setRxBuffer,
  startTxClient, stopTxClient, isTxRunning,
  addTxTarget, removeTxTarget, getTxTargets, setTxCodec,
  getRtpStreamStatus, getRtpStreamDetail,
  addRtpOutTarget, removeRtpOutTarget, setRtpOutCodec,
  updateRtpInConfig, stopRtpStream, startRtpStream,
} from '../lib/gstreamer.js';

function parseUpdates({ port, protocol, address, sampleRate, codec, bitrate, bufferMs, channels, targets } = {}) {
  const u = {};
  if (port       != null) u.port       = Number(port);
  if (protocol   != null) u.protocol   = protocol;
  if (address    != null) u.address    = address;
  if (sampleRate != null) u.sampleRate = Number(sampleRate);
  if (codec      != null) u.codec      = codec;
  if (bitrate    != null) u.bitrate    = Number(bitrate);
  if (bufferMs   != null) u.bufferMs   = Number(bufferMs);
  if (channels   != null) u.channels   = Number(channels);
  if (targets    != null) u.targets    = targets;
  return u;
}

export default function register(socket, { broadcastStatus, config }) {
  // ── RX (legacy) ──────────────────────────────────────────────

  socket.on('rx:start', (cb) => {
    try {
      if (isRxRunning()) return cb?.({ ok: false, error: 'already running' });
      startRxPipeline(config.rtp?.input ?? {});
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('rx:stop', (cb) => {
    try {
      stopRxPipeline();
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('rx:port', ({ port } = {}, cb) => {
    try {
      const was = isRxRunning();
      if (was) stopRxPipeline();
      setRxPort(port);
      if (was) startRxPipeline({});
      broadcastStatus();
      cb?.({ ok: true, port: getRxPort(), restarted: was });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('rx:buffer', ({ bufferMs } = {}, cb) => {
    try {
      if (!bufferMs || bufferMs < 10 || bufferMs > 500)
        return cb?.({ ok: false, error: 'bufferMs must be 10–500' });
      setRxBuffer(bufferMs);
      broadcastStatus();
      cb?.({ ok: true, bufferMs });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── TX (legacy) ──────────────────────────────────────────────

  socket.on('tx:start', (cb) => {
    try {
      if (isTxRunning()) return cb?.({ ok: false, error: 'already running' });
      startTxClient();
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('tx:stop', (cb) => {
    try {
      stopTxClient();
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('tx:target:add', ({ host, port } = {}, cb) => {
    try {
      if (!host || !port) return cb?.({ ok: false, error: 'host and port required' });
      addTxTarget({ host, port: Number(port) });
      broadcastStatus();
      cb?.({ ok: true, targets: getTxTargets() });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('tx:target:remove', ({ host, port } = {}, cb) => {
    try {
      if (!host || !port) return cb?.({ ok: false, error: 'host and port required' });
      removeTxTarget({ host, port: Number(port) });
      broadcastStatus();
      cb?.({ ok: true, targets: getTxTargets() });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('tx:codec', ({ codec, bitrate } = {}, cb) => {
    try {
      if (!codec) return cb?.({ ok: false, error: 'codec required' });
      setTxCodec(codec, bitrate ? Number(bitrate) : undefined);
      broadcastStatus();
      cb?.({ ok: true, codec, bitrate });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── rtp_streams ──────────────────────────────────────────────

  /* 전체 스트림 목록 + 상태 */
  socket.on('rtp:streams:list', (cb) => {
    try { cb?.({ ok: true, streams: getRtpStreamStatus() }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* 특정 스트림 상세 */
  socket.on('rtp:stream:get', ({ client } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      const detail = getRtpStreamDetail(client);
      if (!detail) return cb?.({ ok: false, error: `stream ${client} not found` });
      cb?.({ ok: true, stream: detail });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* 스트림 시작 — rtp_in 전용으로 설정값 동시 적용 가능 */
  socket.on('rtp:stream:start', (data = {}, cb) => {
    try {
      const { client } = data;
      if (!client) return cb?.({ ok: false, error: 'client required' });
      const detail = getRtpStreamDetail(client);
      if (!detail) return cb?.({ ok: false, error: `stream ${client} not found` });
      const updates = parseUpdates(data);
      if (Object.keys(updates).length > 0 && detail.type === 'rtp_in')
        updateRtpInConfig(client, updates);
      startRtpStream(client);
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* 스트림 정지 */
  socket.on('rtp:stream:stop', ({ client } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      stopRtpStream(client);
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* rtp_in 설정 변경 — 저장만, 적용은 rtp:stream:start 로 */
  socket.on('rtp:in:config', (data = {}, cb) => {
    try {
      const { client } = data;
      if (!client) return cb?.({ ok: false, error: 'client required' });
      const detail = getRtpStreamDetail(client);
      if (!detail) return cb?.({ ok: false, error: `stream ${client} not found` });
      if (detail.type !== 'rtp_in') return cb?.({ ok: false, error: 'rtp_in only' });
      const updates = parseUpdates(data);
      delete updates.client;
      if (Object.keys(updates).length === 0) return cb?.({ ok: false, error: 'no fields to update' });
      updateRtpInConfig(client, updates);
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* rtp_out 전송 대상 추가 */
  socket.on('rtp:out:target:add', ({ client, host, port } = {}, cb) => {
    try {
      if (!client || !host || !port)
        return cb?.({ ok: false, error: 'client, host, port required' });
      addRtpOutTarget(client, host, Number(port));
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* rtp_out 전송 대상 제거 */
  socket.on('rtp:out:target:remove', ({ client, host, port } = {}, cb) => {
    try {
      if (!client || !host || !port)
        return cb?.({ ok: false, error: 'client, host, port required' });
      removeRtpOutTarget(client, host, Number(port));
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* rtp_out 코덱 변경 */
  socket.on('rtp:out:codec', ({ client, codec, bitrate } = {}, cb) => {
    try {
      if (!client || !codec)
        return cb?.({ ok: false, error: 'client and codec required' });
      setRtpOutCodec(client, codec, bitrate ? Number(bitrate) : undefined);
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

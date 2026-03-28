import {
  startRxPipeline, stopRxPipeline, isRxRunning, setRxPort, getRxPort, setRxBuffer,
  startTxClient, stopTxClient, isTxRunning,
  addTxTarget, removeTxTarget, getTxTargets, setTxCodec,
  getRtpStreamStatus, getRtpStreamDetail,
  addRtpOutTarget, removeRtpOutTarget, setRtpOutCodec,
  updateRtpInConfig, stopRtpStream, startRtpStream,
} from '../lib/gstreamer.js';

export default function register(socket, { broadcastStatus, config }) {
  // ── RX ──────────────────────────────────────────────────────

  socket.on('rx:start', (cb) => {
    try {
      if (isRxRunning()) return cb?.({ ok: false, error: 'already running' });
      startRxPipeline(config.rtp.input);
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

  // ── TX ──────────────────────────────────────────────────────

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
      addTxTarget({ host, port: Number(port) });
      broadcastStatus();
      cb?.({ ok: true, targets: getTxTargets() });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('tx:target:remove', ({ host, port } = {}, cb) => {
    try {
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

  // ── rtp_streams 관리 ─────────────────────────────────

  /* 전체 스트림 목록 + 상태 */
  socket.on('rtp:streams:list', (cb) => {
    try { cb?.({ ok: true, streams: getRtpStreamStatus() }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* 특정 스트림 상세 (targets, ports 등) */
  socket.on('rtp:stream:get', ({ client } = {}, cb) => {
    try {
      const detail = getRtpStreamDetail(client);
      if (!detail) return cb?.({ ok: false, error: `stream ${client} not found` });
      cb?.({ ok: true, stream: detail });
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

  /* rtp_in 설정 변경 (port, protocol, codec, bufferMs) — 재시작 */
  socket.on('rtp:in:config', ({ client, port, protocol, codec, bufferMs } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      const updates = {};
      if (port      != null) updates.port      = Number(port);
      if (protocol  != null) updates.protocol  = protocol;
      if (codec     != null) updates.codec     = codec;
      if (bufferMs  != null) updates.bufferMs  = Number(bufferMs);
      updateRtpInConfig(client, updates);
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* rtp_out 코덱 변경 */
  socket.on('rtp:out:codec', ({ client, codec, bitrate } = {}, cb) => {
    try {
      if (!client || !codec)
        return cb?.({ ok: false, error: 'client, codec required' });
      setRtpOutCodec(client, codec, bitrate ? Number(bitrate) : undefined);
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* 개별 스트림 정지 */
  socket.on('rtp:stream:stop', ({ client } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      stopRtpStream(client);
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  /* 개별 스트림 시작 */
  socket.on('rtp:stream:start', ({ client } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      startRtpStream(client);
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}
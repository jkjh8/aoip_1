import {
  startRxPipeline, stopRxPipeline, isRxRunning, setRxPort, getRxPort, setRxBuffer,
  startTxClient, stopTxClient, isTxRunning,
  addTxTarget, removeTxTarget, getTxTargets, setTxCodec,
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
}

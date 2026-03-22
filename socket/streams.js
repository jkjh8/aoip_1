import { startRxPipeline, stopRxPipeline, isRxRunning, setRxPort, getRxPort,
         startTxPipeline, stopTxPipeline, isTxRunning,
         addTxTarget, removeTxTarget, getGstStatus } from '../lib/gstreamer.js';

export default function register(socket, { broadcastStatus, config }) {
  socket.on('rx:start', (cb) => {
    try {
      if (isRxRunning()) return cb?.({ ok: false, error: 'already running' });
      startRxPipeline(config.rtp.input); broadcastStatus(); cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('rx:stop', (cb) => {
    try { stopRxPipeline(); broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
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

  socket.on('tx:start', (cb) => {
    try {
      if (isTxRunning()) return cb?.({ ok: false, error: 'already running' });
      startTxPipeline(getGstStatus().tx.targets); broadcastStatus(); cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('tx:stop', (cb) => {
    try { stopTxPipeline(); broadcastStatus(); cb?.({ ok: true }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('tx:target:add', ({ host, port } = {}, cb) => {
    try { addTxTarget({ host, port: Number(port) }); broadcastStatus(); cb?.({ ok: true, targets: getGstStatus().tx.targets }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('tx:target:remove', ({ host, port } = {}, cb) => {
    try { removeTxTarget({ host, port: Number(port) }); broadcastStatus(); cb?.({ ok: true, targets: getGstStatus().tx.targets }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

import {
  getDaemonStatus, startDaemon, stopDaemon, restartDaemon,
  getConfig, setConfig,
  getPtpConfig, setPtpConfig, getPtpStatus,
  getSources, addSource, removeSource, getSourceSdp,
  getSinks, addSink, removeSink, getSinkStatus,
  browseAll, browseMdns, browseSap,
} from '../lib/aes67daemon.js';

/**
 * socket events:
 *
 *   aes67:status          → { running, ready, pid, url }
 *   aes67:start           → { ok }
 *   aes67:stop            → { ok }
 *   aes67:restart         → { ok }
 *
 *   aes67:config:get      → { ...daemonConf }
 *   aes67:config:set      { ...fields }  → { ok }
 *
 *   aes67:ptp:config:get  → { domain, dscp }
 *   aes67:ptp:config:set  { domain?, dscp? }  → { ok }
 *   aes67:ptp:status      → { locked, jitter, masterId, ... }
 *
 *   aes67:sources:list    → [ source, ... ]
 *   aes67:source:add      { id, ...fields }  → { ok }
 *   aes67:source:remove   { id }  → { ok }
 *   aes67:source:sdp      { id }  → { ok, sdp }
 *
 *   aes67:sinks:list      → [ sink, ... ]
 *   aes67:sink:add        { id, ...fields }  → { ok }
 *   aes67:sink:remove     { id }  → { ok }
 *   aes67:sink:status     { id }  → { ok, status }
 *
 *   aes67:browse          { type?: 'mdns'|'sap'|'all' }  → [ remote source, ... ]
 */
export default function register(socket, { broadcastStatus }) {
  // ── 생명주기 ────────────────────────────────────────────

  socket.on('aes67:status', async (cb) => {
    cb?.(await getDaemonStatus());
  });

  socket.on('aes67:start', async (cb) => {
    try {
      await startDaemon();
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:stop', async (cb) => {
    try {
      await stopDaemon();
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:restart', async (cb) => {
    try {
      await restartDaemon();
      await broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── 데몬 설정 ──────────────────────────────────────────

  socket.on('aes67:config:get', async (cb) => {
    try { cb?.({ ok: true, config: await getConfig() }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:config:set', async (data = {}, cb) => {
    try {
      await setConfig(data);
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── PTP ────────────────────────────────────────────────

  socket.on('aes67:ptp:config:get', async (cb) => {
    try { cb?.({ ok: true, config: await getPtpConfig() }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:ptp:config:set', async (data = {}, cb) => {
    try {
      await setPtpConfig(data);
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:ptp:status', async (cb) => {
    try { cb?.({ ok: true, status: await getPtpStatus() }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── Sources ────────────────────────────────────────────

  socket.on('aes67:sources:list', async (cb) => {
    try { cb?.({ ok: true, sources: await getSources() }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:source:add', async ({ id, ...fields } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      await addSource(id, fields);
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:source:remove', async ({ id } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      await removeSource(id);
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:source:sdp', async ({ id } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      const sdp = await getSourceSdp(id);
      cb?.({ ok: true, sdp });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── Sinks ──────────────────────────────────────────────

  socket.on('aes67:sinks:list', async (cb) => {
    try { cb?.({ ok: true, sinks: await getSinks() }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:sink:add', async ({ id, ...fields } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      await addSink(id, fields);
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:sink:remove', async ({ id } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      await removeSink(id);
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:sink:status', async ({ id } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      cb?.({ ok: true, status: await getSinkStatus(id) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // ── Browse ─────────────────────────────────────────────

  socket.on('aes67:browse', async ({ type = 'all' } = {}, cb) => {
    try {
      let result;
      if (type === 'mdns')     result = await browseMdns();
      else if (type === 'sap') result = await browseSap();
      else                     result = await browseAll();
      cb?.({ ok: true, sources: result });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

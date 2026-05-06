import {
  getDaemonStatus,
  getConfig, setConfig,
  getPtpConfig, setPtpConfig, getPtpStatus,
  getSources, fetchSources, addSource, removeSource, getSourceSdp,
  getSinks, fetchSinks, addSink, removeSink, getSinkStatus,
  browseAll, browseMdns, browseSap, enrichSinks,
} from '../lib/aes67daemon.js';
import { syncAes67Active } from '../lib/channels/index.js';

/**
 * socket events:
 *
 *   aes67:status          → { running, ready, url }
 *
 *   aes67:config:get      → { ...daemonConf }
 *   aes67:config:set      { ...fields }  → { ok }
 *
 *   aes67:ptp:config:get  → { domain, dscp }
 *   aes67:ptp:config:set  { domain?, dscp? }  → { ok }
 *
 *   aes67:source:add      { id, ...fields }  → { ok }  → broadcast aes67:sources
 *   aes67:source:remove   { id }  → { ok }            → broadcast aes67:sources
 *   aes67:source:sdp      { id }  → { ok, sdp }
 *
 *   aes67:sink:add        { id, ...fields }  → { ok }  → broadcast aes67:sinks
 *   aes67:sink:remove     { id }  → { ok }             → broadcast aes67:sinks
 *   aes67:sink:status     { id }  → { ok, status }
 *
 *   aes67:browse          { type?: 'mdns'|'sap'|'all' }  → [ remote source, ... ]
 *
 *  on connect: aes67:sources → [ source, ... ]
 *              aes67:sinks   → [ sink, ... ]
 */
export default function register(socket, ctx) {
  const { io, broadcastChannels } = ctx;

  async function broadcastSources() {
    try {
      const sources = await fetchSources();
      io.emit('aes67:sources', sources);
      syncAes67Active('output', sources);
      broadcastChannels();
    } catch { /* ignore */ }
  }

  async function broadcastSinks() {
    try {
      const sinks = await fetchSinks();
      io.emit('aes67:sinks', enrichSinks(sinks));
      syncAes67Active('input', sinks);
      broadcastChannels();
    } catch { /* ignore */ }
  }

  // 접속 시 초기 데이터 전송 — 항상 REST API 우선 (캐시 우회)
  fetchSources().then(s => socket.emit('aes67:sources', s)).catch(() => {});
  fetchSinks().then(s => socket.emit('aes67:sinks', enrichSinks(s))).catch(() => {});
  getPtpStatus().then(s => socket.emit('aes67:ptp:status', s)).catch(() => {});

  // ── 상태 조회 ──────────────────────────────────────────

  socket.on('aes67:status', async (cb) => {
    cb?.(await getDaemonStatus());
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

  socket.on('aes67:source:add', async ({ id, ...fields } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      await addSource(id, fields);
      cb?.({ ok: true });
      broadcastSources();
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:source:remove', async ({ id } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      await removeSource(id);
      cb?.({ ok: true });
      broadcastSources();
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

  socket.on('aes67:sink:add', async ({ id, ...fields } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      await addSink(id, fields);
      cb?.({ ok: true });
      broadcastSinks();
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:sink:remove', async ({ id } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      await removeSink(id);
      cb?.({ ok: true });
      broadcastSinks();
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('aes67:sink:status', async ({ id } = {}, cb) => {
    try {
      if (id == null) return cb?.({ ok: false, error: 'id required' });
      const sinks = await getSinks();
      if (!sinks.some(s => String(s.id) === String(id)))
        return cb?.({ ok: true, status: null });
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

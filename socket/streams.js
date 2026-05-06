import {
  getRtpStreamStatus, getRtpStreamDetail,
  addRtpOutTarget, removeRtpOutTarget, setRtpOutCodec,
  updateRtpInConfig, stopRtpStream, startRtpStream,
} from '../lib/rtp/index.js';

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

export default function register(socket, { broadcastStatus }) {
  socket.on('rtp:streams:list', (cb) => {
    try { cb?.({ ok: true, streams: getRtpStreamStatus() }); }
    catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('rtp:stream:get', ({ client } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      const detail = getRtpStreamDetail(client);
      if (!detail) return cb?.({ ok: false, error: `stream ${client} not found` });
      cb?.({ ok: true, stream: detail });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

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

  socket.on('rtp:stream:stop', ({ client } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      stopRtpStream(client);
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

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

  socket.on('rtp:out:target:add', ({ client, host, port } = {}, cb) => {
    try {
      if (!client || !host || !port)
        return cb?.({ ok: false, error: 'client, host, port required' });
      addRtpOutTarget(client, host, Number(port));
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('rtp:out:target:remove', ({ client, host, port } = {}, cb) => {
    try {
      if (!client || !host || !port)
        return cb?.({ ok: false, error: 'client, host, port required' });
      removeRtpOutTarget(client, host, Number(port));
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('rtp:out:codec', ({ client, codec, bitrate } = {}, cb) => {
    try {
      if (!client || !codec)
        return cb?.({ ok: false, error: 'client and codec required' });
      if (codec !== 'mp3' && codec !== 'raw')
        return cb?.({ ok: false, error: 'codec must be mp3 or raw' });
      setRtpOutCodec(client, codec, bitrate ? Number(bitrate) : undefined);
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

import {
  getRtpStreamStatus, getRtpStreamDetail,
  setRtpOutTarget, clearRtpOutTarget, setRtpOutCodec,
  updateRtpInConfig, updateRtpOutConfig, stopRtpStream, startRtpStream,
  setRtpInFormat, setRtpOutRate,
} from '../lib/rtp/index.js';
import { getAllChannelDefs, setChannelActive } from '../lib/channels/index.js';
import logger from '../lib/logger.js';

function _setRtpActive(client, active) {
  const { inputs, outputs } = getAllChannelDefs()
  for (const ch of inputs)
    if (ch.source?.type === 'rtp' && ch.source.name === client)
      { try { setChannelActive('input',  ch.id, active) } catch { } break }
  for (const ch of outputs)
    if (ch.source?.type === 'rtp' && ch.source.name === client)
      { try { setChannelActive('output', ch.id, active) } catch { } break }
}

function parseUpdates({ port, protocol, address, sampleRate, codec, bitrate, bufferMs, channels } = {}) {
  const u = {};
  if (port       != null) u.port       = Number(port);
  if (protocol   != null) u.protocol   = protocol;
  if (address    != null) u.address    = address;
  if (sampleRate != null) u.sampleRate = Number(sampleRate);
  if (codec      != null) u.codec      = codec;
  if (bitrate    != null) u.bitrate    = Number(bitrate);
  if (bufferMs   != null) u.bufferMs   = Number(bufferMs);
  if (channels   != null) u.channels   = Number(channels);
  return u;
}

export default function register(socket, { broadcastStatus, broadcastChannels }) {
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
      const { client, host: targetHost, port: targetPort } = data;
      if (!client) return cb?.({ ok: false, error: 'client required' });
      const detail = getRtpStreamDetail(client);
      if (!detail) return cb?.({ ok: false, error: `stream ${client} not found` });
      const updates = parseUpdates(data);
      if (Object.keys(updates).length > 0) {
        if (detail.type === 'rtp_in')  updateRtpInConfig(client, updates);
        if (detail.type === 'rtp_out') updateRtpOutConfig(client, updates);
      }
      if (detail.type === 'rtp_out' && targetHost && targetPort)
        setRtpOutTarget(client, targetHost, Number(targetPort));
      logger.info('[socket] rtp:stream:start client=%s sid=%s data=%j', client, socket.id, data);
      startRtpStream(client);
      _setRtpActive(client, true);
      broadcastChannels();
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) {
      logger.error('[socket] rtp:stream:start error client=%s: %s', data?.client, e.message);
      cb?.({ ok: false, error: e.message });
    }
  });

  socket.on('rtp:stream:stop', ({ client } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      logger.info('[socket] rtp:stream:stop client=%s sid=%s', client, socket.id);
      stopRtpStream(client);
      _setRtpActive(client, false);
      broadcastChannels();
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) {
      logger.error('[socket] rtp:stream:stop error client=%s: %s', client, e.message);
      cb?.({ ok: false, error: e.message });
    }
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

  socket.on('rtp:out:target:set', ({ client, host, port } = {}, cb) => {
    try {
      if (!client || !host || !port)
        return cb?.({ ok: false, error: 'client, host, port required' });
      setRtpOutTarget(client, host, Number(port));
      broadcastChannels();
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('rtp:out:target:clear', ({ client } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      clearRtpOutTarget(client);
      broadcastChannels();
      broadcastStatus();
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('rtp:out:codec', ({ client, codec, bitrate } = {}, cb) => {
    try {
      if (!client || !codec)
        return cb?.({ ok: false, error: 'client and codec required' });
      if (!['mp3', 'raw', 'opus'].includes(codec))
        return cb?.({ ok: false, error: 'codec must be mp3, raw, or opus' });
      setRtpOutCodec(client, codec, bitrate ? Number(bitrate) : undefined);
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // rtp_in 포맷/샘플레이트 설정 + 재시작
  // { client, codec: 'l16'|'l24'|'mpa'|'pcmu'|'pcma'|'opus'|'' , sampleRate: number }
  socket.on('rtp:in:format', async ({ client, codec, sampleRate } = {}, cb) => {
    try {
      if (!client) return cb?.({ ok: false, error: 'client required' });
      if (codec === undefined && sampleRate === undefined)
        return cb?.({ ok: false, error: 'codec or sampleRate required' });
      await setRtpInFormat(client, codec, sampleRate);
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  // rtp_out 샘플레이트 변경 + 재시작
  // { client, sampleRate: number }
  socket.on('rtp:out:rate', async ({ client, sampleRate } = {}, cb) => {
    try {
      if (!client || sampleRate == null)
        return cb?.({ ok: false, error: 'client and sampleRate required' });
      await setRtpOutRate(client, sampleRate);
      broadcastStatus();
      cb?.({ ok: true, stream: getRtpStreamDetail(client) });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

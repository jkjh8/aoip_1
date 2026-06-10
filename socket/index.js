import { Server as SocketIO } from 'socket.io';
import { getBridgeStatus } from '../lib/bridges.js';
import { getDaemonStatus, daemonEvents, enrichSinks } from '../lib/aes67daemon.js';
import { getRtpStreamStatus, streamEvents } from '../lib/rtp/index.js';
import { getChannels, getSavedRoutes, syncAes67Active, getI2sMode, i2sModeEvents } from '../lib/channels/index.js';
import { isDspRunning, getDspUptime, getGrSnapshot } from '../lib/dsp/index.js';

import logger from '../lib/logger.js';
import registerStreams   from './streams.js';
import registerChannels from './channels.js';
import registerSystem   from './system.js';
import registerAes67    from './aes67.js';
import registerDspEq    from './dsp_eq.js';
import registerDspDyn   from './dsp_dynamics.js';

const STATUS_INTERVAL = 2000;
const LEVEL_INTERVAL  = 33;   // 30 fps

// level(dB) → Uint8: -120..0 dB → 0..240 (0.5 dB step). 범위 밖은 클램프.
function _encLevel(dB) {
  const v = Math.round((dB + 120) * 2);
  return v < 0 ? 0 : v > 255 ? 255 : v;
}
// gr reduction(dB, 음수/0) → Uint8: 0..-60 dB → 0..120 (0.5 dB step)
function _encGr(dB) {
  const v = Math.round(-dB * 2);
  return v < 0 ? 0 : v > 255 ? 255 : v;
}

function _encodeLevels(ch) {
  const ins = ch.inputs, outs = ch.outputs;
  const n = ins.length, m = outs.length;
  // [u8 n][u8 m] + n*[u8 id, u8 lvl] + m*[u8 id, u8 lvl]
  const buf = Buffer.allocUnsafe(2 + (n + m) * 2);
  buf[0] = n; buf[1] = m;
  let o = 2;
  for (let i = 0; i < n; i++) { buf[o++] = ins[i].id  & 0xff; buf[o++] = _encLevel(ins[i].level); }
  for (let i = 0; i < m; i++) { buf[o++] = outs[i].id & 0xff; buf[o++] = _encLevel(outs[i].level); }
  return buf;
}

function _encodeGr(gr) {
  const ins = gr.inputs, outs = gr.outputs;
  const n = ins.length, m = outs.length;
  // [u8 n][u8 m] + n*[u8 ch, u8 gate, u8 comp] + m*[u8 ch, u8 gate, u8 comp, u8 lim]
  const buf = Buffer.allocUnsafe(2 + n * 3 + m * 4);
  buf[0] = n; buf[1] = m;
  let o = 2;
  for (let i = 0; i < n; i++) {
    const r = ins[i];
    buf[o++] = r.ch & 0xff;
    buf[o++] = _encGr(r.gate ?? 0);
    buf[o++] = _encGr(r.comp ?? 0);
  }
  for (let i = 0; i < m; i++) {
    const r = outs[i];
    buf[o++] = r.ch & 0xff;
    buf[o++] = _encGr(r.gate ?? 0);
    buf[o++] = _encGr(r.comp ?? 0);
    buf[o++] = _encGr(r.lim  ?? 0);
  }
  return buf;
}

let cachedConnections = [];
let cachedAes67Status = { running: false, ready: false, url: 'http://127.0.0.1:8080' };
function _routesToConnections(routes) {
  const map = new Map();
  for (const { src, dst } of routes) {
    if (!map.has(src)) map.set(src, []);
    map.get(src).push(dst);
  }
  return Array.from(map.entries()).map(([port, connections]) => ({ port, connections }));
}

async function snapshot() {
  const connections = _routesToConnections(getSavedRoutes());
  cachedConnections = connections;

  const rtpStreams = getRtpStreamStatus();
  return {
    engine:   { running: isDspRunning(), uptime: getDspUptime() },
    bridges:  getBridgeStatus(),
    streams:  {
      inputs:  rtpStreams.filter(s => s.type === 'rtp_in'),
      outputs: rtpStreams.filter(s => s.type === 'rtp_out'),
    },
    channels:    getChannels(),
    connections,
    aes67:    cachedAes67Status,
    i2s:      getI2sMode(),
  };
}

async function refreshAes67Status() {
  try { cachedAes67Status = await getDaemonStatus(); } catch { /* ignore */ }
}

/**
 * Socket.IO 서버를 초기화하고 이벤트 핸들러를 등록합니다.
 * @param {import('http').Server} httpServer
 * @param {object} config  config/audio.json 내용
 * @returns {{ io: import('socket.io').Server, broadcastStatus: () => Promise<void> }}
 */
export function setupSocket(httpServer, config) {
  const io = new SocketIO(httpServer, {
    cors: { origin: '*', methods: ['GET', 'POST'] },
    transports:   ['websocket'],
    pingInterval: 25000,
    pingTimeout:  60000,
  });

  let _dspBusyUntil = 0;
  function markDspBusy(ms = 400) { _dspBusyUntil = Date.now() + ms; }

  let _statusPending = false;
  async function broadcastStatus() {
    if (io.engine.clientsCount === 0) return;
    if (_statusPending) return;
    if (Date.now() < _dspBusyUntil) return;
    _statusPending = true;
    try {
      const s = await snapshot();
      io.emit('status', s);
    } catch { /* engine not ready */ } finally {
      _statusPending = false;
    }
  }

  function broadcastChannels() {
    if (io.engine.clientsCount === 0) return;
    io.emit('channels', getChannels());
  }

  function broadcastStreams() {
    if (io.engine.clientsCount === 0) return;
    const all = getRtpStreamStatus();
    io.emit('streams', {
      inputs:  all.filter(s => s.type === 'rtp_in'),
      outputs: all.filter(s => s.type === 'rtp_out'),
    });
  }

  // 레벨 미터 — 모든 연결 클라이언트에 binary broadcast (DSP 커맨드 처리 중엔 억제)
  setInterval(() => {
    if (io.engine.clientsCount === 0) return;
    if (Date.now() < _dspBusyUntil) return;
    const ch = getChannels();
    io.emit('levels', _encodeLevels(ch));
    const gr = getGrSnapshot();
    if (gr.inputs.length || gr.outputs.length) io.emit('gr', _encodeGr(gr));
  }, LEVEL_INTERVAL);

  // 전체 상태 — 느린 주기
  setInterval(broadcastStatus, STATUS_INTERVAL);

  // RTP 스트림 상태 변경 → 채널 + 스트림 + 상태 브로드캐스트
  streamEvents.on('state:changed', () => { broadcastChannels(); broadcastStreams(); broadcastStatus(); });

  // AES67 데몬 이벤트 → Socket.IO broadcast (클라이언트 없으면 스킵)
  daemonEvents.on('sources:changed', (sources) => {
    if (io.engine.clientsCount > 0) io.emit('aes67:sources', sources);
    syncAes67Active('output', sources);
    broadcastChannels();
  });
  daemonEvents.on('sinks:changed', (sinks) => {
    if (io.engine.clientsCount > 0) io.emit('aes67:sinks', enrichSinks(sinks));
    syncAes67Active('input', sinks);
    broadcastChannels();
  });
  daemonEvents.on('ptp:changed', (data) => {
    if (io.engine.clientsCount > 0) io.emit('aes67:ptp:status', data);
  });

  // I2S mono/stereo mode 변경 → 클라이언트 브로드캐스트 + 채널 상태 갱신
  i2sModeEvents.on('changed', (info) => {
    if (io.engine.clientsCount > 0) io.emit('dsp:mode', info.mode);
    broadcastChannels();
    broadcastStatus();
  });

  const ctx = {
    io,
    broadcastStatus,
    broadcastChannels,
    markDspBusy,
    getCached: () => cachedConnections,
    config
  };

  io.on('connection', async (socket) => {
    logger.info('[io] client connected:', socket.id);
    await refreshAes67Status();
    try { socket.emit('status', await snapshot()); } catch { /* ignore */ }
    socket.emit('channels', getChannels());
    socket.emit('dsp:mode', getI2sMode());
    const _allStreams = getRtpStreamStatus();
    socket.emit('streams', { inputs: _allStreams.filter(s => s.type === 'rtp_in'), outputs: _allStreams.filter(s => s.type === 'rtp_out') });

    socket.on('disconnect', (reason) => {
      logger.info('[io] disconnected: %s reason=%s', socket.id, reason);
    });

    registerStreams(socket, ctx);
    registerChannels(socket, ctx);
    registerSystem(socket);
    registerAes67(socket, ctx);
    registerDspEq(socket, ctx);
    registerDspDyn(socket, ctx);
  });

  return { io, broadcastStatus, broadcastChannels };
}

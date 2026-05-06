import { Server as SocketIO } from 'socket.io';
import { getBridgeStatus } from '../lib/bridges.js';
import { getDaemonStatus, daemonEvents, enrichSinks } from '../lib/aes67daemon.js';
import { getRtpStreamStatus, streamEvents } from '../lib/rtp/index.js';
import { getChannels, getSavedRoutes, syncAes67Active } from '../lib/channels/index.js';
import { isDspRunning, getDspUptime }               from '../lib/dsp/index.js';

import logger from '../lib/logger.js';
import registerStreams   from './streams.js';
import registerChannels from './channels.js';
import registerSystem   from './system.js';
import registerAes67    from './aes67.js';

const STATUS_INTERVAL = 2000;
const LEVEL_INTERVAL  = 80;   // ~12 fps

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
    cors: { origin: '*', methods: ['GET', 'POST'] }
  });

  async function broadcastStatus() {
    if (io.engine.clientsCount === 0) return;
    try {
      const s = await snapshot();
      io.emit('status', s);
    } catch { /* engine not ready */ }
  }

  function broadcastChannels() {
    if (io.engine.clientsCount === 0) return;
    io.emit('channels', getChannels());
  }

  // 레벨 미터 — 빠른 주기로 별도 emit
  setInterval(() => {
    if (io.engine.clientsCount === 0) return;
    const ch = getChannels();
    io.emit('levels', {
      inputs:  ch.inputs.map(c  => ({ id: c.id, level: c.level })),
      outputs: ch.outputs.map(c => ({ id: c.id, level: c.level })),
    });
  }, LEVEL_INTERVAL);

  // 전체 상태 — 느린 주기
  setInterval(broadcastStatus, STATUS_INTERVAL);

  // RTP 스트림 상태 변경 → 채널 + 상태 브로드캐스트
  streamEvents.on('state:changed', () => { broadcastChannels(); broadcastStatus(); });

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

  const ctx = {
    io,
    broadcastStatus,
    broadcastChannels,
    getCached: () => cachedConnections,
    config
  };

  io.on('connection', async (socket) => {
    logger.info('[io] client connected:', socket.id);
    await refreshAes67Status();
    try { socket.emit('status', await snapshot()); } catch { /* ignore */ }
    socket.emit('channels', getChannels());

    socket.on('disconnect', () => {
      logger.info('[io] disconnected:', socket.id);
    });

    registerStreams(socket, ctx);
    registerChannels(socket, ctx);
    registerSystem(socket);
    registerAes67(socket, ctx);
  });

  return { io, broadcastStatus };
}

import { Server as SocketIO } from 'socket.io';
import { getPorts, getConnections, isJackRunning } from '../lib/jack.js';
import { getBridgeStatus }                          from '../lib/bridges.js';
import { getGstStatus, getRxStats }                 from '../lib/gstreamer.js';
import { getChannels }                              from '../lib/channels.js';
import { getLimiterMeters }                         from '../lib/gainer.js';

import logger from '../lib/logger.js';
import registerJack     from './jack.js';
import registerBridges  from './bridges.js';
import registerStreams   from './streams.js';
import registerChannels from './channels.js';
import registerDsp      from './dsp.js';

const STATUS_INTERVAL = 2000;
const LEVEL_INTERVAL  = 80;   // ~12 fps

let cachedConnections = [];
const limiterWatchers = new Map();   // socketId → Set<chId>

function watchedChannels() {
  const ids = new Set();
  for (const set of limiterWatchers.values()) for (const id of set) ids.add(id);
  return ids;
}

async function snapshot() {
  const [ports, connections] = isJackRunning()
    ? await Promise.all([getPorts(), getConnections()]).catch(() => [[], []])
    : [[], []];

  cachedConnections = connections;

  return {
    jack:     { running: isJackRunning(), ports, connections },
    bridges:  getBridgeStatus(),
    streams:  getGstStatus(),
    rxStats:  getRxStats(),
    channels: getChannels(connections)
  };
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
      io.emit('rx:stats', s.rxStats);
    } catch { /* jack not ready */ }
  }

  // 레벨 미터 — 빠른 주기로 별도 emit
  setInterval(() => {
    if (io.engine.clientsCount === 0) return;
    const ch        = getChannels(cachedConnections);
    const limMeters = getLimiterMeters();
    const watched   = watchedChannels();
    io.emit('levels', {
      inputs:  ch.inputs.map(c  => ({ id: c.id, level: c.level })),
      outputs: ch.outputs.map(c => ({
        id:      c.id,
        level:   c.level,
        limiter: watched.has(c.id) ? (limMeters.get(`out ${c.id}`) ?? null) : undefined
      }))
    });
  }, LEVEL_INTERVAL);

  // 전체 상태 — 느린 주기
  setInterval(broadcastStatus, STATUS_INTERVAL);

  const ctx = {
    io,
    broadcastStatus,
    getCached: () => cachedConnections,
    limiterWatchers,
    config
  };

  io.on('connection', async (socket) => {
    logger.info('[io] client connected:', socket.id);
    try { socket.emit('status', await snapshot()); } catch { /* ignore */ }

    socket.on('disconnect', () => {
      logger.info('[io] disconnected:', socket.id);
      limiterWatchers.delete(socket.id);
    });

    registerJack(socket, ctx);
    registerBridges(socket, ctx);
    registerStreams(socket, ctx);
    registerChannels(socket, ctx);
    registerDsp(socket, ctx);
  });

  return { io, broadcastStatus };
}

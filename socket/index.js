import { Server as SocketIO } from 'socket.io';
import { isJackRunning } from '../lib/jack.js';
import { getBridgeStatus, getUsbGadgetEnabled, isUdcConnected } from '../lib/bridges.js';
import { getGstStatus, getRxStats, getRtpStreamStatus } from '../lib/gstreamer.js';
import { getChannels, getSavedRoutes }              from '../lib/channels.js';
import { getLimiterMeters }                         from '../lib/dsp.js';
import { getDaemonStatus }                          from '../lib/aes67daemon.js';

import logger from '../lib/logger.js';
import registerJack     from './jack.js';
import registerBridges  from './bridges.js';
import registerStreams   from './streams.js';
import registerChannels from './channels.js';
import registerDsp      from './dsp.js';
import registerUsb      from './usb.js';
import registerSystem   from './system.js';
import registerAes67    from './aes67.js';

const STATUS_INTERVAL = 2000;
const AES67_INTERVAL  = 10000;  // aes67 상태는 10초마다 (execAsync fork 최소화)
const LEVEL_INTERVAL  = 80;   // ~12 fps

let cachedConnections = [];
let cachedAes67Status = { running: false, ready: false, url: 'http://127.0.0.1:8080' };
const limiterWatchers = new Map();   // socketId → Set<chId>

function watchedChannels() {
  const ids = new Set();
  for (const set of limiterWatchers.values()) for (const id of set) ids.add(id);
  return ids;
}

/** state.routing ({ src, dst }[]) → JACK connections 형식으로 변환 */
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

  return {
    jack:     { running: false, ports: [], connections },
    bridges:  getBridgeStatus(),
    streams:  { ...getGstStatus(), rtpStreams: getRtpStreamStatus() },
    rxStats:  getRxStats(),
    channels: getChannels(connections),
    usb:      { enabled: getUsbGadgetEnabled(), connected: isUdcConnected() },
    aes67:    cachedAes67Status,
  };
}

// aes67 상태는 별도 느린 타이머로만 갱신 (execAsync fork가 RT 스레드 방해 방지)
async function refreshAes67Status() {
  try { cachedAes67Status = await getDaemonStatus(); } catch { /* ignore */ }
}
refreshAes67Status();
setInterval(refreshAes67Status, AES67_INTERVAL);

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

  // 전체 상태 — 느린 주기 (rtpStreams stats 포함)
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
    registerUsb(socket, ctx);
    registerSystem(socket);
    registerAes67(socket, ctx);
  });

  return { io, broadcastStatus };
}

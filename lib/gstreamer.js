import { spawn, execSync } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import logger from './logger.js';
import { getConfig, saveConfig } from './config.js';
import { connect } from './jack.js';
import { getInputSrcPorts, getOutputSinkPorts, getDspClientOf, getDspLocalId, getRtpChStart } from './channels.js';
import { sendToEngine } from './dsp.js';

const __dirname  = dirname(fileURLToPath(import.meta.url));
const SCRIPTS    = join(__dirname, '../scripts');

let _startupComplete = false;
export function notifyRtpStartupComplete() { _startupComplete = true; }
function saveAudioConfig(updatedCfg) {
  const raw = getConfig();
  const idx = (raw.rtp_streams ?? []).findIndex(s => s.client === updatedCfg.client);
  if (idx >= 0) {
    const keep = ['type','name','client','port','channels','protocol','sampleRate',
                  'bufferMs','codec','bitrate','targets','enabled'];
    keep.forEach(k => { if (updatedCfg[k] !== undefined) raw.rtp_streams[idx][k] = updatedCfg[k]; });
  }
  saveConfig();
}

// ─────────────────────────────────────────────
// RX state
// ─────────────────────────────────────────────
let rxProcess    = null;
let rxPort       = 10001;
let rxChannels   = 2;
let rxBufMs      = 100;
let rxPorts      = [];           // reported JACK port names
let rxReady      = false;        // true once "[rtp_recv] ready" received
let rxShouldRun  = false;        // intent flag for auto-restart
let rxLastCfg    = {};           // last config passed to startRxPipeline

const rxStats = {
  codec:       'unknown',
  bitrateKbps: 0,
  bufUsedMs:   0,
  packets:     0,
  drops:       0,
  srcIp:       null,
  srcPort:     null,
};

// ─────────────────────────────────────────────
// TX / rtp_send state
// ─────────────────────────────────────────────
let txProcess  = null;         // rtp_send process
let txPorts    = [];           // reported JACK port names
let txTargets  = [];           // [{ host, port }, ...]
let txCodec    = 'mp3';
let txBitrate  = 320;
let txReady    = false;

// ─────────────────────────────────────────────
// RX public API
// ─────────────────────────────────────────────

export function setRxPort(port) {
  rxPort = Number(port);
  logger.info('[gst] rx port set to %d', rxPort);
}

export function getRxPort()     { return rxPort; }
export function getRxPorts()    { return [...rxPorts]; }
export function getRxStats()    { return { ...rxStats }; }

/** Resolves when rtp_recv reports ready (or rejects after timeoutMs). */
export function waitForRxReady(timeoutMs = 8000) {
  if (rxReady) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('rtp_recv ready timeout')), timeoutMs);
    const poll = setInterval(() => {
      if (rxReady) { clearInterval(poll); clearTimeout(t); resolve(); }
    }, 100);
  });
}

/** Resolves when rtp_send reports ready (or rejects after timeoutMs). */
export function waitForTxReady(timeoutMs = 8000) {
  if (txReady) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('rtp_send ready timeout')), timeoutMs);
    const poll = setInterval(() => {
      if (txReady) { clearInterval(poll); clearTimeout(t); resolve(); }
    }, 100);
  });
}

export function startRxPipeline(cfg) {
  rxShouldRun = true;
  rxLastCfg   = cfg || {};
  if (rxProcess) {
    logger.info('[gst] rx already running (pid %d)', rxProcess.pid);
    return;
  }

  const port     = rxPort ?? cfg?.port ?? 10001;
  const ch       = rxChannels;
  const protocol = cfg?.protocol ?? 'raw';

  const bin  = join(SCRIPTS, 'rtp_recv');
  const args = [String(port), String(ch), protocol];

  logger.info('[gst] starting rtp_recv: %s %s', bin, args.join(' '));

  const proc = spawn(bin, args, {
    stdio: ['ignore', 'pipe', 'pipe'],
    detached: false
  });

  rxProcess = proc;
  rxPorts   = [];
  rxReady   = false;

  let stdoutBuf = '';

  proc.stdout.on('data', (d) => {
    stdoutBuf += d.toString();
    let nl;
    while ((nl = stdoutBuf.indexOf('\n')) >= 0) {
      const line = stdoutBuf.slice(0, nl).trim();
      stdoutBuf = stdoutBuf.slice(nl + 1);
      _parseRxLine(line);
    }
  });

  proc.stderr.on('data', (d) =>
    d.toString().split('\n').filter(Boolean)
      .forEach(l => logger.debug('[rtp_recv] %s', l))
  );

  proc.on('exit', (code, signal) => {
    logger.info('[rtp_recv] exited code=%s signal=%s', code, signal);
    rxProcess = null;
    rxPorts   = [];
    rxReady   = false;
    if (rxShouldRun) {
      setTimeout(() => {
        if (rxShouldRun && !rxProcess) {
          logger.info('[gst] auto-restarting rtp_recv');
          startRxPipeline(rxLastCfg);
        }
      }, 2000);
    }
  });

  proc.on('error', (err) => {
    logger.error('[rtp_recv] error: %s', err.message);
    rxProcess = null;
  });
}

function _parseRxLine(line) {
  if (!line.startsWith('stats ')) logger.debug('[rtp_recv] %s', line);

  if (line.startsWith('[rtp_recv] ready')) {
    rxReady = true;
    logger.info('[gst] rx started  rtp_recv + gst udpsrc port=%d codec=%s',
      rxPort, rxStats.codec);
    return;
  }

  if (line.startsWith('ports:')) {
    rxPorts = line.replace('ports:', '').split(',').map(s => s.trim());
    logger.debug('[rtp_recv] ready  ports: %s', rxPorts.join(', '));
    return;
  }

  // stats codec=... bufMs=N packets=N drops=N srcIp=... srcPort=N bitrateKbps=N
  if (line.startsWith('stats ')) {
    const m = line.match(
      /codec=(.+?)\s+bufMs=(\d+)\s+packets=(\d+)\s+drops=(\d+)\s+srcIp=(\S+)\s+srcPort=(\d+)\s+bitrateKbps=(\d+)/
    );
    if (m) {
      rxStats.codec       = m[1];
      rxStats.bufUsedMs   = Number(m[2]);
      rxStats.packets     = Number(m[3]);
      rxStats.drops       = Number(m[4]);
      rxStats.srcIp       = m[5] === 'none' ? null : m[5];
      rxStats.srcPort     = Number(m[6]) || null;
      rxStats.bitrateKbps = Number(m[7]);
    }
  }
}

export function stopRxPipeline() {
  rxShouldRun = false;
  if (!rxProcess) return;
  logger.info('[gst] stopping rx (pid %d)', rxProcess.pid);
  rxProcess.kill('SIGTERM');
  rxProcess = null;
}

export function isRxRunning() {
  return rxProcess !== null && !rxProcess.killed;
}

// ─────────────────────────────────────────────
// TX / rtp_send public API
// ─────────────────────────────────────────────

/** Start the persistent rtp_send JACK client (no targets = fakesink). */
export function startTxClient({ channels = 2 } = {}) {
  if (txProcess) {
    logger.info('[gst] rtp_send already running (pid %d)', txProcess.pid);
    return;
  }

  const bin  = join(SCRIPTS, 'rtp_send');
  const args = [String(channels)];

  logger.info('[gst] starting rtp_send: %s %s', bin, args.join(' '));

  const proc = spawn(bin, args, {
    stdio: ['pipe', 'pipe', 'pipe'],
    detached: false
  });

  txProcess = proc;
  txPorts   = [];
  txReady   = false;

  let stdoutBuf = '';

  proc.stdout.on('data', (d) => {
    stdoutBuf += d.toString();
    let nl;
    while ((nl = stdoutBuf.indexOf('\n')) >= 0) {
      const line = stdoutBuf.slice(0, nl).trim();
      stdoutBuf = stdoutBuf.slice(nl + 1);
      _parseTxLine(line);
    }
  });

  proc.stderr.on('data', (d) =>
    d.toString().split('\n').filter(Boolean)
      .forEach(l => logger.debug('[rtp_send] %s', l))
  );

  proc.on('exit', (code, signal) => {
    logger.info('[rtp_send] exited code=%s signal=%s', code, signal);
    txProcess = null;
    txPorts   = [];
    txReady   = false;
  });

  proc.on('error', (err) => {
    logger.error('[rtp_send] error: %s', err.message);
    txProcess = null;
  });

  // restore existing targets + codec
  if (txTargets.length > 0 || txCodec !== 'mp3' || txBitrate !== 320) {
    // wait briefly for process to be ready, then re-send state
    setTimeout(() => {
      if (!txProcess) return;
      txTargets.forEach(t => _txCmd(`add ${t.host} ${t.port}`));
      _txCmd(`codec ${txCodec} ${txBitrate}`);
    }, 500);
  }
}

function _parseTxLine(line) {
  logger.debug('[rtp_send] %s', line);

  if (line.startsWith('[rtp_send] ready')) {
    txReady = true;
    logger.info('[gst] rtp_send started  ports: %s',
      txPorts.length ? txPorts.join(', ') : '(waiting)');
    return;
  }

  if (line.startsWith('ports:')) {
    txPorts = line.replace('ports:', '').split(',').map(s => s.trim());
    logger.debug('[rtp_send] ready  ports: %s', txPorts.join(', '));
  }
}

function _txCmd(cmd) {
  if (!txProcess || txProcess.killed) return;
  txProcess.stdin.write(cmd + '\n');
}

export function stopTxClient() {
  if (!txProcess) return;
  logger.info('[gst] stopping rtp_send (pid %d)', txProcess.pid);
  _txCmd('quit');
  txProcess.kill('SIGTERM');
  txProcess = null;
  txPorts   = [];
}

export function isTxRunning() {
  return txProcess !== null && !txProcess.killed;
}

export function getTxPorts()   { return [...txPorts]; }
export function getTxTargets() { return [...txTargets]; }

// ─────────────────────────────────────────────
// TX target management (stdin commands)
// ─────────────────────────────────────────────

export function addTxTarget(target) {
  const exists = txTargets.some(
    t => t.host === target.host && t.port === target.port
  );
  if (exists) return;
  txTargets.push({ ...target });
  _txCmd(`add ${target.host} ${target.port}`);
  logger.info('[gst] tx target add %s:%d', target.host, target.port);
}

export function removeTxTarget(target) {
  const before = txTargets.length;
  txTargets = txTargets.filter(
    t => !(t.host === target.host && t.port === target.port)
  );
  if (txTargets.length !== before) {
    _txCmd(`remove ${target.host} ${target.port}`);
    logger.info('[gst] tx target remove %s:%d', target.host, target.port);
  }
}

// ─────────────────────────────────────────────
// TX codec
// ─────────────────────────────────────────────

export function setTxCodec(codec, bitrate) {
  txCodec   = codec   ?? txCodec;
  txBitrate = bitrate ?? txBitrate;
  _txCmd(`codec ${txCodec} ${txBitrate}`);
  logger.info('[gst] tx codec %s %dk', txCodec, txBitrate);
}

// ─────────────────────────────────────────────
// RX buffer (restart with new bufMs)
// ─────────────────────────────────────────────

export function setRxBuffer(ms) {
  rxBufMs = ms;
  if (isRxRunning()) {
    const wasRunning = true;
    stopRxPipeline();
    if (wasRunning) startRxPipeline({});
  }
}

// ─────────────────────────────────────────────
// Legacy TX pipeline API (kept for compatibility)
// ─────────────────────────────────────────────

export function startTxPipeline(targets, opts = {}) {
  if (!targets || targets.length === 0) {
    logger.info('[gst] no tx targets, skipping');
    return;
  }
  const { bitrate } = opts;
  if (bitrate) txBitrate = bitrate;
  targets.forEach(t => addTxTarget(t));
}

export function stopTxPipeline() {
  txTargets = [];
  if (txProcess) _txCmd('codec mp3 320'); // reset by rebuilding
  logger.info('[gst] tx targets cleared');
}

// ─────────────────────────────────────────────
// Multi-stream RTP (rtp_streams config)
// ─────────────────────────────────────────────

/** @type {Map<string, { proc: import('child_process').ChildProcess|null, cfg: object, ready: boolean, ports: string[], shouldRun: boolean }>} */
const streamInstances = new Map();

/** shm 이름 생성 헬퍼 (POSIX shm_open 이름 규칙: /로 시작) */
function _shmName(type, key) {
  return `/${type}_${key}`;
}

async function _launchRtpIn(cfg) {
  const key       = cfg.client;
  const protoMode = cfg.protocol === 'pcm' ? 'pcm' : cfg.protocol === 'rtp' ? 'rtp' : 'raw';
  const address   = cfg.address && cfg.address !== '0.0.0.0' ? cfg.address : '0.0.0.0';
  const shmName   = _shmName('rtp_in', key);
  const ch        = cfg.channels ?? 2;
  const chStart   = getRtpChStart(key, 'in');
  const bin       = join(SCRIPTS, 'rtp_recv');

  const inst = {
    proc: null, bridgeProc: null, cfg, ready: false, ports: [], shouldRun: true,
    stats: { codec: 'unknown', bufUsedMs: 0, packets: 0, drops: 0,
             srcIp: null, srcPort: null, bitrateKbps: 0 }
  };
  streamInstances.set(key, inst);

  /* aoip_engine에 rtp_in shm 등록 (shm 생성 후 rtp_recv가 attach) */
  sendToEngine(`rtp_in add ${key} ${shmName} ${ch} ${chStart}`);

  /* rtp_recv shm 모드 기동 */
  const args = [
    String(cfg.port ?? 10001), String(ch), protoMode, key,
    String(cfg.bufferMs ?? 100), String(cfg.sampleRate ?? 48000),
    protoMode === 'rtp' ? (cfg.rtpEncoding ?? 'L24') : '', address,
    'shm', shmName
  ];
  logger.info('[gst] starting rtp_recv (shm) %s  port=%d addr=%s shm=%s', key, cfg.port ?? 10001, address, shmName);

  const proc = spawn('taskset', ['-c', '2,3', 'chrt', '-f', '20', bin, ...args],
    { stdio: ['ignore', 'pipe', 'pipe'], detached: false });
  inst.proc = proc;

  proc.stderr.on('data', d => {
    d.toString().split('\n').filter(Boolean).forEach(line => {
      if (line.startsWith('[rtp_recv] ready')) {
        if (!inst.ready) {
          inst.ready = true;
          logger.info('[gst] rtp_in %s ready (pipe mode)', key);
        }
      } else if (line.startsWith('stats ')) {
        const m = line.match(
          /codec=(.+?)\s+bufMs=(\d+)\s+packets=(\d+)\s+drops=(\d+)\s+srcIp=(\S+)\s+srcPort=(\d+)\s+bitrateKbps=(\d+)/
        );
        if (m) {
          inst.stats.codec       = m[1];
          inst.stats.bufUsedMs   = Number(m[2]);
          inst.stats.packets     = Number(m[3]);
          inst.stats.drops       = Number(m[4]);
          inst.stats.srcIp       = m[5] === 'none' ? null : m[5];
          inst.stats.srcPort     = Number(m[6]) || null;
          inst.stats.bitrateKbps = Number(m[7]);
        }
      } else if (line.startsWith('[rtp_recv] auto-codec:')) {
        const parts = line.split(' ');
        if (parts.length >= 3) {
          inst.cfg.rtpEncoding = parts[2];
          if (parts[3]) inst.cfg.sampleRate = Number(parts[3]);
          saveAudioConfig(inst.cfg);
          logger.info('[gst] rtp_in %s auto-codec: %s %sHz', key, inst.cfg.rtpEncoding, inst.cfg.sampleRate);
        }
      } else {
        logger.debug('[rtp_recv:%s] %s', key, line);
      }
    });
  });

  /* stdout: stats 등 */
  proc.stdout.on('data', d => {
    d.toString().split('\n').filter(Boolean).forEach(line => {
      if (line.startsWith('[rtp_recv] ready') && !inst.ready) {
        inst.ready = true;
        logger.info('[gst] rtp_in %s ready (pipe/stdout)', key);
      } else if (line.startsWith('stats ')) {
        const m = line.match(
          /codec=(.+?)\s+bufMs=(\d+)\s+packets=(\d+)\s+drops=(\d+)\s+srcIp=(\S+)\s+srcPort=(\d+)\s+bitrateKbps=(\d+)/
        );
        if (m) {
          inst.stats.codec       = m[1];
          inst.stats.bufUsedMs   = Number(m[2]);
          inst.stats.packets     = Number(m[3]);
          inst.stats.drops       = Number(m[4]);
          inst.stats.srcIp       = m[5] === 'none' ? null : m[5];
          inst.stats.srcPort     = Number(m[6]) || null;
          inst.stats.bitrateKbps = Number(m[7]);
        }
      }
    });
  });

  proc.on('exit', (code, signal) => {
    logger.info('[rtp_recv:%s] exited code=%s signal=%s', key, code, signal);
    inst.proc = null; inst.ready = false; inst.ports = [];
    if (!inst.shouldRun || signal === 'SIGTERM') return;
    const delay = code === 2 ? 200 : 2000;
    setTimeout(() => { if (inst.shouldRun) _launchRtpIn(inst.cfg); }, delay);
  });

  proc.on('error', err => {
    logger.error('[rtp_recv:%s] error: %s', key, err.message);
    inst.proc = null;
  });
}

async function _launchRtpOut(cfg) {
  const key     = cfg.client;
  const shmName = _shmName('rtp_out', key);
  const ch      = cfg.channels ?? 2;
  const chStart = getRtpChStart(key, 'out');

  const inst = { proc: null, bridgeProc: null, cfg, ready: false, ports: [], shouldRun: true };
  streamInstances.set(key, inst);

  /* aoip_engine에 rtp_out shm 등록 (shm 생성 후 rtp_send가 attach) */
  sendToEngine(`rtp_out add ${key} ${shmName} ${ch} ${chStart}`);

  /* rtp_send 기동 (shm reader) */
  _launchRtpSend(cfg, shmName, inst);
}

function _launchRtpSend(cfg, shmName, inst) {
  const key  = cfg.client;
  const bin  = join(SCRIPTS, 'rtp_send');
  const proto = cfg.protocol === 'rtp' ? 'rtp' : 'raw';
  const args = [String(cfg.channels ?? 2), key, proto,
                String(cfg.sampleRate ?? 0), 'shm', shmName];

  logger.info('[gst] starting rtp_send (pipe) %s: %s %s', key, bin, args.join(' '));

  /* rtp_send: 인코딩/전송 (CPU 3, 일반 우선순위) */
  const proc = spawn('taskset', ['-c', '2,3', 'chrt', '-f', '20', bin, ...args],
    { stdio: ['pipe', 'pipe', 'pipe'], detached: false });
  inst.proc = proc;

  let buf = '';
  proc.stdout.on('data', d => {
    buf += d.toString();
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (line.startsWith('[rtp_send] ready')) {
        inst.ready = true;
        logger.info('[gst] rtp_out %s ready (shm mode)', key);
        for (const t of (cfg.targets ?? []))
          proc.stdin.write(`add ${t.host} ${t.port}\n`);
        if (cfg.codec) proc.stdin.write(`codec ${cfg.codec} ${cfg.bitrate ?? 320}\n`);
      } else if (line.startsWith('stats ')) {
        const m = line.match(/targets=(\d+)\s+codec=(\S+)\s+bitrateKbps=(\d+)\s+bytesSent=(\d+)/);
        if (m) {
          inst.stats = {
            targets:     Number(m[1]),
            codec:       m[2],
            bitrateKbps: Number(m[3]),
            bytesSent:   Number(m[4]),
          };
        }
      }
    }
  });
  proc.stderr.on('data', d =>
    d.toString().split('\n').filter(Boolean)
      .forEach(l => logger.debug('[rtp_send:%s] %s', key, l))
  );
  proc.on('exit', (code, signal) => {
    logger.info('[rtp_send:%s] exited code=%s signal=%s', key, code, signal);
    inst.proc = null; inst.ready = false;
    if (inst.shouldRun && signal !== 'SIGTERM' && code !== 0) {
      setTimeout(() => {
        if (!inst.shouldRun) return;
        try { execSync(`pkill -f "rtp_send.*${key}"`, { stdio: 'ignore' }); } catch { /* ignore */ }
        setTimeout(() => { if (inst.shouldRun) _launchRtpSend(inst.cfg, shmName, inst); }, 500);
      }, 2000);
    }
  });
  proc.on('error', err => {
    logger.error('[rtp_send:%s] error: %s', key, err.message);
    inst.proc = null;
  });
}

/** rtp_out 인스턴스에 전송 대상 추가.
 * 다른 rtp_out 인스턴스가 이미 같은 host:port로 전송 중이면 먼저 제거.
 */
export function addRtpOutTarget(client, host, port) {
  const inst = streamInstances.get(client);
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`);

  /* 다른 인스턴스에서 같은 주소 사용 중이면 제거 */
  for (const [key, other] of streamInstances) {
    if (key === client || other.cfg.type !== 'rtp_out') continue;
    const dup = (other.cfg.targets ?? []).find(t => t.host === host && t.port === port);
    if (dup) {
      other.cfg.targets = other.cfg.targets.filter(t => !(t.host === host && t.port === port));
      if (other.proc && !other.proc.killed) other.proc.stdin.write(`remove ${host} ${port}\n`);
      logger.info('[gst] rtp_out %s: removed duplicate target %s:%d (moved to %s)', key, host, port, client);
    }
  }

  const targets = inst.cfg.targets ?? [];
  if (!targets.find(t => t.host === host && t.port === port)) {
    targets.push({ host, port });
    inst.cfg.targets = targets;
  }
  if (inst.proc && !inst.proc.killed) inst.proc.stdin.write(`add ${host} ${port}\n`);
  saveAudioConfig(inst.cfg);
  logger.info('[gst] rtp_out %s add target %s:%d', client, host, port);
}

/** rtp_out 인스턴스에서 전송 대상 제거 */
export function removeRtpOutTarget(client, host, port) {
  const inst = streamInstances.get(client);
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`);
  inst.cfg.targets = (inst.cfg.targets ?? []).filter(t => !(t.host === host && t.port === port));
  if (inst.proc && !inst.proc.killed) inst.proc.stdin.write(`remove ${host} ${port}\n`);
  saveAudioConfig(inst.cfg);
  logger.info('[gst] rtp_out %s remove target %s:%d', client, host, port);
}

/** rtp_out 코덱 변경 */
export function setRtpOutCodec(client, codec, bitrate) {
  const inst = streamInstances.get(client);
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`);
  inst.cfg.codec   = codec   ?? inst.cfg.codec;
  inst.cfg.bitrate = bitrate ?? inst.cfg.bitrate;
  if (inst.proc && !inst.proc.killed)
    inst.proc.stdin.write(`codec ${inst.cfg.codec} ${inst.cfg.bitrate ?? 320}\n`);
  saveAudioConfig(inst.cfg);
  logger.info('[gst] rtp_out %s codec %s %d', client, inst.cfg.codec, inst.cfg.bitrate ?? 320);
}

/** 개별 스트림 상태 (targets, stats 포함) */
export function getRtpStreamDetail(client) {
  const inst = streamInstances.get(client);
  if (!inst) return null;
  return {
    client,
    type:     inst.cfg.type,
    name:     inst.cfg.name,
    ready:    inst.ready,
    ports:    [...inst.ports],
    protocol: inst.cfg.protocol ?? 'raw',
    codec:    inst.cfg.codec,
    bitrate:  inst.cfg.bitrate,
    port:       inst.cfg.port,
    address:    inst.cfg.address ?? '0.0.0.0',
    bufferMs:   inst.cfg.bufferMs ?? 100,
    sampleRate: inst.cfg.sampleRate ?? 48000,
    targets:  [...(inst.cfg.targets ?? [])],
    channels: inst.cfg.channels ?? 2,
    stats:    inst.cfg.type === 'rtp_in' ? { ...inst.stats } : undefined,
  };
}

export function startRtpStreams(streams) {
  // 이름 충돌 방지: 동일 client명을 가진 기존 프로세스 먼저 종료 (동기)
  for (const cfg of (streams ?? [])) {
    if (cfg.enabled === false || !cfg.client) continue;
    try { execSync(`pkill -f "rtp_recv.*${cfg.client}|rtp_send.*${cfg.client}"`, { stdio: 'ignore' }); } catch { /* 없으면 무시 */ }
  }

  for (const cfg of (streams ?? [])) {
    if (cfg.enabled === false) continue;
    const key = cfg.client;
    if (streamInstances.has(key)) continue;
    if (cfg.type === 'rtp_in') _launchRtpIn(cfg);
    else if (cfg.type === 'rtp_out') _launchRtpOut(cfg);
  }
}

/**
 * rtp_in 설정 변경 (port, protocol, codec, bufferMs) — 프로세스 재시작
 */
export function updateRtpInConfig(client, updates) {
  const inst = streamInstances.get(client);
  if (!inst || inst.cfg.type !== 'rtp_in') throw new Error(`rtp_in ${client} not found`);
  Object.assign(inst.cfg, updates);
  saveAudioConfig(inst.cfg);
  logger.info('[gst] rtp_in %s config updated (restart required to apply)', client);
}

export function stopRtpStream(client) {
  const inst = streamInstances.get(client);
  if (!inst) throw new Error();
  inst.shouldRun = false;
  if (inst.proc && !inst.proc.killed) {
    try { inst.proc.stdin?.write('quit\n'); } catch { /* ignore */ }
    inst.proc.kill('SIGTERM');
  }
  /* aoip_engine rtp FIFO 해제 */
  if (inst.cfg.type === 'rtp_in')  sendToEngine(`rtp_in remove ${client}`);
  if (inst.cfg.type === 'rtp_out') sendToEngine(`rtp_out remove ${client}`);
}

export function startRtpStream(client) {
  const inst = streamInstances.get(client);
  if (!inst) throw new Error();
  if (inst.proc && !inst.proc.killed) return;
  inst.shouldRun = true;
  if (inst.cfg.type === 'rtp_in') _launchRtpIn(inst.cfg);
  else if (inst.cfg.type === 'rtp_out') _launchRtpOut(inst.cfg);
}

export function stopRtpStreams() {
  for (const [client, inst] of streamInstances) {
    inst.shouldRun = false;
    if (inst.proc) {
      try { inst.proc.stdin?.write('quit\n'); } catch { /* ignore */ }
      inst.proc.kill('SIGTERM');
    }
    if (inst.cfg.type === 'rtp_in')  sendToEngine(`rtp_in remove ${client}`);
    if (inst.cfg.type === 'rtp_out') sendToEngine(`rtp_out remove ${client}`);
  }
  streamInstances.clear();
}

export function getRtpStreamStatus() {
  return Array.from(streamInstances.entries()).map(([key, inst]) => ({
    client:   key,
    type:     inst.cfg.type,
    name:     inst.cfg.name,
    running:  inst.proc !== null && !inst.proc?.killed,
    ready:    inst.ready,
    ports:    [...inst.ports],
    stats:    inst.cfg.type === 'rtp_in' ? { ...inst.stats } : undefined,
  }));
}

/** Wait for all rtp_streams instances to be ready (or timeout). */
export function waitForRtpStreamsReady(timeoutMs = 8000) {
  return new Promise((resolve, reject) => {
    if (streamInstances.size === 0) return resolve();
    const t = setTimeout(() => reject(new Error('rtp_streams ready timeout')), timeoutMs);
    const poll = setInterval(() => {
      const allReady = Array.from(streamInstances.values()).every(i => i.ready);
      if (allReady) { clearInterval(poll); clearTimeout(t); resolve(); }
    }, 100);
  });
}

// ─────────────────────────────────────────────
// Status
// ─────────────────────────────────────────────

export function getGstStatus() {
  return {
    rx: {
      running:  isRxRunning(),
      port:     rxPort,
      bufferMs: rxBufMs,
      codec:    rxStats.codec,
    },
    tx: {
      running: isTxRunning(),
      targets: getTxTargets(),
      codec:   txCodec,
      bitrate: txBitrate,
    },
    rtpStreams: getRtpStreamStatus(),
  };
}

/** rtp_out 코덱/비트레이트/채널 변경. 채널 변경 시 프로세스 재시작. */

import { spawn, execSync } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import logger from './logger.js';
import { connect } from './jack.js';
import { getInputSrcPorts, getOutputSinkPorts } from './channels.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const SCRIPTS   = join(__dirname, '../scripts');

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

/** rtp_in 재시작 후 gainer 입력 포트 재연결 */
function _reconnectRtpIn(key) {
  const srcPorts = getInputSrcPorts();
  setTimeout(() => {
    for (let i = 0; i < srcPorts.length; i++) {
      if (!srcPorts[i].startsWith(`${key}:`)) continue;
      const src = srcPorts[i];
      const dst = `gainer:in_${i + 1}`;
      connect(src, dst).catch(e =>
        logger.warn('[gst] rtp_in %s reconnect %s→%s: %s', key, src, dst, e.message)
      );
    }
    logger.info('[gst] rtp_in %s reconnecting JACK ports', key);
  }, 500);
}

/** rtp_out 재시작 후 gainer 출력 포트 재연결 */
function _reconnectRtpOut(key) {
  const sinkPorts = getOutputSinkPorts();
  setTimeout(() => {
    for (let i = 0; i < sinkPorts.length; i++) {
      if (!sinkPorts[i].startsWith(`${key}:`)) continue;
      const src = `gainer:sout_${i + 1}`;
      const dst = sinkPorts[i];
      connect(src, dst).catch(e =>
        logger.warn('[gst] rtp_out %s reconnect %s→%s: %s', key, src, dst, e.message)
      );
    }
    logger.info('[gst] rtp_out %s reconnecting JACK ports', key);
  }, 500);
}

function _launchRtpIn(cfg) {
  const key = cfg.client;
  const bin  = join(SCRIPTS, 'rtp_recv');

  /* protocol: raw (auto-detect via decodebin) | rtp (auto-detect via rtpptdemux+decodebin) */
  const protoMode = cfg.protocol === 'rtp' ? 'rtp' : 'raw';
  const inst = {
    proc: null, cfg, ready: false, ports: [], shouldRun: true,
    stats: { codec: 'unknown', bufUsedMs: 0, packets: 0, drops: 0,
             srcIp: null, srcPort: null, bitrateKbps: 0 }
  };
  streamInstances.set(key, inst);

  const args = [String(cfg.port ?? 10001), String(cfg.channels ?? 2), protoMode, key,
                String(cfg.bufferMs ?? 100)];

  logger.info('[gst] starting rtp_in %s: %s %s', key, bin, args.join(' '));

  const proc = spawn(bin, args, {
    stdio: ['ignore', 'ignore', 'pipe'],   // stderr → pipe, JACK이 fd2는 오염 안 함
    detached: false
  });
  inst.proc = proc;

  /* stderr에서 ports / ready / stats 파싱 — 나머지는 debug 로그 */
  let stderrBuf = '';
  proc.stderr.on('data', d => {
    stderrBuf += d.toString();
    let nl;
    while ((nl = stderrBuf.indexOf('\n')) >= 0) {
      const line = stderrBuf.slice(0, nl).trim();
      stderrBuf  = stderrBuf.slice(nl + 1);
      if (!line) continue;
      if (line.startsWith('[rtp_recv] ready')) {
        inst.ready = true;
        logger.info('[gst] rtp_in %s ready  ports: %s', key, inst.ports.join(', '));
        _reconnectRtpIn(key);
      } else if (line.startsWith('ports:')) {
        inst.ports = line.replace('ports:', '').split(',').map(s => s.trim());
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
      } else {
        logger.debug('[rtp_in:%s] %s', key, line);
      }
    }
  });

  proc.on('exit', (code, signal) => {
    logger.info('[rtp_in:%s] exited code=%s signal=%s', key, code, signal);
    inst.proc = null; inst.ready = false; inst.ports = [];
    if (inst.shouldRun && signal !== 'SIGTERM') {
      setTimeout(() => { if (inst.shouldRun) _launchRtpIn(cfg); }, 2000);
    }
  });
  proc.on('error', err => {
    logger.error('[rtp_in:%s] error: %s', key, err.message);
    inst.proc = null;
  });
}

function _launchRtpOut(cfg) {
  const key = cfg.client;
  const bin  = join(SCRIPTS, 'rtp_send');
  const proto = cfg.protocol === 'rtp' ? 'rtp' : 'raw';
  const args = [String(cfg.channels ?? 2), key, proto];

  const inst = { proc: null, cfg, ready: false, ports: [], shouldRun: true };
  streamInstances.set(key, inst);

  logger.info('[gst] starting rtp_out %s: %s %s', key, bin, args.join(' '));

  const proc = spawn(bin, args, { stdio: ['pipe', 'pipe', 'pipe'], detached: false });
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
        for (const t of (cfg.targets ?? []))
          proc.stdin.write(`add ${t.host} ${t.port}\n`);
        if (cfg.codec) proc.stdin.write(`codec ${cfg.codec} ${cfg.bitrate ?? 320}\n`);
        logger.info('[gst] rtp_out %s ready  ports: %s', key, inst.ports.join(', '));
        _reconnectRtpOut(key);
      } else if (line.startsWith('ports:')) {
        inst.ports = line.replace('ports:', '').split(',').map(s => s.trim());
      }
    }
  });
  proc.stderr.on('data', d =>
    d.toString().split('\n').filter(Boolean)
      .forEach(l => logger.debug('[rtp_out:%s] %s', key, l))
  );
  proc.on('exit', (code, signal) => {
    logger.info('[rtp_out:%s] exited code=%s signal=%s', key, code, signal);
    inst.proc = null; inst.ready = false; inst.ports = [];
    if (inst.shouldRun && signal !== 'SIGTERM') {
      setTimeout(() => { if (inst.shouldRun) _launchRtpOut(cfg); }, 2000);
    }
  });
  proc.on('error', err => {
    logger.error('[rtp_out:%s] error: %s', key, err.message);
    inst.proc = null;
  });
}

/** rtp_out 인스턴스에 전송 대상 추가 */
export function addRtpOutTarget(client, host, port) {
  const inst = streamInstances.get(client);
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`);
  const targets = inst.cfg.targets ?? [];
  if (!targets.find(t => t.host === host && t.port === port)) {
    targets.push({ host, port });
    inst.cfg.targets = targets;
  }
  if (inst.proc && !inst.proc.killed) inst.proc.stdin.write(`add ${host} ${port}\n`);
  logger.info('[gst] rtp_out %s add target %s:%d', client, host, port);
}

/** rtp_out 인스턴스에서 전송 대상 제거 */
export function removeRtpOutTarget(client, host, port) {
  const inst = streamInstances.get(client);
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`);
  inst.cfg.targets = (inst.cfg.targets ?? []).filter(t => !(t.host === host && t.port === port));
  if (inst.proc && !inst.proc.killed) inst.proc.stdin.write(`remove ${host} ${port}\n`);
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
    port:     inst.cfg.port,
    bufferMs: inst.cfg.bufferMs ?? 100,
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
  // 재시작
  inst.shouldRun = false;
  if (inst.proc && !inst.proc.killed) inst.proc.kill('SIGTERM');
  setTimeout(() => {
    inst.shouldRun = true;
    _launchRtpIn(inst.cfg);
  }, 500);
  logger.info('[gst] rtp_in %s config updated — restarting', client);
}

export function stopRtpStream(client) {
  const inst = streamInstances.get(client);
  if (!inst) throw new Error();
  inst.shouldRun = false;
  if (inst.proc && !inst.proc.killed) {
    try { inst.proc.stdin?.write('quit'); } catch { /* ignore */ }
    inst.proc.kill('SIGTERM');
  }
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
  for (const [, inst] of streamInstances) {
    inst.shouldRun = false;
    if (inst.proc) {
      try { inst.proc.stdin?.write('quit\n'); } catch { /* ignore */ }
      inst.proc.kill('SIGTERM');
    }
  }
  streamInstances.clear();
}

export function getRtpStreamStatus() {
  return Array.from(streamInstances.entries()).map(([key, inst]) => ({
    client:   key,
    type:     inst.cfg.type,
    name:     inst.cfg.name,
    running:  inst.proc !== null && !inst.proc.killed,
    ready:    inst.ready,
    ports:    [...inst.ports],
    stats:    inst.cfg.type === 'rtp_in' ? { ...inst.stats } : undefined,
  }));
}

/** Wait for all rtp_streams instances to be ready AND have ports registered (or timeout). */
export function waitForRtpStreamsReady(timeoutMs = 8000) {
  return new Promise((resolve, reject) => {
    if (streamInstances.size === 0) return resolve();
    const t = setTimeout(() => reject(new Error('rtp_streams ready timeout')), timeoutMs);
    const poll = setInterval(() => {
      const allReady = Array.from(streamInstances.values()).every(i => i.ready && i.ports.length > 0);
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

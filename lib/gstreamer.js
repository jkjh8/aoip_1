import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import logger from './logger.js';

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
  };
}

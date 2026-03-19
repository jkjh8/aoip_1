import { spawn } from 'child_process';
import { createWriteStream } from 'fs';

/**
 * UDP MP3 input pipeline: UDP → jackaudiosink
 * One global receiver process.
 */
let rxProcess = null;
let rxPort = 10001;  // runtime-configurable

/**
 * RTP output pipeline: jackaudiosrc → lamemp3enc → tee → udpsink × N
 * One global sender process (rebuilt when targets change).
 */
let txProcess = null;

/** @type {Array<{ host: string, port: number }>} */
let txTargets = [];

// ─────────────────────────────────────────────
// RX
// ─────────────────────────────────────────────

/**
 * Start the RTP receive pipeline.
 * @param {{ port: number }} cfg
 */
export function setRxPort(port) {
  rxPort = Number(port);
  console.log('[gst] rx port set to %d', rxPort);
}

export function getRxPort() {
  return rxPort;
}

export function startRxPipeline(cfg) {
  if (rxProcess) {
    console.log('[gst] rx already running (pid %d)', rxProcess.pid);
    return;
  }

  // cfg.port is the default from config; runtime override takes precedence
  const port = rxPort ?? cfg?.port ?? 10001;

  // raw UDP MP3 → decode → resample → JACK
  const pipeline = [
    'udpsrc', `port=${port}`,
    '!', 'mpegaudioparse',
    '!', 'mpg123audiodec',
    '!', 'audioconvert',
    '!', 'audioresample',
    '!', 'audio/x-raw,rate=48000,channels=2',
    '!', 'jackaudiosink', 'client-name=rtp_in', 'connect=none'
  ];

  _spawn('rtp_rx', 'gst-launch-1.0', ['-q', ...pipeline], (proc) => {
    rxProcess = proc;
  }, () => {
    rxProcess = null;
  });
}

export function stopRxPipeline() {
  if (!rxProcess) return;
  console.log('[gst] stopping rx (pid %d)', rxProcess.pid);
  rxProcess.kill('SIGTERM');
  rxProcess = null;
}

export function isRxRunning() {
  return rxProcess !== null && !rxProcess.killed;
}

// ─────────────────────────────────────────────
// TX
// ─────────────────────────────────────────────

/**
 * Start (or restart) the RTP send pipeline with current targets.
 * @param {Array<{ host: string, port: number }>} targets
 * @param {{ bitrate?: number }} [opts]
 */
export function startTxPipeline(targets, { bitrate = 320 } = {}) {
  if (txProcess) {
    console.log('[gst] stopping existing tx before rebuild');
    txProcess.kill('SIGTERM');
    txProcess = null;
  }

  if (!targets || targets.length === 0) {
    console.log('[gst] no tx targets, skipping');
    txTargets = [];
    return;
  }

  txTargets = [...targets];

  // jackaudiosrc → encode → tee → udpsink × N  (raw UDP, no RTP wrapping)
  const sinks = txTargets.flatMap((t) => [
    't.', '!', 'queue', '!', 'udpsink', `host=${t.host}`, `port=${t.port}`
  ]);

  const pipeline = [
    'jackaudiosrc', 'client-name=rtp_out', 'connect=none',
    '!', 'audioconvert',
    '!', 'lamemp3enc', `bitrate=${bitrate}`,
    '!', 'tee', 'name=t',
    ...sinks
  ];

  _spawn('rtp_tx', 'gst-launch-1.0', ['-q', ...pipeline], (proc) => {
    txProcess = proc;
  }, () => {
    txProcess = null;
  });
}

export function stopTxPipeline() {
  if (!txProcess) return;
  console.log('[gst] stopping tx (pid %d)', txProcess.pid);
  txProcess.kill('SIGTERM');
  txProcess = null;
  txTargets = [];
}

export function isTxRunning() {
  return txProcess !== null && !txProcess.killed;
}

export function getTxTargets() {
  return [...txTargets];
}

/**
 * Add a target and rebuild the TX pipeline.
 * @param {{ host: string, port: number }} target
 * @param {{ bitrate?: number }} [opts]
 */
export function addTxTarget(target, opts) {
  const exists = txTargets.some((t) => t.host === target.host && t.port === target.port);
  if (exists) return;
  startTxPipeline([...txTargets, target], opts);
}

/**
 * Remove a target and rebuild the TX pipeline.
 * @param {{ host: string, port: number }} target
 * @param {{ bitrate?: number }} [opts]
 */
export function removeTxTarget(target, opts) {
  const next = txTargets.filter((t) => !(t.host === target.host && t.port === target.port));
  startTxPipeline(next, opts);
}

// ─────────────────────────────────────────────
// Status
// ─────────────────────────────────────────────

export function getGstStatus() {
  return {
    rx: { running: isRxRunning(), pid: rxProcess?.pid ?? null, port: rxPort },
    tx: { running: isTxRunning(), pid: txProcess?.pid ?? null, targets: getTxTargets() }
  };
}

// ─────────────────────────────────────────────
// Internal
// ─────────────────────────────────────────────

function _spawn(name, cmd, args, onStart, onExit) {
  const logPath = `/tmp/gst_${name}.log`;
  const log = createWriteStream(logPath, { flags: 'a' });

  console.log('[gst] starting %s: %s %s', name, cmd, args.join(' '));
  console.log('[gst] log → %s', logPath);

  const proc = spawn(cmd, args, {
    stdio: ['ignore', 'pipe', 'pipe'],
    detached: false
  });

  proc.stdout.pipe(log);
  proc.stderr.pipe(log);

  proc.on('exit', (code, signal) => {
    console.log('[gst] %s exited (code=%s signal=%s)', name, code, signal);
    onExit();
  });

  proc.on('error', (err) => {
    console.error('[gst] %s error: %s', name, err.message);
    onExit();
  });

  onStart(proc);
}

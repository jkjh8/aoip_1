import { spawn } from 'child_process';
import { createWriteStream, readFileSync, writeFileSync, readdirSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import logger from './logger.js';
import { setBridgeEnabled, getBridgeChannelDef } from './channels.js';

const __dirname        = dirname(fileURLToPath(import.meta.url));
const AUDIO_CONFIG_PATH = join(__dirname, '../config/audio.json');

/** @type {Map<string, { process: import('child_process').ChildProcess, config: object }>} */
const bridgeMap = new Map();

/** startOneBridge 진행 중인 브릿지 이름 (중복 기동 방지) */
const pendingSet = new Set();

const STARTUP_GRACE_MS = 3000; // 이 시간 안에 종료되면 기동 실패로 판정
const MAX_RETRIES      = 3;
const RETRY_DELAY_MS   = 2000;
const UDC_POLL_MS      = 5000; // UDC 상태 감시 주기

/** 현재 UDC 연결 상태 확인 ('configured' = USB 호스트 연결됨) */
function isUdcConnected() {
  try {
    const entries = readdirSync('/sys/class/udc');
    if (!entries.length) return false;
    const state = readFileSync(`/sys/class/udc/${entries[0]}/state`, 'utf8').trim();
    return state === 'configured';
  } catch {
    return false;
  }
}

/**
 * Build the argument list for an alsa_in / alsa_out process.
 * @param {{ name: string, type: string, device: string, period: number, rate: number, periods: number }} cfg
 * @returns {string[]}
 */
function buildArgs(cfg) {
  const args = [
    '-j', cfg.name,
    '-d', cfg.device,
    '-r', String(cfg.rate),
    '-p', String(cfg.period),
    '-n', String(cfg.periods),
    '-c', String(cfg.channels ?? 2)
  ];
  const isZita = cfg.type === 'zita-a2j' || cfg.type === 'zita-j2a';
  if (cfg.quality != null) args.push(isZita ? '-Q' : '-q', String(cfg.quality));
  if (isZita && cfg.forceL16) args.push('-L');
  return args;
}

/**
 * Start a single bridge, retrying up to attemptsLeft times if it exits immediately.
 * Resolves true if the bridge stabilises, false if all retries are exhausted.
 * USB 가젯 브릿지는 실패 시 false 반환 — watcher가 재시작을 담당.
 * @param {object} cfg
 * @param {number} attemptsLeft
 * @returns {Promise<boolean>}
 */
function startOneBridge(cfg, attemptsLeft = MAX_RETRIES) {
  if (attemptsLeft === MAX_RETRIES) pendingSet.add(cfg.name);
  return new Promise((resolve) => {
    const _resolve = (v) => { if (attemptsLeft === MAX_RETRIES) pendingSet.delete(cfg.name); resolve(v); };
    const logPath   = `/tmp/${cfg.name}.log`;
    const logStream = createWriteStream(logPath, { flags: 'a' });
    logStream.on('error', () => {});
    const attempt   = MAX_RETRIES - attemptsLeft + 1;

    const binary = cfg.type;
    const args   = buildArgs(cfg);

    logger.info('[bridges] Starting %s (%s %s) attempt=%d → log: %s',
      cfg.name, binary, args.join(' '), attempt, logPath);

    const proc = spawn('chrt', ['-f', '85', binary, ...args], {
      stdio: ['ignore', 'pipe', 'pipe'],
      detached: false
    });

    proc.stdout.pipe(logStream, { end: false });
    proc.stderr.pipe(logStream, { end: false });

    let settled = false;

    const stableTimer = setTimeout(() => {
      if (settled) return;
      settled = true;
      logger.info('[bridges] %s is running (pid=%d)', cfg.name, proc.pid);
      bridgeMap.set(cfg.name, { process: proc, config: cfg });
      _resolve(true);
    }, STARTUP_GRACE_MS);

    proc.on('exit', (code, signal) => {
      if (!settled) {
        // ── 기동 직후 종료 → 재시도 ──
        clearTimeout(stableTimer);
        settled = true;
        logStream.end();
        logger.warn('[bridges] %s exited early (code=%s signal=%s), attempts left=%d',
          cfg.name, code, signal, attemptsLeft - 1);

        if (attemptsLeft > 1) {
          setTimeout(() => _resolve(startOneBridge(cfg, attemptsLeft - 1)), RETRY_DELAY_MS);
        } else {
          // usb_gadget: watcher가 재시작 담당 → false 반환하고 즉시 종료
          if (!cfg.usb_gadget) {
            logger.warn('[bridges] %s: all retries exhausted — skipping device', cfg.name);
          }
          _resolve(false);
        }
      } else {
        // ── 안정 실행 중 종료 → 자동 재기동 (usb_gadget은 watcher가 담당) ──
        logStream.end();
        logger.info('[bridges] %s exited (code=%s signal=%s)', cfg.name, code, signal);
        bridgeMap.delete(cfg.name);
        if (signal !== 'SIGTERM' && !cfg.usb_gadget) {
          try {
            const cur = JSON.parse(readFileSync(AUDIO_CONFIG_PATH, 'utf8'));
            const curCfg = (cur.bridges ?? []).find(b => b.name === cfg.name);
            if (curCfg?.enabled === false) {
              logger.info('[bridges] %s is disabled — skipping restart', cfg.name);
              return;
            }
          } catch { /* config 읽기 실패 시 재시작 허용 */ }
          logger.info('[bridges] %s will restart in 5s', cfg.name);
          setTimeout(() => startOneBridge(cfg), 5000);
        }
      }
    });

    proc.on('error', (err) => {
      if (!settled) {
        clearTimeout(stableTimer);
        settled = true;
        logStream.end();
        logger.error('[bridges] %s process error: %s', cfg.name, err.message);

        if (attemptsLeft > 1) {
          setTimeout(() => _resolve(startOneBridge(cfg, attemptsLeft - 1)), RETRY_DELAY_MS);
        } else {
          if (!cfg.usb_gadget) {
            logger.warn('[bridges] %s: all retries exhausted — skipping device', cfg.name);
          }
          _resolve(false);
        }
      } else {
        logger.error('[bridges] %s process error: %s', cfg.name, err.message);
        bridgeMap.delete(cfg.name);
      }
    });
  });
}

/**
 * Start all non-USB-gadget bridges.
 * USB 가젯 브릿지는 startUsbGadgetWatcher() 가 관리.
 * @param {Array<object>} bridges
 * @returns {Promise<void>}
 */
export async function startBridges(bridges) {
  await Promise.all((bridges ?? []).map(cfg => {
    if (cfg.enabled === false) {
      logger.info('[bridges] %s is disabled, skipping', cfg.name);
      return Promise.resolve();
    }
    if (cfg.usb_gadget) return Promise.resolve(); // watcher가 담당
    if (bridgeMap.has(cfg.name)) {
      logger.info('[bridges] %s is already running, skipping', cfg.name);
      return Promise.resolve();
    }
    return startOneBridge(cfg);
  }));
}

/**
 * USB 가젯 UDC 상태를 감시하며 연결 시 브릿지를 자동 시작/정지.
 * 앱 기동 시 한 번 호출.
 */
export function startUsbGadgetWatcher() {
  let prevConnected = isUdcConnected();

  // 이미 연결된 상태라면 즉시 시작
  if (prevConnected) {
    _startUsbBridges();
  } else {
    logger.info('[bridges] USB gadget watcher started — waiting for host connection');
  }

  setInterval(() => {
    const connected = isUdcConnected();

    if (connected !== prevConnected) {
      prevConnected = connected;
      if (connected) logger.info('[bridges] USB host connected');
      else           logger.info('[bridges] USB host disconnected');
    }

    // 연결 중인데 브릿지가 없으면 (재)시작
    if (connected) _startUsbBridges();
  }, UDC_POLL_MS);
}

function _startUsbBridges() {
  try {
    const config = JSON.parse(readFileSync(AUDIO_CONFIG_PATH, 'utf8'));
    const usbCfgs = (config.bridges ?? []).filter(b => b.usb_gadget && b.enabled !== false);
    for (const cfg of usbCfgs) {
      if (!bridgeMap.has(cfg.name) && !pendingSet.has(cfg.name)) {
        startOneBridge(cfg);
      }
    }
  } catch (e) {
    logger.error('[bridges] _startUsbBridges error: %s', e.message);
  }
}

/**
 * Stop all running bridge processes.
 */
export function stopBridges() {
  for (const [name, { process: proc }] of bridgeMap.entries()) {
    logger.info('[bridges] Stopping %s (pid %d)', name, proc.pid);
    proc.kill('SIGTERM');
  }
  bridgeMap.clear();
}

/**
 * Return status for all configured bridges.
 * @returns {Array<{ name: string, running: boolean, pid: number|null }>}
 */
export function getBridgeStatus() {
  return Array.from(bridgeMap.entries()).map(([name, { process: proc }]) => ({
    name,
    running: !proc.killed && proc.exitCode === null,
    pid: proc.pid ?? null
  }));
}

/* ── USB 가젯 전용 토글 ──────────────────────────────────────── */

/**
 * USB 가젯 활성화/비활성화 토글.
 * 활성화 시 USB 연결 중이면 브릿지 즉시 시작,
 * 비활성화 시 JACK 포트 연결만 끊는다 (zita 프로세스 유지).
 * @param {boolean} enable
 * @returns {Promise<boolean>}
 */
export async function setUsbGadgetEnabled(enable) {
  const config = JSON.parse(readFileSync(AUDIO_CONFIG_PATH, 'utf8'));
  const usbCfgs = (config.bridges ?? []).filter(b => b.usb_gadget);
  if (!usbCfgs.length) throw new Error('usb_gadget bridge config not found');

  for (const cfg of usbCfgs) cfg.enabled = enable;
  writeFileSync(AUDIO_CONFIG_PATH, JSON.stringify(config, null, 2));
  logger.info('[bridges] usb_gadget enabled=%s', enable);

  if (enable) {
    // 채널 state에 먼저 추가 (UI 반영)
    for (const cfg of usbCfgs) setBridgeEnabled(cfg.name, true);

    if (!isUdcConnected()) {
      logger.info('[bridges] usb_gadget enabled but USB not connected — watcher will start bridges on connect');
      return true;
    }
    const results = await Promise.all(usbCfgs.map(cfg => {
      if (bridgeMap.has(cfg.name)) return Promise.resolve(true);
      return startOneBridge(cfg);
    }));

    // 브릿지 기동 후 JACK 포트 재연결
    if (results.every(Boolean)) {
      const { connect } = await import('./jack.js');
      for (const cfg of usbCfgs) {
        const def = getBridgeChannelDef(cfg.name);
        if (!def) continue;
        for (const ch of def.inputs) {
          try { await connect(ch.srcPort, `gainer:in_${ch.id}`); }
          catch (e) { logger.warn('[bridges] connect %s→gainer:in_%d: %s', ch.srcPort, ch.id, e.message); }
        }
        for (const ch of def.outputs) {
          try { await connect(`gainer:sout_${ch.id}`, ch.sinkPort); }
          catch (e) { logger.warn('[bridges] connect gainer:sout_%d→%s: %s', ch.id, ch.sinkPort, e.message); }
        }
      }
    }

    return results.every(Boolean);
  } else {
    // 채널 state에서 먼저 제거 (UI 반영)
    for (const cfg of usbCfgs) setBridgeEnabled(cfg.name, false);

    // 프로세스 종료 → JACK 포트 소멸
    const usbNames = new Set(usbCfgs.map(c => c.name));
    for (const name of usbNames) {
      const entry = bridgeMap.get(name);
      if (entry) {
        logger.info('[bridges] stopping usb_gadget bridge: %s', name);
        entry.process.kill('SIGTERM');
        bridgeMap.delete(name);
      }
    }
    return true;
  }
}

/**
 * USB 가젯 현재 enabled 상태 반환 (audio.json 기준).
 * @returns {boolean}
 */
export function getUsbGadgetEnabled() {
  try {
    const config = JSON.parse(readFileSync(AUDIO_CONFIG_PATH, 'utf8'));
    const usbCfgs = (config.bridges ?? []).filter(b => b.usb_gadget);
    return usbCfgs.length > 0 && usbCfgs.every(b => b.enabled !== false);
  } catch {
    return false;
  }
}

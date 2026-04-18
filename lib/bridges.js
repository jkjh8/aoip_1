import { spawn } from 'child_process';
import { createWriteStream, readdirSync, readFileSync } from 'fs';
import logger from './logger.js';
import { getBridgeChannelDef, getDspClientOf, getDspLocalId } from './channels.js';
import { getConfig, saveConfig } from './config.js';

/** @type {Map<string, { process: import('child_process').ChildProcess, config: object }>} */
const bridgeMap = new Map();

/** startOneBridge 진행 중인 브릿지 이름 (중복 기동 방지) */
const pendingSet = new Set();

const STARTUP_GRACE_MS   = 3000; // 이 시간 안에 종료되면 기동 실패로 판정
const BRIDGE_STAGGER_MS  = 500; // 브릿지 간 순차 기동 대기 (동시 xrun 방지)
const UDC_POLL_MS        = 2000; // UDC 상태 감시 주기

let _startupComplete = false;
export function notifyBridgeStartupComplete() { _startupComplete = true; }

/** 현재 UDC 연결 상태 확인 ('configured' = USB 호스트 연결됨) */
export function isUdcConnected() {
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
 * Build the argument list for a zita_in / zita_out process.
 * @param {{ name: string, type: string, device: string, period: number, rate: number, periods: number }} cfg
 * @returns {string[]}
 */
const ZITA_BINARY = { 'zita_in': 'zita-a2j', 'zita_out': 'zita-j2a' };

function getBinary(cfg) {
  return ZITA_BINARY[cfg.type] ?? cfg.type;
}

function buildArgs(cfg) {
  const args = [
    '-j', cfg.name,
    '-d', cfg.device,
    '-r', String(cfg.rate),
    '-p', String(cfg.period),
    '-n', String(cfg.periods),
    '-c', String(cfg.channels ?? 2)
  ];
  if (cfg.quality >= 16) args.push('-Q', String(cfg.quality));  // 0 = auto (omit -Q)
  return args;
}

function startOneBridge(cfg) {
  pendingSet.add(cfg.name);
  return new Promise((resolve) => {
    const done = (v) => { pendingSet.delete(cfg.name); resolve(v); };
    const logPath   = `/tmp/${cfg.name}.log`;
    const logStream = createWriteStream(logPath, { flags: 'a' });
    logStream.on('error', () => {});

    const binary = getBinary(cfg);
    const args   = buildArgs(cfg);
    logger.info('[bridges] Starting %s (%s %s) → log: %s', cfg.name, binary, args.join(' '), logPath);

    const proc = spawn('taskset', ['-c', '1', 'chrt', '-f', '80', binary, ...args], {
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
      done(true);
    }, STARTUP_GRACE_MS);

    proc.on('exit', (code, signal) => {
      logStream.end();
      if (!settled) {
        clearTimeout(stableTimer);
        settled = true;
        logger.warn('[bridges] %s exited early (code=%s signal=%s)', cfg.name, code, signal);
        done(false);
      } else {
        logger.info('[bridges] %s exited (code=%s signal=%s)', cfg.name, code, signal);
        bridgeMap.delete(cfg.name);
        if (signal !== 'SIGTERM' && !cfg.usb_gadget) {
          try {
            const curCfg = (getConfig().bridges ?? []).find(b => b.name === cfg.name);
            if (curCfg?.enabled === false) {
              logger.info('[bridges] %s is disabled — skipping restart', cfg.name);
              return;
            }
          } catch {}
          logger.info('[bridges] %s will restart in 5s', cfg.name);
          setTimeout(() => startOneBridge(cfg), 5000);
        }
      }
    });

    proc.on('error', (err) => {
      logStream.end();
      logger.error('[bridges] %s process error: %s', cfg.name, err.message);
      if (!settled) {
        clearTimeout(stableTimer);
        settled = true;
        done(false);
      } else {
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
export async function killOrphanBridges() {
  const binaries = ['zita_in', 'zita_out', 'zita-a2j', 'zita-j2a'];
  const { exec } = await import('child_process');
  await Promise.all(binaries.map(bin =>
    new Promise(r => exec(`pkill -x ${bin}`, r))
  ));
  await new Promise(r => setTimeout(r, 500));
}

export async function startBridges(bridges) {
  const tasks = [];
  for (const cfg of (bridges ?? [])) {
    if (cfg.enabled === false) {
      logger.info('[bridges] %s is disabled, skipping', cfg.name);
      continue;
    }
    if (cfg.usb_gadget) continue; // watcher가 담당
    if (bridgeMap.has(cfg.name) || pendingSet.has(cfg.name)) {
      logger.info('[bridges] %s is already running, skipping', cfg.name);
      continue;
    }
    tasks.push(cfg);
  }

  // 동시 기동 시 xrun 방지: BRIDGE_STAGGER_MS 간격으로 순차 기동
  for (let i = 0; i < tasks.length; i++) {
    if (i > 0) await new Promise(r => setTimeout(r, BRIDGE_STAGGER_MS));
    await startOneBridge(tasks[i]);
  }
}

/**
 * USB 가젯 UDC 상태를 감시하며 연결 시 브릿지를 자동 시작/정지.
 * 앱 기동 시 한 번 호출.
 */
export function startUsbGadgetWatcher() {
  let prevConnected = isUdcConnected();

  if (prevConnected) {
    logger.info('[bridges] USB host already connected at boot — starting bridges');
    _startUsbBridges({ skipRouting: true });
  } else {
    logger.info('[bridges] USB gadget watcher started — waiting for host connection');
  }

  setInterval(() => {
    const connected = isUdcConnected();

    if (connected !== prevConnected) {
      prevConnected = connected;
      if (connected) {
        logger.info('[bridges] USB host connected — starting bridges');
        _startUsbBridges({ skipRouting: false });
      } else {
        logger.info('[bridges] USB host disconnected — removing JACK routing');
        _applyUsbRouting(false);
      }
    }

    // 연결 중이면 매 poll마다 브릿지 누락 여부 확인 후 재기동
    if (connected) _startUsbBridges({ skipRouting: false });
  }, UDC_POLL_MS);
}

async function _applyUsbRouting(enable) {
  const usbCfgs = (getConfig().bridges ?? []).filter(b => b.usb_gadget && b.enabled !== false);
  if (!usbCfgs.length) return;
  try {
    const { connect, disconnect } = await import('./jack.js');
    for (const cfg of usbCfgs) {
      const def = getBridgeChannelDef(cfg.name);
      if (!def) continue;
      for (const ch of def.inputs) {
        try {
          if (enable) await connect(ch.srcPort, `${getDspClientOf(ch.id)}:in_${getDspLocalId(ch.id)}`);
          else        await disconnect(ch.srcPort, `${getDspClientOf(ch.id)}:in_${getDspLocalId(ch.id)}`);
        } catch (e) { logger.warn('[bridges] routing %s %s→%s:in_%d: %s', enable ? 'connect' : 'disconnect', ch.srcPort, getDspClientOf(ch.id), getDspLocalId(ch.id), e.message); }
      }
      for (const ch of def.outputs) {
        try {
          if (enable) await connect(`${getDspClientOf(ch.id)}:sout_${getDspLocalId(ch.id)}`, ch.sinkPort);
          else        await disconnect(`${getDspClientOf(ch.id)}:sout_${getDspLocalId(ch.id)}`, ch.sinkPort);
        } catch (e) { logger.warn('[bridges] routing %s %s:sout_%d→%s: %s', enable ? 'connect' : 'disconnect', getDspClientOf(ch.id), getDspLocalId(ch.id), ch.sinkPort, e.message); }
      }
    }
  } catch (e) {
    logger.error('[bridges] _applyUsbRouting error: %s', e.message);
  }
}

function _startUsbBridges({ skipRouting = false } = {}) {
  try {
    const usbCfgs = (getConfig().bridges ?? []).filter(b => b.usb_gadget && b.enabled !== false);
    let started = false;
    for (const cfg of usbCfgs) {
      if (!bridgeMap.has(cfg.name) && !pendingSet.has(cfg.name)) {
        startOneBridge(cfg);
        started = true;
      }
    }
    // 새로 기동된 브릿지가 있으면 안정화 후 라우팅 재연결 (startup 완료 후에만 — 부팅 시엔 startup이 담당)
    if (started && !skipRouting) setTimeout(() => { if (_startupComplete) _applyUsbRouting(true); }, STARTUP_GRACE_MS + 500);
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
 * USB 가젯 브릿지를 kill — watcher가 최신 config로 재기동.
 * USB 미연결 시엔 kill만 하고 watcher가 연결 시 재기동.
 */
export function restartUsbBridges() {
  const usbCfgs = (getConfig().bridges ?? []).filter(b => b.usb_gadget);
  for (const { name } of usbCfgs) {
    const entry = bridgeMap.get(name);
    if (entry) {
      logger.info('[bridges] restarting %s for period change (pid=%d)', name, entry.process.pid);
      entry.process.kill('SIGTERM');
      bridgeMap.delete(name);
    }
  }
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
 * 비활성화 시 JACK 포트 연결만 끊는다 (브릿지 프로세스 유지).
 * @param {boolean} enable
 * @returns {Promise<boolean>}
 */
export async function setUsbGadgetEnabled(enable) {
  const config = getConfig();
  const usbCfgs = (config.bridges ?? []).filter(b => b.usb_gadget);
  if (!usbCfgs.length) throw new Error('usb_gadget bridge config not found');

  for (const cfg of usbCfgs) cfg.enabled = enable;
  saveConfig();
  logger.info('[bridges] usb_gadget enabled=%s', enable);

  // 라우팅은 물리적으로 연결된 경우에만 적용
  if (isUdcConnected()) {
    await _applyUsbRouting(enable);
  } else {
    logger.info('[bridges] USB not physically connected — routing will apply on connect');
  }

  return true;
}

/**
 * USB 가젯 현재 enabled 상태 반환 (audio.json 기준).
 * @returns {boolean}
 */
export function getUsbGadgetEnabled() {
  try {
    const usbCfgs = (getConfig().bridges ?? []).filter(b => b.usb_gadget);
    return usbCfgs.length > 0 && usbCfgs.every(b => b.enabled !== false);
  } catch {
    return false;
  }
}

import { spawn } from 'child_process';
import { createWriteStream, readFileSync, writeFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import logger from './logger.js';

const __dirname        = dirname(fileURLToPath(import.meta.url));
const AUDIO_CONFIG_PATH = join(__dirname, '../config/audio.json');

/** @type {Map<string, { process: import('child_process').ChildProcess, config: object }>} */
const bridgeMap = new Map();

const STARTUP_GRACE_MS = 3000; // 이 시간 안에 종료되면 기동 실패로 판정
const MAX_RETRIES      = 3;
const RETRY_DELAY_MS   = 2000;

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
  if (cfg.quality != null) args.push('-Q', String(cfg.quality));
  if (cfg.forceL16) args.push('-L');
  return args;
}

/**
 * usb_jack_bridge 전용: 바이너리 경로와 인수 반환
 * @param {{ name: string, device: string }} cfg
 * @returns {{ binary: string, args: string[] }}
 */
function buildUsbArgs(cfg) {
  return {
    binary: join(__dirname, '../scripts/usb_jack_bridge'),
    args:   [cfg.device ?? 'hw:UAC2Gadget', cfg.name]
  };
}

/**
 * Start a single bridge, retrying up to attemptsLeft times if it exits immediately.
 * Resolves true if the bridge stabilises, false if all retries are exhausted.
 * @param {object} cfg
 * @param {number} attemptsLeft
 * @returns {Promise<boolean>}
 */
function startOneBridge(cfg, attemptsLeft = MAX_RETRIES) {
  return new Promise((resolve) => {
    const logPath   = `/tmp/${cfg.name}.log`;
    const logStream = createWriteStream(logPath, { flags: 'a' });
    logStream.on('error', () => {});
    const attempt   = MAX_RETRIES - attemptsLeft + 1;

    let binary, args;
    if (cfg.type === 'usb_jack_bridge') {
      ({ binary, args } = buildUsbArgs(cfg));
    } else {
      binary = cfg.type;
      args   = buildArgs(cfg);
    }

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
      resolve(true);
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
          setTimeout(() => resolve(startOneBridge(cfg, attemptsLeft - 1)), RETRY_DELAY_MS);
        } else {
          logger.warn('[bridges] %s: all retries exhausted — skipping device', cfg.name);
          resolve(false);
        }
      } else {
        // ── 안정 실행 중 종료 → 자동 재기동 (disabled 이면 스킵) ──
        logStream.end();
        logger.info('[bridges] %s exited (code=%s signal=%s)', cfg.name, code, signal);
        bridgeMap.delete(cfg.name);
        if (signal !== 'SIGTERM') {
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
          setTimeout(() => resolve(startOneBridge(cfg, attemptsLeft - 1)), RETRY_DELAY_MS);
        } else {
          logger.warn('[bridges] %s: all retries exhausted — skipping device', cfg.name);
          resolve(false);
        }
      } else {
        logger.error('[bridges] %s process error: %s', cfg.name, err.message);
        bridgeMap.delete(cfg.name);
      }
    });
  });
}

/**
 * Start all bridges defined in the config array.
 * Each bridge is retried up to MAX_RETRIES times; failures are skipped.
 * @param {Array<object>} bridges
 * @returns {Promise<void>}
 */
export async function startBridges(bridges) {
  await Promise.all((bridges ?? []).map(cfg => {
    if (cfg.enabled === false) {
      logger.info('[bridges] %s is disabled, skipping', cfg.name);
      return Promise.resolve();
    }
    if (bridgeMap.has(cfg.name)) {
      logger.info('[bridges] %s is already running, skipping', cfg.name);
      return Promise.resolve();
    }
    return startOneBridge(cfg);
  }));
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
 * audio.json 의 enabled 플래그를 갱신하고,
 * 활성화 시 usb_jack_bridge 프로세스를 시작,
 * 비활성화 시 프로세스를 중지한다.
 * @param {boolean} enable
 * @returns {Promise<boolean>} 결과 성공 여부
 */
export async function setUsbGadgetEnabled(enable) {
  const config = JSON.parse(readFileSync(AUDIO_CONFIG_PATH, 'utf8'));
  const usbCfg = (config.bridges ?? []).find(b => b.type === 'usb_jack_bridge');
  if (!usbCfg) throw new Error('usb_jack_bridge config not found');

  usbCfg.enabled = enable;
  writeFileSync(AUDIO_CONFIG_PATH, JSON.stringify(config, null, 2));
  logger.info('[bridges] usb_gadget enabled=%s', enable);

  if (enable) {
    return startOneBridge(usbCfg);
  } else {
    const entry = bridgeMap.get(usbCfg.name);
    if (entry) {
      entry.process.kill('SIGTERM');
      bridgeMap.delete(usbCfg.name);
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
    const usbCfg = (config.bridges ?? []).find(b => b.type === 'usb_jack_bridge');
    return usbCfg?.enabled ?? false;
  } catch {
    return false;
  }
}

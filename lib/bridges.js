import { spawn } from 'child_process';
import { createWriteStream } from 'fs';
import logger from './logger.js';

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
    const args      = buildArgs(cfg);
    const attempt   = MAX_RETRIES - attemptsLeft + 1;

    logger.info('[bridges] Starting %s (%s %s) attempt=%d → log: %s',
      cfg.name, cfg.type, args.join(' '), attempt, logPath);

    const proc = spawn('chrt', ['-f', '85', cfg.type, ...args], {
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
        // ── 안정 실행 중 종료 → 자동 재기동 ──
        logStream.end();
        logger.info('[bridges] %s exited (code=%s signal=%s)', cfg.name, code, signal);
        bridgeMap.delete(cfg.name);
        if (signal !== 'SIGTERM') {
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
  await Promise.all(bridges.map(cfg => {
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

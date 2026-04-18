import { execFile, spawn } from 'child_process';
import { promisify } from 'util';
import logger from './logger.js';
import { getConfig } from './config.js';

const execFileAsync = promisify(execFile);

let jackProcess = null;
let _onJackCrash   = null;
let _onJackRecover = null;

/** JACK 비정상 종료 시 호출될 콜백 등록 */
export function setJackCrashHandler(fn)   { _onJackCrash   = fn; }
/** JACK 재시작 완료(ready) 후 호출될 콜백 등록 */
export function setJackRecoverHandler(fn) { _onJackRecover = fn; }

/**
 * Start the jackd process using config/audio.json settings.
 */
export function startJack() {
  if (jackProcess) {
    logger.info('[jack] jackd is already running (pid %d)', jackProcess.pid);
    return;
  }

  const { device, rate, period, periods, channels, format } = getConfig().jack;

  const args = [
    '-R',
    '-d', 'alsa',
    '-d', device,
    '-r', String(rate),
    '-p', String(period),
    '-n', String(periods)
  ];

  // Add channel specification if provided
  if (channels) {
    args.push('-i', String(channels));
    args.push('-o', String(channels));
  }

  // Add sample format if provided (16bit, 24bit, 32bit)
  if (format) {
    args.push('-S', format);
  }

  logger.info('[jack] Starting jackd: chrt -f -p 85 jackd %s', args.join(' '));

  jackProcess = spawn('taskset', ['-c', '3', 'chrt', '-f', '85', 'jackd','-R','-P','85',...args], {
    stdio: ['ignore', 'pipe', 'pipe'],
    detached: false,
    env: { ...process.env, JACK_NO_AUDIO_RESERVATION: '1' }
  });

  jackProcess.stdout.on('data', (d) => process.stdout.write(`[jackd] ${d}`));
  jackProcess.stderr.on('data', (d) => process.stderr.write(`[jackd] ${d}`));

  jackProcess.on('exit', (code, signal) => {
    logger.info('[jack] jackd exited (code=%s, signal=%s)', code, signal);
    jackProcess = null;
    jackReady   = false;

    if (signal === 'SIGTERM' || signal === 'SIGKILL') return;

    // 비정상 종료 → 크래시 콜백(DSP/브릿지 정리) 호출 후 재시작
    logger.warn('[jack] jackd crashed — restarting in 5s');
    try { _onJackCrash?.(); } catch (e) { logger.error('[jack] onJackCrash error: %s', e.message); }

    setTimeout(async () => {
      // stale shm 정리 후 재시작 — 없으면 code=255 루프
      await killExistingJack();
      startJack();
      try {
        await waitForJack();
        try { _onJackRecover?.(); } catch (e) { logger.error('[jack] onJackRecover error: %s', e.message); }
      } catch (e) {
        logger.error('[jack] jackd restart failed: %s', e.message);
      }
    }, 5000);
  });

  jackProcess.on('error', (err) => {
    logger.error('[jack] jackd process error:', err.message);
    jackProcess = null;
  });
}

/**
 * Stop the jackd process.
 */
export function stopJack() {
  if (!jackProcess) {
    logger.info('[jack] jackd is not running');
    return;
  }
  logger.info('[jack] Stopping jackd (pid %d)', jackProcess.pid);
  jackProcess.kill('SIGTERM');
  jackProcess = null;
}

/**
 * jack_lsp 한 번 실행해서 JACK이 이미 살아 있는지 확인.
 * @returns {Promise<boolean>}
 */
export function checkJackAlive() {
  return new Promise(resolve =>
    execFile('jack_lsp', [], { timeout: 2000 }, (err) => resolve(!err))
  );
}

/**
 * jackd + 브릿지 프로세스가 있으면 SIGKILL로 종료, shm 정리.
 * jackd가 없어도 stale shm은 항상 정리.
 * @returns {Promise<void>}
 */
export function killExistingJack() {
  return new Promise((resolve) => {
    const cleanupShm = () => {
      execFile('bash', ['-c', 'rm -f /dev/shm/jack*'], () => resolve());
    };

    execFile('pgrep', ['-f', 'jackd'], (err) => {
      if (err) {
        // jackd 없음 — stale shm만 정리
        return cleanupShm();
      }
      logger.info('[jack] stale JACK processes found — cleaning up');
      execFile('pkill', ['-KILL', '-f', 'jackd'], () => {
        execFile('pkill', ['-KILL', '-e', 'zita-a2j'], () => {
          execFile('pkill', ['-KILL', '-e', 'zita-j2a'], () => {
            // 프로세스 소멸 확인 (최대 3s)
            let attempts = 0;
            const wait = () => {
              execFile('pgrep', ['-f', 'jackd'], (err) => {
                if (err || attempts++ >= 15) return cleanupShm();
                setTimeout(wait, 200);
              });
            };
            setTimeout(wait, 200);
          });
        });
      });
    });
  });
}

/**
 * Poll jack_lsp every 500 ms until JACK is ready (max 15 s).
 * @returns {Promise<void>}
 */
export function waitForJack() {
  return new Promise((resolve, reject) => {
    const maxAttempts = 60; // 60 × (200ms timeout + 300ms wait) = 최대 30s
    let attempts = 0;

    const poll = () => {
      execFile('jack_lsp', [], { timeout: 200 }, (err) => {
        if (!err) {
          logger.info('[jack] JACK is ready (attempt %d)', attempts + 1);
          setJackReady(true);
          return resolve();
        }
        attempts++;
        if (attempts >= maxAttempts) {
          return reject(new Error('[jack] Timed out waiting for JACK to become ready'));
        }
        setTimeout(poll, 300);
      });
    };

    poll();
  });
}

/**
 * List all JACK ports.
 * @returns {Promise<string[]>}
 */
export async function getPorts() {
  const { stdout } = await execFileAsync('jack_lsp', []);
  return stdout
    .split('\n')
    .map((l) => l.trim())
    .filter(Boolean);
}

/**
 * List all JACK connections.
 * @returns {Promise<Array<{ port: string, connections: string[] }>>}
 */
export async function getConnections() {
  const { stdout } = await execFileAsync('jack_lsp', ['-c']);
  const lines = stdout.split('\n');
  const result = [];
  let current = null;

  for (const raw of lines) {
    if (!raw) continue;

    // A port line has no leading whitespace; a connection line is indented.
    if (raw[0] !== ' ' && raw[0] !== '\t') {
      current = { port: raw.trim(), connections: [] };
      result.push(current);
    } else if (current) {
      current.connections.push(raw.trim());
    }
  }

  return result;
}

/**
 * Connect two JACK ports.
 * @param {string} src
 * @param {string} dst
 */
export async function connect(src, dst) {
  logger.info('[jack] connect %s -> %s', src, dst);
  try {
    await execFileAsync('jack_connect', [src, dst]);
  } catch (err) {
    if (!err.stderr?.includes('already connected')) throw err;
  }
}

/**
 * Disconnect two JACK ports.
 * @param {string} src
 * @param {string} dst
 */
export async function disconnect(src, dst) {
  logger.info('[jack] disconnect %s -> %s', src, dst);
  try {
    await execFileAsync('jack_disconnect', [src, dst]);
  } catch (err) {
    if (!err.stderr?.includes('not connected')) throw err;
  }
}

/**
 * Returns whether jackd is currently managed and running.
 * jackReady is set true by waitForJack() regardless of who started jackd.
 * @returns {boolean}
 */
let jackReady = false;
export function isJackRunning() {
  return jackReady || (jackProcess !== null && !jackProcess.killed);
}
export function setJackReady(v) { jackReady = v; }

import { execFile, spawn } from 'child_process';
import { promisify } from 'util';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import logger from './logger.js';

const execFileAsync = promisify(execFile);

const __dirname = dirname(fileURLToPath(import.meta.url));
const config = JSON.parse(readFileSync(join(__dirname, '../config/audio.json'), 'utf8'));

let jackProcess = null;

/**
 * Start the jackd process using config/audio.json settings.
 */
export function startJack() {
  if (jackProcess) {
    logger.info('[jack] jackd is already running (pid %d)', jackProcess.pid);
    return;
  }

  const { device, rate, period, periods, channels, format } = config.jack;

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

  logger.info('[jack] Starting jackd: chrt -f 85 jackd %s', args.join(' '));

  jackProcess = spawn('chrt', ['-f', '85', 'jackd', ...args], {
    stdio: ['ignore', 'pipe', 'pipe'],
    detached: false,
    env: { ...process.env, JACK_NO_AUDIO_RESERVATION: '1' }
  });

  jackProcess.stdout.on('data', (d) => process.stdout.write(`[jackd] ${d}`));
  jackProcess.stderr.on('data', (d) => process.stderr.write(`[jackd] ${d}`));

  jackProcess.on('exit', (code, signal) => {
    logger.info('[jack] jackd exited (code=%s, signal=%s)', code, signal);
    jackProcess = null;
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
 * 없으면 즉시 반환.
 * @returns {Promise<void>}
 */
export function killExistingJack() {
  return new Promise((resolve) => {
    execFile('pgrep', ['-f', 'jackd'], (err) => {
      if (err) return resolve(); // 실행 중인 jackd 없음 → 즉시 반환
      logger.info('[jack] stale JACK processes found — cleaning up');
      execFile('pkill', ['-KILL', '-f', 'jackd'], () => {
        execFile('pkill', ['-KILL', '-e', 'zita-a2j'], () => {
          execFile('pkill', ['-KILL', '-e', 'zita-j2a'], () => {
            execFile('bash', ['-c', 'rm -f /dev/shm/jack*'], () => {
              // 프로세스 소멸 확인 (최대 3s)
              let attempts = 0;
              const wait = () => {
                execFile('pgrep', ['-f', 'jackd'], (err) => {
                  if (err || attempts++ >= 15) return resolve();
                  setTimeout(wait, 200);
                });
              };
              setTimeout(wait, 200);
            });
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

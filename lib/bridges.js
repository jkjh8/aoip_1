import { spawn } from 'child_process';
import { createWriteStream } from 'fs';

/** @type {Map<string, { process: import('child_process').ChildProcess, config: object }>} */
const bridgeMap = new Map();

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
 * Start all bridges defined in the config array.
 * Each bridge's stdout/stderr is redirected to /tmp/{name}.log.
 * @param {Array<{ name: string, type: string, device: string, period: number, rate: number, periods: number }>} bridges
 */
export function startBridges(bridges) {
  for (const cfg of bridges) {
    if (bridgeMap.has(cfg.name)) {
      console.log('[bridges] %s is already running, skipping', cfg.name);
      continue;
    }

    const logPath = `/tmp/${cfg.name}.log`;
    const logStream = createWriteStream(logPath, { flags: 'a' });
    const args = buildArgs(cfg);

    console.log('[bridges] Starting %s (%s %s) → log: %s', cfg.name, cfg.type, args.join(' '), logPath);

    const proc = spawn('chrt', ['-f', '85', cfg.type, ...args], {
      stdio: ['ignore', 'pipe', 'pipe'],
      detached: false
    });

    proc.stdout.pipe(logStream);
    proc.stderr.pipe(logStream);

    proc.on('exit', (code, signal) => {
      console.log('[bridges] %s exited (code=%s, signal=%s)', cfg.name, code, signal);
      bridgeMap.delete(cfg.name);
      if (signal !== 'SIGTERM') {
        const delay = 5000;
        console.log('[bridges] %s will restart in %dms', cfg.name, delay);
        setTimeout(() => startBridges([cfg]), delay);
      }
    });

    proc.on('error', (err) => {
      console.error('[bridges] %s process error: %s', cfg.name, err.message);
      bridgeMap.delete(cfg.name);
    });

    bridgeMap.set(cfg.name, { process: proc, config: cfg });
  }
}

/**
 * Stop all running bridge processes.
 */
export function stopBridges() {
  for (const [name, { process: proc }] of bridgeMap.entries()) {
    console.log('[bridges] Stopping %s (pid %d)', name, proc.pid);
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

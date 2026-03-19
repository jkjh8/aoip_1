import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const SCRIPT    = join(__dirname, '../scripts/gainer.py');

let proc    = null;
let saved_n_in  = 0;
let saved_n_out = 0;

export function startGainer(n_in, n_out) {
  if (proc) return;
  saved_n_in  = n_in;
  saved_n_out = n_out;

  proc = spawn('chrt', ['-f', '85', 'python3', SCRIPT, String(n_in), String(n_out)], {
    stdio: ['pipe', 'pipe', 'pipe'],
    detached: false
  });

  proc.stdout.on('data', d => process.stdout.write('[gainer] ' + d));
  proc.stderr.on('data', d => process.stderr.write('[gainer] ' + d));

  proc.on('exit', (code, signal) => {
    console.log('[gainer] exited code=%s signal=%s', code, signal);
    proc = null;
    if (signal !== 'SIGTERM') {
      console.log('[gainer] restarting in 3s...');
      setTimeout(() => startGainer(saved_n_in, saved_n_out), 3000);
    }
  });

  proc.on('error', err => {
    console.error('[gainer] process error:', err.message);
    proc = null;
  });

  console.log('[gainer] started (in=%d out=%d)', n_in, n_out);
}

export function stopGainer() {
  proc?.kill('SIGTERM');
  proc = null;
}

export function isGainerRunning() {
  return !!proc && !proc.killed && proc.exitCode === null;
}

/**
 * 슬라이더 값(0–150)을 dB 선형 → linear amplitude 로 변환해 gainer에 전송.
 *   0        → -∞  (0.0)
 *   0–100    → -60dB ~ 0dB
 *   100–150  → 0dB ~ +6dB
 * @param {'in'|'out'} dir  @param {number} ch 1-based  @param {number} sliderVal 0-150
 */
export function sendGain(dir, ch, sliderVal) {
  if (!proc?.stdin.writable) return;
  let linear;
  if (sliderVal <= 0) {
    linear = 0;
  } else if (sliderVal <= 100) {
    const db = (sliderVal / 100) * 60 - 60;   // -60dB ~ 0dB
    linear = Math.pow(10, db / 20);
  } else {
    const db = ((sliderVal - 100) / 50) * 6;   // 0dB ~ +6dB
    linear = Math.pow(10, db / 20);
  }
  proc.stdin.write(`gain ${dir} ${ch} ${Math.min(2.0, linear).toFixed(4)}\n`);
}

/** @param {'in'|'out'} dir  @param {number} ch 1-based  @param {boolean} muted */
export function sendMute(dir, ch, muted) {
  if (!proc?.stdin.writable) return;
  proc.stdin.write(`mute ${dir} ${ch} ${muted ? '1' : '0'}\n`);
}

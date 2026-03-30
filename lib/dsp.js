import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { execFile } from 'child_process';
import logger from './logger.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const BINARY    = join(__dirname, '../scripts/dsp_engine');

let proc    = null;
let saved_n_in  = 0;
let saved_n_out = 0;

/* Limiter I/O level meters: Map<`out ${ch}`, { pre: dB, post: dB }> */
const g_limMeter  = new Map();
/* Input channel peak levels: Map<`gainer:out_${ch}`, dB> */
const g_inLevel   = new Map();
/* Output channel peak levels: Map<`gainer:sout_${ch}`, dB> */
const g_outLevel  = new Map();
let   g_stdoutBuf = '';

export function getLimiterMeters() { return g_limMeter; }
export function getInLevel(jackPort)  { return g_inLevel.get(jackPort)  ?? -120; }
export function getOutLevel(jackPort) { return g_outLevel.get(jackPort) ?? -120; }

export function startDsp(n_in, n_out) {
  if (proc) return;
  saved_n_in  = n_in;
  saved_n_out = n_out;

  proc = spawn('chrt', ['-f', '85', BINARY, String(n_in), String(n_out)], {
    stdio: ['pipe', 'pipe', 'pipe'],
    detached: false
  });

  g_limMeter.clear();
  g_stdoutBuf = '';

  proc.stdout.on('data', d => {
    g_stdoutBuf += d.toString();
    let nl;
    while ((nl = g_stdoutBuf.indexOf('\n')) >= 0) {
      const line = g_stdoutBuf.slice(0, nl).trim();
      g_stdoutBuf = g_stdoutBuf.slice(nl + 1);
      if (!line) continue;
      const parts = line.split(' ');
      /* lvl in <ch> <db> */
      if (parts[0] === 'lvl' && parts[1] === 'in' && parts.length >= 4) {
        const ch = parseInt(parts[2], 10);
        const db = parseFloat(parts[3]);
        if (!isNaN(ch)) g_inLevel.set(`gainer:out_${ch}`, db);
      /* lvl out <ch> <db> */
      } else if (parts[0] === 'lvl' && parts[1] === 'out' && parts.length >= 4) {
        const ch = parseInt(parts[2], 10);
        const db = parseFloat(parts[3]);
        if (!isNaN(ch)) g_outLevel.set(`gainer:sout_${ch}`, db);
      /* lm out <ch> <pre_db> <post_db> */
      } else if (parts[0] === 'lm' && parts.length >= 5) {
        const ch   = parseInt(parts[2], 10);
        const pre  = parseFloat(parts[3]);
        const post = parseFloat(parts[4]);
        if (!isNaN(ch)) g_limMeter.set(`${parts[1]} ${ch}`, { pre, post });
      } else {
        logger.debug('[dsp] ' + line + '\n');
      }
    }
  });
  proc.stderr.on('data', d => logger.debug('[dsp] ' + d));

  proc.on('exit', (code, signal) => {
    logger.info('[dsp] exited code=%s signal=%s', code, signal);
    proc = null;
    if (signal !== 'SIGTERM') {
      // JACK이 살아 있을 때만 재시작
      execFile('jack_lsp', [], { timeout: 1000 }, (err) => {
        if (err) {
          logger.warn('[dsp] JACK not running — skipping restart');
          return;
        }
        logger.info('[dsp] restarting in 3s...');
        setTimeout(() => startDsp(saved_n_in, saved_n_out), 3000);
      });
    }
  });

  proc.on('error', err => {
    logger.error('[dsp] process error:', err.message);
    proc = null;
  });

  logger.info('[dsp] started (in=%d out=%d)', n_in, n_out);
}

export function stopDsp() {
  proc?.kill('SIGTERM');
  proc = null;
}

export function isDspRunning() {
  return !!proc && !proc.killed && proc.exitCode === null;
}

function write(line) {
  if (!proc?.stdin.writable) return;
  proc.stdin.write(line + '\n');
}

/**
 * 슬라이더 값(0–150)을 dB 선형 → linear amplitude 로 변환해 dsp_engine에 전송.
 *   0        → -∞  (0.0)
 *   0–100    → -60dB ~ 0dB
 *   100–150  → 0dB ~ +6dB
 * @param {'in'|'out'} dir  @param {number} ch 1-based  @param {number} sliderVal 0-150
 */
export function sendGain(dir, ch, sliderVal) {
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
  write(`gain ${dir} ${ch} ${Math.min(2.0, linear).toFixed(4)}`);
}

/** @param {'in'|'out'} dir  @param {number} ch 1-based  @param {boolean} muted */
export function sendMute(dir, ch, muted) {
  write(`mute ${dir} ${ch} ${muted ? '1' : '0'}`);
}

/** @param {'in'|'out'} dir  @param {number} ch 1-based  @param {boolean} bypass */
export function sendBypass(dir, ch, bypass) {
  write(`bypass ${dir} ${ch} ${bypass ? '1' : '0'}`);
}

/**
 * HPF 파라미터 전송 (입력 채널 전용)
 * @param {number} ch 1-based
 * @param {{ enabled?: boolean, freq?: number }} params
 */
export function sendHpf(ch, params) {
  if (params.enabled !== undefined)
    write(`hpf in ${ch} enable ${params.enabled ? '1' : '0'}`);
  if (params.freq !== undefined)
    write(`hpf in ${ch} freq ${params.freq}`);
}

/**
 * EQ 밴드 직접 계수 전송
 * @param {'in'|'out'} dir
 * @param {number} ch 1-based
 * @param {number} band 0-based
 * @param {{ enabled?: boolean, b0?: number, b1?: number, b2?: number, a1?: number, a2?: number }} params
 */
export function sendEqCoeffs(dir, ch, band, params) {
  if (params.enabled !== undefined)
    write(`eq ${dir} ${ch} ${band} enable ${params.enabled ? '1' : '0'}`);
  if (params.b0 !== undefined)
    write(`eq ${dir} ${ch} ${band} coeffs ${params.b0} ${params.b1} ${params.b2} ${params.a1} ${params.a2}`);
}

/**
 * 리미터 파라미터 전송 (출력 채널 전용)
 * @param {number} ch 1-based
 * @param {{ enabled?: boolean, threshold?: number, attack?: number, release?: number, makeup?: number }} params
 */
export function sendLimiter(ch, params) {
  if (params.enabled   !== undefined) write(`limiter out ${ch} enable    ${params.enabled ? '1' : '0'}`);
  if (params.threshold !== undefined) write(`limiter out ${ch} threshold ${params.threshold}`);
  if (params.attack    !== undefined) write(`limiter out ${ch} attack    ${params.attack}`);
  if (params.release   !== undefined) write(`limiter out ${ch} release   ${params.release}`);
  if (params.makeup    !== undefined) write(`limiter out ${ch} makeup    ${params.makeup}`);
}

/**
 * 채널의 전체 DSP 상태를 dsp_engine에 한 번에 전송 (startup 복원용)
 * @param {object} channels — getChannels() 결과 { inputs, outputs }
 */
export function sendAllDsp(channels) {
  for (const ch of channels.inputs) {
    const { hpf, eq } = ch.dsp;
    sendHpf(ch.id, hpf);
    for (let b = 0; b < eq.length; b++) sendEqCoeffs('in', ch.id, b, eq[b]);
  }
  for (const ch of channels.outputs) {
    const { eq, limiter } = ch.dsp;
    for (let b = 0; b < eq.length; b++) sendEqCoeffs('out', ch.id, b, eq[b]);
    if (limiter) sendLimiter(ch.id, limiter);
  }
}

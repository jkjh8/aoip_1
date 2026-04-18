import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { execFile } from 'child_process';
import logger from './logger.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const BINARY    = join(__dirname, '../scripts/dsp_engine');

/*
 * DSP 인스턴스 맵
 *   key   : JACK client 이름 (e.g. "gainer", "mixer")
 *   value : { proc, n_in, n_out, restarting, stdoutBuf,
 *             limMeter, inLevel, outLevel }
 */
const instances = new Map();

/* ── CPU 코어 할당 (인스턴스 생성 순서대로 2, 3, ... 배정) ── */
const BASE_CPU = 2;
let   _cpuCounter = 0;

/* ── 인스턴스 시작 ─────────────────────────────────────── */

export function startDsp(name, n_in, n_out) {
  let inst = instances.get(name);
  if (inst) {
    if (inst.proc || inst.restarting) return;
  } else {
    inst = {
      proc: null, n_in, n_out, restarting: false,
      cpu: BASE_CPU + (_cpuCounter++),
      stdoutBuf: '',
      limMeter: new Map(),
      inLevel:  new Map(),
      outLevel: new Map(),
    };
    instances.set(name, inst);
  }
  inst.n_in  = n_in;
  inst.n_out = n_out;
  _launch(name, inst);
}

function _launch(name, inst) {
  inst.limMeter.clear();
  inst.stdoutBuf = '';

  const proc = spawn(
    'taskset', ['-c', String(inst.cpu), 'chrt', '-f', '85', BINARY,
                String(inst.n_in), String(inst.n_out), '--name', name],
    { stdio: ['pipe', 'pipe', 'pipe'], detached: false }
  );
  inst.proc = proc;

  proc.stdout.on('data', d => {
    inst.stdoutBuf += d.toString();
    let nl;
    while ((nl = inst.stdoutBuf.indexOf('\n')) >= 0) {
      const line = inst.stdoutBuf.slice(0, nl).trim();
      inst.stdoutBuf = inst.stdoutBuf.slice(nl + 1);
      if (!line) continue;
      const parts = line.split(' ');
      /* lvl in <ch> <db> */
      if (parts[0] === 'lvl' && parts[1] === 'in' && parts.length >= 4) {
        const ch = parseInt(parts[2], 10);
        if (!isNaN(ch)) inst.inLevel.set(`${name}:out_${ch}`, parseFloat(parts[3]));
      /* lvl out <ch> <db> */
      } else if (parts[0] === 'lvl' && parts[1] === 'out' && parts.length >= 4) {
        const ch = parseInt(parts[2], 10);
        if (!isNaN(ch)) inst.outLevel.set(`${name}:sout_${ch}`, parseFloat(parts[3]));
      /* lm out <ch> <pre> <post> */
      } else if (parts[0] === 'lm' && parts[1] === 'out' && parts.length >= 5) {
        const ch = parseInt(parts[2], 10);
        if (!isNaN(ch))
          inst.limMeter.set(`out ${ch}`, { pre: parseFloat(parts[3]), post: parseFloat(parts[4]) });
      } else {
        logger.debug('[dsp:%s] %s', name, line + '\n');
      }
    }
  });

  proc.stderr.on('data', d => logger.debug('[dsp:%s] %s', name, d));

  proc.on('exit', (code, signal) => {
    logger.info('[dsp:%s] exited code=%s signal=%s', name, code, signal);
    inst.proc = null;
    if (signal !== 'SIGTERM') {
      inst.restarting = true;
      execFile('jack_lsp', [], { timeout: 1000 }, (err) => {
        if (err) {
          logger.warn('[dsp:%s] JACK not running — skipping restart', name);
          inst.restarting = false;
          return;
        }
        logger.info('[dsp:%s] restarting in 3s...', name);
        setTimeout(() => { inst.restarting = false; _launch(name, inst); }, 3000);
      });
    }
  });

  proc.on('error', err => {
    logger.error('[dsp:%s] process error: %s', name, err.message);
    inst.proc = null;
  });

  logger.info('[dsp] started %s (in=%d out=%d cpu=%d)', name, inst.n_in, inst.n_out, inst.cpu);
}

/* ── 인스턴스 종료 ─────────────────────────────────────── */

export function stopDsp(name) {
  if (name) {
    const inst = instances.get(name);
    if (!inst) return;
    inst.restarting = false;
    inst.proc?.kill('SIGTERM');
    inst.proc = null;
  } else {
    for (const [n, inst] of instances) {
      inst.restarting = false;
      inst.proc?.kill('SIGTERM');
      inst.proc = null;
    }
  }
}

export function isDspRunning(name) {
  if (name === undefined) {
    for (const inst of instances.values())
      if (inst?.proc && !inst.proc.killed && inst.proc.exitCode === null) return true;
    return false;
  }
  const inst = instances.get(name);
  return !!(inst?.proc && !inst.proc.killed && inst.proc.exitCode === null);
}

/* ── 레벨 미터 ─────────────────────────────────────────── */

/** jackPort 형식: "gainer:out_N" 또는 "mixer:out_N" */
export function getInLevel(jackPort) {
  const [client] = jackPort.split(':');
  return instances.get(client)?.inLevel.get(jackPort) ?? -120;
}

export function getOutLevel(jackPort) {
  const [client] = jackPort.split(':');
  return instances.get(client)?.outLevel.get(jackPort) ?? -120;
}

export function getLimiterMeters(name = 'gainer') {
  return instances.get(name)?.limMeter ?? new Map();
}

/* ── stdin 커맨드 헬퍼 ─────────────────────────────────── */

function write(name, line) {
  const inst = instances.get(name);
  if (!inst?.proc?.stdin?.writable) return;
  inst.proc.stdin.write(line + '\n');
}

/* ── DSP 커맨드 API ────────────────────────────────────── */
/*
 * globalId : 1-based 전체 채널 번호
 * splitAt  : 이 번호 이하는 "gainer", 초과는 "mixer" (기본 2)
 */
const SPLIT_AT = 2;

function resolve(globalId) {
  if (globalId <= SPLIT_AT) return { name: 'gainer', localId: globalId };
  return { name: 'mixer', localId: globalId - SPLIT_AT };
}

export function sendGain(dir, globalId, sliderVal) {
  const { name, localId } = resolve(globalId);
  let linear;
  if (sliderVal <= 0) {
    linear = 0;
  } else if (sliderVal <= 100) {
    const db = (sliderVal / 100) * 60 - 60;
    linear = Math.pow(10, db / 20);
  } else {
    const db = ((sliderVal - 100) / 50) * 6;
    linear = Math.pow(10, db / 20);
  }
  write(name, `gain ${dir} ${localId} ${Math.min(2.0, linear).toFixed(4)}`);
}

export function sendMute(dir, globalId, muted) {
  const { name, localId } = resolve(globalId);
  write(name, `mute ${dir} ${localId} ${muted ? '1' : '0'}`);
}

export function sendBypass(dir, globalId, bypass) {
  const { name, localId } = resolve(globalId);
  write(name, `bypass ${dir} ${localId} ${bypass ? '1' : '0'}`);
}

export function sendHpf(globalId, params) {
  const { name, localId } = resolve(globalId);
  const dir = params.type === 'output' ? 'out' : 'in';
  if (params.slope !== undefined)
    write(name, `hpf ${dir} ${localId} slope ${params.slope}`);
  if (params.freq !== undefined)
    write(name, `hpf ${dir} ${localId} freq ${params.freq}`);
  if (params.enabled !== undefined)
    write(name, `hpf ${dir} ${localId} enable ${params.enabled ? '1' : '0'}`);
}

const BAND_TYPE_MAP = {
  peak: 'peak', low_shelf: 'loshelf', high_shelf: 'hishelf', lp: 'lp', hp: 'hp',
};

export function sendEqBand(dir, globalId, bandIndex, params) {
  const { name, localId } = resolve(globalId);
  const band = bandIndex + 1;
  if (params.enabled !== undefined)
    write(name, `eq ${dir} ${localId} ${band} enable ${params.enabled ? '1' : '0'}`);
  if (params.b0 !== undefined)
    write(name, `eq ${dir} ${localId} ${band} coeffs ${params.b0} ${params.b1} ${params.b2} ${params.a1} ${params.a2}`);
  if (params.freq !== undefined)
    write(name, `eq ${dir} ${localId} ${band} freq ${params.freq}`);
  if (params.gain !== undefined)
    write(name, `eq ${dir} ${localId} ${band} gain ${params.gain}`);
  if (params.q !== undefined)
    write(name, `eq ${dir} ${localId} ${band} q ${params.q}`);
  if (params.bandType !== undefined)
    write(name, `eq ${dir} ${localId} ${band} type ${BAND_TYPE_MAP[params.bandType] ?? 'peak'}`);
}

export function sendLimiter(globalId, params) {
  const { name, localId } = resolve(globalId);
  if (params.enabled   !== undefined) write(name, `limiter out ${localId} enable    ${params.enabled   ? '1' : '0'}`);
  if (params.threshold !== undefined) write(name, `limiter out ${localId} threshold ${params.threshold}`);
  if (params.attack    !== undefined) write(name, `limiter out ${localId} attack    ${params.attack}`);
  if (params.release   !== undefined) write(name, `limiter out ${localId} release   ${params.release}`);
  if (params.makeup    !== undefined) write(name, `limiter out ${localId} makeup    ${params.makeup}`);
}

/* ── 전체 DSP 상태 일괄 전송 ──────────────────────────── */

export function sendAllDsp({ inputs, outputs }) {
  for (const ch of inputs) {
    if (ch.bypassDsp) { sendBypass('in', ch.id, true); continue; }
    const { dsp } = ch;
    if (dsp?.hpf) sendHpf(ch.id, dsp.hpf);
    if (dsp?.eq)  dsp.eq.forEach((b, i) => sendEqBand('in', ch.id, i, b));
  }
  for (const ch of outputs) {
    if (ch.bypassDsp) { sendBypass('out', ch.id, true); continue; }
    const { dsp } = ch;
    if (dsp?.eq)      dsp.eq.forEach((b, i) => sendEqBand('out', ch.id, i, b));
    if (dsp?.limiter) sendLimiter(ch.id, dsp.limiter);
  }
}

/*
 * dsp.js — aoip_engine 단일 프로세스 관리
 *
 * 기존 dsp_engine (다중 인스턴스) → aoip_engine (단일 통합 프로세스) 으로 교체.
 * 외부 API (startDsp, stopDsp, sendGain, …) 시그니처 완전 유지.
 *
 * 채널 번호 규칙:
 *   - globalId  : channels.js 가 부여한 전역 채널 ID (1-based)
 *   - aoip_engine 명령에서 <ch>는 globalId 를 그대로 사용 (1-based)
 */
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import logger from './logger.js';
import { setJackRouteFns } from './jack.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const BINARY    = join(__dirname, '../scripts/aoip_engine');

/* ── 단일 엔진 상태 ──────────────────────────────────────── */
let _proc      = null;   // ChildProcess
let _ready     = false;
let _stdoutBuf = '';

/* 인스턴스 레지스트리 (name → { n_in, n_out }) */
const _instances = new Map();
let   _totalIn   = 0;
let   _totalOut  = 0;
let   _launchTimer = null;

/* ready 대기 콜백 (name → [resolve]) */
const _readyCbs = new Map();

/* ── 채널 맵 ─────────────────────────────────────────────── */
/* globalId → { name, localId } */
const _channelDsp   = new Map();
/* globalId → 입력 jackPort ("analog:out_1") */
const _inChToPort   = new Map();
/* jackPort → globalId */
const _inPortToGlob = new Map();
/* globalId → 출력 jackPort ("analog:sin_1") */
const _outChToPort  = new Map();
const _outPortToGlob = new Map();

/* ── 레벨 미터 (jackPort → dB) ──────────────────────────── */
const _inLevel  = new Map();
const _outLevel = new Map();
const _limMeter = new Map();  // `out ${ch}` → { pre, post }

/* ─────────────────────────────────────────────────────────── */

/** channels.js 가 입력 채널 등록 시 호출 */
export function registerChannelDsp(id, name, localId) {
  _channelDsp.set(id, { name, localId });
  const port = `${name}:out_${localId}`;
  _inChToPort.set(id, port);
  _inPortToGlob.set(port, id);
  /* 출력 포트도 대칭으로 예측 등록 (sin_ 접두사) */
  const outPort = `${name}:sin_${localId}`;
  _outChToPort.set(id, outPort);
  _outPortToGlob.set(outPort, id);
}

/* ── aoip_engine 글로벌 채널 번호 → jackPort ─────────────── */
function _inGlobToPort(ch)  { return _inChToPort.get(ch);   }
function _outGlobToPort(ch) { return _outChToPort.get(ch);  }
function _inPortToGlobId(p) { return _inPortToGlob.get(p);  }
function _outPortToGlobId(p){ return _outPortToGlob.get(p); }

/* ── stdin 송신 헬퍼 ─────────────────────────────────────── */
function _write(line) {
  if (_proc?.stdin?.writable) _proc.stdin.write(line + '\n');
}

/* ── 라우팅 콜백 (jack.js stub 에 등록) ──────────────────── */
function _routeConnect(src, dst) {
  /* src = 입력 jackPort (e.g. "analog:out_1")
   * dst = 출력 jackPort (e.g. "analog:sin_1")
   * 하드웨어 포트 (system:capture 등)는 내부 라우팅 불필요 → 무시 */
  const inGlob  = _inPortToGlobId(src);
  const outGlob = _outPortToGlobId(dst);
  if (inGlob == null || outGlob == null) return;
  _write(`route add ${inGlob} ${outGlob}`);
  logger.debug('[dsp] route add %d→%d  (%s→%s)', inGlob, outGlob, src, dst);
}

function _routeDisconnect(src, dst) {
  const inGlob  = _inPortToGlobId(src);
  const outGlob = _outPortToGlobId(dst);
  if (inGlob == null || outGlob == null) return;
  _write(`route remove ${inGlob} ${outGlob}`);
  logger.debug('[dsp] route remove %d→%d  (%s→%s)', inGlob, outGlob, src, dst);
}

setJackRouteFns(_routeConnect, _routeDisconnect);

/* ── stdout 파서 ─────────────────────────────────────────── */
function _parseLine(line) {
  const parts = line.split(' ');

  /* lvl in <ch> <db> */
  if (parts[0] === 'lvl' && parts[1] === 'in' && parts.length >= 4) {
    const ch = parseInt(parts[2], 10);
    const port = _inGlobToPort(ch);
    if (port) _inLevel.set(port, parseFloat(parts[3]));
    return;
  }

  /* lvl out <ch> <db> */
  if (parts[0] === 'lvl' && parts[1] === 'out' && parts.length >= 4) {
    const ch = parseInt(parts[2], 10);
    const port = _outGlobToPort(ch);
    if (port) _outLevel.set(port, parseFloat(parts[3]));
    return;
  }

  /* lm out <ch> <pre> <post> */
  if (parts[0] === 'lm' && parts[1] === 'out' && parts.length >= 5) {
    const ch = parseInt(parts[2], 10);
    _limMeter.set(`out ${ch}`, { pre: parseFloat(parts[3]), post: parseFloat(parts[4]) });
    return;
  }

  /* [aoip_engine] ready … */
  if (line.includes('[aoip_engine]') && line.includes('ready')) {
    logger.info('[dsp] aoip_engine ready');
    _ready = true;
    for (const cbs of _readyCbs.values()) cbs.forEach(fn => fn());
    _readyCbs.clear();
    return;
  }

  /* bridge:<name>:ready / stopped */
  if (line.startsWith('bridge:')) {
    logger.debug('[dsp] %s', line);
    return;
  }

  logger.debug('[dsp] %s', line);
}

/* ── 엔진 기동 ──────────────────────────────────────────────*/
function _launch() {
  _launchTimer = null;
  if (_proc) return;

  _ready = false;
  _stdoutBuf = '';

  const proc = spawn(
    'chrt', ['-f', '85', BINARY,
      String(_totalIn), String(_totalOut), '--name', 'main'],
    { stdio: ['pipe', 'pipe', 'pipe'], detached: false }
  );
  _proc = proc;

  proc.stdout.on('data', d => {
    _stdoutBuf += d.toString();
    let nl;
    while ((nl = _stdoutBuf.indexOf('\n')) >= 0) {
      const line = _stdoutBuf.slice(0, nl).trim();
      _stdoutBuf = _stdoutBuf.slice(nl + 1);
      if (line) _parseLine(line);
    }
  });

  proc.stderr.on('data', d => {
    const s = d.toString();
    logger.debug('[aoip_engine] %s', s.trimEnd());
    if (s.includes('[aoip_engine]') && s.includes('ready') && !_ready) {
      _ready = true;
      for (const cbs of _readyCbs.values()) cbs.forEach(fn => fn());
      _readyCbs.clear();
    }
  });

  proc.on('exit', (code, signal) => {
    logger.info('[dsp] aoip_engine exited code=%s signal=%s', code, signal);
    _proc  = null;
    _ready = false;
    if (signal === 'SIGTERM') return;
    logger.info('[dsp] aoip_engine restarting in 3s...');
    setTimeout(() => { _totalIn = 0; _totalOut = 0; _instances.clear(); }, 0);
  });

  proc.on('error', err => {
    logger.error('[dsp] aoip_engine error: %s', err.message);
    _proc = null;
  });

  logger.info('[dsp] started aoip_engine (in=%d out=%d)', _totalIn, _totalOut);
}

/* ── Public API ─────────────────────────────────────────── */

/** DSP 클라이언트가 ready 될 때까지 대기 (모든 name이 같은 엔진 공유) */
export function waitForDspReady(name, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    if (_ready) return resolve();
    const t = setTimeout(() => {
      const cbs = _readyCbs.get(name) ?? [];
      _readyCbs.set(name, cbs.filter(f => f !== cb));
      reject(new Error(`dsp ${name} ready timeout`));
    }, timeoutMs);
    const cb = () => { clearTimeout(t); resolve(); };
    const cbs = _readyCbs.get(name) ?? [];
    cbs.push(cb);
    _readyCbs.set(name, cbs);
  });
}

/**
 * DSP 인스턴스 시작 (기존 API 유지).
 * 모든 호출이 단일 aoip_engine에 통합됨.
 * 중복 호출을 처리하기 위해 deferred launch 사용.
 */
export function startDsp(name, n_in, n_out) {
  const prev = _instances.get(name);
  _totalIn  = _totalIn  - (prev?.n_in  ?? 0) + n_in;
  _totalOut = _totalOut - (prev?.n_out ?? 0) + n_out;
  _instances.set(name, { n_in, n_out });

  if (!_launchTimer && !_proc) {
    _launchTimer = setTimeout(_launch, 0);
  }
}

export function stopDsp(name) {
  if (!name) {
    /* 전체 중지 */
    _proc?.kill('SIGTERM');
    _proc  = null;
    _ready = false;
    _instances.clear();
    _totalIn = _totalOut = 0;
  }
  /* 개별 name 중지는 no-op (단일 엔진이므로 개별 중지 불가) */
}

export function isDspRunning(name) {
  if (!_proc || _proc.killed || _proc.exitCode !== null) return false;
  if (name) return _instances.has(name);
  return true;
}

/** aoip_engine stdin 직접 송신 (bridges.js, gstreamer.js 에서 사용) */
export function sendToEngine(line) {
  _write(line);
}

/** bridges.js 등에서 엔진이 준비될 때까지 대기하는 Promise 반환 */
export function waitForEngineReady(timeoutMs = 5000) {
  return waitForDspReady('_engine', timeoutMs);
}

/* ── 레벨 미터 API ──────────────────────────────────────── */

export function getInLevel(jackPort) {
  return _inLevel.get(jackPort) ?? -120;
}

export function getOutLevel(jackPort) {
  return _outLevel.get(jackPort) ?? -120;
}

export function getLimiterMeters(name = 'analog') {
  /* 이름 관계없이 전역 리미터 맵 반환 */
  return _limMeter;
}

/* ── DSP 명령 API ────────────────────────────────────────── */

/** globalId → dsp 클라이언트 정보 조회 */
function _resolve(globalId) {
  const entry = _channelDsp.get(globalId);
  if (!entry) throw new Error(`no DSP registered for channel ${globalId}`);
  return entry;
}

export function sendGain(dir, globalId, sliderVal) {
  _resolve(globalId);  // 유효성 확인
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
  _write(`gain ${dir} ${globalId} ${Math.min(2.0, linear).toFixed(4)}`);
}

export function sendMute(dir, globalId, muted) {
  _resolve(globalId);
  _write(`mute ${dir} ${globalId} ${muted ? '1' : '0'}`);
}

export function sendBypass(dir, globalId, bypass) {
  _resolve(globalId);
  _write(`bypass ${dir} ${globalId} ${bypass ? '1' : '0'}`);
}

export function sendHpf(globalId, params) {
  _resolve(globalId);
  const dir = params.type === 'output' ? 'out' : 'in';
  if (params.slope !== undefined)
    _write(`hpf ${dir} ${globalId} slope ${params.slope}`);
  if (params.freq !== undefined)
    _write(`hpf ${dir} ${globalId} freq ${params.freq}`);
  if (params.enabled !== undefined)
    _write(`hpf ${dir} ${globalId} enable ${params.enabled ? '1' : '0'}`);
}

const BAND_TYPE_MAP = {
  peak: 'peak', low_shelf: 'loshelf', high_shelf: 'hishelf', lp: 'lp', hp: 'hp',
};

export function sendEqBand(dir, globalId, bandIndex, params) {
  _resolve(globalId);
  const band = bandIndex + 1;
  if (params.enabled !== undefined)
    _write(`eq ${dir} ${globalId} ${band} enable ${params.enabled ? '1' : '0'}`);
  if (params.b0 !== undefined)
    _write(`eq ${dir} ${globalId} ${band} coeffs ${params.b0} ${params.b1} ${params.b2} ${params.a1} ${params.a2}`);
  if (params.freq !== undefined)
    _write(`eq ${dir} ${globalId} ${band} freq ${params.freq}`);
  if (params.gain !== undefined)
    _write(`eq ${dir} ${globalId} ${band} gain ${params.gain}`);
  if (params.q !== undefined)
    _write(`eq ${dir} ${globalId} ${band} q ${params.q}`);
  if (params.bandType !== undefined)
    _write(`eq ${dir} ${globalId} ${band} type ${BAND_TYPE_MAP[params.bandType] ?? 'peak'}`);
}

export function sendLimiter(globalId, params) {
  _resolve(globalId);
  if (params.enabled   !== undefined) _write(`limiter out ${globalId} enable    ${params.enabled   ? '1' : '0'}`);
  if (params.threshold !== undefined) _write(`limiter out ${globalId} threshold ${params.threshold}`);
  if (params.attack    !== undefined) _write(`limiter out ${globalId} attack    ${params.attack}`);
  if (params.release   !== undefined) _write(`limiter out ${globalId} release   ${params.release}`);
  if (params.makeup    !== undefined) _write(`limiter out ${globalId} makeup    ${params.makeup}`);
}

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

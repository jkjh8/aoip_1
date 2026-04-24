import { readFileSync, writeFileSync, existsSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { getInLevel, getOutLevel, registerChannelDsp } from './dsp.js';
import logger from './logger.js';
import { getConfig } from './config.js';

const __dirname  = dirname(fileURLToPath(import.meta.url));
const STATE_FILE = join(__dirname, '../config/channels.json');

const config = getConfig();

export const ANALOG_COUNT = config.jack.channels ?? 2;

/* ── DSP 클라이언트 할당 테이블 ── */
const _dspMap     = new Map(); // channelId → { client, localId }
const _dspInCnt   = new Map(); // dspClient → 입력 채널 수
const _dspOutCnt  = new Map(); // dspClient → 출력 채널 수

function _assignInput(id, client) {
  const localId = (_dspInCnt.get(client) ?? 0) + 1;
  _dspInCnt.set(client, localId);
  _dspMap.set(`in:${id}`, { client, localId });
  registerChannelDsp(id, client, localId);
  return localId;
}
function _assignOutput(id, client) {
  const localId = (_dspOutCnt.get(client) ?? 0) + 1;
  _dspOutCnt.set(client, localId);
  _dspMap.set(`out:${id}`, { client, localId });
  return localId;
}

export function getDspClientOf(id, dir = 'in') {
  return _dspMap.get(`${dir}:${id}`)?.client ?? 'analog';
}
export function getDspLocalId(id, dir = 'in') {
  return _dspMap.get(`${dir}:${id}`)?.localId ?? id;
}
/** index.js가 startDsp 호출 시 사용: { name → { n_in, n_out } } */
export function getDspChannelCounts() {
  const result = new Map();
  for (const [client, n_in] of _dspInCnt)
    result.set(client, { n_in, n_out: _dspOutCnt.get(client) ?? 0 });
  for (const [client, n_out] of _dspOutCnt)
    if (!result.has(client)) result.set(client, { n_in: 0, n_out });
  return result;
}

function dspClientOf(id, dir) { return getDspClientOf(id, dir); }
function dspLocalId(id, dir)  { return getDspLocalId(id, dir); }
function inputJackPort(id)    { return `${dspClientOf(id,'in')}:out_${dspLocalId(id,'in')}`; }
function outputJackPort(id)   { return `${dspClientOf(id,'out')}:sin_${dspLocalId(id,'out')}`; }

// ── DSP default factories ────────────────────────────

function defaultEq() {
  return [
    { enabled: false, b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 },
    { enabled: false, b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 },
    { enabled: false, b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 },
    { enabled: false, b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 },
  ];
}

// 입력 채널 원본 소스 포트 목록 (gainer 연결용) — { id, srcPort }[]
const inputSrcPorts = [];

function makeInput(id, label, srcPort, bypassDsp = false) {
  return {
    id, label,
    jackPort: inputJackPort(id),
    srcPort,
    gain: 100, muted: false,
    bypassDsp,
    dsp: { hpf: { enabled: false, freq: 80 }, eq: defaultEq() }
  };
}

function defaultInput(id, label, srcPort, bypassDsp = false) {
  inputSrcPorts.push({ id, srcPort });
  return makeInput(id, label, srcPort, bypassDsp);
}

export function getInputSrcPorts() { return inputSrcPorts; }

// 출력 채널 원본 싱크 포트 목록 (DSP → 실제 재생 포트 연결용) — { id, sinkPort }[]
const outputSinkPorts = [];

function makeOutput(id, label, sinkPort, bypassDsp = false) {
  return {
    id, label,
    jackPort: outputJackPort(id),
    sinkPort,
    gain: 100, muted: false,
    bypassDsp,
    dsp: {
      eq: defaultEq(),
      limiter: { enabled: false, threshold: -6, attack: 5, release: 100, makeup: 0 }
    }
  };
}

function defaultOutput(id, label, sinkPort, bypassDsp = false) {
  outputSinkPorts.push({ id, sinkPort });
  return makeOutput(id, label, sinkPort, bypassDsp);
}

export function getOutputSinkPorts() { return outputSinkPorts; }

// ── 브릿지 채널 레이블 생성 헬퍼 ─────────────────────
function bridgeLabel(name) {
  // hifiberry_in → HFB, uac2_in → UAC2
  const base = name.replace(/_in$/, '').replace(/_out$/, '');
  const abbr = {
    hifiberry: 'HFB',
    sndrpihifiberry: 'HFB',
    uac2: 'UAC2',
    uac2gadget: 'UAC2'
  };
  return abbr[base.toLowerCase()] ?? base.toUpperCase();
}

// ── State init — Analog + 브릿지 채널 ──────────────

const baseName = config.jack.name ?? 'Analog';
const ravennaInputs  = Array.from({ length: ANALOG_COUNT }, (_, i) => {
  const id = i + 1;
  _assignInput(id, 'analog');
  return defaultInput(id, `${baseName} ${id}`, `system:capture_${id}`);
});
const ravennaOutputs = Array.from({ length: ANALOG_COUNT }, (_, i) => {
  const id = i + 1;
  _assignOutput(id, 'analog');
  return defaultOutput(id, `${baseName} ${id}`, `system:playback_${id}`);
});

// audio.json 브릿지 설정에서 추가 채널 생성
// 처리 순서: 일반 브릿지(USB 등) → RTP 스트림 → Ravenna/AES67
// → Ravenna가 항상 가장 높은 DSP 채널 번호를 갖는다 (e.g. 7-8)
const bridgeInputs  = [];
const bridgeOutputs = [];
const bridgeChannelDefs = new Map(); // name → { inputs, outputs }
let nextInId  = ANALOG_COUNT + 1;
let nextOutId = ANALOG_COUNT + 1;

function bridgeDspClient(b) {
  if (b.usb_gadget) return 'usb';
  if (b.aes67)      return 'aes67';
  return b.name.toLowerCase().replace(/[^a-z0-9]/g, '_');
}

function addBridge(b) {
  const chCount   = b.channels ?? 2;
  const abbr      = bridgeLabel(b.name);
  const enabled   = b.enabled !== false;
  const isRavenna   = b.type === 'ravenna' || b.aes67 === true;
  const needsSuffix = b.type === 'ravenna';
  const isInput     = b.type === 'zita_in'  || b.type === 'audio_in'  || b.type === 'ravenna';
  const isOutput    = b.type === 'zita_out' || b.type === 'audio_out' || b.type === 'ravenna';
  const dspClient   = bridgeDspClient(b);

  const bIns = [], bOuts = [];

  if (isInput) {
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextInId++;
      _assignInput(id, dspClient);
      const srcPort = needsSuffix
        ? `${b.name}_in:capture_${ch}`
        : b.type === 'audio_in'
          ? `${b.name}:audio_in_${ch}`
          : `${b.name}:capture_${ch}`;
      const chan = makeInput(id, `${abbr} ${ch}`, srcPort, true);
      bIns.push(chan);
      if (enabled) { inputSrcPorts.push({ id, srcPort: chan.srcPort, noRetry: b.usb_gadget === true || isRavenna, usbGadget: b.usb_gadget === true }); bridgeInputs.push(chan); }
    }
  }
  if (isOutput) {
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextOutId++;
      _assignOutput(id, dspClient);
      const sinkPort = needsSuffix
        ? `${b.name}_out:playback_${ch}`
        : b.type === 'audio_out'
          ? `${b.name}:audio_out_${ch}`
          : `${b.name}:playback_${ch}`;
      const chan = makeOutput(id, `${abbr} ${ch}`, sinkPort, true);
      bOuts.push(chan);
      if (enabled) { outputSinkPorts.push({ id, sinkPort: chan.sinkPort, noRetry: b.usb_gadget === true || isRavenna, usbGadget: b.usb_gadget === true }); bridgeOutputs.push(chan); }
    }
  }

  bridgeChannelDefs.set(b.name, { inputs: bIns, outputs: bOuts });
}

// 1단계: 일반 브릿지 (USB 등) — ravenna/aes67 제외
for (const b of (config.bridges ?? []).filter(b => b.type !== 'ravenna' && !b.aes67)) addBridge(b);

// 2단계: RTP 스트림 채널
const rtpInputs  = [];
const rtpOutputs = [];
for (const s of (config.rtp_streams ?? [])) {
  const chCount = s.channels ?? 2;
  const label   = s.name ?? s.client;
  const enabled = s.enabled !== false;
  if (s.type === 'rtp_in') {
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextInId++;
      _assignInput(id, 'stream');
      const chan = makeInput(id, `${label} ${ch}`, `${s.client}:out_${ch}`, true);
      if (enabled) { inputSrcPorts.push({ id, srcPort: chan.srcPort }); rtpInputs.push(chan); }
    }
  } else if (s.type === 'rtp_out') {
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextOutId++;
      _assignOutput(id, 'stream');
      const chan = makeOutput(id, `${label} ${ch}`, `${s.client}:in_src_${ch}`, true);
      if (enabled) { outputSinkPorts.push({ id, sinkPort: chan.sinkPort }); rtpOutputs.push(chan); }
    }
  }
}

// 3단계: Ravenna/AES67 브릿지 — 마지막으로 배정 (DSP 7-8번)
for (const b of (config.bridges ?? []).filter(b => b.type === 'ravenna' || b.aes67)) addBridge(b);

export function getTotalInputCount()  { return nextInId  - 1; }
export function getTotalOutputCount() { return nextOutId - 1; }

let state = {
  inputs:  [...ravennaInputs,  ...bridgeInputs,  ...rtpInputs ].sort((a, b) => a.id - b.id),
  outputs: [...ravennaOutputs, ...bridgeOutputs, ...rtpOutputs].sort((a, b) => a.id - b.id),
  routing: []   // 사용자가 설정한 라우팅 매트릭스 연결 목록 [{src, dst}]
};

// 저장된 상태 병합 (jackPort 기준으로 매칭)
// savedInMap / savedOutMap 은 setBridgeEnabled 에서도 재사용
let savedInMap  = new Map();
let savedOutMap = new Map();

if (existsSync(STATE_FILE)) {
  try {
    const saved = JSON.parse(readFileSync(STATE_FILE, 'utf8'));
    const byPort = (arr) => {
      const m = new Map();
      for (const ch of (arr ?? [])) m.set(ch.jackPort, ch);
      return m;
    };
    savedInMap  = byPort(saved.inputs);
    savedOutMap = byPort(saved.outputs);
    state.inputs  = state.inputs.map(def  => { const { id, jackPort, srcPort,  ...u } = savedInMap.get(def.jackPort)  ?? {}; return deepMerge(def, u); });
    state.outputs = state.outputs.map(def => { const { id, jackPort, sinkPort, ...u } = savedOutMap.get(def.jackPort) ?? {}; return deepMerge(def, u); });
    const validSrcs = new Set(state.inputs.map(c => c.jackPort));
    const validDsts = new Set(state.outputs.map(c => c.jackPort));
    state.routing = (saved.routing ?? []).filter(r => validSrcs.has(r.src) && validDsts.has(r.dst));
  } catch (e) {
    logger.warn('[channels] load failed:', e.message);
  }
}

function deepMerge(target, source) {
  if (!source || typeof source !== 'object') return target;
  const out = { ...target };
  for (const [k, v] of Object.entries(source)) {
    if (v && typeof v === 'object' && !Array.isArray(v)) {
      out[k] = deepMerge(target[k] ?? {}, v);
    } else {
      out[k] = v;
    }
  }
  return out;
}

function save() {
  try { writeFileSync(STATE_FILE, JSON.stringify(state, null, 2)); }
  catch (e) { logger.warn('[channels] save failed:', e.message); }
}

// ── Bridge channel toggle ────────────────────────────

export function getBridgeChannelDef(name) {
  return bridgeChannelDefs.get(name);
}

/**
 * 브릿지 채널을 state에 추가/제거.
 * bridges.js 에서 usb_gadget 토글 시 호출.
 * @param {string} bridgeName
 * @param {boolean} enabled
 */
export function setBridgeEnabled(bridgeName, enabled) {
  const def = bridgeChannelDefs.get(bridgeName);
  if (!def) { logger.warn('[channels] bridge not found: %s', bridgeName); return; }

  const inPorts  = new Set(def.inputs.map(c => c.jackPort));
  const outPorts = new Set(def.outputs.map(c => c.jackPort));

  state.inputs  = state.inputs.filter(c => !inPorts.has(c.jackPort));
  state.outputs = state.outputs.filter(c => !outPorts.has(c.jackPort));

  if (enabled) {
    const ins  = def.inputs.map(c  => { const { id, jackPort, srcPort,  ...u } = savedInMap.get(c.jackPort)  ?? {}; return deepMerge(c, u); });
    const outs = def.outputs.map(c => { const { id, jackPort, sinkPort, ...u } = savedOutMap.get(c.jackPort) ?? {}; return deepMerge(c, u); });
    state.inputs  = [...state.inputs,  ...ins ].sort((a, b) => a.id - b.id);
    state.outputs = [...state.outputs, ...outs].sort((a, b) => a.id - b.id);
  }

  logger.info('[channels] bridge %s %s', bridgeName, enabled ? 'enabled' : 'disabled');
}

// ── Getters ──────────────────────────────────────────

/**
 * @param {Array<{port:string, connections:string[]}>} connections
 *   JACK 커넥션 목록. 출력 레벨은 연결된 소스 포트의 최대 레벨로 계산.
 */
export function getChannels(connections = []) {
  // playback port → 연결된 capture port[] 역방향 맵
  const srcsByDst = new Map();
  for (const { port, connections: conns } of connections) {
    for (const dst of conns) {
      if (!srcsByDst.has(dst)) srcsByDst.set(dst, []);
      srcsByDst.get(dst).push(port);
    }
  }

  return {
    inputs:  state.inputs.map(ch => ({
      ...ch,
      level: ch.muted ? -100 : getInLevel(ch.jackPort)
    })),
    outputs: state.outputs.map(ch => {
      if (ch.muted) return { ...ch, level: -100 };
      const level = getOutLevel(`${dspClientOf(ch.id)}:sout_${dspLocalId(ch.id)}`);
      return { ...ch, level };
    })
  };
}

// ── Channel setters ──────────────────────────────────

function find(type, id) {
  const list = type === 'input' ? state.inputs : state.outputs;
  const ch = list.find(c => c.id === Number(id));
  if (!ch) throw new Error(`${type} channel ${id} not found`);
  return ch;
}

export function setGain(type, id, gain) {
  find(type, id).gain = Math.max(0, Math.min(150, Number(gain)));
  save();
}

export function setMute(type, id, muted) {
  find(type, id).muted = Boolean(muted);
  save();
}

export function setLabel(type, id, label) {
  find(type, id).label = String(label).slice(0, 32);
  save();
}

// ── DSP setters ──────────────────────────────────────

export function setHpf(id, params) {
  const ch = find('input', id);
  Object.assign(ch.dsp.hpf, params);
  save();
}

export function setEqBand(type, id, bandIndex, params) {
  const ch = find(type, id);
  if (bandIndex < 0 || bandIndex >= ch.dsp.eq.length) throw new Error('invalid band index');
  Object.assign(ch.dsp.eq[bandIndex], params);
  save();
}

export function setLimiter(id, params) {
  const ch = find('output', id);
  if (!ch.dsp.limiter)
    ch.dsp.limiter = { enabled: false, threshold: -6, attack: 5, release: 100, makeup: 0 };
  Object.assign(ch.dsp.limiter, params);
  save();
}

// ── Routing ──────────────────────────────────────────

export function addRoute(src, dst) {
  if (!state.routing.find(r => r.src === src && r.dst === dst)) {
    state.routing.push({ src, dst });
    save();
  }
}

export function removeRoute(src, dst) {
  state.routing = state.routing.filter(r => !(r.src === src && r.dst === dst));
  save();
}

export function getSavedRoutes() { return state.routing; }

// ── Metering ─────────────────────────────────────────
// 레벨 미터는 dsp_engine reporter 스레드에서 직접 측정 (jack_meter 클라이언트 불필요)
export function startMeters() {}
export function stopMeters() {}

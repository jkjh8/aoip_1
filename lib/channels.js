import { readFileSync, writeFileSync, existsSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { getInLevel } from './gainer.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const STATE_FILE  = join(__dirname, '../config/channels.json');
const CONFIG_FILE = join(__dirname, '../config/audio.json');

const config = JSON.parse(readFileSync(CONFIG_FILE, 'utf8'));

export const RAVENNA_COUNT = 2;

// ── DSP default factories ────────────────────────────

function defaultEq() {
  return [
    { enabled: false, b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 },
    { enabled: false, b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 },
    { enabled: false, b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 },
    { enabled: false, b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 },
  ];
}

// 입력 채널 원본 소스 포트 목록 (gainer 연결용, id 순서대로)
const inputSrcPorts = [];

function defaultInput(id, label, srcPort) {
  inputSrcPorts.push(srcPort);
  return {
    id, label,
    jackPort: `gainer:out_${id}`,   // post-gain 포트
    srcPort,                          // gainer 연결 원본
    gain: 100, muted: false,
    dsp: { hpf: { enabled: false, freq: 80 }, eq: defaultEq() }
  };
}

export function getInputSrcPorts() { return inputSrcPorts; }

// 출력 채널 원본 싱크 포트 목록 (gainer → 실제 재생 포트 연결용)
const outputSinkPorts = [];

function defaultOutput(id, label, sinkPort) {
  outputSinkPorts.push(sinkPort);
  return {
    id, label,
    jackPort: `gainer:sin_${id}`,   // 매트릭스가 여기에 연결
    sinkPort,                         // gainer:sout_N → 이 포트로 출력
    gain: 100, muted: false,
    dsp: {
      eq: defaultEq(),
      limiter: { enabled: false, threshold: -6, attack: 5, release: 100, makeup: 0 }
    }
  };
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

// ── State init — RAVENNA + 브릿지 채널 ──────────────

const ravennaInputs  = Array.from({ length: RAVENNA_COUNT }, (_, i) =>
  defaultInput(i + 1, `IN ${i + 1}`, `system:capture_${i + 1}`)
);
const ravennaOutputs = Array.from({ length: RAVENNA_COUNT }, (_, i) =>
  defaultOutput(i + 1, `OUT ${i + 1}`, `system:playback_${i + 1}`)
);

// audio.json 브릿지 설정에서 추가 채널 생성
// alsa_in  → JACK output ports (소스) → 입력 채널
// alsa_out → JACK input  ports (싱크) → 출력 채널
const bridgeInputs  = [];
const bridgeOutputs = [];
let nextInId  = RAVENNA_COUNT + 1;
let nextOutId = RAVENNA_COUNT + 1;

for (const b of (config.bridges ?? [])) {
  const chCount = b.channels ?? 2;   // 기본 스테레오
  const abbr    = bridgeLabel(b.name);

  const isInput  = b.type === 'alsa_in'  || b.type === 'zita-a2j';
  const isOutput = b.type === 'alsa_out' || b.type === 'zita-j2a';

  if (isInput) {
    for (let ch = 1; ch <= chCount; ch++) {
      bridgeInputs.push(
        defaultInput(nextInId++, `${abbr} ${ch}`, `${b.name}:capture_${ch}`)
      );
    }
  } else if (isOutput) {
    for (let ch = 1; ch <= chCount; ch++) {
      bridgeOutputs.push(
        defaultOutput(nextOutId++, `${abbr} ${ch}`, `${b.name}:playback_${ch}`)
      );
    }
  }
}

let state = {
  inputs:  [...ravennaInputs,  ...bridgeInputs ],
  outputs: [...ravennaOutputs, ...bridgeOutputs]
};

// 저장된 상태 병합 (jackPort 기준으로 매칭)
if (existsSync(STATE_FILE)) {
  try {
    const saved = JSON.parse(readFileSync(STATE_FILE, 'utf8'));
    const byPort = (arr) => {
      const m = new Map();
      for (const ch of (arr ?? [])) m.set(ch.jackPort, ch);
      return m;
    };
    const savedIn  = byPort(saved.inputs);
    const savedOut = byPort(saved.outputs);
    state.inputs  = state.inputs.map(def  => deepMerge(def, savedIn.get(def.jackPort)  ?? {}));
    state.outputs = state.outputs.map(def => deepMerge(def, savedOut.get(def.jackPort) ?? {}));
  } catch (e) {
    console.warn('[channels] load failed:', e.message);
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
  catch (e) { console.warn('[channels] save failed:', e.message); }
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
      const srcs = (srcsByDst.get(ch.jackPort) ?? [])
        .filter(p => {
          const inp = state.inputs.find(i => i.jackPort === p);
          return !inp || !inp.muted;
        });
      const level = srcs.length > 0 ? Math.max(...srcs.map(p => getInLevel(p))) : -100;
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

// ── Metering ─────────────────────────────────────────
// 레벨 미터는 dsp_engine reporter 스레드에서 직접 측정 (jack_meter 클라이언트 불필요)
export function startMeters() {}
export function stopMeters() {}

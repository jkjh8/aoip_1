import { readFileSync, writeFileSync, existsSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import { getInLevel, getOutLevel,
         connect, disconnect, sendGain, sendMute,
         sendTrim, sendHpf, sendEqBand,
         sendGate, sendComp, sendLim } from '../dsp/index.js'
import { defaultInputDsp, defaultOutputDsp } from './dsp_state.js'
import logger from '../logger.js'
import { getConfig } from '../config.js'
import {
  _assignInput, _assignOutput,
  getDspClientOf, getDspLocalId, getDspChannelCounts,
  inputSrcPorts, outputSinkPorts,
  makeInput, makeOutput, defaultInput, defaultOutput,
  bridgeLabel, bridgeDspClient, deepMerge,
} from './helpers.js'
import {
  _initI2sMode, getI2sMode, setI2sMode as _setI2sMode,
  isStereoLinked, getPair, i2sModeEvents,
} from './i2s_mode.js'

export { getDspClientOf, getDspLocalId, getDspChannelCounts }
export { getI2sMode, isStereoLinked, getPair, i2sModeEvents }

const __dirname  = dirname(fileURLToPath(import.meta.url))
const STATE_FILE = join(__dirname, '../../config/channels.json')

const config = getConfig()

const ANALOG_COUNT = config.jack.channels ?? 2

const baseName = config.jack.name ?? 'Analog'

const ANALOG_DSP = config.jack?.dspEnabled === true
const ravennaInputs = Array.from({ length: ANALOG_COUNT }, (_, i) => {
  const id = i + 1
  _assignInput(id, 'analog')
  return { ...defaultInput(id, `${baseName} ${id}`, `system:capture_${id}`, ANALOG_DSP), source: { type: 'analog', ch: i + 1 }, active: true }
})
const ravennaOutputs = Array.from({ length: ANALOG_COUNT }, (_, i) => {
  const id = i + 1
  _assignOutput(id, 'analog')
  return { ...defaultOutput(id, `${baseName} ${id}`, `system:playback_${id}`, ANALOG_DSP), source: { type: 'analog', ch: i + 1 }, active: true }
})

const bridgeInputs      = []
const bridgeOutputs     = []
const bridgeChannelDefs = new Map()
let nextInId  = ANALOG_COUNT + 1
let nextOutId = ANALOG_COUNT + 1

function addBridge(b) {
  const chCount   = b.channels ?? 2
  const abbr      = bridgeLabel(b.name)
  const enabled   = b.enabled !== false
  const isRavenna   = b.type === 'ravenna' || b.aes67 === true
  const needsSuffix = b.type === 'ravenna'
  const isInput     = b.type === 'zita_in'  || b.type === 'audio_in'  || b.type === 'ravenna'
  const isOutput    = b.type === 'zita_out' || b.type === 'audio_out' || b.type === 'ravenna'
  const dspClient   = bridgeDspClient(b)
  const dspEnabled  = b.dspEnabled === true

  const bIns = [], bOuts = []
  const srcType = b.aes67 ? 'aes67' : b.type === 'ravenna' ? 'ravenna' : b.type
  // AES67/Ravenna는 sinks/sources 매핑으로 active가 동적으로 결정되므로 false 시작.
  // 그 외 일반 브리지(USB 등 audio_in/audio_out, zita_in/zita_out)는 enabled를 따라간다.
  const defaultActive = (isRavenna ? false : enabled)

  if (isInput) {
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextInId++
      _assignInput(id, dspClient)
      const srcPort = needsSuffix
        ? `${b.name}_in:capture_${ch}`
        : b.type === 'audio_in'
          ? `${b.name}:audio_in_${ch}`
          : `${b.name}:capture_${ch}`
      const chan = { ...makeInput(id, `${abbr} ${ch}`, srcPort, dspEnabled), source: { type: srcType, name: b.name, ch }, active: defaultActive }
      bIns.push(chan)
      if (enabled) inputSrcPorts.push({ id, srcPort: chan.srcPort, noRetry: isRavenna })
      bridgeInputs.push(chan)
    }
  }
  if (isOutput) {
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextOutId++
      _assignOutput(id, dspClient)
      const sinkPort = needsSuffix
        ? `${b.name}_out:playback_${ch}`
        : b.type === 'audio_out'
          ? `${b.name}:audio_out_${ch}`
          : `${b.name}:playback_${ch}`
      const chan = { ...makeOutput(id, `${abbr} ${ch}`, sinkPort, dspEnabled), source: { type: srcType, name: b.name, ch }, active: defaultActive }
      bOuts.push(chan)
      if (enabled) outputSinkPorts.push({ id, sinkPort: chan.sinkPort, noRetry: isRavenna })
      bridgeOutputs.push(chan)
    }
  }

  bridgeChannelDefs.set(b.name, { inputs: bIns, outputs: bOuts })
}

// Stage 1: non-ravenna bridges
for (const b of (config.bridges ?? []).filter(b => b.type !== 'ravenna' && !b.aes67)) addBridge(b)

// Stage 2: RTP streams
const rtpInputs   = []
const rtpOutputs  = []
const _rtpChStart = new Map()

for (const s of (config.rtp_streams ?? [])) {
  const chCount = s.channels ?? 2
  const label   = s.name ?? s.client
  const enabled = s.enabled !== false
  const dspEnabled = s.dspEnabled === true
  if (s.type === 'rtp_in') {
    _rtpChStart.set(`in:${s.client}`, nextInId - 1)
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextInId++
      _assignInput(id, 'stream')
      const chan = { ...makeInput(id, `${label} ${ch}`, `${s.client}:out_${ch}`, dspEnabled), source: { type: 'rtp', name: s.client, ch }, active: false }
      if (enabled) inputSrcPorts.push({ id, srcPort: chan.srcPort })
      rtpInputs.push(chan)
    }
  } else if (s.type === 'rtp_out') {
    _rtpChStart.set(`out:${s.client}`, nextOutId - 1)
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextOutId++
      _assignOutput(id, 'stream')
      const chan = { ...makeOutput(id, `${label} ${ch}`, `${s.client}:in_src_${ch}`, dspEnabled), source: { type: 'rtp', name: s.client, ch }, active: false }
      if (enabled) outputSinkPorts.push({ id, sinkPort: chan.sinkPort })
      rtpOutputs.push(chan)
    }
  }
}

export function getRtpChStart(client, dir = 'in') {
  return _rtpChStart.get(`${dir}:${client}`) ?? 0
}

export function getBridgeChStart(name, dir = 'in') {
  const def = bridgeChannelDefs.get(name)
  if (!def) return 0
  const chs = dir === 'in' ? def.inputs : def.outputs
  return chs.length ? chs[0].id - 1 : 0
}

// Stage 3: Ravenna/AES67 — last (highest DSP channel numbers)
for (const b of (config.bridges ?? []).filter(b => b.type === 'ravenna' || b.aes67)) addBridge(b)

let state = {
  inputs:  [...ravennaInputs,  ...bridgeInputs,  ...rtpInputs ].sort((a, b) => a.id - b.id),
  outputs: [...ravennaOutputs, ...bridgeOutputs, ...rtpOutputs].sort((a, b) => a.id - b.id),
  routing: [],
}

let savedInMap  = new Map()
let savedOutMap = new Map()
let savedI2sMode = null

if (existsSync(STATE_FILE)) {
  try {
    const saved = JSON.parse(readFileSync(STATE_FILE, 'utf8'))
    const byPort = (arr) => { const m = new Map(); for (const ch of (arr ?? [])) m.set(ch.port, ch); return m }
    savedInMap  = byPort(saved.inputs)
    savedOutMap = byPort(saved.outputs)
    state.inputs  = state.inputs.map(def  => { const { id, port, srcPort,  active: _a, dspEnabled: _de, ...u } = savedInMap.get(def.port)  ?? {}; return deepMerge(def, u) })
    state.outputs = state.outputs.map(def => { const { id, port, sinkPort, active: _a, dspEnabled: _de, ...u } = savedOutMap.get(def.port) ?? {}; return deepMerge(def, u) })
    const validSrcs = new Set(state.inputs.map(c => c.port))
    const validDsts = new Set(state.outputs.map(c => c.port))
    state.routing = (saved.routing ?? []).filter(r => validSrcs.has(r.src) && validDsts.has(r.dst))
    savedI2sMode = saved.i2s ?? null
  } catch (e) {
    logger.warn('[channels] load failed:', e.message)
  }
}

_initI2sMode(ANALOG_COUNT, savedI2sMode)

function save() {
  const strip = ({ source, ...rest }) => rest
  const data = { inputs: state.inputs.map(strip), outputs: state.outputs.map(strip), routing: state.routing, i2s: getI2sMode() }
  try { writeFileSync(STATE_FILE, JSON.stringify(data, null, 2)) }
  catch (e) { logger.warn('[channels] save failed:', e.message) }
}

// 포맷 정규화 — 새 필드(active 등)가 추가됐을 때 파일에 즉시 반영
save()

export function addRtpStreamChannels(cfg) {
  const chCount = cfg.channels ?? 2
  const label   = cfg.name ?? cfg.client
  const enabled = cfg.enabled !== false
  const dspEnabled = cfg.dspEnabled === true

  if (cfg.type === 'rtp_in') {
    _rtpChStart.set(`in:${cfg.client}`, nextInId - 1)
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextInId++
      _assignInput(id, 'stream')
      const chan = { ...makeInput(id, `${label} ${ch}`, `${cfg.client}:out_${ch}`, dspEnabled), source: { type: 'rtp', name: cfg.client, ch }, active: false }
      if (enabled) inputSrcPorts.push({ id, srcPort: chan.srcPort })
      state.inputs.push(chan)
    }
    state.inputs.sort((a, b) => a.id - b.id)
  } else if (cfg.type === 'rtp_out') {
    _rtpChStart.set(`out:${cfg.client}`, nextOutId - 1)
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextOutId++
      _assignOutput(id, 'stream')
      const chan = { ...makeOutput(id, `${label} ${ch}`, `${cfg.client}:in_src_${ch}`, dspEnabled), source: { type: 'rtp', name: cfg.client, ch }, active: false }
      if (enabled) outputSinkPorts.push({ id, sinkPort: chan.sinkPort })
      state.outputs.push(chan)
    }
    state.outputs.sort((a, b) => a.id - b.id)
  }
  save()
}

export function removeRtpStreamChannels(client) {
  state.inputs  = state.inputs.filter(ch  => !(ch.source?.type === 'rtp' && ch.source.name === client))
  state.outputs = state.outputs.filter(ch => !(ch.source?.type === 'rtp' && ch.source.name === client))
  _rtpChStart.delete(`in:${client}`)
  _rtpChStart.delete(`out:${client}`)
  save()
}

export function getBridgeChannelDef(name) {
  return bridgeChannelDefs.get(name)
}

export function getChannels() {
  const serialize = ({ id, label, port, gain, muted, active, dsp, dspEnabled }, level) => ({ id, label, port, gain, muted, active, level, dsp, dspEnabled: Boolean(dspEnabled) })

  return {
    inputs:  state.inputs.map(ch => serialize(ch, ch.muted ? -100 : getInLevel(ch.port))),
    outputs: state.outputs.map(ch => {
      const level = ch.muted ? -100 : getOutLevel(`${getDspClientOf(ch.id, 'out')}:sin_${getDspLocalId(ch.id, 'out')}`)
      return serialize(ch, level)
    }),
  }
}

export function getAllChannelDefs() {
  return {
    inputs:  state.inputs.map(({ id, label, port, active, source, dspEnabled }) => ({ id, label, port, active, source, dspEnabled: Boolean(dspEnabled) })),
    outputs: state.outputs.map(({ id, label, port, active, source, dspEnabled }) => ({ id, label, port, active, source, dspEnabled: Boolean(dspEnabled) })),
  }
}

// AES67 채널 active 일괄 동기화 — sinks/sources 변경 시 호출
// items: AES67 daemon의 sinks(input) 또는 sources(output) 배열
// item.map: 0-based ALSA 채널 인덱스 배열 → ch는 1-based
export function syncAes67Active(direction, items) {
  const list = direction === 'input' ? state.inputs : state.outputs
  const activeSet = new Set()
  for (const item of (items ?? [])) {
    for (const mapCh of (item.map ?? [])) {
      activeSet.add(mapCh + 1)  // 0-based → 1-based
    }
  }
  for (const ch of list) {
    if (ch.source?.type !== 'aes67') continue
    ch.active = activeSet.has(ch.source.ch)
  }
  save()
}

// sink/source 삭제 후 더 이상 커버되지 않는 채널의 라우팅만 정리
// remainingItems: 삭제 후 남은 sinks/sources 목록
export function clearAes67StaleRoutes(direction, remainingItems) {
  const list = direction === 'input' ? state.inputs : state.outputs
  const coveredSet = new Set()
  for (const item of (remainingItems ?? [])) {
    for (const mapCh of (item.map ?? [])) coveredSet.add(mapCh + 1)
  }
  let changed = false
  for (const ch of list) {
    if (ch.source?.type !== 'aes67') continue
    if (!ch.active || coveredSet.has(ch.source.ch)) continue
    const snapshot = [...state.routing]
    for (const { src, dst } of snapshot) {
      const matched = direction === 'input' ? src === ch.port : dst === ch.port
      if (matched) {
        try { disconnect(src, dst) } catch { }
        state.routing = state.routing.filter(r => !(r.src === src && r.dst === dst))
      }
    }
    ch.active = false
    changed = true
  }
  if (changed) save()
}

export function setChannelActive(type, id, active) {
  const list = type === 'input' ? state.inputs : state.outputs
  const ch = list.find(c => c.id === Number(id))
  if (!ch) throw new Error(`${type} channel ${id} not found`)
  if (ch.source?.type === 'analog') throw new Error('analog channels cannot be deactivated')
  if (ch.source?.type === 'rtp') {
    for (const c of list)
      if (c.source?.name === ch.source.name) c.active = Boolean(active)
  } else {
    ch.active = Boolean(active)
  }
  save()
  return ch
}

function find(type, id) {
  const list = type === 'input' ? state.inputs : state.outputs
  const ch = list.find(c => c.id === Number(id))
  if (!ch) throw new Error(`${type} channel ${id} not found`)
  return ch
}

export function setGain(type, id, gain) {
  const clamped = Math.max(0, Math.min(150, Number(gain)))
  find(type, id).gain = clamped
  const pair = getPair(type, id)
  if (pair) find(type, pair).gain = clamped
  save()
}

export function setMute(type, id, muted) {
  const v = Boolean(muted)
  find(type, id).muted = v
  const pair = getPair(type, id)
  if (pair) find(type, pair).muted = v
  save()
}

export function setLabel(type, id, label) {
  find(type, id).label = String(label).slice(0, 32)
  save()
}


export function getChannelDsp(type, id) {
  const ch = find(type, id)
  if (type === 'input') return ch.dsp ?? defaultInputDsp()
  return ch.dsp ?? defaultOutputDsp()
}

function _applyDsp(ch, type, section, params) {
  if (!ch.dsp) ch.dsp = type === 'input' ? defaultInputDsp() : defaultOutputDsp()
  if (section === 'trim') {
    ch.dsp.trim = Number(params)
  } else if (section === 'eq' && params?.band != null) {
    const idx = Number(params.band) - 1
    if (!Array.isArray(ch.dsp.eq)) ch.dsp.eq = (type === 'input' ? defaultInputDsp() : defaultOutputDsp()).eq
    ch.dsp.eq[idx] = { ...ch.dsp.eq[idx], ...params }
  } else {
    ch.dsp[section] = { ...(ch.dsp[section] ?? {}), ...params }
  }
}

export function setChannelDsp(type, id, section, params) {
  _applyDsp(find(type, id), type, section, params)
  const pair = getPair(type, id)
  if (pair) _applyDsp(find(type, pair), type, section, params)
  save()
}

export function addRoute(src, dst) {
  if (!state.routing.find(r => r.src === src && r.dst === dst)) {
    state.routing.push({ src, dst })
    save()
  }
}

export function removeRoute(src, dst) {
  state.routing = state.routing.filter(r => !(r.src === src && r.dst === dst))
  save()
}

export function getSavedRoutes() { return state.routing }

async function connectWithRetry(src, dst, retries = 5) {
  for (let i = 0; i < retries; i++) {
    try { await connect(src, dst); return true; } catch (e) {
      if (i < retries - 1) await new Promise(r => setTimeout(r, 1000));
      else logger.warn('[startup] connect %s→%s failed: %s', src, dst, e.message);
    }
  }
  return false;
}

export async function restoreRoutes() {
  const savedRoutes = getSavedRoutes();
  if (savedRoutes.length === 0) return;
  const { inputs: activeIn, outputs: activeOut } = getChannels([]);
  const validSrcs = new Set(activeIn.map(ch => ch.port));
  const validDsts = new Set(activeOut.map(ch => ch.port));
  logger.info('[startup] Restoring %d saved routes...', savedRoutes.length);
  for (const { src, dst } of savedRoutes) {
    if (!validSrcs.has(src) || !validDsts.has(dst)) continue;
    await connectWithRetry(src, dst);
  }
}

export function restoreDspState() {
  for (const ch of state.inputs) {
    sendGain('in', ch.id, ch.gain)
    if (ch.muted) sendMute('in', ch.id, true)
    const dsp = ch.dsp
    if (!dsp) continue
    if (dsp.trim !== undefined && dsp.trim !== 0) sendTrim('in', ch.id, dsp.trim)
    if (dsp.hpf) sendHpf('in', ch.id, dsp.hpf)
    if (Array.isArray(dsp.eq)) {
      for (const band of dsp.eq) sendEqBand('in', ch.id, band.band, band)
    }
    if (dsp.gate) sendGate('in', ch.id, dsp.gate)
    if (dsp.comp) sendComp('in', ch.id, dsp.comp)
  }
  for (const ch of state.outputs) {
    sendGain('out', ch.id, ch.gain)
    if (ch.muted) sendMute('out', ch.id, true)
    const dsp = ch.dsp
    if (!dsp) continue
    if (dsp.gate) sendGate('out', ch.id, dsp.gate)
    if (Array.isArray(dsp.eq)) {
      for (const band of dsp.eq) sendEqBand('out', ch.id, band.band, band)
    }
    if (dsp.comp) sendComp('out', ch.id, dsp.comp)
    if (dsp.lim)  sendLim(ch.id, dsp.lim)
  }
}

// I2S stereo 모드 진입(또는 재적용) 시 ch1의 DSP/gain/mute를 ch2에 복제하고
// 엔진에 ch2 명령을 재발행한다. mono direction에 대해 호출하면 no-op.
export function applyStereoMirror(direction) {
  if (!isStereoLinked(direction)) return
  const dir = direction === 'input' ? 'in' : 'out'
  const list = direction === 'input' ? state.inputs : state.outputs
  const master = list.find(c => c.id === 1)
  const slave  = list.find(c => c.id === 2)
  if (!master || !slave) return

  slave.gain  = master.gain
  slave.muted = master.muted
  slave.dsp   = master.dsp ? JSON.parse(JSON.stringify(master.dsp)) : undefined
  save()

  sendGain(dir, 2, slave.gain)
  sendMute(dir, 2, Boolean(slave.muted))
  const dsp = slave.dsp
  if (!dsp) return
  if (direction === 'input') {
    if (dsp.trim !== undefined) sendTrim('in', 2, dsp.trim)
    if (dsp.hpf) sendHpf('in', 2, dsp.hpf)
    if (Array.isArray(dsp.eq)) for (const band of dsp.eq) sendEqBand('in', 2, band.band, band)
    if (dsp.gate) sendGate('in', 2, dsp.gate)
    if (dsp.comp) sendComp('in', 2, dsp.comp)
  } else {
    if (dsp.gate) sendGate('out', 2, dsp.gate)
    if (Array.isArray(dsp.eq)) for (const band of dsp.eq) sendEqBand('out', 2, band.band, band)
    if (dsp.comp) sendComp('out', 2, dsp.comp)
    if (dsp.lim)  sendLim(2, dsp.lim)
  }
}

// 모드 변경 진입점. mono→stereo 시 ch2를 ch1로 미러링하고 엔진 재명령.
// stereo→mono 시 값은 그대로 유지. 변경 사항을 i2sModeEvents 'changed'로 emit.
export function setI2sMode(direction, mode) {
  const result = _setI2sMode(direction, mode)
  if (!result.changed) return result
  if (result.to === 'stereo') applyStereoMirror(direction)
  else save()  // mono 전환은 state 저장만
  i2sModeEvents.emit('changed', result)
  return result
}

import { readFileSync, writeFileSync, existsSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import { getInLevel, getOutLevel,
         connect, sendGain, sendMute } from '../dsp/index.js'
import logger from '../logger.js'
import { getConfig } from '../config.js'
import {
  _assignInput, _assignOutput,
  getDspClientOf, getDspLocalId, getDspChannelCounts,
  inputSrcPorts, outputSinkPorts,
  makeInput, makeOutput, defaultInput, defaultOutput,
  bridgeLabel, bridgeDspClient, deepMerge,
} from './helpers.js'

export { getDspClientOf, getDspLocalId, getDspChannelCounts }

const __dirname  = dirname(fileURLToPath(import.meta.url))
const STATE_FILE = join(__dirname, '../../config/channels.json')

const config = getConfig()

const ANALOG_COUNT = config.jack.channels ?? 2

const baseName = config.jack.name ?? 'Analog'

const ravennaInputs = Array.from({ length: ANALOG_COUNT }, (_, i) => {
  const id = i + 1
  _assignInput(id, 'analog')
  return { ...defaultInput(id, `${baseName} ${id}`, `system:capture_${id}`), source: { type: 'analog', ch: i + 1 } }
})
const ravennaOutputs = Array.from({ length: ANALOG_COUNT }, (_, i) => {
  const id = i + 1
  _assignOutput(id, 'analog')
  return { ...defaultOutput(id, `${baseName} ${id}`, `system:playback_${id}`), source: { type: 'analog', ch: i + 1 } }
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

  const bIns = [], bOuts = []
  const srcType = b.aes67 ? 'aes67' : b.type === 'ravenna' ? 'ravenna' : b.type

  if (isInput) {
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextInId++
      _assignInput(id, dspClient)
      const srcPort = needsSuffix
        ? `${b.name}_in:capture_${ch}`
        : b.type === 'audio_in'
          ? `${b.name}:audio_in_${ch}`
          : `${b.name}:capture_${ch}`
      const chan = { ...makeInput(id, `${abbr} ${ch}`, srcPort), source: { type: srcType, name: b.name, ch } }
      bIns.push(chan)
      if (enabled) {
        inputSrcPorts.push({ id, srcPort: chan.srcPort, noRetry: isRavenna })
        bridgeInputs.push(chan)
      }
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
      const chan = { ...makeOutput(id, `${abbr} ${ch}`, sinkPort), source: { type: srcType, name: b.name, ch } }
      bOuts.push(chan)
      if (enabled) {
        outputSinkPorts.push({ id, sinkPort: chan.sinkPort, noRetry: isRavenna })
        bridgeOutputs.push(chan)
      }
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
  if (s.type === 'rtp_in') {
    _rtpChStart.set(`in:${s.client}`, nextInId - 1)
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextInId++
      _assignInput(id, 'stream')
      const chan = { ...makeInput(id, `${label} ${ch}`, `${s.client}:out_${ch}`), source: { type: 'rtp', name: s.client, ch } }
      if (enabled) { inputSrcPorts.push({ id, srcPort: chan.srcPort }); rtpInputs.push(chan) }
    }
  } else if (s.type === 'rtp_out') {
    _rtpChStart.set(`out:${s.client}`, nextOutId - 1)
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextOutId++
      _assignOutput(id, 'stream')
      const chan = { ...makeOutput(id, `${label} ${ch}`, `${s.client}:in_src_${ch}`), source: { type: 'rtp', name: s.client, ch } }
      if (enabled) { outputSinkPorts.push({ id, sinkPort: chan.sinkPort }); rtpOutputs.push(chan) }
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

if (existsSync(STATE_FILE)) {
  try {
    const saved = JSON.parse(readFileSync(STATE_FILE, 'utf8'))
    const byPort = (arr) => { const m = new Map(); for (const ch of (arr ?? [])) m.set(ch.port, ch); return m }
    savedInMap  = byPort(saved.inputs)
    savedOutMap = byPort(saved.outputs)
    state.inputs  = state.inputs.map(def  => { const { id, port, srcPort,  ...u } = savedInMap.get(def.port)  ?? {}; return deepMerge(def, u) })
    state.outputs = state.outputs.map(def => { const { id, port, sinkPort, ...u } = savedOutMap.get(def.port) ?? {}; return deepMerge(def, u) })
    const validSrcs = new Set(state.inputs.map(c => c.port))
    const validDsts = new Set(state.outputs.map(c => c.port))
    state.routing = (saved.routing ?? []).filter(r => validSrcs.has(r.src) && validDsts.has(r.dst))
  } catch (e) {
    logger.warn('[channels] load failed:', e.message)
  }
}

function save() {
  try { writeFileSync(STATE_FILE, JSON.stringify(state, null, 2)) }
  catch (e) { logger.warn('[channels] save failed:', e.message) }
}

export function getBridgeChannelDef(name) {
  return bridgeChannelDefs.get(name)
}

export function setBridgeEnabled(bridgeName, enabled) {
  const def = bridgeChannelDefs.get(bridgeName)
  if (!def) { logger.warn('[channels] bridge not found: %s', bridgeName); return }

  const inPorts  = new Set(def.inputs.map(c => c.port))
  const outPorts = new Set(def.outputs.map(c => c.port))

  state.inputs  = state.inputs.filter(c => !inPorts.has(c.port))
  state.outputs = state.outputs.filter(c => !outPorts.has(c.port))

  if (enabled) {
    const ins  = def.inputs.map(c  => { const { id, port, srcPort,  ...u } = savedInMap.get(c.port)  ?? {}; return deepMerge(c, u) })
    const outs = def.outputs.map(c => { const { id, port, sinkPort, ...u } = savedOutMap.get(c.port) ?? {}; return deepMerge(c, u) })
    state.inputs  = [...state.inputs,  ...ins ].sort((a, b) => a.id - b.id)
    state.outputs = [...state.outputs, ...outs].sort((a, b) => a.id - b.id)
  }

  logger.info('[channels] bridge %s %s', bridgeName, enabled ? 'enabled' : 'disabled')
}

export function getChannels() {
  const serialize = ({ id, label, port, gain, muted }, level) => ({ id, label, port, gain, muted, level })

  return {
    inputs:  state.inputs.map(ch => serialize(ch, ch.muted ? -100 : getInLevel(ch.port))),
    outputs: state.outputs.map(ch => {
      const level = ch.muted ? -100 : getOutLevel(`${getDspClientOf(ch.id, 'out')}:sin_${getDspLocalId(ch.id, 'out')}`)
      return serialize(ch, level)
    }),
  }
}

function find(type, id) {
  const list = type === 'input' ? state.inputs : state.outputs
  const ch = list.find(c => c.id === Number(id))
  if (!ch) throw new Error(`${type} channel ${id} not found`)
  return ch
}

export function setGain(type, id, gain) {
  find(type, id).gain = Math.max(0, Math.min(150, Number(gain)))
  save()
}

export function setMute(type, id, muted) {
  find(type, id).muted = Boolean(muted)
  save()
}

export function setLabel(type, id, label) {
  find(type, id).label = String(label).slice(0, 32)
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

export function getChannelsConfig() {
  const { inputs, outputs } = state
  return { inputs, outputs }
}

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
  const { inputs, outputs } = getChannels([]);
  for (const ch of inputs) {
    sendGain('in', ch.id, ch.gain);
    if (ch.muted) sendMute('in', ch.id, true);
  }
  for (const ch of outputs) {
    sendGain('out', ch.id, ch.gain);
    if (ch.muted) sendMute('out', ch.id, true);
  }
}

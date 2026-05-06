import { readFileSync, writeFileSync, existsSync, readdirSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import { getInLevel, getOutLevel,
         connect, sendGain, sendMute, sendBypass, sendAllDsp } from '../dsp/index.js'
import logger from '../logger.js'
import { getConfig } from '../config.js'
import {
  _assignInput, _assignOutput,
  getDspClientOf, getDspLocalId, getDspChannelCounts,
  inputSrcPorts, outputSinkPorts, getInputSrcPorts, getOutputSinkPorts,
  makeInput, makeOutput, defaultInput, defaultOutput,
  bridgeLabel, bridgeDspClient, deepMerge,
} from './helpers.js'

export { getDspClientOf, getDspLocalId, getDspChannelCounts, getInputSrcPorts, getOutputSinkPorts }

const __dirname  = dirname(fileURLToPath(import.meta.url))
const STATE_FILE = join(__dirname, '../../config/channels.json')

const config = getConfig()

export const ANALOG_COUNT = config.jack.channels ?? 2

const baseName = config.jack.name ?? 'Analog'

const ravennaInputs = Array.from({ length: ANALOG_COUNT }, (_, i) => {
  const id = i + 1
  _assignInput(id, 'analog')
  return defaultInput(id, `${baseName} ${id}`, `system:capture_${id}`)
})
const ravennaOutputs = Array.from({ length: ANALOG_COUNT }, (_, i) => {
  const id = i + 1
  _assignOutput(id, 'analog')
  return defaultOutput(id, `${baseName} ${id}`, `system:playback_${id}`)
})

const bridgeInputs      = []
const bridgeOutputs     = []
const bridgeChannelDefs = new Map()
let nextInId  = ANALOG_COUNT + 1
let nextOutId = ANALOG_COUNT + 1

function _isUdcConnected() {
  try {
    const entries = readdirSync('/sys/class/udc')
    if (!entries.length) return false
    return readFileSync(`/sys/class/udc/${entries[0]}/state`, 'utf8').trim() === 'configured'
  } catch { return false }
}

function addBridge(b) {
  const chCount   = b.channels ?? 2
  const abbr      = bridgeLabel(b.name)
  const enabled   = b.enabled !== false && (!b.usb_gadget || _isUdcConnected())
  const isRavenna   = b.type === 'ravenna' || b.aes67 === true
  const needsSuffix = b.type === 'ravenna'
  const isInput     = b.type === 'zita_in'  || b.type === 'audio_in'  || b.type === 'ravenna'
  const isOutput    = b.type === 'zita_out' || b.type === 'audio_out' || b.type === 'ravenna'
  const dspClient   = bridgeDspClient(b)

  const bIns = [], bOuts = []

  if (isInput) {
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextInId++
      _assignInput(id, dspClient)
      const srcPort = needsSuffix
        ? `${b.name}_in:capture_${ch}`
        : b.type === 'audio_in'
          ? `${b.name}:audio_in_${ch}`
          : `${b.name}:capture_${ch}`
      const chan = makeInput(id, `${abbr} ${ch}`, srcPort, true)
      bIns.push(chan)
      if (enabled) {
        inputSrcPorts.push({ id, srcPort: chan.srcPort, noRetry: b.usb_gadget === true || isRavenna, usbGadget: b.usb_gadget === true })
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
      const chan = makeOutput(id, `${abbr} ${ch}`, sinkPort, true)
      bOuts.push(chan)
      if (enabled) {
        outputSinkPorts.push({ id, sinkPort: chan.sinkPort, noRetry: b.usb_gadget === true || isRavenna, usbGadget: b.usb_gadget === true })
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
      const chan = makeInput(id, `${label} ${ch}`, `${s.client}:out_${ch}`, true)
      if (enabled) { inputSrcPorts.push({ id, srcPort: chan.srcPort }); rtpInputs.push(chan) }
    }
  } else if (s.type === 'rtp_out') {
    _rtpChStart.set(`out:${s.client}`, nextOutId - 1)
    for (let ch = 1; ch <= chCount; ch++) {
      const id = nextOutId++
      _assignOutput(id, 'stream')
      const chan = makeOutput(id, `${label} ${ch}`, `${s.client}:in_src_${ch}`, true)
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

export function getTotalInputCount()  { return nextInId  - 1 }
export function getTotalOutputCount() { return nextOutId - 1 }

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
    const byPort = (arr) => { const m = new Map(); for (const ch of (arr ?? [])) m.set(ch.jackPort, ch); return m }
    savedInMap  = byPort(saved.inputs)
    savedOutMap = byPort(saved.outputs)
    state.inputs  = state.inputs.map(def  => { const { id, jackPort, srcPort,  ...u } = savedInMap.get(def.jackPort)  ?? {}; return deepMerge(def, u) })
    state.outputs = state.outputs.map(def => { const { id, jackPort, sinkPort, ...u } = savedOutMap.get(def.jackPort) ?? {}; return deepMerge(def, u) })
    const validSrcs = new Set(state.inputs.map(c => c.jackPort))
    const validDsts = new Set(state.outputs.map(c => c.jackPort))
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

  const inPorts  = new Set(def.inputs.map(c => c.jackPort))
  const outPorts = new Set(def.outputs.map(c => c.jackPort))

  state.inputs  = state.inputs.filter(c => !inPorts.has(c.jackPort))
  state.outputs = state.outputs.filter(c => !outPorts.has(c.jackPort))

  if (enabled) {
    const ins  = def.inputs.map(c  => { const { id, jackPort, srcPort,  ...u } = savedInMap.get(c.jackPort)  ?? {}; return deepMerge(c, u) })
    const outs = def.outputs.map(c => { const { id, jackPort, sinkPort, ...u } = savedOutMap.get(c.jackPort) ?? {}; return deepMerge(c, u) })
    state.inputs  = [...state.inputs,  ...ins ].sort((a, b) => a.id - b.id)
    state.outputs = [...state.outputs, ...outs].sort((a, b) => a.id - b.id)
  }

  logger.info('[channels] bridge %s %s', bridgeName, enabled ? 'enabled' : 'disabled')
}

export function getChannels(connections = []) {
  const srcsByDst = new Map()
  for (const { port, connections: conns } of connections) {
    for (const dst of conns) {
      if (!srcsByDst.has(dst)) srcsByDst.set(dst, [])
      srcsByDst.get(dst).push(port)
    }
  }

  return {
    inputs:  state.inputs.map(ch => ({
      ...ch,
      level: ch.muted ? -100 : getInLevel(ch.jackPort),
    })),
    outputs: state.outputs.map(ch => {
      if (ch.muted) return { ...ch, level: -100 }
      const level = getOutLevel(`${getDspClientOf(ch.id, 'out')}:sin_${getDspLocalId(ch.id, 'out')}`)
      return { ...ch, level }
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

export function setHpf(id, params) {
  const ch = find('input', id)
  Object.assign(ch.dsp.hpf, params)
  save()
}

export function setEqEnabled(type, id, enabled) {
  const ch = find(type, id)
  ch.dsp.eqEnabled = Boolean(enabled)
  save()
}

export function setEqBand(type, id, bandIndex, params) {
  const ch = find(type, id)
  if (bandIndex < 0 || bandIndex >= ch.dsp.eq.length) throw new Error('invalid band index')
  Object.assign(ch.dsp.eq[bandIndex], params)
  save()
}

export function setLimiter(id, params) {
  const ch = find('output', id)
  if (!ch.dsp.limiter)
    ch.dsp.limiter = { enabled: false, threshold: -6, attack: 5, release: 100, makeup: 0 }
  Object.assign(ch.dsp.limiter, params)
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
  const validSrcs = new Set(activeIn.map(ch => ch.jackPort));
  const validDsts = new Set(activeOut.map(ch => ch.jackPort));
  logger.info('[startup] Restoring %d saved routes...', savedRoutes.length);
  for (const { src, dst } of savedRoutes) {
    if (!validSrcs.has(src) || !validDsts.has(dst)) continue;
    await connectWithRetry(src, dst);
  }
}

export function restoreDspState() {
  const { inputs, outputs } = getChannels([]);
  for (const ch of inputs) {
    if (ch.bypassDsp) sendBypass('in', ch.id, true);
    sendGain('in', ch.id, ch.gain);
    if (ch.muted) sendMute('in', ch.id, true);
  }
  for (const ch of outputs) {
    if (ch.bypassDsp) sendBypass('out', ch.id, true);
    sendGain('out', ch.id, ch.gain);
    if (ch.muted) sendMute('out', ch.id, true);
  }
  sendAllDsp({ inputs, outputs });
}

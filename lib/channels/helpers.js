import { registerChannelDsp } from '../dsp/index.js'

const _dspMap    = new Map()
const _dspInCnt  = new Map()
const _dspOutCnt = new Map()

export function _assignInput(id, client) {
  const localId = (_dspInCnt.get(client) ?? 0) + 1
  _dspInCnt.set(client, localId)
  _dspMap.set(`in:${id}`, { client, localId })
  registerChannelDsp(id, client, localId)
  return localId
}

export function _assignOutput(id, client) {
  const localId = (_dspOutCnt.get(client) ?? 0) + 1
  _dspOutCnt.set(client, localId)
  _dspMap.set(`out:${id}`, { client, localId })
  return localId
}

export function getDspClientOf(id, dir = 'in') {
  return _dspMap.get(`${dir}:${id}`)?.client ?? 'analog'
}

export function getDspLocalId(id, dir = 'in') {
  return _dspMap.get(`${dir}:${id}`)?.localId ?? id
}

export function getDspChannelCounts() {
  const result = new Map()
  for (const [client, n_in] of _dspInCnt)
    result.set(client, { n_in, n_out: _dspOutCnt.get(client) ?? 0 })
  for (const [client, n_out] of _dspOutCnt)
    if (!result.has(client)) result.set(client, { n_in: 0, n_out })
  return result
}

function inputJackPort(id)  { return `${getDspClientOf(id, 'in')}:out_${getDspLocalId(id, 'in')}` }
function outputJackPort(id) { return `${getDspClientOf(id, 'out')}:sin_${getDspLocalId(id, 'out')}` }

export const inputSrcPorts  = []
export const outputSinkPorts = []

export function makeInput(id, label, srcPort) {
  return { id, label, port: inputJackPort(id), srcPort, gain: 100, muted: false }
}

export function makeOutput(id, label, sinkPort) {
  return { id, label, port: outputJackPort(id), sinkPort, gain: 100, muted: false }
}

export function defaultInput(id, label, srcPort) {
  inputSrcPorts.push({ id, srcPort })
  return makeInput(id, label, srcPort)
}

export function defaultOutput(id, label, sinkPort) {
  outputSinkPorts.push({ id, sinkPort })
  return makeOutput(id, label, sinkPort)
}

export function bridgeLabel(name) {
  const base = name.replace(/_in$/, '').replace(/_out$/, '')
  const abbr = { hifiberry: 'HFB', sndrpihifiberry: 'HFB', uac2: 'UAC2' }
  return abbr[base.toLowerCase()] ?? base.toUpperCase()
}

export function bridgeDspClient(b) {
  if (b.aes67) return 'aes67'
  return b.name.toLowerCase().replace(/[^a-z0-9]/g, '_')
}

export function deepMerge(target, source) {
  if (!source || typeof source !== 'object') return target
  const out = { ...target }
  for (const [k, v] of Object.entries(source)) {
    if (v && typeof v === 'object' && !Array.isArray(v)) {
      out[k] = deepMerge(target[k] ?? {}, v)
    } else {
      out[k] = v
    }
  }
  return out
}

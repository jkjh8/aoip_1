import { EventEmitter } from 'events'

export const i2sModeEvents = new EventEmitter()

const VALID_MODES = new Set(['mono', 'stereo'])
const DIRECTIONS = ['input', 'output']

let _analogCount = 0
let _state = { input: 'mono', output: 'mono' }

export function _initI2sMode(analogCount, saved) {
  _analogCount = Number(analogCount) || 0
  const next = { input: 'mono', output: 'mono' }
  if (saved && typeof saved === 'object') {
    for (const d of DIRECTIONS) {
      if (VALID_MODES.has(saved[d])) next[d] = saved[d]
    }
  }
  if (_analogCount !== 2) {
    next.input = 'mono'
    next.output = 'mono'
  }
  _state = next
  return getI2sMode()
}

export function getI2sMode() {
  return { input: _state.input, output: _state.output }
}

export function isStereoLinked(type) {
  const dir = type === 'input' || type === 'in' ? 'input'
            : type === 'output' || type === 'out' ? 'output' : null
  if (!dir) return false
  if (_analogCount !== 2) return false
  return _state[dir] === 'stereo'
}

export function getPair(type, id) {
  const numId = Number(id)
  if (numId !== 1 && numId !== 2) return null
  if (!isStereoLinked(type)) return null
  return numId === 1 ? 2 : 1
}

export function setI2sMode(direction, mode) {
  if (!DIRECTIONS.includes(direction)) {
    throw new Error(`invalid direction: ${direction} (expected 'input' or 'output')`)
  }
  if (!VALID_MODES.has(mode)) {
    throw new Error(`invalid mode: ${mode} (expected 'mono' or 'stereo')`)
  }
  if (_analogCount !== 2 && mode === 'stereo') {
    throw new Error(`stereo mode requires exactly 2 analog channels (have ${_analogCount})`)
  }
  const prev = _state[direction]
  if (prev === mode) return { changed: false, mode: getI2sMode() }
  _state[direction] = mode
  return { changed: true, mode: getI2sMode(), direction, from: prev, to: mode }
}

import { sendToEngine, resolveChannel } from './engine.js'

export function sendGain(dir, globalId, sliderVal) {
  resolveChannel(globalId)
  let linear
  if (sliderVal <= 0) {
    linear = 0
  } else if (sliderVal <= 100) {
    const db = (sliderVal / 100) * 60 - 60
    linear = Math.pow(10, db / 20)
  } else {
    const db = ((sliderVal - 100) / 50) * 6
    linear = Math.pow(10, db / 20)
  }
  sendToEngine(`gain ${dir} ${globalId} ${Math.min(2.0, linear).toFixed(4)}`)
}

export function sendMute(dir, globalId, muted) {
  resolveChannel(globalId)
  sendToEngine(`mute ${dir} ${globalId} ${muted ? '1' : '0'}`)
}

export function sendBypass(dir, globalId, bypass) {
  resolveChannel(globalId)
  sendToEngine(`bypass ${dir} ${globalId} ${bypass ? '1' : '0'}`)
}

export function sendHpf(globalId, params) {
  resolveChannel(globalId)
  const dir = params.type === 'output' ? 'out' : 'in'
  if (params.slope   !== undefined) sendToEngine(`hpf ${dir} ${globalId} slope ${params.slope}`)
  if (params.freq    !== undefined) sendToEngine(`hpf ${dir} ${globalId} freq ${params.freq}`)
  if (params.enabled !== undefined) sendToEngine(`hpf ${dir} ${globalId} enable ${params.enabled ? '1' : '0'}`)
}


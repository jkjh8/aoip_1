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

const BAND_TYPE_MAP = {
  peak: 'peak', low_shelf: 'loshelf', high_shelf: 'hishelf', lp: 'lp', hp: 'hp',
}

export function sendEqBand(dir, globalId, bandIndex, params) {
  resolveChannel(globalId)
  const band = bandIndex + 1
  if (params.enabled  !== undefined) sendToEngine(`eq ${dir} ${globalId} ${band} enable ${params.enabled ? '1' : '0'}`)
  if (params.b0       !== undefined) sendToEngine(`eq ${dir} ${globalId} ${band} coeffs ${params.b0} ${params.b1} ${params.b2} ${params.a1} ${params.a2}`)
  if (params.freq     !== undefined) sendToEngine(`eq ${dir} ${globalId} ${band} freq ${params.freq}`)
  if (params.gain     !== undefined) sendToEngine(`eq ${dir} ${globalId} ${band} gain ${params.gain}`)
  if (params.q        !== undefined) sendToEngine(`eq ${dir} ${globalId} ${band} q ${params.q}`)
  if (params.bandType !== undefined) sendToEngine(`eq ${dir} ${globalId} ${band} type ${BAND_TYPE_MAP[params.bandType] ?? 'peak'}`)
}

export function sendLimiter(globalId, params) {
  resolveChannel(globalId)
  if (params.enabled   !== undefined) sendToEngine(`limiter out ${globalId} enable    ${params.enabled   ? '1' : '0'}`)
  if (params.threshold !== undefined) sendToEngine(`limiter out ${globalId} threshold ${params.threshold}`)
  if (params.attack    !== undefined) sendToEngine(`limiter out ${globalId} attack    ${params.attack}`)
  if (params.release   !== undefined) sendToEngine(`limiter out ${globalId} release   ${params.release}`)
  if (params.makeup    !== undefined) sendToEngine(`limiter out ${globalId} makeup    ${params.makeup}`)
}

export function sendAllDsp({ inputs, outputs }) {
  for (const ch of inputs) {
    if (ch.bypassDsp) { sendBypass('in', ch.id, true); continue }
    const { dsp } = ch
    if (dsp?.hpf) sendHpf(ch.id, dsp.hpf)
    if (dsp?.eq)  dsp.eq.forEach((b, i) => sendEqBand('in', ch.id, i, b))
  }
  for (const ch of outputs) {
    if (ch.bypassDsp) { sendBypass('out', ch.id, true); continue }
    const { dsp } = ch
    if (dsp?.eq)      dsp.eq.forEach((b, i) => sendEqBand('out', ch.id, i, b))
    if (dsp?.limiter) sendLimiter(ch.id, dsp.limiter)
  }
}

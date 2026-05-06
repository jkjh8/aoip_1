import { sendToEngine } from './engine.js'

const SR = 48000

/* ── biquad 계수 계산 (Audio EQ Cookbook, RBJ) ───────────────────── */

function calcPeak(fc, q, gainDb, sr = SR) {
  const A    = Math.pow(10, gainDb / 40)
  const w0   = 2 * Math.PI * fc / sr
  const sin0 = Math.sin(w0)
  const cos0 = Math.cos(w0)
  const alpha = sin0 / (2 * q)
  const a0 = 1 + alpha / A
  return {
    b0: (1 + alpha * A) / a0,
    b1: (-2 * cos0) / a0,
    b2: (1 - alpha * A) / a0,
    a1: (-2 * cos0) / a0,
    a2: (1 - alpha / A) / a0,
  }
}

function calcLowShelf(fc, q, gainDb, sr = SR) {
  void q
  const A    = Math.pow(10, gainDb / 40)
  const w0   = 2 * Math.PI * fc / sr
  const sin0 = Math.sin(w0)
  const cos0 = Math.cos(w0)
  const alpha = sin0 / 2 * Math.sqrt(2)   /* S=1 */
  const sqA  = Math.sqrt(A)
  const a0   = (A+1) + (A-1)*cos0 + 2*sqA*alpha
  return {
    b0: (A * ((A+1) - (A-1)*cos0 + 2*sqA*alpha)) / a0,
    b1: (2 * A * ((A-1) - (A+1)*cos0)) / a0,
    b2: (A * ((A+1) - (A-1)*cos0 - 2*sqA*alpha)) / a0,
    a1: (-2 * ((A-1) + (A+1)*cos0)) / a0,
    a2: ((A+1) + (A-1)*cos0 - 2*sqA*alpha) / a0,
  }
}

function calcHighShelf(fc, q, gainDb, sr = SR) {
  void q
  const A    = Math.pow(10, gainDb / 40)
  const w0   = 2 * Math.PI * fc / sr
  const sin0 = Math.sin(w0)
  const cos0 = Math.cos(w0)
  const alpha = sin0 / 2 * Math.sqrt(2)
  const sqA  = Math.sqrt(A)
  const a0   = (A+1) - (A-1)*cos0 + 2*sqA*alpha
  return {
    b0: (A * ((A+1) + (A-1)*cos0 + 2*sqA*alpha)) / a0,
    b1: (-2 * A * ((A-1) + (A+1)*cos0)) / a0,
    b2: (A * ((A+1) + (A-1)*cos0 - 2*sqA*alpha)) / a0,
    a1: (2 * ((A-1) - (A+1)*cos0)) / a0,
    a2: ((A+1) - (A-1)*cos0 - 2*sqA*alpha) / a0,
  }
}

function calcNotch(fc, q, sr = SR) {
  const w0   = 2 * Math.PI * fc / sr
  const sin0 = Math.sin(w0)
  const cos0 = Math.cos(w0)
  const alpha = sin0 / (2 * q)
  const a0   = 1 + alpha
  return {
    b0: 1 / a0,
    b1: (-2 * cos0) / a0,
    b2: 1 / a0,
    a1: (-2 * cos0) / a0,
    a2: (1 - alpha) / a0,
  }
}

function calcHpfSection(fc, q, sr = SR) {
  const w0   = 2 * Math.PI * fc / sr
  const sin0 = Math.sin(w0)
  const cos0 = Math.cos(w0)
  const alpha = sin0 / (2 * q)
  const a0   = 1 + alpha
  return {
    b0: (1 + cos0) / 2 / a0,
    b1: -(1 + cos0) / a0,
    b2: (1 + cos0) / 2 / a0,
    a1: (-2 * cos0) / a0,
    a2: (1 - alpha) / a0,
  }
}

export function calcBiquad(type, fc, q, gainDb, sr = SR) {
  switch (type) {
    case 'peak':       return calcPeak(fc, q, gainDb, sr)
    case 'low_shelf':  return calcLowShelf(fc, q, gainDb, sr)
    case 'high_shelf': return calcHighShelf(fc, q, gainDb, sr)
    case 'notch':      return calcNotch(fc, q, sr)
    default:           return calcPeak(fc, q, gainDb ?? 0, sr)
  }
}

/* ── Butterworth HPF Q 값 ────────────────────────────────────────── */
const BUTTER_Q = {
  12: [0.7071],
  24: [0.5412, 1.3066],
  48: [0.5089, 0.6013, 0.8999, 2.5628],
}

function fmt8(v) { return v.toFixed(8) }

/* ── 엔진 명령 전송 함수 ─────────────────────────────────────────── */

export function sendTrim(dir, id, db) {
  const clamped = Math.max(-20, Math.min(20, Number(db) || 0))
  sendToEngine(`trim ${dir} ${id} ${clamped.toFixed(2)}`)
}

export function sendHpf(dir, id, { slope = 12, fc = 80, enabled = true } = {}) {
  if (!enabled) {
    sendToEngine(`hpf ${dir} ${id} disable`)
    return
  }
  /* C cmd_loop이 slope + fc를 받아 Butterworth 계수를 직접 계산 */
  sendToEngine(`hpf ${dir} ${id} set slope ${slope} fc ${Number(fc).toFixed(2)}`)
}

export function sendEqBand(dir, id, band, { type = 'peak', fc = 1000, q = 0.707, gainDb = 0, enabled = true } = {}) {
  const b1 = Number(band)  /* 1-based */
  if (b1 < 1 || b1 > 4) return
  if (!enabled) {
    sendToEngine(`eq ${dir} ${id} band ${b1} disable`)
    return
  }
  const c = calcBiquad(type, Number(fc), Number(q), Number(gainDb))
  sendToEngine(`eq ${dir} ${id} band ${b1} coef ${fmt8(c.b0)} ${fmt8(c.b1)} ${fmt8(c.b2)} ${fmt8(c.a1)} ${fmt8(c.a2)}`)
}

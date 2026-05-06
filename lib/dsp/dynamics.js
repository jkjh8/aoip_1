import { sendToEngine } from './engine.js'

export function sendGate(dir, id, { threshold = -40, attackMs = 5, releaseMs = 100, holdMs = 50, rangeDb = -80, enabled = true } = {}) {
  if (!enabled) { sendToEngine(`gate ${dir} ${id} disable`); return }
  sendToEngine(`gate ${dir} ${id} set thr ${Number(threshold).toFixed(2)} attack ${Number(attackMs).toFixed(2)} release ${Number(releaseMs).toFixed(2)} hold ${Number(holdMs).toFixed(2)} range ${Number(rangeDb).toFixed(2)}`)
}

export function sendComp(dir, id, { threshold = -20, ratio = 4, knee = 6, attackMs = 10, releaseMs = 100, makeupDb = 0, enabled = true } = {}) {
  if (!enabled) { sendToEngine(`comp ${dir} ${id} disable`); return }
  sendToEngine(`comp ${dir} ${id} set thr ${Number(threshold).toFixed(2)} ratio ${Number(ratio).toFixed(3)} knee ${Number(knee).toFixed(2)} attack ${Number(attackMs).toFixed(2)} release ${Number(releaseMs).toFixed(2)} makeup ${Number(makeupDb).toFixed(2)}`)
}

export function sendLim(id, { threshold = -0.5, releaseMs = 200, enabled = true } = {}) {
  if (!enabled) { sendToEngine(`lim out ${id} disable`); return }
  sendToEngine(`lim out ${id} set thr ${Number(threshold).toFixed(2)} release ${Number(releaseMs).toFixed(2)}`)
}

export function sendGrEnable(enabled) {
  sendToEngine(enabled ? 'gr enable' : 'gr disable')
}

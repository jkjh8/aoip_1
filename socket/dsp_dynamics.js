import { sendGate, sendComp, sendLim, sendGrEnable } from '../lib/dsp/dynamics.js'
import { setChannelDsp } from '../lib/channels/index.js'
import logger from '../lib/logger.js'

export default function register(socket, ctx) {
  const _t = new Map()
  const THROTTLE_MS = 40

  function throttle(key, cb, fn) {
    const now = Date.now()
    const prev = _t.get(key)
    if (!prev) {
      fn()
      cb?.({ ok: true })
      _t.set(key, { last: now, timer: null, pending: null, pendingCb: null })
      return
    }
    const elapsed = now - prev.last
    if (elapsed >= THROTTLE_MS && !prev.timer) {
      fn()
      cb?.({ ok: true })
      prev.last = now
      return
    }
    if (prev.pendingCb) prev.pendingCb({ ok: true })
    prev.pending = fn
    prev.pendingCb = cb
    if (!prev.timer) {
      prev.timer = setTimeout(() => {
        const entry = _t.get(key)
        if (!entry) return
        const pf = entry.pending; const pcb = entry.pendingCb
        entry.timer = null; entry.pending = null; entry.pendingCb = null
        entry.last = Date.now()
        if (pf) { try { pf() } catch (e) { logger.warn('[dsp throttle] %s', e.message) } }
        pcb?.({ ok: true })
      }, THROTTLE_MS - elapsed)
    }
  }

  socket.on('disconnect', () => {
    for (const v of _t.values()) if (v.timer) clearTimeout(v.timer)
    _t.clear()
  })

  socket.on('dsp:gate', ({ type, id, params = {} } = {}, cb) => {
    ctx?.markDspBusy()
    throttle(`gate:${type}:${id}`, cb, () => {
      try {
        const dir = type === 'input' ? 'in' : 'out'
        sendGate(dir, id, params)
        setChannelDsp(type, id, 'gate', params)
        logger.info('[dsp:gate] %s ch%d  %o', type, id, params)
      } catch (e) { logger.warn('[dsp:gate] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })

  socket.on('dsp:comp', ({ type, id, params = {} } = {}, cb) => {
    ctx?.markDspBusy()
    throttle(`comp:${type}:${id}`, cb, () => {
      try {
        const dir = type === 'input' ? 'in' : 'out'
        sendComp(dir, id, params)
        setChannelDsp(type, id, 'comp', params)
        logger.info('[dsp:comp] %s ch%d  %o', type, id, params)
      } catch (e) { logger.warn('[dsp:comp] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })

  socket.on('dsp:lim', ({ id, params = {} } = {}, cb) => {
    ctx?.markDspBusy()
    throttle(`lim:${id}`, cb, () => {
      try {
        sendLim(id, params)
        setChannelDsp('output', id, 'lim', params)
        logger.info('[dsp:lim] output ch%d  %o', id, params)
      } catch (e) { logger.warn('[dsp:lim] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })

  socket.on('dsp:gr:enable', (cb) => {
    try { sendGrEnable(true); logger.info('[dsp:gr] enabled'); cb?.({ ok: true }) }
    catch (e) { cb?.({ ok: false, error: e.message }) }
  })

  socket.on('dsp:gr:disable', (cb) => {
    try { sendGrEnable(false); logger.info('[dsp:gr] disabled'); cb?.({ ok: true }) }
    catch (e) { cb?.({ ok: false, error: e.message }) }
  })
}

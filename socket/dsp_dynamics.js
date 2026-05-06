import { sendGate, sendComp, sendLim, sendGrEnable } from '../lib/dsp/dynamics.js'
import { setChannelDsp } from '../lib/channels/index.js'
import logger from '../lib/logger.js'

export default function register(socket, ctx) {
  const _t = new Map()

  function debounce(key, cb, fn) {
    const prev = _t.get(key)
    if (prev) { clearTimeout(prev.timer); prev.cb?.({ ok: true }) }
    const timer = setTimeout(() => { _t.delete(key); fn() }, 40)
    _t.set(key, { timer, cb })
  }

  socket.on('disconnect', () => {
    for (const { timer } of _t.values()) clearTimeout(timer)
    _t.clear()
  })

  socket.on('dsp:gate', ({ type, id, params = {} } = {}, cb) => {
    ctx?.markDspBusy()
    debounce(`gate:${type}:${id}`, cb, () => {
      try {
        const dir = type === 'input' ? 'in' : 'out'
        sendGate(dir, id, params)
        setChannelDsp(type, id, 'gate', params)
        logger.info('[dsp:gate] %s ch%d  %o', type, id, params)
        cb?.({ ok: true })
      } catch (e) { logger.warn('[dsp:gate] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })

  socket.on('dsp:comp', ({ type, id, params = {} } = {}, cb) => {
    ctx?.markDspBusy()
    debounce(`comp:${type}:${id}`, cb, () => {
      try {
        const dir = type === 'input' ? 'in' : 'out'
        sendComp(dir, id, params)
        setChannelDsp(type, id, 'comp', params)
        logger.info('[dsp:comp] %s ch%d  %o', type, id, params)
        cb?.({ ok: true })
      } catch (e) { logger.warn('[dsp:comp] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })

  socket.on('dsp:lim', ({ id, params = {} } = {}, cb) => {
    ctx?.markDspBusy()
    debounce(`lim:${id}`, cb, () => {
      try {
        sendLim(id, params)
        setChannelDsp('output', id, 'lim', params)
        logger.info('[dsp:lim] output ch%d  %o', id, params)
        cb?.({ ok: true })
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

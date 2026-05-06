import { sendTrim, sendHpf, sendEqBand } from '../lib/dsp/eq.js'
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

  socket.on('dsp:trim', ({ type, id, db } = {}, cb) => {
    ctx?.markDspBusy()
    debounce(`trim:${type}:${id}`, cb, () => {
      try {
        const dir = type === 'input' ? 'in' : 'out'
        sendTrim(dir, id, db)
        setChannelDsp(type, id, 'trim', Number(db))
        logger.info('[dsp:trim] %s ch%d  db=%s', type, id, db)
        cb?.({ ok: true })
      } catch (e) { logger.warn('[dsp:trim] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })

  socket.on('dsp:hpf', ({ id, params = {} } = {}, cb) => {
    ctx?.markDspBusy()
    debounce(`hpf:${id}`, cb, () => {
      try {
        sendHpf('in', id, params)
        setChannelDsp('input', id, 'hpf', params)
        logger.info('[dsp:hpf] input ch%d  %o', id, params)
        cb?.({ ok: true })
      } catch (e) { logger.warn('[dsp:hpf] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })

  socket.on('dsp:eq:band', ({ type, id, band, params = {} } = {}, cb) => {
    ctx?.markDspBusy()
    debounce(`eq:${type}:${id}:${band}`, cb, () => {
      try {
        const dir = type === 'input' ? 'in' : 'out'
        sendEqBand(dir, id, band, params)
        setChannelDsp(type, id, 'eq', { band: Number(band), ...params })
        logger.info('[dsp:eq] %s ch%d band%d  %o', type, id, band, params)
        cb?.({ ok: true })
      } catch (e) { logger.warn('[dsp:eq] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })
}

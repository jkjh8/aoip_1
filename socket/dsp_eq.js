import { sendTrim, sendHpf, sendEqBand } from '../lib/dsp/eq.js'
import { setChannelDsp, getPair } from '../lib/channels/index.js'
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

  function emitChanged(type, id, key, params) {
    ctx?.io?.emit('dsp:changed', { type, id, key, params })
  }

  socket.on('dsp:trim', ({ type, id, db } = {}, cb) => {
    ctx?.markDspBusy()
    debounce(`trim:${type}:${id}`, cb, () => {
      try {
        const dir = type === 'input' ? 'in' : 'out'
        sendTrim(dir, id, db)
        const pair = getPair(type, id)
        if (pair) sendTrim(dir, pair, db)
        setChannelDsp(type, id, 'trim', Number(db))
        emitChanged(type, id, 'trim', Number(db))
        if (pair) emitChanged(type, pair, 'trim', Number(db))
        logger.info('[dsp:trim] %s ch%d%s  db=%s', type, id, pair ? `+ch${pair}` : '', db)
        cb?.({ ok: true })
      } catch (e) { logger.warn('[dsp:trim] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })

  socket.on('dsp:hpf', ({ id, params = {} } = {}, cb) => {
    ctx?.markDspBusy()
    debounce(`hpf:${id}`, cb, () => {
      try {
        sendHpf('in', id, params)
        const pair = getPair('input', id)
        if (pair) sendHpf('in', pair, params)
        setChannelDsp('input', id, 'hpf', params)
        emitChanged('input', id, 'hpf', params)
        if (pair) emitChanged('input', pair, 'hpf', params)
        logger.info('[dsp:hpf] input ch%d%s  %o', id, pair ? `+ch${pair}` : '', params)
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
        const pair = getPair(type, id)
        if (pair) sendEqBand(dir, pair, band, params)
        const eqParams = { band: Number(band), ...params }
        setChannelDsp(type, id, 'eq', eqParams)
        emitChanged(type, id, 'eq', eqParams)
        if (pair) emitChanged(type, pair, 'eq', eqParams)
        logger.info('[dsp:eq] %s ch%d%s band%d  %o', type, id, pair ? `+ch${pair}` : '', band, params)
        cb?.({ ok: true })
      } catch (e) { logger.warn('[dsp:eq] error: %s', e.message); cb?.({ ok: false, error: e.message }) }
    })
  })
}

import { getChannels, setHpf, setEqBand, setLimiter } from '../lib/channels.js';
import { isDspRunning, sendHpf, sendEqBand, sendLimiter } from '../lib/dsp.js';

export default function register(socket, { io, getCached, limiterWatchers }) {
  const hpfTimers  = new Map();
  const hpfPending = new Map();

  socket.on('dsp:hpf', ({ id, ...params } = {}, cb) => {
    try {
      setHpf(id, params);
      io.emit('channels', getChannels(getCached()));
      cb?.({ ok: true });

      if (isDspRunning()) {
        hpfPending.set(id, { id, params });
        clearTimeout(hpfTimers.get(id));
        hpfTimers.set(id, setTimeout(() => {
          hpfTimers.delete(id);
          const p = hpfPending.get(id);
          hpfPending.delete(id);
          if (p && isDspRunning()) sendHpf(p.id, p.params);
        }, 60));
      }
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  const eqTimers  = new Map();
  const eqPending = new Map();

  socket.on('dsp:eq', ({ type, id, band, ...params } = {}, cb) => {
    try {
      setEqBand(type, id, band, params);
      io.emit('channels', getChannels(getCached()));
      cb?.({ ok: true });

      if (isDspRunning()) {
        const key = `${type}:${id}:${band}`;
        eqPending.set(key, { type, id, band, params });
        clearTimeout(eqTimers.get(key));
        eqTimers.set(key, setTimeout(() => {
          eqTimers.delete(key);
          const p = eqPending.get(key);
          eqPending.delete(key);
          if (p && isDspRunning())
            sendEqBand(p.type === 'input' ? 'in' : 'out', p.id, p.band, p.params);
        }, 60));
      }
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });

  socket.on('limiter:watch', ({ id, watch } = {}) => {
    const chId = Number(id);
    if (!limiterWatchers.has(socket.id)) limiterWatchers.set(socket.id, new Set());
    const set = limiterWatchers.get(socket.id);
    watch ? set.add(chId) : set.delete(chId);
  });

  socket.on('dsp:limiter', ({ id, ...params } = {}, cb) => {
    try {
      setLimiter(id, params);
      if (isDspRunning()) sendLimiter(id, params);
      io.emit('channels', getChannels(getCached()));
      cb?.({ ok: true });
    } catch (e) { cb?.({ ok: false, error: e.message }); }
  });
}

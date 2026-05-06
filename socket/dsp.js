import { getChannels, setHpf } from '../lib/channels/index.js';
import { isDspRunning, sendHpf } from '../lib/dsp/index.js';

export default function register(socket, { io, getCached }) {
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
}

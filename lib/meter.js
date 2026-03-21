import { spawn } from 'child_process';
import { createInterface } from 'readline';
import { EventEmitter } from 'events';

export const meterBus = new EventEmitter();

/** portName → { proc, level: dBFS } */
const _meters = new Map();
/** 감시 중인 포트 목록 (stopAllMeters 시 초기화) */
const _watchedPorts = new Set();

// jack_meter -n -f 4 <port>
//   -n : numeric dB output
//   -f 4 : 4 updates/sec (여유로운 캡처 주기)
const FREQ = 4;

export function startAllMeters(portNames) {
  stopAllMeters();
  _watchedPorts.clear();
  for (const port of portNames) {
    _watchedPorts.add(port);
    startMeter(port);
  }
}

function startMeter(portName) {
  if (_meters.has(portName)) return;

  const proc = spawn('jack_meter', ['-n', '-f', String(FREQ), portName], {
    stdio: ['ignore', 'pipe', 'ignore']
  });

  const rl = createInterface({ input: proc.stdout });
  rl.on('line', (line) => {
    const raw = line.trim();
    const v = isFinite(Number(raw)) ? Number(raw) : -100;
    const entry = _meters.get(portName);
    if (entry) entry.level = Math.max(-100, Math.min(0, v));
    meterBus.emit('level', portName, v);
  });

  proc.on('error', (err) => {
    console.warn('[meter] %s: %s', portName, err.message);
    _meters.delete(portName);
  });
  proc.on('exit', () => {
    _meters.delete(portName);
    // 등록된 포트면 5초 후 재시도 (브릿지 재연결 대응)
    if (_watchedPorts.has(portName)) {
      setTimeout(() => {
        if (_watchedPorts.has(portName) && !_meters.has(portName)) startMeter(portName);
      }, 5000);
    }
  });

  _meters.set(portName, { proc, level: -100 });
}

export function stopAllMeters() {
  _watchedPorts.clear();
  for (const { proc } of _meters.values()) proc.kill('SIGTERM');
  _meters.clear();
}

export function getLevel(portName) {
  return _meters.get(portName)?.level ?? -100;
}

export function getAllLevels() {
  const result = {};
  for (const [port, { level }] of _meters) result[port] = level;
  return result;
}

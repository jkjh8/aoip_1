import net from 'net'
import { unlinkSync, readFileSync, mkdirSync } from 'fs'
import { spawn } from 'child_process'
import { fileURLToPath } from 'url'
import { dirname, resolve } from 'path'
import logger from '../logger.js'
import { handleGrLine } from './metering.js'

const __dirname = dirname(fileURLToPath(import.meta.url))
const ENGINE_BIN = resolve(__dirname, '../../scripts/bin/aoip_engine')

let _engineProcess = null

const SOCK_DIR  = '/run/aoip'
const SOCK_PATH = `${SOCK_DIR}/engine.sock`

let _socket    = null
let _server    = null
let _ready     = false
let _engineStartTime = null
let _stdoutBuf = ''

const _instances = new Map()
let   _totalIn   = 0
let   _totalOut  = 0
let   _launchTimer = null

let _pendingRestart = false
let _startupDone   = false
let _shuttingDown  = false
const _engineRestartListeners = []
export function addEngineRestartListener(fn) { _engineRestartListeners.push(fn) }
export function markStartupDone() { _startupDone = true }

const _readyCbs = new Map()

const _channelDsp    = new Map()
const _inChToPort    = new Map()
const _inPortToGlob  = new Map()
const _outChToPort   = new Map()
const _outPortToGlob = new Map()

const _inLevel   = new Map()
const _outLevel  = new Map()
const _rtpBufStats = new Map()   // key → { fillMs, targetMs, pct, underrun }

export function registerChannelDsp(id, name, localId, dir = 'in') {
  /* in/out 의 client name 이 다를 수 있어 (USB: usb_audio_in vs usb_audio_out)
   * 방향에 맞는 맵만 갱신한다. resolveChannel 은 마지막으로 등록된 항목을 보존하되
   * 양쪽 모두 호출돼야 in/out port 모두 라우팅/레벨미터에서 조회 가능. */
  if (!_channelDsp.has(id)) _channelDsp.set(id, { name, localId })
  if (dir === 'out') {
    const outPort = `${name}:sin_${localId}`
    _outChToPort.set(id, outPort)
    _outPortToGlob.set(outPort, id)
  } else {
    const port = `${name}:out_${localId}`
    _inChToPort.set(id, port)
    _inPortToGlob.set(port, id)
  }
}

export function resolveChannel(globalId) {
  const entry = _channelDsp.get(globalId)
  if (!entry) throw new Error(`no DSP registered for channel ${globalId}`)
  return entry
}

function _inGlobToPort(ch)   { return _inChToPort.get(ch) }
function _outGlobToPort(ch)  { return _outChToPort.get(ch) }
function _inPortToGlobId(p)  { return _inPortToGlob.get(p) }
function _outPortToGlobId(p) { return _outPortToGlob.get(p) }

function _write(line) {
  if (_socket && !_socket.destroyed) _socket.write(line + '\n')
}

export function sendToEngine(line) { _write(line) }

export function connect(src, dst) {
  const inGlob  = _inPortToGlobId(src)
  const outGlob = _outPortToGlobId(dst)
  if (inGlob == null || outGlob == null) return
  _write(`route add ${inGlob} ${outGlob}`)
  logger.debug('[dsp] route add %d→%d  (%s→%s)', inGlob, outGlob, src, dst)
}

export function disconnect(src, dst) {
  const inGlob  = _inPortToGlobId(src)
  const outGlob = _outPortToGlobId(dst)
  if (inGlob == null || outGlob == null) return
  _write(`route remove ${inGlob} ${outGlob}`)
  logger.debug('[dsp] route remove %d→%d  (%s→%s)', inGlob, outGlob, src, dst)
}

function _parseLine(line) {
  const parts = line.split(' ')

  if (parts[0] === 'lvl' && parts[1] === 'in' && parts.length >= 4) {
    const port = _inGlobToPort(parseInt(parts[2], 10))
    if (port) _inLevel.set(port, parseFloat(parts[3]))
    return
  }

  if (parts[0] === 'lvl' && parts[1] === 'out' && parts.length >= 4) {
    const port = _outGlobToPort(parseInt(parts[2], 10))
    if (port) _outLevel.set(port, parseFloat(parts[3]))
    return
  }

  if (line.includes('[aoip_engine]') && line.includes('ready')) {
    logger.info('[dsp] aoip_engine ready')
    _ready = true
    for (const cbs of _readyCbs.values()) cbs.forEach(fn => fn())
    _readyCbs.clear()
    if (_pendingRestart || _startupDone) {
      _pendingRestart = false
      for (const fn of _engineRestartListeners) fn()
    }
    return
  }

  if (line.startsWith('bridge:')) { logger.debug('[dsp] %s', line); return }

  if (parts[0] === 'rtp_buf' && parts.length >= 3) {
    const key = parts[1]
    const kv  = Object.fromEntries(parts.slice(2).map(p => p.split('=')))
    _rtpBufStats.set(key, {
      fillMs:        parseInt(kv.fillMs        ?? '0', 10),
      targetMs:      parseInt(kv.targetMs      ?? '0', 10),
      pct:           parseInt(kv.pct           ?? '0', 10),
      underrun:      parseInt(kv.underrun      ?? '0', 10),
      underrunTotal: parseInt(kv.underrunTotal ?? '0', 10),
      overrunTotal:  parseInt(kv.overrunTotal  ?? '0', 10),
    })
    return
  }

  if (parts[0] === 'gr') {
    handleGrLine(parts)
    return
  }

  if (parts[0] === 'clk2') {
    const kv        = Object.fromEntries(parts.slice(1).map(p => p.split('=')))
    const aoip      = parseFloat(kv.aoip_rate ?? '0')
    const rav       = parseFloat(kv.ravenna_rate ?? '0')
    const ppm       = parseFloat(kv.drift_ppm ?? '0')
    const elapsed   = parseFloat(kv.elapsed ?? '0')
    const period    = parseInt(kv.dsp_period ?? '0', 10)
    const tickMs    = parseFloat(kv.dsp_tick_ms ?? '0')
    const sign = (v) => v >= 0 ? '+' : ''
    logger.debug(`[dsp] hw:aoip↔RAVENNA: aoip=${aoip.toFixed(3)}Hz  ravenna=${rav.toFixed(3)}Hz  drift=${sign(ppm)}${ppm.toFixed(3)}ppm  over ${elapsed.toFixed(0)}s  dsp=${period}fr(${tickMs.toFixed(3)}ms)`)
    return
  }

  logger.debug('[dsp] %s', line)
}

function _startServer() {
  if (_server) return

  try { unlinkSync(SOCK_PATH) } catch { /* file may not exist */ }

  _server = net.createServer((socket) => {
    if (_socket && !_socket.destroyed) {
      logger.warn('[dsp] new engine connection while existing one active — closing old')
      _socket.destroy()
    }
    _socket = socket
    _stdoutBuf = ''
    _engineStartTime = Date.now()

    socket.write(`init n_in=${_totalIn} n_out=${_totalOut}\n`)

    socket.on('data', (d) => {
      _stdoutBuf += d.toString()
      let nl
      while ((nl = _stdoutBuf.indexOf('\n')) >= 0) {
        const line = _stdoutBuf.slice(0, nl).trim()
        _stdoutBuf = _stdoutBuf.slice(nl + 1)
        if (line) _parseLine(line)
      }
    })

    socket.on('close', () => {
      logger.info('[dsp] aoip_engine disconnected')
      _ready = false
      _pendingRestart = true
      _socket = null
      _engineStartTime = null
    })

    socket.on('error', (err) => {
      if (err.code !== 'ECONNRESET') logger.error('[dsp] engine socket error: %s', err.message)
    })
  })

  _server.on('error', (err) => {
    logger.error('[dsp] engine server error: %s', err.message)
  })

  _server.listen(SOCK_PATH, () => {
    logger.info('[dsp] engine socket listening on %s', SOCK_PATH)
  })
}

function _launch() {
  _launchTimer = null
  if (_shuttingDown) return
  if (_socket && !_socket.destroyed) return

  _ready = false

  try { mkdirSync('/run/aoip', { recursive: true }) } catch { /* ignore */ }
  for (const f of ['/dev/shm/rtp_in_1', '/dev/shm/rtp_out_1']) {
    try { unlinkSync(f) } catch { /* ignore */ }
  }

  _startServer()

  const child = spawn('chrt', ['-f', '69', 'taskset', '-c', '2,3', ENGINE_BIN, '--name', 'main'], {
    cwd: resolve(__dirname, '../..'),
    stdio: ['ignore', 'ignore', 'pipe'],
  })

  let _errBuf = ''
  child.stderr.on('data', (d) => {
    _errBuf += d.toString()
    let nl
    while ((nl = _errBuf.indexOf('\n')) >= 0) {
      const line = _errBuf.slice(0, nl).trim()
      _errBuf = _errBuf.slice(nl + 1)
      if (!line) continue
      // 주기적 통계 로그는 debug 레벨로 강등 (debug 모드에서만 표시)
      const isPeriodic = line.includes('dsp burst stat') || line.includes('clk2: stabilized')
      if (isPeriodic) logger.debug('[engine] %s', line)
      else            logger.info('[engine] %s', line)
    }
  })

  child.on('error', (err) => {
    logger.error('[dsp] failed to spawn aoip_engine: %s', err.message)
    _engineProcess = null
    _launchTimer = setTimeout(_launch, 3000)
  })

  child.on('exit', (code, signal) => {
    _engineProcess = null
    if (_shuttingDown) {
      logger.info('[dsp] aoip_engine exited (code=%s signal=%s) — shutdown', code, signal)
      return
    }
    logger.warn('[dsp] aoip_engine exited (code=%s signal=%s) — restarting in 3s...', code, signal)
    _launchTimer = setTimeout(_launch, 3000)
  })

  _engineProcess = child
  logger.info('[dsp] aoip_engine spawned (pid=%d in=%d out=%d)', child.pid, _totalIn, _totalOut)
}

export function waitForDspReady(name, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    if (_ready) return resolve()
    const t = setTimeout(() => {
      const cbs = _readyCbs.get(name) ?? []
      _readyCbs.set(name, cbs.filter(f => f !== cb))
      reject(new Error(`dsp ${name} ready timeout`))
    }, timeoutMs)
    const cb = () => { clearTimeout(t); resolve() }
    const cbs = _readyCbs.get(name) ?? []
    cbs.push(cb)
    _readyCbs.set(name, cbs)
  })
}

export function startDsp(name, n_in, n_out) {
  const prev = _instances.get(name)
  _totalIn  = _totalIn  - (prev?.n_in  ?? 0) + n_in
  _totalOut = _totalOut - (prev?.n_out ?? 0) + n_out
  _instances.set(name, { n_in, n_out })
  if (!_launchTimer && !_socket) _launchTimer = setTimeout(_launch, 0)
}

export function isDspRunning(name) {
  if (!_socket || _socket.destroyed) return false
  if (name) return _instances.has(name)
  return true
}

export function getDspUptime() {
  return _engineStartTime ? Math.floor((Date.now() - _engineStartTime) / 1000) : null
}

export function shutdownDsp(timeoutMs = 3000) {
  _shuttingDown = true
  if (_launchTimer) { clearTimeout(_launchTimer); _launchTimer = null }
  const child = _engineProcess
  try { _socket?.destroy() } catch { /* ignore */ }
  try { _server?.close() } catch { /* ignore */ }
  if (!child || child.exitCode != null || child.signalCode != null) {
    return Promise.resolve()
  }
  // SIGTERM만 보내고 종료 대기. timeoutMs는 systemd TimeoutStopSec보다 짧게 잡아
  // node가 먼저 exit하지 않도록 함 — 강제 종료는 systemd가 처리.
  return new Promise((res) => {
    const done = () => { clearTimeout(t); res() }
    const t = setTimeout(res, timeoutMs)
    child.once('exit', done)
    try { child.kill('SIGTERM') } catch { clearTimeout(t); res() }
  })
}

export function waitForEngineReady(timeoutMs = 5000) {
  return waitForDspReady('_engine', timeoutMs)
}

export function getInLevel(port)    { return _inLevel.get(port)  ?? -120 }
export function getOutLevel(port)   { return _outLevel.get(port) ?? -120 }
export function getRtpBufStats(key)     { return _rtpBufStats.get(key)   ?? null }

export function registerAnalogBridge(config) {
  const jackCfg = config.jack;
  if (!jackCfg?.device) return;
  const ch  = jackCfg.channels ?? 2;
  const cmd = `bridge add analog ${jackCfg.device} ${jackCfg.rate ?? 48000} ${jackCfg.period ?? 512} ${jackCfg.periods ?? 3} ${ch} 0`;
  logger.info('[startup] Registering analog bridge: %s', cmd);
  sendToEngine(cmd);
}

function _waitForAlsaCard(device, maxMs = 30000) {
  const cardId = device.replace(/^hw:/, '')
  return new Promise((resolve) => {
    const start = Date.now()
    let logged = false
    const check = () => {
      try {
        const cards = readFileSync('/proc/asound/cards', 'utf8')
        if (cards.includes(cardId)) {
          // Card listed — verify PCM capture+playback devices are also ready
          const lines = cards.split('\n')
          let cardNum = null
          for (const line of lines) {
            if (line.includes(cardId)) {
              const m = line.match(/^\s*(\d+)/)
              if (m) { cardNum = m[1]; break }
            }
          }
          if (cardNum !== null) {
            try {
              readFileSync(`/proc/asound/card${cardNum}/pcm0c/info`, 'utf8')
              readFileSync(`/proc/asound/card${cardNum}/pcm0p/info`, 'utf8')
              logger.info('[startup] ALSA %s (card%s) PCM devices ready after %dms', device, cardNum, Date.now() - start)
              return resolve(true)
            } catch { /* PCM subdevices not yet available */ }
          }
        }
      } catch { /* ignore */ }
      if (!logged) { logger.info('[startup] waiting for ALSA card %s...', device); logged = true }
      if (Date.now() - start >= maxMs) { logger.warn('[startup] ALSA card %s not ready after %dms — continuing anyway', device, maxMs); return resolve(false) }
      setTimeout(check, 500)
    }
    check()
  })
}

export async function startupDsp(dspCounts, config) {
  logger.info('[startup] Starting aoip_engine: %s',
    [...dspCounts.entries()].map(([n, c]) => `${n}(${c.n_in}in/${c.n_out}out)`).join(', '));
  for (const [name, { n_in, n_out }] of dspCounts) {
    try { startDsp(name, n_in, n_out); } catch (e) { logger.warn('[startup] dsp %s:', name, e.message); }
  }
  try { await Promise.all([...dspCounts.keys()].map(name => waitForDspReady(name))); }
  catch (e) { logger.warn('[startup] aoip_engine ready timeout: %s', e.message); }
  if (config.engine?.periodFrames != null) {
    const pf = Math.trunc(config.engine.periodFrames)
    logger.info('[startup] Setting engine period_frames to %d', pf)
    sendToEngine(`set period ${pf}`)
  }
  if (config.jack?.device) await _waitForAlsaCard(config.jack.device)
  registerAnalogBridge(config);
}

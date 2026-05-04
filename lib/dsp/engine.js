import { spawn } from 'child_process'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import logger from '../logger.js'
import { getConfig } from '../config.js'

const __dirname = dirname(fileURLToPath(import.meta.url))
const BINARY    = join(__dirname, '../../scripts/aoip_engine')

let _proc      = null
let _ready     = false
let _stdoutBuf = ''

const _instances = new Map()
let   _totalIn   = 0
let   _totalOut  = 0
let   _launchTimer = null

let _pendingRestart = false
const _engineRestartListeners = []
export function addEngineRestartListener(fn) { _engineRestartListeners.push(fn) }

const _readyCbs = new Map()

const _channelDsp    = new Map()
const _inChToPort    = new Map()
const _inPortToGlob  = new Map()
const _outChToPort   = new Map()
const _outPortToGlob = new Map()

const _inLevel  = new Map()
const _outLevel = new Map()
const _limMeter = new Map()

export function registerChannelDsp(id, name, localId) {
  _channelDsp.set(id, { name, localId })
  const port = `${name}:out_${localId}`
  _inChToPort.set(id, port)
  _inPortToGlob.set(port, id)
  const outPort = `${name}:sin_${localId}`
  _outChToPort.set(id, outPort)
  _outPortToGlob.set(outPort, id)
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
  if (_proc?.stdin?.writable) _proc.stdin.write(line + '\n')
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

  if (parts[0] === 'lm' && parts[1] === 'out' && parts.length >= 5) {
    _limMeter.set(`out ${parseInt(parts[2], 10)}`, { pre: parseFloat(parts[3]), post: parseFloat(parts[4]) })
    return
  }

  if (line.includes('[aoip_engine]') && line.includes('ready')) {
    logger.info('[dsp] aoip_engine ready')
    _ready = true
    for (const cbs of _readyCbs.values()) cbs.forEach(fn => fn())
    _readyCbs.clear()
    if (_pendingRestart) { _pendingRestart = false; for (const fn of _engineRestartListeners) fn() }
    return
  }

  if (line.startsWith('bridge:')) { logger.debug('[dsp] %s', line); return }

  logger.debug('[dsp] %s', line)
}

function _launch() {
  _launchTimer = null
  if (_proc) return

  _ready = false
  _stdoutBuf = ''

  const engineCfg   = getConfig().engine ?? {}
  const dspPrio     = engineCfg.dspPrio     ?? 92
  const alsaPrio    = engineCfg.alsaPrio     ?? 80
  const ravennaPrio = engineCfg.ravennaPrio  ?? 95
  const bypassDsp   = engineCfg.bypassDsp    ?? false

  const args = [
    String(_totalIn), String(_totalOut), '--name', 'main',
    '--dsp-prio',     String(dspPrio),
    '--alsa-prio',    String(alsaPrio),
    '--ravenna-prio', String(ravennaPrio),
  ]
  if (bypassDsp) args.push('--bypass-dsp')

  const proc = spawn(
    'chrt', ['-f', String(dspPrio), BINARY, ...args],
    { stdio: ['pipe', 'pipe', 'pipe'], detached: false }
  )
  _proc = proc

  proc.stdout.on('data', d => {
    _stdoutBuf += d.toString()
    let nl
    while ((nl = _stdoutBuf.indexOf('\n')) >= 0) {
      const line = _stdoutBuf.slice(0, nl).trim()
      _stdoutBuf = _stdoutBuf.slice(nl + 1)
      if (line) _parseLine(line)
    }
  })

  proc.stderr.on('data', d => {
    const s = d.toString()
    logger.debug('[aoip_engine] %s', s.trimEnd())
    if (s.includes('[aoip_engine]') && s.includes('ready') && !_ready) {
      _ready = true
      for (const cbs of _readyCbs.values()) cbs.forEach(fn => fn())
      _readyCbs.clear()
      if (_pendingRestart) { _pendingRestart = false; for (const fn of _engineRestartListeners) fn() }
    }
  })

  proc.on('exit', (code, signal) => {
    logger.info('[dsp] aoip_engine exited code=%s signal=%s', code, signal)
    _proc  = null
    _ready = false
    if (signal === 'SIGTERM') return
    _pendingRestart = true
    logger.info('[dsp] aoip_engine restarting in 3s...')
    _launchTimer = setTimeout(_launch, 3000)
  })

  proc.on('error', err => { logger.error('[dsp] aoip_engine error: %s', err.message); _proc = null })

  logger.info('[dsp] started aoip_engine (in=%d out=%d)', _totalIn, _totalOut)
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
  if (!_launchTimer && !_proc) _launchTimer = setTimeout(_launch, 0)
}

export function stopDsp(name) {
  if (!name) {
    _proc?.kill('SIGTERM')
    _proc  = null
    _ready = false
    _instances.clear()
    _totalIn = _totalOut = 0
  }
}

export function isDspRunning(name) {
  if (!_proc || _proc.killed || _proc.exitCode !== null) return false
  if (name) return _instances.has(name)
  return true
}

export function waitForEngineReady(timeoutMs = 5000) {
  return waitForDspReady('_engine', timeoutMs)
}

export function getInLevel(jackPort)    { return _inLevel.get(jackPort)  ?? -120 }
export function getOutLevel(jackPort)   { return _outLevel.get(jackPort) ?? -120 }
export function getLimiterMeters()      { return _limMeter }

export function registerAnalogBridge(config) {
  const jackCfg = config.jack;
  if (!jackCfg?.device) return;
  const ch  = jackCfg.channels ?? 2;
  const cmd = `bridge add analog ${jackCfg.device} ${jackCfg.rate ?? 48000} ${jackCfg.period ?? 512} ${jackCfg.periods ?? 3} ${ch} 0`;
  logger.info('[startup] Registering analog bridge: %s', cmd);
  sendToEngine(cmd);
}

export async function startupDsp(dspCounts, config) {
  logger.info('[startup] Starting aoip_engine: %s',
    [...dspCounts.entries()].map(([n, c]) => `${n}(${c.n_in}in/${c.n_out}out)`).join(', '));
  for (const [name, { n_in, n_out }] of dspCounts) {
    try { startDsp(name, n_in, n_out); } catch (e) { logger.warn('[startup] dsp %s:', name, e.message); }
  }
  try { await Promise.all([...dspCounts.keys()].map(name => waitForDspReady(name))); }
  catch (e) { logger.warn('[startup] aoip_engine ready timeout: %s', e.message); }
  registerAnalogBridge(config);
}

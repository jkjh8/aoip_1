/**
 * streams.js — 멀티 스트림 RTP 관리 (rtp_streams config 기반)
 *
 * _launchRtpIn / _launchRtpOut / _launchRtpSend
 * startRtpStreams / stopRtpStream / startRtpStream / stopRtpStreams
 * getRtpStreamStatus / waitForRtpStreamsReady
 * addRtpOutTarget / removeRtpOutTarget / setRtpOutCodec
 * updateRtpInConfig / updateRtpOutConfig / getRtpStreamDetail
 */
import { spawn, execSync } from 'child_process'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import logger from '../logger.js'
import { sendToEngine } from '../dsp/index.js'
import { getRtpChStart } from '../channels/index.js'
import { applyRtpInStats, shmName, saveAudioConfig } from './utils.js'

const __dirname = dirname(fileURLToPath(import.meta.url))
const SCRIPTS   = join(__dirname, '../../scripts')

/** @type {Map<string, { proc, cfg, ready, ports, shouldRun, stats }>} */
const streamInstances = new Map()

/* ── rtp_in 기동 ─────────────────────────────────────────────────── */
async function _launchRtpIn(cfg) {
  const key       = cfg.client
  const protoMode = cfg.protocol === 'pcm' ? 'pcm' : cfg.protocol === 'rtp' ? 'rtp' : 'raw'
  const address   = cfg.address && cfg.address !== '0.0.0.0' ? cfg.address : '0.0.0.0'
  const shm       = shmName('rtp_in', key)
  const ch        = cfg.channels ?? 2
  const chStart   = getRtpChStart(key, 'in')
  const bin       = join(SCRIPTS, 'rtp_recv')

  const inst = {
    proc: null, cfg, ready: false, ports: [], shouldRun: true,
    stats: { codec: 'unknown', bufUsedMs: 0, packets: 0, drops: 0, srcIp: null, srcPort: null, bitrateKbps: 0 },
  }
  streamInstances.set(key, inst)

  sendToEngine(`rtp_in add ${key} ${shm} ${ch} ${chStart}`)

  const args = [
    String(cfg.port ?? 10001), String(ch), protoMode, key,
    String(cfg.bufferMs ?? 100), String(cfg.sampleRate ?? 48000),
    protoMode === 'rtp' ? (cfg.rtpEncoding ?? 'L24') : '',
    address, 'shm', shm,
  ]
  logger.info('[gst] starting rtp_recv (shm) %s  port=%d addr=%s shm=%s', key, cfg.port ?? 10001, address, shm)

  const proc = spawn('chrt', ['-f', '20', bin, ...args], {
    stdio: ['ignore', 'pipe', 'pipe'], detached: false,
  })
  inst.proc = proc

  proc.stderr.on('data', (d) => {
    d.toString().split('\n').filter(Boolean).forEach((line) => {
      if (line.startsWith('[rtp_recv] ready')) {
        if (!inst.ready) { inst.ready = true; logger.info('[gst] rtp_in %s ready (pipe mode)', key) }
      } else if (line.startsWith('stats ')) {
        applyRtpInStats(inst.stats, line)
      } else if (line.startsWith('[rtp_recv] auto-codec:')) {
        const parts = line.split(' ')
        if (parts.length >= 3) {
          inst.cfg.rtpEncoding = parts[2]
          if (parts[3]) inst.cfg.sampleRate = Number(parts[3])
          saveAudioConfig(inst.cfg)
          logger.info('[gst] rtp_in %s auto-codec: %s %sHz', key, inst.cfg.rtpEncoding, inst.cfg.sampleRate)
        }
      } else {
        logger.debug('[rtp_recv:%s] %s', key, line)
      }
    })
  })
  proc.stdout.on('data', (d) => {
    d.toString().split('\n').filter(Boolean).forEach((line) => {
      if (line.startsWith('[rtp_recv] ready') && !inst.ready) {
        inst.ready = true; logger.info('[gst] rtp_in %s ready (pipe/stdout)', key)
      } else if (line.startsWith('stats ')) {
        applyRtpInStats(inst.stats, line)
      }
    })
  })
  proc.on('exit', (code, signal) => {
    logger.info('[rtp_recv:%s] exited code=%s signal=%s', key, code, signal)
    inst.proc = null; inst.ready = false; inst.ports = []
    if (!inst.shouldRun || signal === 'SIGTERM') return
    const delay = code === 2 ? 200 : 2000
    setTimeout(() => { if (inst.shouldRun) _launchRtpIn(inst.cfg) }, delay)
  })
  proc.on('error', (err) => { logger.error('[rtp_recv:%s] error: %s', key, err.message); inst.proc = null })
}

/* ── rtp_out 기동 ────────────────────────────────────────────────── */
async function _launchRtpOut(cfg) {
  const key      = cfg.client
  const shm      = shmName('rtp_out', key)
  const ch       = cfg.channels ?? 2
  const chStart  = getRtpChStart(key, 'out')

  const inst = {
    proc: null, cfg, ready: false, ports: [], shouldRun: true,
    stats: { targets: 0, codec: cfg.codec ?? 'raw', bitrateKbps: 0, bytesSent: 0 },
  }
  streamInstances.set(key, inst)

  sendToEngine(`rtp_out add ${key} ${shm} ${ch} ${chStart}`)
  _launchRtpSend(cfg, shm, inst)
}

function _launchRtpSend(cfg, shm, inst) {
  const key      = cfg.client
  const bin      = join(SCRIPTS, 'rtp_send')
  const proto    = cfg.protocol === 'rtp' ? 'rtp' : 'raw'
  const codecArg = cfg.codec   ?? 'mp3'
  const brArg    = String(cfg.bitrate ?? 320)
  const args     = [String(cfg.channels ?? 2), key, proto, String(cfg.sampleRate ?? 0), 'shm', shm, codecArg, brArg]

  logger.info('[gst] starting rtp_send (pipe) %s: %s %s', key, bin, args.join(' '))

  const proc = spawn('chrt', ['-f', '20', bin, ...args], {
    stdio: ['pipe', 'pipe', 'pipe'], detached: false,
  })
  inst.proc = proc

  let buf = ''
  proc.stdout.on('data', (d) => {
    buf += d.toString()
    let nl
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).trim(); buf = buf.slice(nl + 1)
      if (line.startsWith('[rtp_send] ready')) {
        inst.ready = true
        logger.info('[gst] rtp_out %s ready (shm mode)', key)
        for (const t of cfg.targets ?? []) proc.stdin.write(`add ${t.host} ${t.port}\n`)
      } else if (line.startsWith('stats ')) {
        const m = line.match(/targets=(\d+)\s+codec=(\S+)\s+bitrateKbps=(\d+)\s+bytesSent=(\d+)/)
        if (m) inst.stats = { targets: Number(m[1]), codec: m[2], bitrateKbps: Number(m[3]), bytesSent: Number(m[4]) }
      }
    }
  })
  proc.stderr.on('data', (d) =>
    d.toString().split('\n').filter(Boolean).forEach((l) => logger.debug('[rtp_send:%s] %s', key, l))
  )
  proc.on('exit', (code, signal) => {
    logger.info('[rtp_send:%s] exited code=%s signal=%s', key, code, signal)
    inst.proc = null; inst.ready = false
    if (inst.shouldRun && signal !== 'SIGTERM' && code !== 0) {
      setTimeout(() => {
        if (!inst.shouldRun) return
        try { execSync(`pkill -f "rtp_send.*${key}"`, { stdio: 'ignore' }) } catch { /* ignore */ }
        _launchRtpSend(inst.cfg, shm, inst)
      }, 200)
    }
  })
  proc.on('error', (err) => { logger.error('[rtp_send:%s] error: %s', key, err.message); inst.proc = null })
}

/* ── Public API ──────────────────────────────────────────────────── */

export function addRtpOutTarget(client, host, port) {
  const inst = streamInstances.get(client)
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`)

  for (const [key, other] of streamInstances) {
    if (key === client || other.cfg.type !== 'rtp_out') continue
    const dup = (other.cfg.targets ?? []).find((t) => t.host === host && t.port === port)
    if (dup) {
      other.cfg.targets = other.cfg.targets.filter((t) => !(t.host === host && t.port === port))
      if (other.proc && !other.proc.killed) other.proc.stdin.write(`remove ${host} ${port}\n`)
      logger.info('[gst] rtp_out %s: removed duplicate target %s:%d (moved to %s)', key, host, port, client)
    }
  }

  const targets = inst.cfg.targets ?? []
  if (!targets.find((t) => t.host === host && t.port === port)) targets.push({ host, port })
  inst.cfg.targets = targets
  if (inst.proc && !inst.proc.killed) inst.proc.stdin.write(`add ${host} ${port}\n`)
  saveAudioConfig(inst.cfg)
  logger.info('[gst] rtp_out %s add target %s:%d', client, host, port)
}

export function removeRtpOutTarget(client, host, port) {
  const inst = streamInstances.get(client)
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`)
  inst.cfg.targets = (inst.cfg.targets ?? []).filter((t) => !(t.host === host && t.port === port))
  if (inst.proc && !inst.proc.killed) inst.proc.stdin.write(`remove ${host} ${port}\n`)
  saveAudioConfig(inst.cfg)
  logger.info('[gst] rtp_out %s remove target %s:%d', client, host, port)
}

export function setRtpOutCodec(client, codec, bitrate) {
  const inst = streamInstances.get(client)
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`)
  const validCodec   = codec === 'mp3' || codec === 'raw' ? codec : inst.cfg.codec
  inst.cfg.codec     = validCodec
  inst.cfg.bitrate   = bitrate ?? inst.cfg.bitrate
  if (inst.proc && !inst.proc.killed)
    inst.proc.stdin.write(`codec ${inst.cfg.codec} ${inst.cfg.bitrate ?? 320}\n`)
  saveAudioConfig(inst.cfg)
  logger.info('[gst] rtp_out %s codec %s %d', client, inst.cfg.codec, inst.cfg.bitrate ?? 320)
}

export function getRtpStreamDetail(client) {
  const inst = streamInstances.get(client)
  if (!inst) return null
  return {
    client, type: inst.cfg.type, name: inst.cfg.name, ready: inst.ready,
    ports: [...inst.ports], protocol: inst.cfg.protocol ?? 'raw',
    codec: inst.cfg.codec, bitrate: inst.cfg.bitrate,
    port: inst.cfg.port, address: inst.cfg.address ?? '0.0.0.0',
    bufferMs: inst.cfg.bufferMs ?? 100, sampleRate: inst.cfg.sampleRate ?? 48000,
    targets: [...(inst.cfg.targets ?? [])], channels: inst.cfg.channels ?? 2,
    stats: inst.stats ? { ...inst.stats } : undefined,
  }
}

export function startRtpStreams(streams) {
  for (const cfg of streams ?? []) {
    if (cfg.enabled === false || !cfg.client) continue
    try { execSync(`pkill -f "rtp_recv.*${cfg.client}|rtp_send.*${cfg.client}"`, { stdio: 'ignore' }) } catch { /* ignore */ }
  }
  for (const cfg of streams ?? []) {
    if (cfg.enabled === false) continue
    const key = cfg.client
    if (streamInstances.has(key)) continue
    if (cfg.type === 'rtp_in')  _launchRtpIn(cfg)
    else if (cfg.type === 'rtp_out') _launchRtpOut(cfg)
  }
}

export function updateRtpInConfig(client, updates) {
  const inst = streamInstances.get(client)
  if (!inst || inst.cfg.type !== 'rtp_in') throw new Error(`rtp_in ${client} not found`)
  Object.assign(inst.cfg, updates)
  saveAudioConfig(inst.cfg)
  logger.info('[gst] rtp_in %s config updated (restart required to apply)', client)
}

export function updateRtpOutConfig(client, updates) {
  const inst = streamInstances.get(client)
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`)
  const allowed = ['protocol', 'sampleRate', 'channels']
  allowed.forEach((k) => { if (updates[k] !== undefined) inst.cfg[k] = updates[k] })
  saveAudioConfig(inst.cfg)
  logger.info('[gst] rtp_out %s config saved (restart to apply)', client)
}

export function stopRtpStream(client) {
  const inst = streamInstances.get(client)
  if (!inst) throw new Error(`rtp stream ${client} not found`)
  inst.shouldRun = false
  if (inst.cfg.type === 'rtp_in')  sendToEngine(`rtp_in remove ${client}`)
  if (inst.cfg.type === 'rtp_out') sendToEngine(`rtp_out remove ${client}`)
  if (inst.proc && !inst.proc.killed) {
    try { inst.proc.stdin?.write('quit\n') } catch { /* ignore */ }
    inst.proc.kill('SIGTERM')
  }
}

export function startRtpStream(client) {
  const inst = streamInstances.get(client)
  if (!inst) throw new Error(`rtp stream ${client} not found`)
  if (inst.proc && !inst.proc.killed) return
  inst.shouldRun = true
  if (inst.cfg.type === 'rtp_in')       _launchRtpIn(inst.cfg)
  else if (inst.cfg.type === 'rtp_out') _launchRtpOut(inst.cfg)
}

export function stopRtpStreams() {
  for (const [client, inst] of streamInstances) {
    inst.shouldRun = false
    if (inst.proc) {
      try { inst.proc.stdin?.write('quit\n') } catch { /* ignore */ }
      inst.proc.kill('SIGTERM')
    }
    if (inst.cfg.type === 'rtp_in')  sendToEngine(`rtp_in remove ${client}`)
    if (inst.cfg.type === 'rtp_out') sendToEngine(`rtp_out remove ${client}`)
  }
  streamInstances.clear()
}

export function getRtpStreamStatus() {
  return Array.from(streamInstances.entries()).map(([key, inst]) => ({
    client: key, type: inst.cfg.type, name: inst.cfg.name,
    running: inst.proc !== null && !inst.proc?.killed,
    ready: inst.ready, ports: [...inst.ports],
    stats: inst.stats ? { ...inst.stats } : undefined,
  }))
}

export function waitForRtpStreamsReady(timeoutMs = 8000) {
  return new Promise((resolve, reject) => {
    if (streamInstances.size === 0) return resolve()
    const t = setTimeout(() => reject(new Error('rtp_streams ready timeout')), timeoutMs)
    const poll = setInterval(() => {
      const allReady = Array.from(streamInstances.values()).every((i) => i.ready)
      if (allReady) { clearInterval(poll); clearTimeout(t); resolve() }
    }, 100)
  })
}

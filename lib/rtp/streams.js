/**
 * streams.js — 멀티 스트림 RTP 관리 (rtp_streams config 기반)
 *
 * _launchRtpIn / _launchRtpOut / _launchRtpSend
 * startRtpStreams / stopRtpStream / startRtpStream / stopRtpStreams
 * getRtpStreamStatus / waitForRtpStreamsReady
 * addRtpOutTarget / removeRtpOutTarget / setRtpOutCodec
 * updateRtpInConfig / updateRtpOutConfig / getRtpStreamDetail
 */
import net from 'net'
import { unlinkSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname } from 'path'
import { EventEmitter } from 'events'
import logger from '../logger.js'
import { sendToEngine, waitForEngineReady, addEngineRestartListener } from '../dsp/index.js'
import { getRtpChStart } from '../channels/index.js'
import { applyRtpInStats, saveAudioConfig } from './utils.js'

export const streamEvents = new EventEmitter()

const __dirname = dirname(fileURLToPath(import.meta.url))

const SOCK_DIR = '/run/aoip'


/** @type {Map<string, { socket, server, cfg, ready, ports, shouldRun, stats, _restartTimer }>} */
const streamInstances = new Map()

function _scheduleRtpInRestart(inst, key, delayMs) {
  if (inst._restartTimer) return
  inst._restartTimer = setTimeout(async () => {
    inst._restartTimer = null
    if (!inst.shouldRun) return
    logger.info('[rtp_recv:%s] auto-restarting after source disconnect', key)
    const ch      = inst.cfg.channels ?? 2
    const chStart = getRtpChStart(key, 'in')
    sendToEngine(`rtp_in remove ${key}`)
    await new Promise(r => setTimeout(r, 300))
    if (!inst.shouldRun) return
    if (!inst.server) await _spawnRtpIn(inst)
    sendToEngine(`rtp_in add ${key} ${ch} ${chStart}`)
  }, delayMs)
}

function _sockWrite(socket, line) {
  if (socket && !socket.destroyed) socket.write(line + '\n')
}

/* ── rtp_in: 소켓 서버 시작 ──────────────────────────────────────── */
function _startRtpInServer(inst) {
  const key = inst.cfg.client
  const sockPath = `${SOCK_DIR}/rtp_recv_${key}.sock`

  try { unlinkSync(sockPath) } catch { /* file may not exist */ }

  const server = net.createServer((socket) => {
    if (inst.socket && !inst.socket.destroyed) inst.socket.destroy()
    inst.socket = socket
    inst.ready = false
    logger.info('[rtp_recv:%s] connected', key)

    socket.write(
      `config port=${inst.cfg.port ?? 5004} channels=${inst.cfg.channels ?? 2}` +
      ` proto=${inst.cfg.protocol ?? 'rtp'} bufMs=${inst.cfg.bufferMs ?? 100}` +
      ` rate=${inst.cfg.sampleRate ?? 48000} addr=${inst.cfg.address ?? '0.0.0.0'}\n`
    )

    let buf = ''
    socket.on('data', (d) => {
      buf += d.toString()
      let nl
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl).trim()
        buf = buf.slice(nl + 1)
        if (!line) continue
        if (line.startsWith('[rtp_recv] ready')) {
          if (!inst.ready) { inst.ready = true; logger.info('[gst] rtp_in %s ready', key) }
        } else if (line.startsWith('stats ')) {
          const prevSrcIp = inst.stats.srcIp
          applyRtpInStats(inst.stats, line)
          if (inst.stats.srcIp !== prevSrcIp) {
            if (inst.stats.srcIp) {
              logger.info('[rtp_recv:%s] source connected: %s:%d  codec=%s  %dkbps',
                key, inst.stats.srcIp, inst.stats.srcPort, inst.stats.codec, inst.stats.bitrateKbps)
            } else {
              logger.info('[rtp_recv:%s] source disconnected — scheduling restart', key)
              streamEvents.emit('state:changed')
              _scheduleRtpInRestart(inst, key, 2000)
            }
          }
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
      }
    })

    socket.on('close', () => {
      logger.info('[rtp_recv:%s] disconnected', key)
      inst.socket = null; inst.ready = false; inst.ports = []
      streamEvents.emit('state:changed')
    })

    socket.on('error', (err) => {
      if (err.code !== 'ECONNRESET') logger.error('[rtp_recv:%s] socket error: %s', key, err.message)
    })
  })

  server.on('error', (err) => {
    logger.error('[rtp_recv:%s] server error: %s', key, err.message)
  })

  inst.server = server
  return new Promise((resolve) => {
    server.listen(sockPath, () => {
      logger.info('[rtp_recv:%s] socket server listening', key)
      resolve()
    })
  })
}

async function _spawnRtpIn(inst) {
  inst.ready = false
  await _startRtpInServer(inst)
  /* socket server ready — now tell engine to connect */
}

/* ── rtp_in 최초 기동 ────────────────────────────────────────────── */
async function _launchRtpIn(cfg) {
  const key     = cfg.client
  const ch      = cfg.channels ?? 2
  const chStart = getRtpChStart(key, 'in')

  const inst = {
    socket: null, server: null,
    cfg, ready: false, ports: [], shouldRun: true,
    stats: { codec: 'unknown', bufUsedMs: 0, packets: 0, drops: 0, srcIp: null, srcPort: null, bitrateKbps: 0 },
    _restartTimer: null,
  }
  streamInstances.set(key, inst)

  if (inst.shouldRun) await _spawnRtpIn(inst)
  try { await waitForEngineReady(10000) } catch { logger.warn('[rtp] engine not ready, sending rtp_in add anyway') }
  sendToEngine(`rtp_in add ${key} ${ch} ${chStart}`)
}

/* ── rtp_out: 소켓 서버 시작 ─────────────────────────────────────── */
function _startRtpSendServer(inst) {
  const key = inst.cfg.client
  const sockPath = `${SOCK_DIR}/rtp_send_${key}.sock`

  try { unlinkSync(sockPath) } catch { /* file may not exist */ }

  const server = net.createServer((socket) => {
    if (inst.socket && !inst.socket.destroyed) inst.socket.destroy()
    inst.socket = socket
    inst.ready = false
    logger.info('[rtp_send:%s] connected', key)

    socket.write(
      `config channels=${inst.cfg.channels ?? 2}` +
      ` proto=${inst.cfg.protocol ?? 'rtp'}` +
      ` rate=${inst.cfg.sampleRate ?? 48000}` +
      ` codec=${inst.cfg.codec ?? 'raw'}` +
      ` bitrate=${inst.cfg.bitrate ?? 192}\n`
    )

    let buf = ''
    socket.on('data', (d) => {
      buf += d.toString()
      let nl
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl).trim()
        buf = buf.slice(nl + 1)
        if (!line) continue
        if (line.startsWith('[rtp_send] ready')) {
          inst.ready = true
          logger.info('[gst] rtp_out %s ready', key)
          for (const t of inst.cfg.targets ?? []) socket.write(`add ${t.host} ${t.port}\n`)
        } else if (line.startsWith('stats ')) {
          const m = line.match(/targets=(\d+)\s+codec=(\S+)\s+bitrateKbps=(\d+)\s+bytesSent=(\d+)/)
          if (m) inst.stats = { targets: Number(m[1]), codec: m[2], bitrateKbps: Number(m[3]), bytesSent: Number(m[4]) }
        } else {
          logger.debug('[rtp_send:%s] %s', key, line)
        }
      }
    })

    socket.on('close', () => {
      logger.info('[rtp_send:%s] disconnected', key)
      inst.socket = null; inst.ready = false
      streamEvents.emit('state:changed')
    })

    socket.on('error', (err) => {
      if (err.code !== 'ECONNRESET') logger.error('[rtp_send:%s] socket error: %s', key, err.message)
    })
  })

  server.on('error', (err) => {
    logger.error('[rtp_send:%s] server error: %s', key, err.message)
  })

  inst.server = server
  server.listen(sockPath, () => {
    logger.info('[rtp_send:%s] socket server listening', key)
  })
}

/* ── rtp_out 기동 ────────────────────────────────────────────────── */
function _launchRtpSend(cfg, inst) {
  _startRtpSendServer(inst)
  /* RTP send is now an internal thread in aoip_engine — no separate service */
}

/* ── rtp_out 최초 기동 ───────────────────────────────────────────── */
async function _launchRtpOut(cfg) {
  const key     = cfg.client
  const ch      = cfg.channels ?? 2
  const chStart = getRtpChStart(key, 'out')

  const inst = {
    socket: null, server: null,
    cfg, ready: false, ports: [], shouldRun: false,
    stats: { targets: 0, codec: cfg.codec ?? 'raw', bitrateKbps: 0, bytesSent: 0 },
  }
  streamInstances.set(key, inst)

  if (!(cfg.targets ?? []).length) return

  inst.shouldRun = true
  try { await waitForEngineReady(10000) } catch { logger.warn('[rtp] engine not ready, sending rtp_out add anyway') }
  sendToEngine(`rtp_out add ${key} ${ch} ${chStart}`)
  _launchRtpSend(cfg, inst)
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
      _sockWrite(other.socket, `remove ${host} ${port}`)
      logger.info('[gst] rtp_out %s: removed duplicate target %s:%d (moved to %s)', key, host, port, client)
    }
  }

  const targets = inst.cfg.targets ?? []
  if (!targets.find((t) => t.host === host && t.port === port)) targets.push({ host, port })
  inst.cfg.targets = targets
  _sockWrite(inst.socket, `add ${host} ${port}`)
  saveAudioConfig(inst.cfg)
  logger.info('[gst] rtp_out %s add target %s:%d', client, host, port)
}

export function removeRtpOutTarget(client, host, port) {
  const inst = streamInstances.get(client)
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`)
  inst.cfg.targets = (inst.cfg.targets ?? []).filter((t) => !(t.host === host && t.port === port))
  _sockWrite(inst.socket, `remove ${host} ${port}`)
  saveAudioConfig(inst.cfg)
  logger.info('[gst] rtp_out %s remove target %s:%d', client, host, port)
  if (inst.cfg.targets.length === 0 && inst.server) {
    logger.info('[gst] rtp_out %s: no targets left — stopping', client)
    stopRtpStream(client)
    streamEvents.emit('state:changed')
  }
}

export function setRtpOutCodec(client, codec, bitrate) {
  const inst = streamInstances.get(client)
  if (!inst || inst.cfg.type !== 'rtp_out') throw new Error(`rtp_out ${client} not found`)
  const validCodec   = codec === 'mp3' || codec === 'raw' ? codec : inst.cfg.codec
  inst.cfg.codec     = validCodec
  inst.cfg.bitrate   = bitrate ?? inst.cfg.bitrate
  _sockWrite(inst.socket, `codec ${inst.cfg.codec} ${inst.cfg.bitrate ?? 320}`)
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
    const key = cfg.client
    /* legacy separate services no longer used; engine handles recv/send internally */
  }
  for (const cfg of streams ?? []) {
    if (cfg.enabled === false) continue
    const key = cfg.client
    if (streamInstances.has(key)) continue
    if (cfg.type === 'rtp_in')       _launchRtpIn(cfg)
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
  const allowed = ['protocol', 'sampleRate', 'channels', 'codec', 'bitrate']
  allowed.forEach((k) => { if (updates[k] !== undefined) inst.cfg[k] = updates[k] })
  if ((updates.codec !== undefined || updates.bitrate !== undefined) && inst.socket)
    _sockWrite(inst.socket, `codec ${inst.cfg.codec ?? 'mp3'} ${inst.cfg.bitrate ?? 320}`)
  saveAudioConfig(inst.cfg)
  logger.info('[gst] rtp_out %s config saved (restart to apply)', client)
}

export function stopRtpStream(client) {
  const inst = streamInstances.get(client)
  if (!inst) throw new Error(`rtp stream ${client} not found`)
  inst.shouldRun = false
  inst.ready = false
  if (inst.cfg.type === 'rtp_in') {
    if (inst._restartTimer) { clearTimeout(inst._restartTimer); inst._restartTimer = null }
    sendToEngine(`rtp_in remove ${client}`)
    inst.socket?.destroy(); inst.socket = null
    inst.server?.close(); inst.server = null
    inst.stats = { codec: 'unknown', bufUsedMs: 0, packets: 0, drops: 0, srcIp: null, srcPort: null, bitrateKbps: 0 }
  }
  if (inst.cfg.type === 'rtp_out') {
    for (const t of inst.cfg.targets ?? []) _sockWrite(inst.socket, `remove ${t.host} ${t.port}`)
    sendToEngine(`rtp_out remove ${client}`)
    inst.socket?.destroy(); inst.socket = null
    inst.server?.close(); inst.server = null
    inst.stats = { targets: 0, codec: inst.cfg.codec ?? 'raw', bitrateKbps: 0, bytesSent: 0 }
  }
}

export async function startRtpStream(client) {
  const inst = streamInstances.get(client)
  if (!inst) throw new Error(`rtp stream ${client} not found`)
  if (inst.server) return
  inst.shouldRun = true
  try { await waitForEngineReady(10000) } catch { logger.warn('[rtp] engine not ready, sending add command anyway') }
  if (inst.cfg.type === 'rtp_in') {
    const ch      = inst.cfg.channels ?? 2
    const chStart = getRtpChStart(client, 'in')
    sendToEngine(`rtp_in remove ${client}`)
    await new Promise(r => setTimeout(r, 100))
    if (inst.shouldRun) await _spawnRtpIn(inst)
    sendToEngine(`rtp_in add ${client} ${ch} ${chStart}`)
  } else if (inst.cfg.type === 'rtp_out') {
    if (!(inst.cfg.targets ?? []).length) return
    const ch      = inst.cfg.channels ?? 2
    const chStart = getRtpChStart(client, 'out')
    sendToEngine(`rtp_out remove ${client}`)
    await new Promise(r => setTimeout(r, 100))
    sendToEngine(`rtp_out add ${client} ${ch} ${chStart}`)
    if (inst.shouldRun) _launchRtpSend(inst.cfg, inst)
  }
}

export function stopRtpStreams() {
  for (const [client, inst] of streamInstances) {
    inst.shouldRun = false
    if (inst.cfg.type === 'rtp_in')  sendToEngine(`rtp_in remove ${client}`)
    if (inst.cfg.type === 'rtp_out') sendToEngine(`rtp_out remove ${client}`)
    inst.socket?.destroy(); inst.socket = null
    inst.server?.close(); inst.server = null
  }
  streamInstances.clear()
}

export function getRtpStreamStatus() {
  return Array.from(streamInstances.entries()).map(([key, inst]) => ({
    client: key, type: inst.cfg.type, name: inst.cfg.name,
    running: !!inst.server,
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

addEngineRestartListener(() => {
  logger.info('[rtp] engine restarted — re-registering all active streams')
  for (const [key, inst] of streamInstances) {
    if (!inst.shouldRun) continue
    const ch      = inst.cfg.channels ?? 2
    if (inst.cfg.type === 'rtp_in') {
      const chStart = getRtpChStart(key, 'in')
      sendToEngine(`rtp_in add ${key} ${ch} ${chStart}`)
    } else if (inst.cfg.type === 'rtp_out') {
      const chStart = getRtpChStart(key, 'out')
      sendToEngine(`rtp_out add ${key} ${ch} ${chStart}`)
    }
  }
})

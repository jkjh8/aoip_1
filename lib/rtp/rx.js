/**
 * rx.js — 단일 RTP 수신 스트림 (레거시 단일 인스턴스 모드)
 *
 * startRxPipeline / stopRxPipeline / isRxRunning
 * setRxPort / setRxBuffer / waitForRxReady
 */
import { spawn } from 'child_process'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import logger from '../logger.js'
import { applyRtpInStats, waitForFlag } from './utils.js'

const __dirname = dirname(fileURLToPath(import.meta.url))
const SCRIPTS   = join(__dirname, '../../scripts')

let rxProcess   = null
let rxPort      = 10001
let rxChannels  = 2
let rxBufMs     = 100
let rxPorts     = []
let rxReady     = false
let rxShouldRun = false
let rxLastCfg   = {}

export const rxStats = {
  codec: 'unknown', bitrateKbps: 0, bufUsedMs: 0,
  packets: 0, drops: 0, srcIp: null, srcPort: null,
}

export function setRxPort(port)    { rxPort = Number(port); logger.info('[gst] rx port set to %d', rxPort) }
export function getRxPort()        { return rxPort }
export function getRxPorts()       { return [...rxPorts] }
export function getRxStats()       { return { ...rxStats } }
export function isRxRunning()      { return rxProcess !== null && !rxProcess.killed }
export function waitForRxReady(ms) { return waitForFlag(() => rxReady, 'rtp_recv', ms ?? 8000) }

export function setRxBuffer(ms) {
  rxBufMs = ms
  if (isRxRunning()) {
    stopRxPipeline()
    startRxPipeline({})
  }
}

export function startRxPipeline(cfg) {
  rxShouldRun = true
  rxLastCfg   = cfg || {}
  if (rxProcess) {
    logger.info('[gst] rx already running (pid %d)', rxProcess.pid)
    return
  }
  const port     = rxPort ?? cfg?.port ?? 10001
  const ch       = rxChannels
  const protocol = cfg?.protocol ?? 'raw'
  const bin      = join(SCRIPTS, 'rtp_recv')
  const args     = [String(port), String(ch), protocol]

  logger.info('[gst] starting rtp_recv: %s %s', bin, args.join(' '))
  const proc = spawn(bin, args, { stdio: ['ignore', 'pipe', 'pipe'], detached: false })
  rxProcess = proc
  rxPorts   = []
  rxReady   = false

  let stdoutBuf = ''
  proc.stdout.on('data', (d) => {
    stdoutBuf += d.toString()
    let nl
    while ((nl = stdoutBuf.indexOf('\n')) >= 0) {
      const line = stdoutBuf.slice(0, nl).trim()
      stdoutBuf  = stdoutBuf.slice(nl + 1)
      _parseRxLine(line)
    }
  })
  proc.stderr.on('data', (d) =>
    d.toString().split('\n').filter(Boolean).forEach((l) => logger.debug('[rtp_recv] %s', l))
  )
  proc.on('exit', (code, signal) => {
    logger.info('[rtp_recv] exited code=%s signal=%s', code, signal)
    rxProcess = null; rxPorts = []; rxReady = false
    if (rxShouldRun) {
      setTimeout(() => { if (rxShouldRun && !rxProcess) { logger.info('[gst] auto-restarting rtp_recv'); startRxPipeline(rxLastCfg) } }, 2000)
    }
  })
  proc.on('error', (err) => { logger.error('[rtp_recv] error: %s', err.message); rxProcess = null })
}

function _parseRxLine(line) {
  if (!line.startsWith('stats ')) logger.debug('[rtp_recv] %s', line)
  if (line.startsWith('[rtp_recv] ready')) {
    rxReady = true
    logger.info('[gst] rx started  rtp_recv + gst udpsrc port=%d codec=%s', rxPort, rxStats.codec)
    return
  }
  if (line.startsWith('ports:')) {
    rxPorts = line.replace('ports:', '').split(',').map((s) => s.trim())
    logger.debug('[rtp_recv] ready  ports: %s', rxPorts.join(', '))
    return
  }
  if (line.startsWith('stats ')) applyRtpInStats(rxStats, line)
}

export function stopRxPipeline() {
  rxShouldRun = false
  if (!rxProcess) return
  logger.info('[gst] stopping rx (pid %d)', rxProcess.pid)
  rxProcess.kill('SIGTERM')
  rxProcess = null
}

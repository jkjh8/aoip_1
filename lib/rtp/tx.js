/**
 * tx.js — 단일 RTP 전송 스트림 (레거시 단일 인스턴스 모드)
 *
 * startTxClient / stopTxClient / isTxRunning
 * addTxTarget / removeTxTarget / setTxCodec / waitForTxReady
 */
import { spawn } from 'child_process'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import logger from '../logger.js'
import { waitForFlag } from './utils.js'

const __dirname = dirname(fileURLToPath(import.meta.url))
const SCRIPTS   = join(__dirname, '../../scripts')

let txProcess = null
let txPorts   = []
let txTargets = []
let txCodec   = 'mp3'
let txBitrate = 320
let txReady   = false

export function isTxRunning()      { return txProcess !== null && !txProcess.killed }
export function getTxPorts()       { return [...txPorts] }
export function getTxTargets()     { return [...txTargets] }
export function waitForTxReady(ms) { return waitForFlag(() => txReady, 'rtp_send', ms ?? 8000) }

export function startTxClient({ channels = 2 } = {}) {
  if (txProcess) {
    logger.info('[gst] rtp_send already running (pid %d)', txProcess.pid)
    return
  }
  const bin  = join(SCRIPTS, 'rtp_send')
  const args = [String(channels)]

  logger.info('[gst] starting rtp_send: %s %s', bin, args.join(' '))
  const proc = spawn(bin, args, { stdio: ['pipe', 'pipe', 'pipe'], detached: false })
  txProcess = proc; txPorts = []; txReady = false

  let stdoutBuf = ''
  proc.stdout.on('data', (d) => {
    stdoutBuf += d.toString()
    let nl
    while ((nl = stdoutBuf.indexOf('\n')) >= 0) {
      const line = stdoutBuf.slice(0, nl).trim()
      stdoutBuf  = stdoutBuf.slice(nl + 1)
      _parseTxLine(line)
    }
  })
  proc.stderr.on('data', (d) =>
    d.toString().split('\n').filter(Boolean).forEach((l) => logger.debug('[rtp_send] %s', l))
  )
  proc.on('exit', (code, signal) => {
    logger.info('[rtp_send] exited code=%s signal=%s', code, signal)
    txProcess = null; txPorts = []; txReady = false
  })
  proc.on('error', (err) => { logger.error('[rtp_send] error: %s', err.message); txProcess = null })

  if (txTargets.length > 0 || txCodec !== 'mp3' || txBitrate !== 320) {
    setTimeout(() => {
      if (!txProcess) return
      txTargets.forEach((t) => _txCmd(`add ${t.host} ${t.port}`))
      _txCmd(`codec ${txCodec} ${txBitrate}`)
    }, 500)
  }
}

function _parseTxLine(line) {
  logger.debug('[rtp_send] %s', line)
  if (line.startsWith('[rtp_send] ready')) {
    txReady = true
    logger.info('[gst] rtp_send started  ports: %s', txPorts.length ? txPorts.join(', ') : '(waiting)')
    return
  }
  if (line.startsWith('ports:')) {
    txPorts = line.replace('ports:', '').split(',').map((s) => s.trim())
    logger.debug('[rtp_send] ready  ports: %s', txPorts.join(', '))
  }
}

function _txCmd(cmd) {
  if (!txProcess || txProcess.killed) return
  txProcess.stdin.write(cmd + '\n')
}

export function stopTxClient() {
  if (!txProcess) return
  logger.info('[gst] stopping rtp_send (pid %d)', txProcess.pid)
  _txCmd('quit')
  txProcess.kill('SIGTERM')
  txProcess = null; txPorts = []
}

export function addTxTarget(target) {
  const exists = txTargets.some((t) => t.host === target.host && t.port === target.port)
  if (exists) return
  txTargets.push({ ...target })
  _txCmd(`add ${target.host} ${target.port}`)
  logger.info('[gst] tx target add %s:%d', target.host, target.port)
}

export function removeTxTarget(target) {
  const before = txTargets.length
  txTargets = txTargets.filter((t) => !(t.host === target.host && t.port === target.port))
  if (txTargets.length !== before) {
    _txCmd(`remove ${target.host} ${target.port}`)
    logger.info('[gst] tx target remove %s:%d', target.host, target.port)
  }
}

export function setTxCodec(codec, bitrate) {
  txCodec   = codec   ?? txCodec
  txBitrate = bitrate ?? txBitrate
  _txCmd(`codec ${txCodec} ${txBitrate}`)
  logger.info('[gst] tx codec %s %dk', txCodec, txBitrate)
}

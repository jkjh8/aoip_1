import net from 'net'
import fs from 'fs'
import { execSync } from 'child_process'
import logger from '../logger.js'

let _tcpServer = null
let _rx = null
let _tx = null
let _clients = new Set()
let _config = null

function _configurePort(device, baudRate, dataBits, stopBits, parity) {
  const parityFlag = parity === 'even' ? 'parenb -parodd'
    : parity === 'odd'  ? 'parenb parodd'
    : '-parenb'
  const stopFlag = stopBits === 2 ? 'cstopb' : '-cstopb'
  const csFlag = dataBits === 7 ? 'cs7' : 'cs8'
  execSync(`stty -F ${device} ${baudRate} raw -echo -echoe -echok ${csFlag} ${stopFlag} ${parityFlag}`)
  logger.info('[serial] port configured: %s %d %d%s%d', device, baudRate, dataBits, parity[0].toUpperCase(), stopBits)
}

function _openPort(device) {
  const fd = fs.openSync(device, 'r+')
  _rx = fs.createReadStream(null, { fd, autoClose: false })
  _tx = fs.createWriteStream(null, { fd, autoClose: false })

  _rx.on('data', (chunk) => {
    logger.debug('[serial] rx %d bytes → %d client(s)', chunk.length, _clients.size)
    for (const c of _clients) {
      if (!c.destroyed) c.write(chunk)
    }
  })

  _rx.on('error', (err) => {
    logger.error('[serial] rx error: %s', err.message)
  })
}

function _onClientConnect(socket) {
  const addr = `${socket.remoteAddress}:${socket.remotePort}`
  _clients.add(socket)
  logger.info('[serial] client connected %s (total: %d)', addr, _clients.size)

  socket.on('data', (chunk) => {
    logger.debug('[serial] tx %d bytes ← %s', chunk.length, addr)
    if (_tx && !_tx.destroyed) _tx.write(chunk)
  })

  socket.on('close', () => {
    _clients.delete(socket)
    logger.info('[serial] client disconnected %s (total: %d)', addr, _clients.size)
  })

  socket.on('error', (err) => {
    logger.warn('[serial] client %s error: %s', addr, err.message)
    _clients.delete(socket)
  })
}

export function startSerial(config) {
  const {
    enabled  = false,
    device   = '/dev/ttyAMA0',
    baudRate = 9600,
    dataBits = 8,
    stopBits = 1,
    parity   = 'none',
    tcpPort  = 4001,
  } = config.serial ?? {}

  if (!enabled) {
    logger.info('[serial] disabled — skipping')
    return
  }

  _config = { device, baudRate, dataBits, stopBits, parity, tcpPort }

  try {
    _configurePort(device, baudRate, dataBits, stopBits, parity)
    _openPort(device)
  } catch (err) {
    logger.error('[serial] failed to open %s: %s', device, err.message)
    return
  }

  _tcpServer = net.createServer(_onClientConnect)

  _tcpServer.on('error', (err) => {
    logger.error('[serial] TCP server error: %s', err.message)
  })

  _tcpServer.listen(tcpPort, () => {
    logger.info('[serial] TCP server listening on port %d → %s@%d', tcpPort, device, baudRate)
  })
}

export function stopSerial() {
  for (const c of _clients) c.destroy()
  _clients.clear()
  if (_tcpServer) { _tcpServer.close(); _tcpServer = null }
  if (_rx) { _rx.destroy(); _rx = null }
  if (_tx) { _tx.destroy(); _tx = null }
  logger.info('[serial] stopped')
}

export function restartSerial(config) {
  stopSerial()
  startSerial(config)
}

export function getSerialStatus() {
  return {
    running: _tcpServer?.listening ?? false,
    clients: _clients.size,
    config:  _config,
  }
}

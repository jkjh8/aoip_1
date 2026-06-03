import { Router } from 'express'
import { getSystemConfig, saveSystemConfig } from '../../lib/config.js'
import { startSerial, stopSerial, restartSerial, getSerialStatus } from '../../lib/serial/index.js'

const router = Router()

const ALLOWED_PARITIES  = ['none', 'even', 'odd']
const ALLOWED_DATA_BITS = [7, 8]
const ALLOWED_STOP_BITS = [1, 2]

function _validate(body) {
  const { baudRate, dataBits, stopBits, parity, tcpPort, device } = body
  if (baudRate   !== undefined && (typeof baudRate !== 'number'   || baudRate <= 0))         return 'invalid baudRate'
  if (dataBits   !== undefined && !ALLOWED_DATA_BITS.includes(dataBits))                     return 'dataBits must be 7 or 8'
  if (stopBits   !== undefined && !ALLOWED_STOP_BITS.includes(stopBits))                     return 'stopBits must be 1 or 2'
  if (parity     !== undefined && !ALLOWED_PARITIES.includes(parity))                        return 'parity must be none, even, or odd'
  if (tcpPort    !== undefined && (typeof tcpPort !== 'number' || tcpPort < 1 || tcpPort > 65535)) return 'invalid tcpPort'
  if (device     !== undefined && typeof device !== 'string')                                return 'invalid device'
  return null
}

// GET /serial/status
router.get('/status', (_req, res) => {
  res.json(getSerialStatus())
})

// GET /serial/config
router.get('/config', (_req, res) => {
  const { serial = {} } = getSystemConfig()
  res.json(serial)
})

// POST /serial/config
// body: { enabled?, device?, baudRate?, dataBits?, stopBits?, parity?, tcpPort? }
router.post('/config', (req, res) => {
  const err = _validate(req.body)
  if (err) return res.status(400).json({ ok: false, error: err })

  const sys = getSystemConfig()
  sys.serial = { ...(sys.serial ?? {}), ...req.body }
  saveSystemConfig()

  restartSerial(sys)
  res.json({ ok: true, serial: sys.serial })
})

// POST /serial/start
router.post('/start', (_req, res) => {
  const sys = getSystemConfig()
  sys.serial = { ...(sys.serial ?? {}), enabled: true }
  saveSystemConfig()
  startSerial(sys)
  res.json({ ok: true })
})

// POST /serial/stop
router.post('/stop', (_req, res) => {
  const sys = getSystemConfig()
  sys.serial = { ...(sys.serial ?? {}), enabled: false }
  saveSystemConfig()
  stopSerial()
  res.json({ ok: true })
})

export default router

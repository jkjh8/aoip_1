import winston from 'winston'
import DailyRotateFile from 'winston-daily-rotate-file'
import { readFileSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'

const __dirname = dirname(fileURLToPath(import.meta.url))
const logDir = join(__dirname, '../logs')
const SYSTEM_PATH = join(__dirname, '../config/system.json')

function _readLogCfg() {
  try {
    const sys = JSON.parse(readFileSync(SYSTEM_PATH, 'utf8'))
    return { enabled: true, debug: false, ...(sys.log ?? {}) }
  } catch {
    return { enabled: true, debug: false }
  }
}

const _logCfg = _readLogCfg()

const { combine, timestamp, printf, colorize, splat } = winston.format

winston.addColors({
  error: 'red',
  warn: 'yellow',
  info: 'green',
  debug: 'blue'
})

const logFormat = printf(
  ({ timestamp, level, message, stack }) =>
    `${timestamp} [${level}]: ${message}${stack ? ' ' + stack : ''}`
)

const consoleTransport = new winston.transports.Console({
  level: _logCfg.debug ? 'debug' : 'info',
  silent: !_logCfg.enabled,
  format: combine(
    splat(),
    colorize({ all: true }),
    timestamp({ format: 'YYYY-MM-DD HH:mm:ss' }),
    logFormat
  )
})

const fileTransport = new DailyRotateFile({
  filename: join(logDir, 'application-%DATE%.log'),
  datePattern: 'YYYY-MM-DD',
  zippedArchive: true,
  maxFiles: 30,
  level: _logCfg.debug ? 'debug' : 'info'
})

const logger = winston.createLogger({
  level: 'debug',
  levels: { error: 0, warn: 1, info: 2, debug: 3 },
  format: combine(
    splat(),
    timestamp({ format: 'YYYY-MM-DD HH:mm:ss' }),
    logFormat
  ),
  transports: [consoleTransport, fileTransport]
})

/** 콘솔 로그 출력 전체 on/off */
export function setLogEnabled(enabled) {
  consoleTransport.silent = !enabled
}

/** debug 레벨 노출 on/off — 콘솔과 파일 모두 적용 */
export function setLogDebug(debug) {
  consoleTransport.level = debug ? 'debug' : 'info'
  fileTransport.level    = debug ? 'debug' : 'info'
}

/** system.json 다시 읽어 로그 설정 갱신 */
export function reloadLogSettings() {
  const cfg = _readLogCfg()
  setLogEnabled(cfg.enabled)
  setLogDebug(cfg.debug)
  return cfg
}

export default logger

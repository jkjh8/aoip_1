import winston from 'winston';
import DailyRotateFile from 'winston-daily-rotate-file';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const logDir    = join(__dirname, '../logs');

const { combine, timestamp, printf, colorize, splat } = winston.format;

winston.addColors({
  error: 'red',
  warn:  'yellow',
  info:  'green',
  debug: 'blue'
});

const logFormat = printf(({ timestamp, level, message, stack }) =>
  `${timestamp} [${level}]: ${message}${stack ? ' ' + stack : ''}`
);

const logger = winston.createLogger({
  level: 'debug',
  levels: { error: 0, warn: 1, info: 2, debug: 3 },
  format: combine(
    splat(),
    timestamp({ format: 'YYYY-MM-DD HH:mm:ss' }),
    logFormat
  ),
  transports: [
    new winston.transports.Console({
      level: 'debug',
      format: combine(
        splat(),
        colorize({ all: true }),
        timestamp({ format: 'YYYY-MM-DD HH:mm:ss' }),
        logFormat
      )
    }),
    new DailyRotateFile({
      filename:     join(logDir, 'application-%DATE%.log'),
      datePattern:  'YYYY-MM-DD',
      zippedArchive: true,
      maxFiles:     30
    })
  ]
});

export default logger;

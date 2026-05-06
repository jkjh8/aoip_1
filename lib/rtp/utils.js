import { getConfig, saveConfig } from '../config.js'

export const RTP_IN_STATS_RE =
  /codec=(.+?)\s+bufMs=(\d+)\s+packets=(\d+)\s+drops=(\d+)\s+srcIp=(\S+)\s+srcPort=(\d+)\s+bitrateKbps=(\d+)/

export function applyRtpInStats(stats, line) {
  const m = line.match(RTP_IN_STATS_RE)
  if (!m) return
  stats.codec       = m[1]
  stats.bufUsedMs   = Number(m[2])
  stats.packets     = Number(m[3])
  stats.drops       = Number(m[4])
  stats.srcIp       = m[5] === 'none' ? null : m[5]
  stats.srcPort     = Number(m[6]) || null
  stats.bitrateKbps = Number(m[7])
}

/** POSIX shm_open 이름 생성 (/ 로 시작) */
export function shmName(key) {
  return `/${key}`
}

export function saveAudioConfig(updatedCfg) {
  const raw = getConfig()
  const idx = (raw.rtp_streams ?? []).findIndex((s) => s.client === updatedCfg.client)
  if (idx >= 0) {
    const keep = [
      'type', 'name', 'client', 'port', 'channels', 'protocol',
      'sampleRate', 'bufferMs', 'codec', 'bitrate', 'targets', 'enabled',
    ]
    keep.forEach((k) => {
      if (updatedCfg[k] !== undefined) raw.rtp_streams[idx][k] = updatedCfg[k]
    })
  }
  saveConfig()
}

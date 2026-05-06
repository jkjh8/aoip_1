export * from './streams.js'
export { streamEvents } from './streams.js'

import { getRtpStreamStatus } from './streams.js'
import { startRtpStreams, waitForRtpStreamsReady } from './streams.js'
import { getAllChannelDefs, setChannelActive } from '../channels/index.js'
import logger from '../logger.js'

export async function startupRtp(config) {
  if (!config.rtp_streams?.length) return

  const all     = config.rtp_streams
  const enabled = all.filter(s => s.enabled !== false)
  logger.info('[startup] RTP streams: %d total, %d enabled', all.length, enabled.length)
  try { startRtpStreams(all) } catch (e) { logger.warn('[startup] rtp_streams:', e.message) }
  try { await waitForRtpStreamsReady(6000) }
  catch (e) { logger.warn('[startup] rtp_streams ready timeout: %s', e.message) }

  // 실제 running 상태 기준으로 채널 활성화
  const defs = getAllChannelDefs()
  const statusList = getRtpStreamStatus()
  for (const s of all) {
    const st = statusList.find(ss => ss.client === s.client)
    const active = st?.running === true
    const inCh  = defs.inputs.find(c => c.source?.type === 'rtp' && c.source.name === s.client)
    const outCh = defs.outputs.find(c => c.source?.type === 'rtp' && c.source.name === s.client)
    if (inCh)  try { setChannelActive('input',  inCh.id,  active) } catch {}
    if (outCh) try { setChannelActive('output', outCh.id, active) } catch {}
  }
}


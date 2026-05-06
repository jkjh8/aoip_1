export * from './streams.js'
export { streamEvents } from './streams.js'

import { getRtpStreamStatus } from './streams.js'
import { startRtpStreams, waitForRtpStreamsReady } from './streams.js'
import logger from '../logger.js'

export async function startupRtp(config) {
  if (config.rtp_streams?.length) {
    logger.info('[startup] Starting RTP streams (%d)...', config.rtp_streams.length);
    try { startRtpStreams(config.rtp_streams); } catch (e) { logger.warn('[startup] rtp_streams:', e.message); }
    try { await waitForRtpStreamsReady(6000); }
    catch (e) { logger.warn('[startup] rtp_streams ready timeout: %s', e.message); }
  }
}

export function getGstStatus() {
  return {
    rtpStreams: getRtpStreamStatus(),
  }
}

export function getRxStats() {
  return getRtpStreamStatus()
    .filter(s => s.type === 'rtp_in')
    .map(s => ({ client: s.client, name: s.name, ...s.stats }))
}

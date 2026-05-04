export * from './rx.js'
export * from './tx.js'
export * from './streams.js'

import { isRxRunning, getRxPort, rxStats as _rxStats } from './rx.js'
import { isTxRunning, getTxTargets } from './tx.js'
import { getRtpStreamStatus } from './streams.js'

let txCodecRef = 'mp3'
let txBitrateRef = 320

import { startRxPipeline, waitForRxReady } from './rx.js'
import { startTxClient, waitForTxReady }  from './tx.js'
import { startRtpStreams, waitForRtpStreamsReady } from './streams.js'
import logger from '../logger.js'

export async function startupRtp(config) {
  if (config.rtp_streams?.length) {
    logger.info('[startup] Starting RTP streams (%d)...', config.rtp_streams.length);
    try { startRtpStreams(config.rtp_streams); } catch (e) { logger.warn('[startup] rtp_streams:', e.message); }
    try { await waitForRtpStreamsReady(6000); }
    catch (e) { logger.warn('[startup] rtp_streams ready timeout: %s', e.message); }
  } else if (config.rtp) {
    logger.info('[startup] Starting GStreamer RTP (legacy)...');
    try { startRxPipeline(config.rtp.input); } catch (e) { logger.warn('[startup] gst rx:', e.message); }
    try { startTxClient({ channels: 2 }); }   catch (e) { logger.warn('[startup] rtp_send:', e.message); }
    try { await Promise.all([waitForRxReady(6000), waitForTxReady(6000)]); }
    catch (e) { logger.warn('[startup] rtp ready timeout: %s', e.message); }
  }
}

export function getGstStatus() {
  return {
    rx: {
      running:  isRxRunning(),
      port:     getRxPort(),
      codec:    _rxStats.codec,
    },
    tx: {
      running:  isTxRunning(),
      targets:  getTxTargets(),
      codec:    txCodecRef,
      bitrate:  txBitrateRef,
    },
    rtpStreams: getRtpStreamStatus(),
  }
}

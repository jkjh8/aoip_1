export * from './rx.js'
export * from './tx.js'
export * from './streams.js'

import { isRxRunning, getRxPort, rxStats as _rxStats } from './rx.js'
import { isTxRunning, getTxTargets } from './tx.js'
import { getRtpStreamStatus } from './streams.js'

let txCodecRef = 'mp3'
let txBitrateRef = 320

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

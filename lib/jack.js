/*
 * jack.js — 라우팅 위임 모듈
 *
 * JACK 데몬은 제거되었고 모든 라우팅은 aoip_engine이 담당.
 * connect/disconnect는 dsp.js의 route add/remove 명령으로 위임.
 * index.js startup 코드와의 호환성을 위해 함수 시그니처 유지.
 */
import logger from './logger.js';

/* dsp.js가 등록하는 라우팅 콜백 */
let _onConnect    = null;
let _onDisconnect = null;

export function setJackRouteFns(connectFn, disconnectFn) {
  _onConnect    = connectFn;
  _onDisconnect = disconnectFn;
}

export async function connect(src, dst) {
  if (_onConnect) {
    try { await _onConnect(src, dst); } catch (e) {
      logger.debug('[route] connect %s→%s: %s', src, dst, e.message);
    }
  }
}

export async function disconnect(src, dst) {
  if (_onDisconnect) {
    try { await _onDisconnect(src, dst); } catch (e) {
      logger.debug('[route] disconnect %s→%s: %s', src, dst, e.message);
    }
  }
}

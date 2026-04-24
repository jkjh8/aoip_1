/*
 * jack.js — JACK 제거 후 stub 모듈
 *
 * 모든 JACK 관련 함수는 no-op 또는 빈 결과를 반환.
 * connect/disconnect는 aoip_engine의 route add/remove 명령으로 위임.
 *
 * 기존 API 시그니처 그대로 유지 (프론트엔드/라우터 코드 무수정).
 */
import logger from './logger.js';

/* ── 라우팅 콜백 (dsp.js가 등록) ─────────────────────────── */
let _onConnect    = null;
let _onDisconnect = null;

export function setJackRouteFns(connectFn, disconnectFn) {
  _onConnect    = connectFn;
  _onDisconnect = disconnectFn;
}

/* ── 크래시/복구 핸들러 (index.js가 등록, 더 이상 사용 안 함) ── */
export function setJackCrashHandler()   {}
export function setJackRecoverHandler() {}

/* ── jackd 프로세스 관리 (모두 no-op) ────────────────────── */
export function startJack()          {}
export function stopJack()           {}
export function isJackRunning()      { return false; }
export function setJackReady()       {}

export function checkJackAlive()     { return Promise.resolve(false); }
export function killExistingJack()   { return Promise.resolve(); }
export function waitForJack()        { return Promise.resolve(); }

/* ── JACK 포트/커넥션 조회 ────────────────────────────────── */
export function getPorts()       { return Promise.resolve([]); }
export function getConnections() { return Promise.resolve([]); }

/* ── 포트 연결 / 해제 ─────────────────────────────────────── */
/**
 * JACK 포트 연결 → aoip_engine route add 위임.
 * src/dst가 하드웨어 포트(system:capture 등)이면 내부 라우팅 불필요 → no-op.
 */
export async function connect(src, dst) {
  if (_onConnect) {
    try { await _onConnect(src, dst); } catch (e) {
      logger.debug('[jack stub] connect %s→%s: %s', src, dst, e.message);
    }
  }
}

export async function disconnect(src, dst) {
  if (_onDisconnect) {
    try { await _onDisconnect(src, dst); } catch (e) {
      logger.debug('[jack stub] disconnect %s→%s: %s', src, dst, e.message);
    }
  }
}

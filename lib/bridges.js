/*
 * bridges.js — ALSA 브릿지 관리 (aoip_engine stdin 명령 방식)
 *
 * 기존 audio_in/audio_out/zita-a2j/zita-j2a 프로세스 spawn 제거.
 * 대신 aoip_engine의 `bridge add_in`, `bridge add_out` 명령으로 ALSA 장치 등록.
 */
import logger from './logger.js';
import { getBridgeChStart } from './channels/index.js';
import { getConfig } from './config.js';
import { sendToEngine, waitForEngineReady, addEngineRestartListener } from './dsp/index.js';

/* 엔진 재시작 시 bridge map 리셋 — 재등록 시 add_in/add_out 사용하도록 */
addEngineRestartListener(() => {
  for (const [name, entry] of _bridgeMap) {
    _bridgeMap.set(name, { ...entry, registered: false, stopped: false });
  }
  logger.info('[bridges] engine restarted — bridge map reset for re-registration');
});

/* 등록된 브릿지 상태 추적 */
const _bridgeMap = new Map(); // name → { cfg, registered }

function _getChStart(cfg) {
  const dir = cfg.type === 'audio_out' || cfg.type === 'zita_out' ? 'out' : 'in';
  return getBridgeChStart(cfg.name, dir);
}

export async function startBridges(bridges) {
  try { await waitForEngineReady(8000); } catch (e) {
    logger.warn('[bridges] aoip_engine not ready: %s', e.message);
  }

  for (const cfg of (bridges ?? [])) {
    if (cfg.enabled === false) {
      logger.info('[bridges] %s is disabled, skipping', cfg.name);
      continue;
    }
    _registerBridge(cfg, _getChStart(cfg));
  }
}

function _safeName(name) { return name.replace(/\s+/g, '_'); }

function _registerBridge(cfg, chStart) {
  const entry = _bridgeMap.get(cfg.name);
  if (entry?.registered) return;

  const sn = entry?.safeName ?? _safeName(cfg.name);

  if (entry?.stopped) {
    logger.info('[bridges] restarting %s', cfg.name);
    sendToEngine(`bridge start ${sn}`);
  } else {
    const sub = cfg.type === 'audio_in'  ? 'add_in'  :
                cfg.type === 'audio_out' ? 'add_out' : 'add';
    const cmd = `bridge ${sub} ${sn} ${cfg.device} ${cfg.rate ?? 48000} ${cfg.period ?? 512} ${cfg.periods ?? 8} ${cfg.channels ?? 2} ${chStart}`;
    logger.info('[bridges] registering %s: %s', cfg.name, cmd);
    sendToEngine(cmd);
  }
  _bridgeMap.set(cfg.name, { cfg, registered: true, stopped: false, safeName: sn });
}

export function getBridgeStatus() {
  return Array.from(_bridgeMap.entries()).map(([name, { registered }]) => ({
    name,
    running: registered,
    pid: null,
  }));
}

export async function startupBridges() {
  const config = getConfig();
  try { await startBridges(config.bridges); } catch (e) { logger.warn('[startup] bridges:', e.message); }
}

export async function reregisterBridges() {
  const config = getConfig();
  try { await startBridges(config.bridges); } catch (e) { logger.warn('[startup] bridges restart: %s', e.message); }
}

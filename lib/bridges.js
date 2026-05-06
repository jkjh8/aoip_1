/*
 * bridges.js — ALSA 브릿지 관리 (aoip_engine stdin 명령 방식)
 *
 * 기존 audio_in/audio_out/zita-a2j/zita-j2a 프로세스 spawn 제거.
 * 대신 aoip_engine의 `bridge add_in`, `bridge add_out` 명령으로 ALSA 장치 등록.
 *
 * USB 가젯 watcher: UDC 연결 감시 → 브릿지 시작/중지 명령 전송.
 */
import { readdirSync, readFileSync } from 'fs';
import logger from './logger.js';
import { getBridgeChStart, setBridgeEnabled, getSavedRoutes, getBridgeChannelDef } from './channels/index.js';
import { connect, disconnect } from './dsp/index.js';
import { getConfig, saveConfig } from './config.js';
import { sendToEngine, waitForEngineReady, addEngineRestartListener } from './dsp/index.js';

/* 엔진 재시작 시 bridge map 리셋 — 재등록 시 add_in/add_out 사용하도록 */
addEngineRestartListener(() => {
  for (const [name, entry] of _bridgeMap) {
    _bridgeMap.set(name, { ...entry, registered: false, stopped: false });
  }
  logger.info('[bridges] engine restarted — bridge map reset for re-registration');
});

const UDC_POLL_MS = 2000;

/** 현재 UDC 연결 상태 확인 ('configured' = USB 호스트 연결됨) */
export function isUdcConnected() {
  try {
    const entries = readdirSync('/sys/class/udc');
    if (!entries.length) return false;
    const state = readFileSync(`/sys/class/udc/${entries[0]}/state`, 'utf8').trim();
    return state === 'configured';
  } catch {
    return false;
  }
}

/* 등록된 브릿지 상태 추적 */
const _bridgeMap = new Map(); // name → { cfg, registered }

/**
 * 브릿지의 ch_start를 channels.js에서 가져옴.
 * type: audio_in → in방향, audio_out → out방향, 그 외(양방향) → in방향 기준
 */
function _getChStart(cfg) {
  const dir = cfg.type === 'audio_out' || cfg.type === 'zita_out' ? 'out' : 'in';
  return getBridgeChStart(cfg.name, dir);
}

/**
 * 모든 비-USB 브릿지 등록 (aoip_engine 시작 후 호출).
 */
export async function startBridges(bridges) {
  try { await waitForEngineReady(8000); } catch (e) {
    logger.warn('[bridges] aoip_engine not ready: %s', e.message);
  }

  for (const cfg of (bridges ?? [])) {
    if (cfg.enabled === false) {
      logger.info('[bridges] %s is disabled, skipping', cfg.name);
      continue;
    }
    if (cfg.usb_gadget) continue;  // watcher가 담당
    _registerBridge(cfg, _getChStart(cfg));
  }
}

/* aoip_engine 명령은 공백 구분자이므로 이름의 공백을 _로 치환 */
function _safeName(name) { return name.replace(/\s+/g, '_'); }

function _registerBridge(cfg, chStart) {
  const entry = _bridgeMap.get(cfg.name);
  if (entry?.registered) return;

  const sn = entry?.safeName ?? _safeName(cfg.name);

  if (entry?.stopped) {
    // aoip_engine에 이미 등록된 장치 — start 명령으로 재사용
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

/**
 * USB 가젯 UDC 상태를 감시하며 연결 시 브릿지를 자동 시작/정지.
 */
export function startUsbGadgetWatcher() {
  let prevConnected = isUdcConnected();
  if (prevConnected) {
    logger.info('[bridges] USB host already connected at boot — starting bridges');
    _startUsbBridges();
  } else {
    logger.info('[bridges] USB gadget watcher started — waiting for host connection');
  }

  setInterval(() => {
    const connected = isUdcConnected();
    if (connected !== prevConnected) {
      prevConnected = connected;
      if (connected) {
        logger.info('[bridges] USB host connected — starting bridges');
        _startUsbBridges();
      } else {
        logger.info('[bridges] USB host disconnected — stopping USB bridges');
        _stopUsbBridges();
      }
    }
  }, UDC_POLL_MS);
}

async function _startUsbBridges() {
  try { await waitForEngineReady(8000); } catch (e) {
    logger.warn('[bridges] engine not ready for USB bridges: %s', e.message);
    return;
  }
  const config = getConfig();
  for (const cfg of (config.bridges ?? []).filter(b => b.usb_gadget && b.enabled !== false)) {
    _registerBridge(cfg, _getChStart(cfg));
    setBridgeEnabled(cfg.name, true);
    _restoreUsbRoutes(cfg.name);
  }
}

function _stopUsbBridges() {
  const config = getConfig();
  for (const cfg of (config.bridges ?? []).filter(b => b.usb_gadget)) {
    const entry = _bridgeMap.get(cfg.name);
    if (entry?.registered) {
      _disconnectUsbRoutes(cfg.name);
      sendToEngine(`bridge stop ${entry.safeName ?? _safeName(cfg.name)}`);
      _bridgeMap.set(cfg.name, { ...entry, registered: false, stopped: true });
      logger.info('[bridges] stopped USB bridge %s', cfg.name);
    }
    setBridgeEnabled(cfg.name, false);
  }
}

function _disconnectUsbRoutes(bridgeName) {
  const def = getBridgeChannelDef(bridgeName);
  if (!def) return;
  const inPorts  = new Set(def.inputs.map(c => c.jackPort));
  const outPorts = new Set(def.outputs.map(c => c.jackPort));
  for (const { src, dst } of getSavedRoutes()) {
    if (inPorts.has(src) || outPorts.has(dst)) disconnect(src, dst);
  }
}

function _restoreUsbRoutes(bridgeName) {
  const def = getBridgeChannelDef(bridgeName);
  if (!def) return;
  const inPorts  = new Set(def.inputs.map(c => c.jackPort));
  const outPorts = new Set(def.outputs.map(c => c.jackPort));
  for (const { src, dst } of getSavedRoutes()) {
    if (inPorts.has(src) || outPorts.has(dst)) connect(src, dst);
  }
}


/** 브릿지 상태 목록 */
export function getBridgeStatus() {
  return Array.from(_bridgeMap.entries()).map(([name, { registered }]) => ({
    name,
    running: registered,
    pid: null,
  }));
}

/** USB 가젯 재시작 (period 변경 등) */
export function restartUsbBridges() {
  _stopUsbBridges();
  if (isUdcConnected()) {
    setTimeout(_startUsbBridges, 1000);
  }
}

/* ── USB 가젯 토글 ─────────────────────────────────────── */

export async function setUsbGadgetEnabled(enable) {
  const config  = getConfig();
  const usbCfgs = (config.bridges ?? []).filter(b => b.usb_gadget);
  if (!usbCfgs.length) throw new Error('usb_gadget bridge config not found');

  for (const cfg of usbCfgs) cfg.enabled = enable;
  saveConfig();
  logger.info('[bridges] usb_gadget enabled=%s', enable);

  if (!enable && isUdcConnected()) {
    _stopUsbBridges();
  } else if (enable && isUdcConnected()) {
    _startUsbBridges();
  }
  return true;
}

export function getUsbGadgetEnabled() {
  try {
    const usbCfgs = (getConfig().bridges ?? []).filter(b => b.usb_gadget);
    return usbCfgs.length > 0 && usbCfgs.every(b => b.enabled !== false);
  } catch {
    return false;
  }
}

export async function startupBridges() {
  const config = getConfig();
  try { await startBridges(config.bridges); } catch (e) { logger.warn('[startup] bridges:', e.message); }
  startUsbGadgetWatcher();
}

export async function reregisterBridges() {
  const config = getConfig();
  try { await startBridges(config.bridges); } catch (e) { logger.warn('[startup] bridges restart: %s', e.message); }
}

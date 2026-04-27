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
import { getBridgeChStart } from './channels.js';
import { getConfig, saveConfig } from './config.js';
import { sendToEngine, waitForEngineReady } from './dsp.js';

const UDC_POLL_MS = 2000;

let _startupComplete = false;
export function notifyBridgeStartupComplete() { _startupComplete = true; }

let _onBridgeReady = null;
export function setOnBridgeReady(fn) { _onBridgeReady = fn; }

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
  if (_bridgeMap.get(cfg.name)?.registered) return;

  const sub  = cfg.type === 'audio_in'  ? 'add_in'  :
               cfg.type === 'audio_out' ? 'add_out' : 'add';
  const sn   = _safeName(cfg.name);
  const cmd  = `bridge ${sub} ${sn} ${cfg.device} ${cfg.rate ?? 48000} ${cfg.period ?? 512} ${cfg.periods ?? 8} ${cfg.channels ?? 2} ${chStart}`;
  logger.info('[bridges] registering %s: %s', cfg.name, cmd);
  sendToEngine(cmd);
  _bridgeMap.set(cfg.name, { cfg, registered: true, safeName: sn });
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
    if (connected) _startUsbBridges();
  }, UDC_POLL_MS);
}

function _startUsbBridges() {
  const config = getConfig();
  for (const cfg of (config.bridges ?? []).filter(b => b.usb_gadget && b.enabled !== false)) {
    _registerBridge(cfg, _getChStart(cfg));
  }
}

function _stopUsbBridges() {
  const config = getConfig();
  for (const cfg of (config.bridges ?? []).filter(b => b.usb_gadget)) {
    const entry = _bridgeMap.get(cfg.name);
    if (entry?.registered) {
      sendToEngine(`bridge stop ${entry.safeName ?? _safeName(cfg.name)}`);
      _bridgeMap.delete(cfg.name);
      logger.info('[bridges] stopped USB bridge %s', cfg.name);
    }
  }
}

/** 고아 프로세스 정리 (이전 audio_in/out 프로세스가 남아 있을 경우 제거) */
export async function killOrphanBridges() {
  const { exec } = await import('child_process');
  const binaries = ['audio_in', 'audio_out', 'zita-a2j', 'zita-j2a'];
  await Promise.all(binaries.map(bin =>
    new Promise(r => exec(`pkill -x ${bin}`, r))
  ));
  await new Promise(r => setTimeout(r, 300));
}

/** 전체 브릿지 중지 */
export function stopBridges() {
  for (const [name, entry] of _bridgeMap) {
    sendToEngine(`bridge stop ${entry.safeName ?? _safeName(name)}`);
    logger.info('[bridges] stopped %s', name);
  }
  _bridgeMap.clear();
}

/** 브릿지 상태 목록 */
export function getBridgeStatus() {
  return Array.from(_bridgeMap.entries()).map(([name, { registered }]) => ({
    name,
    running: registered,
    pid: null,
  }));
}

/** 특정 브릿지 재시작 */
export function restartBridge(name) {
  const entry = _bridgeMap.get(name);
  if (!entry) return false;
  sendToEngine(`bridge stop ${name}`);
  setTimeout(() => sendToEngine(`bridge start ${name}`), 1000);
  logger.warn('[bridges] restarting %s', name);
  return true;
}

/** USB 가젯 재시작 (period 변경 등) */
export function restartUsbBridges() {
  const config = getConfig();
  for (const cfg of (config.bridges ?? []).filter(b => b.usb_gadget)) {
    if (_bridgeMap.get(cfg.name)?.registered) {
      sendToEngine(`bridge stop ${cfg.name}`);
      _bridgeMap.delete(cfg.name);
      logger.info('[bridges] stopped USB bridge %s for restart', cfg.name);
    }
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

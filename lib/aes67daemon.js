/**
 * AES67 daemon HTTP API client.
 * The daemon lifecycle is managed by systemd (aes67.service).
 */

import { exec, spawn } from 'child_process';
import { promisify } from 'util';
import { EventEmitter } from 'events';
import { watch, readFile } from 'fs';
import logger from './logger.js';

const execAsync = promisify(exec);
const DAEMON_URL   = 'http://127.0.0.1:8080';
const SERVICE      = 'aes67-daemon.service';
const STATUS_FILE  = '/home/kjh/aes67/status.json';
const DAEMON_CONF  = '/home/kjh/aes67/daemon.conf';

export const daemonEvents = new EventEmitter();

// ─── In-memory state ────────────────────────────────────
let _sources   = null;
let _sinks     = null;
let _ptpStatus = null;
const _sdpCache = new Map(); // sinkId → SDP string

// ─── status.json file watcher ───────────────────────────
// daemon이 sources/sinks 변경 시 status.json을 갱신함.
// REST 호출 없이 파일에서 직접 읽어 캐시를 갱신.
async function _loadStatusFile() {
  try {
    const raw = await readFile(STATUS_FILE, 'utf8');
    const data = JSON.parse(raw);
    let changed = false;

    if (Array.isArray(data.sources)) {
      _sources = data.sources;
      changed = true;
      daemonEvents.emit('sources:changed', _sources);
    }
    if (Array.isArray(data.sinks)) {
      _sinks = data.sinks;
      changed = true;
      daemonEvents.emit('sinks:changed', _sinks);
    }
    if (changed) logger.debug('[aes67] status.json loaded');
  } catch (e) {
    logger.debug('[aes67] status.json read failed: %s', e.message);
  }
}

let _fileWatchTimer = null;

export function startStatusFileWatcher() {
  _loadStatusFile(); // 초기 로드

  try {
    watch(STATUS_FILE, () => {
      // rename/truncate 방식 write 대응: 짧은 디바운스
      clearTimeout(_fileWatchTimer);
      _fileWatchTimer = setTimeout(_loadStatusFile, 150);
    });
    logger.info('[aes67] watching %s', STATUS_FILE);
  } catch (e) {
    logger.warn('[aes67] cannot watch status.json: %s — falling back to REST', e.message);
  }
}

// ─── Debounced fetchers ──────────────────────────────────
// 로그 이벤트 감지 시 호출됨. 파일 watcher가 동작 중이면
// 파일에서 읽고, 실패 시만 REST fallback.
let _srcTimer = null;
let _snkTimer = null;

function _scheduleFetchSources() {
  clearTimeout(_srcTimer);
  _srcTimer = setTimeout(async () => {
    try {
      const raw = await readFile(STATUS_FILE, 'utf8');
      const data = JSON.parse(raw);
      if (Array.isArray(data.sources)) {
        _sources = data.sources;
        daemonEvents.emit('sources:changed', _sources);
        return;
      }
    } catch { }
    // fallback to REST
    try {
      _sources = await api.get('/api/sources');
      daemonEvents.emit('sources:changed', _sources);
    } catch { }
  }, 300);
}

function _scheduleFetchSinks() {
  clearTimeout(_snkTimer);
  _snkTimer = setTimeout(async () => {
    try {
      const raw = await readFile(STATUS_FILE, 'utf8');
      const data = JSON.parse(raw);
      if (Array.isArray(data.sinks)) {
        _sinks = data.sinks;
        daemonEvents.emit('sinks:changed', _sinks);
        return;
      }
    } catch { }
    // fallback to REST
    try {
      _sinks = await api.get('/api/sinks');
      daemonEvents.emit('sinks:changed', _sinks);
    } catch { }
  }, 300);
}

// ─── Log line parser ─────────────────────────────────────
// SDP capture state machine
let _sdpSinkId  = null;
let _sdpCapture = false;
let _sdpLines   = [];

const _SDP_FIELD = /^[vosibtmackerpzu]=/;

function _finalizeSdp() {
  if (_sdpSinkId !== null && _sdpLines.length > 0) {
    _sdpCache.set(_sdpSinkId, _sdpLines.join('\r\n'));
    logger.info('[aes67] cached SDP for sink %d', _sdpSinkId);
  }
  _sdpCapture = false;
  _sdpSinkId  = null;
  _sdpLines   = [];
}

function _parseDaemonLine(line) {
  // SDP content lines (no timestamp prefix)
  if (_sdpCapture) {
    if (_SDP_FIELD.test(line)) {
      _sdpLines.push(line);
      return;
    }
    _finalizeSdp();
    // fall through to parse this non-SDP line
  }

  // PTP status change — fetch full status from REST (includes gmid, jitter)
  const ptpMatch = line.match(/new PTP clock status (\w+)/);
  if (ptpMatch) {
    api.get('/api/ptp/status').then(st => {
      _ptpStatus = st;
      daemonEvents.emit('ptp:changed', _ptpStatus);
    }).catch(() => {
      _ptpStatus = { status: ptpMatch[1] };
      daemonEvents.emit('ptp:changed', _ptpStatus);
    });
    return;
  }

  // Sink SDP incoming — note sink id and trigger sinks refresh
  const sdpChangeMatch = line.match(/session_manager:: sink (\d+) SDP change detected/);
  if (sdpChangeMatch) {
    _sdpSinkId  = parseInt(sdpChangeMatch[1], 10);
    _sdpCapture = false;
    _sdpLines   = [];
    _scheduleFetchSinks();
    return;
  }

  // Start of SDP block
  if (line.includes('session_manager:: using SDP')) {
    _sdpCapture = true;
    _sdpLines   = [];
    return;
  }

  // Sink added
  if (line.includes('session_manager:: added sink')) {
    _scheduleFetchSinks();
    return;
  }

  // SAP source added / removed
  if (line.includes('browser:: SAP source') || line.includes('browser:: removing SAP source')) {
    _scheduleFetchSources();
  }
}

let _journalProc = null;

export function startDaemonLogForwarder() {
  if (_journalProc) return;

  _journalProc = spawn('journalctl', ['-f', '-u', SERVICE, '-o', 'cat', '--no-pager'], {
    stdio: ['ignore', 'pipe', 'ignore'],
  });

  const _NOISY = [
    'sap::announcement',
    'next SAP announcements in',
  ];

  let _buf = '';
  _journalProc.stdout.on('data', (d) => {
    _buf += d.toString();
    let nl;
    while ((nl = _buf.indexOf('\n')) >= 0) {
      const line = _buf.slice(0, nl).trim();
      _buf = _buf.slice(nl + 1);
      if (!line) continue;
      if (_NOISY.some(p => line.includes(p))) continue;
      _parseDaemonLine(line);
      logger.info('[aes67] %s', line);
    }
  });

  _journalProc.on('exit', () => {
    _journalProc = null;
  });

  logger.info('[aes67] log forwarder started');
}


let _daemonStatus = { running: false, ready: false };

export function getAes67State() {
  return {
    status:    _daemonStatus,
    sources:   _sources   ?? [],
    sinks:     _sinks     ?? [],
    ptpStatus: _ptpStatus ?? {},
  };
}

export async function getDaemonStatus() {
  let active = false;
  try {
    const { stdout } = await execAsync(`systemctl is-active ${SERVICE}`);
    active = stdout.trim() === 'active';
  } catch { active = false; }
  _daemonStatus = { running: active, ready: active };
  return { ..._daemonStatus, url: DAEMON_URL };
}

// ─── HTTP API 헬퍼 ───────────────────────────────────────

async function _fetch(method, path, body) {
  const opts = {
    method,
    headers: { 'Content-Type': 'application/json' },
    signal: AbortSignal.timeout(5000),
  };
  if (body !== undefined) opts.body = JSON.stringify(body);

  const res = await fetch(`${DAEMON_URL}${path}`, opts);
  if (!res.ok) {
    const text = await res.text().catch(() => '');
    throw new Error(`daemon ${method} ${path} → ${res.status}: ${text}`);
  }
  const ct = res.headers.get('content-type') ?? '';
  if (ct.includes('application/json')) return res.json();
  return res.text();
}

const api = {
  get:    (path)       => _fetch('GET',    path),
  post:   (path, body) => _fetch('POST',   path, body),
  put:    (path, body) => _fetch('PUT',    path, body),
  delete: (path)       => _fetch('DELETE', path),
};

// ─── 설정 (daemon.conf 직접 읽기, 쓰기만 REST) ──────────
export async function getConfig() {
  const raw = await readFile(DAEMON_CONF, 'utf8');
  return JSON.parse(raw);
}
export const setConfig    = (body) => api.post('/api/config', body);

// ─── PTP ─────────────────────────────────────────────────
export async function getPtpConfig() {
  const raw = await readFile(DAEMON_CONF, 'utf8');
  const conf = JSON.parse(raw);
  return { domain: conf.ptp_domain, dscp: conf.ptp_dscp };
}
export const setPtpConfig = (body) => api.post('/api/ptp/config', body);

// ─── On-demand cache (sources / sinks / ptp_status) ─────
// startStatusFileWatcher()가 초기 로드를 담당.
// 캐시가 비어있으면 파일 → REST 순으로 fallback.

export async function getSources() {
  if (_sources) return _sources;
  try {
    const raw = await readFile(STATUS_FILE, 'utf8');
    const data = JSON.parse(raw);
    if (Array.isArray(data.sources)) return (_sources = data.sources);
  } catch { }
  return (_sources = await api.get('/api/sources'));
}

export async function getSinks() {
  if (_sinks) return _sinks;
  try {
    const raw = await readFile(STATUS_FILE, 'utf8');
    const data = JSON.parse(raw);
    if (Array.isArray(data.sinks)) return (_sinks = data.sinks);
  } catch { }
  return (_sinks = await api.get('/api/sinks'));
}

export const getPtpStatus = async () => {
  if (_ptpStatus?.gmid) return _ptpStatus;
  return (_ptpStatus = await api.get('/api/ptp/status'));
};

// ─── Sources ─────────────────────────────────────────────
export function invalidateSources() { _sources = null; }
export function invalidateSinks()   { _sinks   = null; }

export const addSource    = (id, body)   => api.put(`/api/source/${id}`, body);
export const removeSource = (id)         => api.delete(`/api/source/${id}`);

export async function getSourceSdp(id) {
  const key = `src_${id}`;
  if (_sdpCache.has(key)) return _sdpCache.get(key);
  const sdp = await api.get(`/api/source/sdp/${id}`);
  _sdpCache.set(key, sdp);
  return sdp;
}

// ─── Sinks ───────────────────────────────────────────────
export const addSink       = (id, body)  => api.put(`/api/sink/${id}`, body);
export const removeSink    = (id)        => api.delete(`/api/sink/${id}`);
export const getSinkStatus = (id)        => api.get(`/api/sink/status/${id}`);
export const getSinkSdp    = (id)        => _sdpCache.get(id) ?? null;

// ─── Browse (원격 AES67 소스 탐색) ───────────────────────
export const browseAll  = () => api.get('/api/browse/sources/all');
export const browseMdns = () => api.get('/api/browse/sources/mdns');
export const browseSap  = () => api.get('/api/browse/sources/sap');

// ─── Sink 패킷 오류 통계 (on-demand) ────────────────────
// 프론트 요청 시에만 조회, 플래그 전환을 감지해 누적

const _sinkStats = new Map(); // sinkId → stats object

function _makeSinkStats() {
  return {
    seq_id_errors:  0,
    ssrc_errors:    0,
    payload_errors: 0,
    sac_errors:     0,
    mute_events:    0,
    ptp_jitter_ns:  null,
    receiving:      false,
    last_flags:     null,
    started_at:     Date.now(),
    polled_at:      null,
  };
}

export async function fetchSinkStats(id) {
  if (!_sinkStats.has(id)) _sinkStats.set(id, _makeSinkStats());
  const s = _sinkStats.get(id);

  const st = await api.get(`/api/sink/status/${id}`).catch(() => null);

  if (st) {
    const f = st.sink_flags ?? {};
    if (s.last_flags) {
      if (!s.last_flags.rtp_seq_id_error      && f.rtp_seq_id_error)       { s.seq_id_errors++;  logger.warn('[aes67] sink%d seq_id_error', id); }
      if (!s.last_flags.rtp_ssrc_error         && f.rtp_ssrc_error)         { s.ssrc_errors++;    logger.warn('[aes67] sink%d ssrc_error', id); }
      if (!s.last_flags.rtp_payload_type_error && f.rtp_payload_type_error) { s.payload_errors++; logger.warn('[aes67] sink%d payload_type_error', id); }
      if (!s.last_flags.rtp_sac_error          && f.rtp_sac_error)          { s.sac_errors++;     logger.warn('[aes67] sink%d sac_error', id); }
      if (!s.last_flags.muted                  && f.muted)                  { s.mute_events++;    logger.warn('[aes67] sink%d muted', id); }
    }
    s.receiving  = !!f.receiving_rtp_packet;
    s.last_flags = f;
    s.polled_at  = Date.now();
  }

  if (_ptpStatus?.jitter != null)
    s.ptp_jitter_ns = _ptpStatus.jitter;

  return s;
}

export function resetSinkStats(id) {
  _sinkStats.set(id, _makeSinkStats());
}

// startSinkStatPoller — 하위 호환 유지용 (no-op)
export function startSinkStatPoller() {}

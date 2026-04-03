/**
 * AES67 daemon HTTP API client.
 * The daemon lifecycle is managed by systemd (aes67.service).
 */

import { exec } from 'child_process';
import { promisify } from 'util';

const execAsync = promisify(exec);
const DAEMON_URL = 'http://127.0.0.1:8080';
const SERVICE    = 'aes67.service';

// ─── systemd 제어 ────────────────────────────────────────

async function systemctl(action) {
  await execAsync(`systemctl ${action} ${SERVICE}`);
}

export async function startDaemon()   { await systemctl('start'); }
export async function stopDaemon()    { await systemctl('stop'); }
export async function restartDaemon() { await systemctl('restart'); }

export async function getDaemonStatus() {
  let active = false;
  try {
    const { stdout } = await execAsync(`systemctl is-active ${SERVICE}`);
    active = stdout.trim() === 'active';
  } catch { active = false; }

  let ready = false;
  if (active) {
    try {
      const res = await fetch(`${DAEMON_URL}/api/config`, { signal: AbortSignal.timeout(1000) });
      ready = res.ok;
    } catch { ready = false; }
  }

  return { running: active, ready, url: DAEMON_URL };
}

// ─── HTTP API 헬퍼 ───────────────────────────────────────

async function _fetch(method, path, body) {
  const opts = {
    method,
    headers: { 'Content-Type': 'application/json' },
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

// ─── 설정 ────────────────────────────────────────────────
export const getConfig    = ()     => api.get('/api/config');
export const setConfig    = (body) => api.post('/api/config', body);

// ─── PTP ─────────────────────────────────────────────────
export const getPtpConfig = ()     => api.get('/api/ptp/config');
export const setPtpConfig = (body) => api.post('/api/ptp/config', body);
export const getPtpStatus = ()     => api.get('/api/ptp/status');

// ─── Sources ─────────────────────────────────────────────
export const getSources   = ()           => api.get('/api/sources');
export const addSource    = (id, body)   => api.put(`/api/source/${id}`, body);
export const removeSource = (id)         => api.delete(`/api/source/${id}`);
export const getSourceSdp = (id)         => api.get(`/api/source/sdp/${id}`);

// ─── Sinks ───────────────────────────────────────────────
export const getSinks      = ()          => api.get('/api/sinks');
export const addSink       = (id, body)  => api.put(`/api/sink/${id}`, body);
export const removeSink    = (id)        => api.delete(`/api/sink/${id}`);
export const getSinkStatus = (id)        => api.get(`/api/sink/status/${id}`);

// ─── Browse (원격 AES67 소스 탐색) ───────────────────────
export const browseAll  = () => api.get('/api/browse/sources/all');
export const browseMdns = () => api.get('/api/browse/sources/mdns');
export const browseSap  = () => api.get('/api/browse/sources/sap');

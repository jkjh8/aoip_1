import { execSync, exec } from 'child_process';
import { readFileSync } from 'fs';
import logger from './logger.js';

// systemd-networkd .network 파일 경로 (eth0 고정)
const NETWORK_FILE = '/etc/systemd/network/20-wired.network';

/**
 * 현재 인터페이스 네트워크 정보 반환.
 * .network 파일에 Address 가 있으면 mode:'static', DHCP=yes 면 mode:'dhcp'.
 * @param {string} iface
 * @returns {{ iface: string, ip: string|null, subnet: string|null, gateway: string|null, dns: string|null, mode: 'static'|'dhcp', mac: string|null }}
 */
export function getNetworkInfo(iface = 'eth0') {
  let ip = null, prefix = null, mac = null;
  try {
    const raw = execSync(`ip -j addr show ${iface}`, { timeout: 3000 }).toString();
    const info = JSON.parse(raw)[0] ?? {};
    mac = info.address ?? null;
    const addr = (info.addr_info ?? []).find(a => a.family === 'inet');
    if (addr) { ip = addr.local; prefix = addr.prefixlen; }
  } catch { /* 인터페이스 없음 */ }

  let gateway = null;
  try {
    const routeRaw = execSync(`ip -j route show default dev ${iface}`, { timeout: 3000 }).toString();
    const routes = JSON.parse(routeRaw);
    gateway = routes[0]?.gateway ?? null;
  } catch { /* 기본 라우트 없음 */ }

  const fileConf = _parseNetworkFile();
  const mode = fileConf.dhcp ? 'dhcp' : 'static';
  const resolvedPrefix = fileConf.prefix ?? prefix;

  return {
    iface,
    ip:      fileConf.ip      ?? ip,
    subnet:  _prefixToNetmask(resolvedPrefix),
    gateway: fileConf.gateway ?? gateway,
    dns:     fileConf.dns     ?? null,
    mode,
    mac
  };
}

/**
 * Static IP 설정 후 systemd-networkd 재로드.
 * @param {{ iface?: string, ip: string, subnet?: string, gateway: string, dns?: string }} opts
 */
export async function setStaticIp({ iface = 'eth0', ip, subnet = '255.255.255.0', gateway, dns = '8.8.8.8' }) {
  if (!ip || !gateway) throw new Error('ip and gateway are required');
  _validateIp(ip);
  _validateIp(gateway);
  if (dns) _validateIp(dns);

  const prefix = _netmaskToPrefix(subnet);
  const content = `[Match]\nName=${iface}\n\n[Network]\nAddress=${ip}/${prefix}\nGateway=${gateway}\nDNS=${dns}\n`;
  await _writeNetworkFile(content);
  logger.info('[system] static IP set: %s/%s (%s) gw=%s dns=%s on %s', ip, prefix, subnet, gateway, dns, iface);

  await _reloadNetwork();
}

/**
 * DHCP 모드로 전환.
 * @param {string} iface
 */
export async function setDhcp(iface = 'eth0') {
  const content = `[Match]\nName=${iface}\n\n[Network]\nDHCP=yes\n`;
  await _writeNetworkFile(content);
  logger.info('[system] DHCP mode set on %s', iface);
  await _reloadNetwork();
}

/**
 * 시스템 재부팅 (1초 후).
 */
export function rebootSystem() {
  logger.info('[system] rebooting...');
  setTimeout(() => {
    exec('sudo systemctl reboot', (err) => {
      if (err) logger.error('[system] reboot error: %s', err.message);
    });
  }, 1000);
}

/* ── 내부 헬퍼 ──────────────────────────────────────────────── */

/** .network 파일에서 IP 설정 파싱 */
function _parseNetworkFile() {
  try {
    const content = readFileSync(NETWORK_FILE, 'utf8');
    const get = (key) => {
      const m = content.match(new RegExp(`^${key}=(.+)$`, 'm'));
      return m ? m[1].trim() : null;
    };

    const dhcp = /^DHCP=yes/m.test(content);
    const addrFull = get('Address');
    let ip = null, prefix = null;
    if (addrFull) {
      [ip, prefix] = addrFull.split('/');
      prefix = prefix ? Number(prefix) : 24;
    }
    const gateway = get('Gateway');
    const dns = get('DNS');
    return { dhcp, ip, prefix, gateway, dns };
  } catch {
    return { dhcp: false, ip: null, prefix: null, gateway: null, dns: null };
  }
}

/** prefix(CIDR) → 넷마스크 문자열 (예: 24 → '255.255.255.0') */
function _prefixToNetmask(prefix) {
  if (prefix == null) return null;
  const mask = prefix === 0 ? 0 : (~0 << (32 - prefix)) >>> 0;
  return [(mask >>> 24) & 0xff, (mask >>> 16) & 0xff, (mask >>> 8) & 0xff, mask & 0xff].join('.');
}

/** 넷마스크 문자열 → prefix(CIDR) (예: '255.255.255.0' → 24) */
function _netmaskToPrefix(netmask) {
  if (!netmask) return 24;
  return netmask.split('.').reduce((acc, octet) => {
    let n = Number(octet);
    let bits = 0;
    while (n & 0x80) { bits++; n = (n << 1) & 0xff; }
    return acc + bits;
  }, 0);
}

function _validateIp(ip) {
  const parts = ip.split('.');
  if (parts.length !== 4 || parts.some(p => isNaN(p) || Number(p) < 0 || Number(p) > 255)) {
    throw new Error(`Invalid IP address: ${ip}`);
  }
}

function _writeNetworkFile(content) {
  return new Promise((resolve, reject) => {
    const child = exec(`sudo tee ${NETWORK_FILE}`, { timeout: 5000 }, (err) => {
      if (err) reject(err); else resolve();
    });
    child.stdin.write(content);
    child.stdin.end();
  });
}

function _reloadNetwork() {
  return new Promise((resolve, reject) => {
    exec('sudo networkctl reload', { timeout: 10000 }, (err) => {
      if (err) {
        logger.error('[system] networkctl reload failed: %s', err.message);
        reject(err);
      } else {
        logger.info('[system] networkd reloaded');
        resolve();
      }
    });
  });
}

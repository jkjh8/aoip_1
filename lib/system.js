import { execSync, exec } from 'child_process';
import { readFileSync, writeFileSync } from 'fs';
import logger from './logger.js';

// systemd-networkd .network 파일 경로 (eth0 고정)
const NETWORK_FILE = '/etc/systemd/network/20-wired.network';

/**
 * 현재 인터페이스 네트워크 정보 반환.
 * .network 파일에 Address 가 있으면 mode:'static', DHCP=yes 면 mode:'dhcp'.
 * @param {string} iface
 * @returns {{ iface: string, ip: string|null, prefix: number|null, gateway: string|null, dns: string|null, mode: 'static'|'dhcp', mac: string|null }}
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

  return {
    iface,
    ip:      fileConf.ip      ?? ip,
    prefix:  fileConf.prefix  ?? prefix,
    gateway: fileConf.gateway ?? gateway,
    dns:     fileConf.dns     ?? null,
    mode,
    mac
  };
}

/**
 * Static IP 설정 후 systemd-networkd 재로드.
 * @param {{ iface?: string, ip: string, prefix?: number, gateway: string, dns?: string }} opts
 */
export async function setStaticIp({ iface = 'eth0', ip, prefix = 24, gateway, dns = '8.8.8.8' }) {
  if (!ip || !gateway) throw new Error('ip and gateway are required');
  _validateIp(ip);
  _validateIp(gateway);
  if (dns) _validateIp(dns);

  const content = `[Match]\nName=${iface}\n\n[Network]\nAddress=${ip}/${prefix}\nGateway=${gateway}\nDNS=${dns}\n`;
  writeFileSync(NETWORK_FILE, content);
  logger.info('[system] static IP set: %s/%s gw=%s dns=%s on %s', ip, prefix, gateway, dns, iface);

  await _reloadNetwork();
}

/**
 * DHCP 모드로 전환.
 * @param {string} iface
 */
export async function setDhcp(iface = 'eth0') {
  const content = `[Match]\nName=${iface}\n\n[Network]\nDHCP=yes\n`;
  writeFileSync(NETWORK_FILE, content);
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

function _validateIp(ip) {
  const parts = ip.split('.');
  if (parts.length !== 4 || parts.some(p => isNaN(p) || Number(p) < 0 || Number(p) > 255)) {
    throw new Error(`Invalid IP address: ${ip}`);
  }
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

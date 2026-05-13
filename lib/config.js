import { readFileSync, writeFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __dirname   = dirname(fileURLToPath(import.meta.url));
const CONFIG_PATH = join(__dirname, '../config/audio.json');

let _config = JSON.parse(readFileSync(CONFIG_PATH, 'utf8'));

/** 메모리에 올라간 설정 객체를 반환. 직접 변이(mutation) 후 saveConfig() 호출. */
export function getConfig() {
  return _config;
}

/** 파일에서 설정을 다시 읽어 메모리를 갱신. aoip_engine 재시작 시 호출. */
export function reloadConfig() {
  const fresh = JSON.parse(readFileSync(CONFIG_PATH, 'utf8'));
  Object.assign(_config, fresh);
  return _config;
}

/** 현재 메모리 설정을 파일에 저장. */
export function saveConfig() {
  writeFileSync(CONFIG_PATH, JSON.stringify(_config, null, 2), 'utf8');
}

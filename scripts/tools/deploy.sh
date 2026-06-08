#!/usr/bin/env bash
# 동기화 후 원격 기기에서 Node.js 앱 재시작
set -e

REMOTE_HOST="kjh@192.168.10.97"
REMOTE_DIR="/home/kjh/aoip_1/"
LOCAL_DIR="/mnt/c/Users/kjh/Desktop/DEV/aoip_1/"

EXCLUDES=(
  --exclude='.git/'
  --exclude='.claude/'
  --exclude='node_modules/'
  --exclude='.vscode/'
  --exclude='*.log'
  --exclude='config/channels.json'
)

echo "[deploy] 파일 동기화..."
rsync -avz --delete "${EXCLUDES[@]}" "$LOCAL_DIR" "${REMOTE_HOST}:${REMOTE_DIR}"

echo "[deploy] 빌드..."
ssh "$REMOTE_HOST" "cd ${REMOTE_DIR}scripts && make -j\$(nproc) 2>&1" || \
  echo "[deploy] 빌드 실패"

echo "[deploy] 앱 재시작..."
ssh "$REMOTE_HOST" "cd ${REMOTE_DIR} && pm2 restart aoip_1 2>/dev/null || \
  (pkill -f 'node index.js' 2>/dev/null; nohup node index.js > /tmp/aoip.log 2>&1 &) && \
  echo '앱 재시작 완료'"

echo "[deploy] CPU 배치 적용 (3초 대기)..."
ssh "$REMOTE_HOST" "sleep 3 && sudo ${REMOTE_DIR}scripts/tools/cpu_affinity.sh apply" || \
  echo "[deploy] CPU 배치 실패 — sudo 권한 또는 NOPASSWD 설정 확인"

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

echo "[deploy] 앱 재시작..."
# pm2 사용 시: pm2 restart aoip_1
# systemd 사용 시: sudo systemctl restart aoip_1
# 직접 실행 시: 아래 주석 해제
ssh "$REMOTE_HOST" "cd ${REMOTE_DIR} && pm2 restart aoip_1 2>/dev/null || \
  (pkill -f 'node index.js' 2>/dev/null; nohup node index.js > /tmp/aoip.log 2>&1 &) && \
  echo '앱 재시작 완료'"

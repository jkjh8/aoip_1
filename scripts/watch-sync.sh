#!/usr/bin/env bash
# 파일 변경 감지 후 자동 rsync — inotifywait 필요 (apt install inotify-tools)
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

WATCH_EXCLUDES=(
  --exclude='\.git'
  --exclude='node_modules'
  --exclude='\.claude'
  --exclude='.*\.log'
)

if ! command -v inotifywait &>/dev/null; then
  echo "[오류] inotify-tools가 없습니다: sudo apt install inotify-tools"
  exit 1
fi

echo "[watch-sync] 감시 시작: ${LOCAL_DIR}"
echo "[watch-sync] 대상: ${REMOTE_HOST}:${REMOTE_DIR}"
echo "[watch-sync] 중지: Ctrl+C"
echo ""

# 시작 시 전체 동기화
rsync -az "${EXCLUDES[@]}" "$LOCAL_DIR" "${REMOTE_HOST}:${REMOTE_DIR}"
echo "[watch-sync] 초기 동기화 완료"

# 변경 감지 루프
while inotifywait -r -e modify,create,delete,move \
    "${WATCH_EXCLUDES[@]}" \
    --quiet "$LOCAL_DIR" 2>/dev/null; do
  echo "[watch-sync] $(date '+%H:%M:%S') 변경 감지 → 동기화 중..."
  rsync -az "${EXCLUDES[@]}" "$LOCAL_DIR" "${REMOTE_HOST}:${REMOTE_DIR}" \
    && echo "[watch-sync] 동기화 완료" \
    || echo "[watch-sync] 동기화 실패"
done

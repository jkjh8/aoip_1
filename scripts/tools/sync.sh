#!/usr/bin/env bash
# 로컬 PC → 임베디드 기기 단방향 rsync 동기화
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

echo "[sync] $(date '+%H:%M:%S') → ${REMOTE_HOST}:${REMOTE_DIR}"

rsync -avz \
  "${EXCLUDES[@]}" \
  "$LOCAL_DIR" \
  "${REMOTE_HOST}:${REMOTE_DIR}"

echo "[sync] 완료"

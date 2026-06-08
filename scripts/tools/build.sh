#!/usr/bin/env bash
# 타겟 기기에서 scripts/ 바이너리 빌드
# 사용법: ./build.sh [clean]
set -e

REMOTE_HOST="kjh@192.168.10.97"
REMOTE_DIR="/home/kjh/aoip_1/scripts"

if [[ "$1" == "clean" ]]; then
  echo "[build] clean..."
  ssh "$REMOTE_HOST" "cd ${REMOTE_DIR} && make clean"
fi

echo "[build] 빌드 시작 (${REMOTE_HOST})..."
ssh "$REMOTE_HOST" "cd ${REMOTE_DIR} && make -j\$(nproc) 2>&1"

echo "[build] 완료"

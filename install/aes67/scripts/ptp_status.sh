#!/bin/bash

AOIP_API="http://127.0.0.1:3000"

if [ "$1" == "locked" ]; then
  echo "$0 >> PTP locked — restarting AES67 bridges"
  curl -s -X POST "${AOIP_API}/bridges/restart-aes67" -o /dev/null || true
elif [ "$1" == "locking" ]; then
  echo "$0 >> PTP locking"
elif [ "$1" == "unlocked" ]; then
  echo "$0 >> PTP unlocked"
fi

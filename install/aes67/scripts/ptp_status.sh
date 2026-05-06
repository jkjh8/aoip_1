#!/bin/bash

if [ "$1" == "locked" ]; then
  echo "$0 >> PTP locked"
elif [ "$1" == "locking" ]; then
  echo "$0 >> PTP locking"
elif [ "$1" == "unlocked" ]; then
  echo "$0 >> PTP unlocked"
fi

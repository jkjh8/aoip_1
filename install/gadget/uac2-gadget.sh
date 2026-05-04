#!/bin/bash

CONFIGFS_DIR="/sys/kernel/config/usb_gadget"
GADGET_NAME="uac2"
GADGET_DIR="${CONFIGFS_DIR}/${GADGET_NAME}"

# ConfigFS 마운트 확인
if [ ! -d "${CONFIGFS_DIR}" ]; then
    modprobe libcomposite
    mount -t configfs none /sys/kernel/config
fi

# 기존 gadget 제거
if [ -d "${GADGET_DIR}" ]; then
    echo "Removing existing gadget..."
    echo "" > "${GADGET_DIR}/UDC" 2>/dev/null
    rm -f "${GADGET_DIR}/configs/c.1/uac2.0"
    rmdir "${GADGET_DIR}/configs/c.1/strings/0x409" 2>/dev/null
    rmdir "${GADGET_DIR}/configs/c.1" 2>/dev/null
    rmdir "${GADGET_DIR}/functions/uac2.0" 2>/dev/null
    rmdir "${GADGET_DIR}/strings/0x409" 2>/dev/null
    rmdir "${GADGET_DIR}"
fi

# 새 gadget 생성
mkdir -p "${GADGET_DIR}"
cd "${GADGET_DIR}"

# USB 디바이스 속성 설정
echo 0x1d6b > idVendor   # Linux Foundation
echo 0x0104 > idProduct  # Multifunction Composite Gadget
echo 0x0100 > bcdDevice  # v1.0.0
echo 0x0200 > bcdUSB     # USB2

# 디바이스 문자열 (Windows에 표시되는 이름)
mkdir -p strings/0x409
echo "fedcba9876543210" > strings/0x409/serialnumber
echo "AOIP"             > strings/0x409/manufacturer
echo "AOIP"             > strings/0x409/product

# UAC2 기능 생성
mkdir -p functions/uac2.0

# Playback (호스트 -> 디바이스)
echo 3      > functions/uac2.0/p_chmask    # 스테레오
echo 48000  > functions/uac2.0/p_srate     # 48kHz
echo 4      > functions/uac2.0/p_ssize     # 32-bit

# Playback 볼륨/뮤트 활성화
echo 0      > functions/uac2.0/p_mute_present    # 뮤트 하드웨어 없음
echo 0      > functions/uac2.0/p_volume_present  # 볼륨 하드웨어 없음

# Capture (디바이스 -> 호스트)
echo 3      > functions/uac2.0/c_chmask    # 스테레오
echo 48000  > functions/uac2.0/c_srate     # 48kHz
echo 4      > functions/uac2.0/c_ssize     # 32-bit

# Capture 볼륨/뮤트 활성화
echo 1      > functions/uac2.0/c_mute_present    # 뮤트 지원
echo 1      > functions/uac2.0/c_volume_present  # 볼륨 지원
echo -6400  > functions/uac2.0/c_volume_min      # -25 dB (6400/256=25, 나누어 떨어짐)
echo 0      > functions/uac2.0/c_volume_max      # 0 dB
echo 256    > functions/uac2.0/c_volume_res      # 1 dB 단위

# Function 인터페이스 이름
echo "aoip"              > functions/uac2.0/function_name
echo "aoip Control"      > functions/uac2.0/if_ctrl_name
echo "aoip Playback"     > functions/uac2.0/p_it_name
echo "aoip Speaker"      > functions/uac2.0/p_ot_name
echo "aoip Capture"      > functions/uac2.0/c_it_name
echo "aoip Microphone"   > functions/uac2.0/c_ot_name

# Configuration 생성
mkdir -p configs/c.1/strings/0x409
echo "aoip UAC2"  > configs/c.1/strings/0x409/configuration
echo 250          > configs/c.1/MaxPower

# 기능을 configuration에 연결
ln -s functions/uac2.0 configs/c.1/

# UDC 활성화
UDC_DEVICE=$(ls /sys/class/udc | head -n1)
if [ -z "$UDC_DEVICE" ]; then
    echo "[ERROR] UDC를 찾을 수 없습니다."
    exit 1
fi
echo "${UDC_DEVICE}" > UDC

echo "[OK] aoip UAC2 gadget 활성화: ${UDC_DEVICE}"

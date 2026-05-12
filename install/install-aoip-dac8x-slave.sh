#!/bin/bash
# =============================================================================
# AoIP DAC8x + ADC8x 드라이버 설치 스크립트 — I2S Slave Mode
# 대상: Raspberry Pi CM5 (BCM2712), Raspberry Pi OS (6.12.y 커널)
# 목적: RPi를 I2S clock consumer(slave)로 설정 — 외부 컨버터가 BCK/LRCLK 공급
# =============================================================================

set -e

KVER=$(uname -r)
KBRANCH="rpi-6.12.y"
SRC_URL="https://raw.githubusercontent.com/raspberrypi/linux/${KBRANCH}/sound/soc/bcm/rpi-simple-soundcard.c"
BUILD_DIR="/tmp/aoip-dac8x-slave-mod"
MODULE_DIR="/lib/modules/${KVER}/kernel/sound/soc/bcm"
OVERLAY_DIR="/boot/firmware/overlays"
CONFIG_FILE="/boot/firmware/config.txt"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 색상 출력
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()   { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

echo "============================================="
echo " AoIP DAC8x + ADC8x 드라이버 설치 (Slave)"
echo " 커널: ${KVER}"
echo "============================================="
echo ""

# -----------------------------------------------------------------------------
# 1. root 권한 확인
# -----------------------------------------------------------------------------
if [ "$EUID" -ne 0 ]; then
    error "root 권한이 필요합니다. sudo 로 실행하세요."
fi

# -----------------------------------------------------------------------------
# 2. 필수 패키지 확인 및 설치
# -----------------------------------------------------------------------------
info "필수 패키지 확인 중..."
PKGS_NEEDED=()

for pkg in build-essential curl device-tree-compiler; do
    if ! dpkg -l "$pkg" &>/dev/null; then
        PKGS_NEEDED+=("$pkg")
    fi
done

HEADER_PKG="linux-headers-${KVER}"
if [ ! -d "/lib/modules/${KVER}/build" ]; then
    PKGS_NEEDED+=("$HEADER_PKG")
fi

if [ ${#PKGS_NEEDED[@]} -gt 0 ]; then
    info "설치 필요: ${PKGS_NEEDED[*]}"
    apt-get update -qq
    apt-get install -y "${PKGS_NEEDED[@]}"
else
    info "필수 패키지 모두 설치되어 있음"
fi

# -----------------------------------------------------------------------------
# 3. 소스 다운로드
# -----------------------------------------------------------------------------
info "드라이버 소스 다운로드 중... (${KBRANCH})"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if ! curl -fsSL "${SRC_URL}" -o rpi-simple-soundcard.c; then
    error "소스 다운로드 실패. 인터넷 연결을 확인하세요."
fi

if ! grep -q "hifiberry_dac8x_init" rpi-simple-soundcard.c; then
    error "다운로드한 소스가 예상과 다릅니다. 커널 브랜치를 확인하세요."
fi

info "소스 다운로드 완료"

# -----------------------------------------------------------------------------
# 4. 소스 패치 — GPIO 제거 + ADC8x 항상 활성화 + I2S slave (codec master)
# -----------------------------------------------------------------------------
info "드라이버 패치 적용 중..."

python3 - <<'PYEOF'
import re, sys

with open('rpi-simple-soundcard.c', 'r') as f:
    src = f.read()

# hifiberry_dac8x_init: GPIO 변수 선언 제거
old_pattern = re.compile(
    r'(static int hifiberry_dac8x_init\(struct snd_soc_pcm_runtime \*rtd\)\s*\{)'
    r'(\s*struct snd_soc_dai \*codec_dai = snd_soc_rtd_to_codec\(rtd, 0\);)'
    r'(\s*struct snd_soc_card \*card = rtd->card;)'
    r'(\s*struct gpio_desc \*gpio_desc;)'
    r'(\s*bool has_adc;)',
    re.DOTALL
)
new_header = (
    'static int hifiberry_dac8x_init(struct snd_soc_pcm_runtime *rtd)\n'
    '{\n'
    '\tstruct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);'
)
src, count = old_pattern.subn(new_header, src)
if count == 0:
    src = src.replace(
        'struct snd_soc_card *card = rtd->card;\n\tstruct gpio_desc *gpio_desc;\n\tbool has_adc;',
        ''
    )

# GPIO 체크 블록 → 항상 ADC8x 활성화
old_gpio_block = re.compile(
    r'/\* Activate capture based on ADC8x detection \*/.*?'
    r'rtd->dai_link->playback_only = 1;.*?\}',
    re.DOTALL
)
new_gpio_block = (
    '/* Always enable ADC8x capture (GPIO check removed) */\n'
    '\t{\n'
    '\t\tstruct snd_soc_dai_link *dai = rtd->dai_link;\n'
    '\n'
    '\t\tdev_info(rtd->card->dev, "ADC8x capture always enabled (no GPIO check)");\n'
    '\t\tcodec_dai->driver->symmetric_rate = 1;\n'
    '\t\tcodec_dai->driver->symmetric_channels = 1;\n'
    '\t\tcodec_dai->driver->symmetric_sample_bits = 1;\n'
    '\t\tcodec_dai->driver->capture.rates = SNDRV_PCM_RATE_8000_192000;\n'
    '\t\tcodec_dai->driver->capture.channels_max = 8;\n'
    '\t\tdai->name = "AoIP 8";\n'
    '\t\tdai->stream_name = "AoIP 8 HiFi";\n'
    '\t}'
)
src, count = old_gpio_block.subn(new_gpio_block, src)
if count == 0:
    print("ERROR: GPIO 블록을 찾지 못했습니다.")
    sys.exit(1)

# dai_fmt: codec slave(master) → codec master(slave) 변경
# 신 커널(5.15+): CBC_CFC → CBP_CFP
# 구 커널:        CBS_CFS → CBP_CFP  (CBM_CFM은 이미 codec master이므로 그대로)
replaced = False
for old_fmt, new_fmt in [
    ('SND_SOC_DAIFMT_CBC_CFC', 'SND_SOC_DAIFMT_CBP_CFP'),
    ('SND_SOC_DAIFMT_CBS_CFS', 'SND_SOC_DAIFMT_CBP_CFP'),
]:
    if old_fmt in src:
        src = src.replace(old_fmt, new_fmt)
        print(f"dai_fmt: {old_fmt} → {new_fmt}")
        replaced = True
if not replaced:
    print("WARN: dai_fmt CBC_CFC/CBS_CFS 를 찾지 못했습니다. 소스를 확인하세요.")

# card_name 및 DAI 이름
src = src.replace('.card_name = "snd_rpi_hifiberry_dac8x"', '.card_name = "aoip"')
src = src.replace('.name           = "HifiBerry DAC8x"',    '.name           = "AoIP 8"')
src = src.replace('.stream_name    = "HifiBerry DAC8x HiFi"', '.stream_name    = "AoIP 8 HiFi"')

# of_match 테이블
old_match = re.compile(
    r'static const struct of_device_id snd_rpi_simple_of_match\[\]\s*=\s*\{.*?\{\},\s*\};',
    re.DOTALL
)
new_match = (
    'static const struct of_device_id snd_rpi_simple_of_match[] = {\n'
    '\t{ .compatible = "aoip,aoip-dac8x",\n'
    '\t\t.data = (void *) &drvdata_hifiberry_dac8x },\n'
    '\t{},\n'
    '};\n'
)
src, count = old_match.subn(new_match, src)
if count == 0:
    print("ERROR: of_device_id 테이블을 찾지 못했습니다.")
    sys.exit(1)

# 드라이버 이름
src = src.replace('.name   = "snd-rpi-simple"', '.name   = "snd-aoip"')

with open('aoip-soundcard.c', 'w') as f:
    f.write(src)

print("패치 성공 → aoip-soundcard.c 생성됨 (I2S slave / codec master)")
PYEOF

info "패치 완료"

# -----------------------------------------------------------------------------
# 5. Device Tree 오버레이 복사
# -----------------------------------------------------------------------------
info "Device Tree 오버레이 복사 중..."

DTS_SRC="${SCRIPT_DIR}/aoip-dac8x-slave.dts"
if [ ! -f "${DTS_SRC}" ]; then
    error "aoip-dac8x-slave.dts 를 찾을 수 없습니다: ${DTS_SRC}"
fi
cp "${DTS_SRC}" "${BUILD_DIR}/aoip-dac8x-slave.dts"
info "aoip-dac8x-slave.dts 복사 완료"

# -----------------------------------------------------------------------------
# 6. Makefile 작성
# -----------------------------------------------------------------------------
cat > Makefile <<'EOF'
obj-m += snd-soc-aoip-soundcard.o
snd-soc-aoip-soundcard-objs := aoip-soundcard.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
EOF

# -----------------------------------------------------------------------------
# 7. 커널 모듈 빌드
# -----------------------------------------------------------------------------
info "모듈 빌드 중..."
if ! make 2>&1; then
    error "빌드 실패. 위 오류 메시지를 확인하세요."
fi
info "빌드 완료: snd-soc-aoip-soundcard.ko"

# -----------------------------------------------------------------------------
# 8. Device Tree 오버레이 컴파일
# -----------------------------------------------------------------------------
info "Device Tree 오버레이 컴파일 중..."
if ! dtc -@ -I dts -O dtb -o aoip-dac8x-slave.dtbo aoip-dac8x-slave.dts 2>&1; then
    error "DTS 컴파일 실패."
fi
info "컴파일 완료: aoip-dac8x-slave.dtbo"

# -----------------------------------------------------------------------------
# 9. 모듈 및 오버레이 설치
# -----------------------------------------------------------------------------
info "모듈 설치 중..."
cp "${BUILD_DIR}/snd-soc-aoip-soundcard.ko" "${MODULE_DIR}/"
depmod -a
info "snd-soc-aoip-soundcard.ko 설치 완료"

info "Device Tree 오버레이 설치 중..."
cp "${BUILD_DIR}/aoip-dac8x-slave.dtbo" "${OVERLAY_DIR}/"
info "aoip-dac8x-slave.dtbo 설치 완료"

# -----------------------------------------------------------------------------
# 10. /boot/firmware/config.txt 수정
# -----------------------------------------------------------------------------
info "config.txt 수정 중..."

# 기존 관련 오버레이를 aoip-dac8x-slave로 교체
for old in "hifiberry-dac8x" "hifiberry-studio-dac8x" "hifiberry-adc8x" "i2s-dummy" "aoip-dac8x"; do
    if grep -q "dtoverlay=${old}$" "${CONFIG_FILE}" || grep -q "dtoverlay=${old} " "${CONFIG_FILE}"; then
        sed -i "s/dtoverlay=${old}/dtoverlay=aoip-dac8x-slave/" "${CONFIG_FILE}"
        warn "dtoverlay=${old} → dtoverlay=aoip-dac8x-slave 로 변경"
    fi
done

# aoip-dac8x-slave가 없으면 [all] 섹션에 추가
if ! grep -q "dtoverlay=aoip-dac8x-slave" "${CONFIG_FILE}"; then
    if grep -q "^\[all\]" "${CONFIG_FILE}"; then
        sed -i '/^\[all\]/a dtoverlay=aoip-dac8x-slave' "${CONFIG_FILE}"
    else
        echo -e "\n[all]\ndtoverlay=aoip-dac8x-slave" >> "${CONFIG_FILE}"
    fi
    info "dtoverlay=aoip-dac8x-slave 추가됨"
else
    info "dtoverlay=aoip-dac8x-slave 이미 설정됨"
fi

# i2s 활성화
if ! grep -q "^dtparam=i2s=on" "${CONFIG_FILE}"; then
    sed -i 's/^#dtparam=i2s=on/dtparam=i2s=on/' "${CONFIG_FILE}" || \
    echo "dtparam=i2s=on" >> "${CONFIG_FILE}"
    info "dtparam=i2s=on 활성화"
fi

# -----------------------------------------------------------------------------
# 11. 완료
# -----------------------------------------------------------------------------
echo ""
echo "============================================="
echo -e " ${GREEN}설치 완료! (I2S Slave Mode)${NC}"
echo "============================================="
echo ""
echo " 설치된 파일:"
echo "   - ${MODULE_DIR}/snd-soc-aoip-soundcard.ko"
echo "   - ${OVERLAY_DIR}/aoip-dac8x-slave.dtbo"
echo ""
echo " 적용된 설정:"
echo "   - dtoverlay=aoip-dac8x-slave"
echo "   - RPi I2S = clock consumer (slave)"
echo "   - 외부 컨버터(DAC8x/ADC8x)가 BCK/LRCLK 공급"
echo ""
echo " 재부팅 후 확인:"
echo "   dmesg | grep -i 'ADC8x\|aoip'"
echo "   aplay -l && arecord -l"
echo ""

read -r -p "지금 재부팅할까요? [y/N] " answer
if [[ "$answer" =~ ^[Yy]$ ]]; then
    info "재부팅합니다..."
    reboot
else
    warn "나중에 직접 재부팅하세요: sudo reboot"
fi

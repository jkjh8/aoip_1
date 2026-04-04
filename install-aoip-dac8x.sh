#!/bin/bash
# =============================================================================
# AoIP DAC8x + ADC8x 드라이버 설치 스크립트
# 대상: Raspberry Pi CM5 (BCM2712), Raspberry Pi OS (6.12.y 커널)
# 목적: GPIO 없이 ADC8x 8채널 캡처를 항상 활성화, 별도 aoip 드라이버로 설치
# =============================================================================

set -e

KVER=$(uname -r)
KBRANCH="rpi-6.12.y"
SRC_URL="https://raw.githubusercontent.com/raspberrypi/linux/${KBRANCH}/sound/soc/bcm/rpi-simple-soundcard.c"
BUILD_DIR="/tmp/aoip-dac8x-mod"
MODULE_DIR="/lib/modules/${KVER}/kernel/sound/soc/bcm"
OVERLAY_DIR="/boot/firmware/overlays"
CONFIG_FILE="/boot/firmware/config.txt"

# 색상 출력
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()   { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

echo "============================================="
echo " AoIP DAC8x + ADC8x 드라이버 설치 스크립트"
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

# 현재 커널용 헤더 확인
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

# 소스 내 버전 확인
if ! grep -q "hifiberry_dac8x_init" rpi-simple-soundcard.c; then
    error "다운로드한 소스가 예상과 다릅니다. 커널 브랜치를 확인하세요."
fi

info "소스 다운로드 완료"

# -----------------------------------------------------------------------------
# 4. 소스 패치 후 aoip-soundcard.c 로 저장
#    - GPIO 체크 제거, ADC8x 항상 활성화
#    - compatible string: hifiberry,hifiberry-dac8x → aoip,aoip-dac8x
#    - 드라이버 이름: snd-rpi-simple → snd-aoip
#    - of_match 테이블: aoip-dac8x 항목만 남김
# -----------------------------------------------------------------------------
info "드라이버 패치 적용 중..."

python3 - <<'PYEOF'
import re, sys

with open('rpi-simple-soundcard.c', 'r') as f:
    src = f.read()

# hifiberry_dac8x_init 함수에서 GPIO 관련 변수 선언 제거
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

# GPIO 체크 블록 전체를 "항상 활성화" 코드로 교체
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
    print("ERROR: GPIO 블록을 찾지 못했습니다. 소스 구조가 변경되었을 수 있습니다.")
    sys.exit(1)

# card_name 및 DAI 이름을 aoip로 변경
src = src.replace(
    '.card_name = "snd_rpi_hifiberry_dac8x"',
    '.card_name = "aoip"'
)
src = src.replace(
    '.name           = "HifiBerry DAC8x"',
    '.name           = "AoIP 8"'
)
src = src.replace(
    '.stream_name    = "HifiBerry DAC8x HiFi"',
    '.stream_name    = "AoIP 8 HiFi"'
)

# of_match 테이블을 aoip-dac8x 항목만 남기도록 교체
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

# 드라이버 이름 변경 (원본 모듈과 충돌 방지)
src = src.replace(
    '.name   = "snd-rpi-simple"',
    '.name   = "snd-aoip"'
)

# aoip-soundcard.c 로 저장 (원본은 그대로 유지)
with open('aoip-soundcard.c', 'w') as f:
    f.write(src)

print("패치 성공 → aoip-soundcard.c 생성됨")
PYEOF

info "패치 완료"

# -----------------------------------------------------------------------------
# 5. Device Tree 오버레이 소스 생성 (aoip-dac8x.dts)
# -----------------------------------------------------------------------------
info "Device Tree 오버레이 생성 중..."

cat > aoip-dac8x.dts <<'EOF'
/dts-v1/;
/plugin/;

/*
 * AoIP DAC8x + ADC8x Device Tree Overlay
 * compatible: aoip,aoip-dac8x
 * 원본 hifiberry-dac8x 오버레이 기반, hasadc-gpio 제거 (항상 ADC8x 활성화)
 */

/ {
	compatible = "brcm,bcm2712";

	fragment@0 {
		target = <&rp1_gpio>;
		__overlay__ {
			rp1_i2s0_aoip: rp1_i2s0_aoip {
				function = "i2s0";
				pins = "gpio18", "gpio19", "gpio20", "gpio21",
				       "gpio22", "gpio23", "gpio24", "gpio25",
				       "gpio26", "gpio27";
				bias-disable;
				status = "okay";
			};
		};
	};

	fragment@1 {
		target = <&i2s_clk_producer>;
		__overlay__ {
			pinctrl-names = "default";
			pinctrl-0 = <&rp1_i2s0_aoip>;
			status = "okay";
		};
	};

	fragment@2 {
		target-path = "/";
		__overlay__ {
			dummy-codec {
				#sound-dai-cells = <0>;
				compatible = "snd-soc-dummy";
				status = "okay";
			};
		};
	};

	fragment@3 {
		target = <&sound>;
		__overlay__ {
			compatible = "aoip,aoip-dac8x";
			i2s-controller = <&i2s_clk_producer>;
			status = "okay";
		};
	};
};
EOF

info "aoip-dac8x.dts 생성 완료"

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
if ! dtc -@ -I dts -O dtb -o aoip-dac8x.dtbo aoip-dac8x.dts 2>&1; then
    error "DTS 컴파일 실패."
fi
info "컴파일 완료: aoip-dac8x.dtbo"

# -----------------------------------------------------------------------------
# 9. 모듈 및 오버레이 설치 (원본 snd-soc-rpi-simple-soundcard 는 그대로 유지)
# -----------------------------------------------------------------------------
info "모듈 설치 중..."
cp "${BUILD_DIR}/snd-soc-aoip-soundcard.ko" "${MODULE_DIR}/"
depmod -a
info "snd-soc-aoip-soundcard.ko 설치 완료"

info "Device Tree 오버레이 설치 중..."
cp "${BUILD_DIR}/aoip-dac8x.dtbo" "${OVERLAY_DIR}/"
info "aoip-dac8x.dtbo 설치 완료"

# -----------------------------------------------------------------------------
# 10. /boot/firmware/config.txt 수정
# -----------------------------------------------------------------------------
info "config.txt 수정 중..."

# 기존 hifiberry-dac8x 관련 오버레이를 aoip-dac8x로 교체
for old in "hifiberry-dac8x" "hifiberry-studio-dac8x" "hifiberry-adc8x" "i2s-dummy"; do
    if grep -q "dtoverlay=${old}" "${CONFIG_FILE}"; then
        sed -i "s/dtoverlay=${old}/dtoverlay=aoip-dac8x/" "${CONFIG_FILE}"
        warn "dtoverlay=${old} → dtoverlay=aoip-dac8x 로 변경"
    fi
done

# aoip-dac8x가 없으면 [all] 섹션에 추가
if ! grep -q "dtoverlay=aoip-dac8x" "${CONFIG_FILE}"; then
    if grep -q "^\[all\]" "${CONFIG_FILE}"; then
        sed -i '/^\[all\]/a dtoverlay=aoip-dac8x' "${CONFIG_FILE}"
    else
        echo -e "\n[all]\ndtoverlay=aoip-dac8x" >> "${CONFIG_FILE}"
    fi
    info "dtoverlay=aoip-dac8x 추가됨"
else
    info "dtoverlay=aoip-dac8x 이미 설정됨"
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
echo -e " ${GREEN}설치 완료!${NC}"
echo "============================================="
echo ""
echo " 설치된 파일:"
echo "   - ${MODULE_DIR}/snd-soc-aoip-soundcard.ko"
echo "   - ${OVERLAY_DIR}/aoip-dac8x.dtbo"
echo ""
echo " 적용된 설정:"
echo "   - dtoverlay=aoip-dac8x"
echo "   - ADC8x GPIO 체크 제거 (항상 8채널 캡처 활성화)"
echo "   - 원본 snd-soc-rpi-simple-soundcard 모듈 유지됨"
echo ""
echo " 재부팅 후 확인:"
echo "   dmesg | grep -i 'ADC8x\|aoip'"
echo "   aplay -l && arecord -l"
echo "   arecord -D hw:0,0 -f S32_LE -r 48000 -c 8 -d 3 test.wav"
echo ""

read -r -p "지금 재부팅할까요? [y/N] " answer
if [[ "$answer" =~ ^[Yy]$ ]]; then
    info "재부팅합니다..."
    reboot
else
    warn "나중에 직접 재부팅하세요: sudo reboot"
fi

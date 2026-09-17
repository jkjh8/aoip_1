#!/bin/bash
# =============================================================================
# AoIP 사운드카드 커널 모듈 타깃 재빌드 (카드/스트림 표시명 = "AoIP 2")
# - install-aoip-dac8x.sh 의 패치 로직만 재사용, 문자열은 "AoIP 2" 로 변경
# - config.txt 수정/재부팅 없음 (오버레이 설정은 별도: dtoverlay=aoip-dac2x 유지)
# - card_name 은 "aoip" 그대로 (hw:aoip alias/서비스 호환 유지)
# - compatible 은 aoip,aoip-dac8x / -slave 그대로 (오버레이 of_match 매칭 유지)
# 대상: Raspberry Pi CM5 (BCM2712)
# =============================================================================
set -e

KVER=$(uname -r)
KBRANCH="rpi-6.18.y"
SRC_URL="https://raw.githubusercontent.com/raspberrypi/linux/${KBRANCH}/sound/soc/bcm/rpi-simple-soundcard.c"
BUILD_DIR="/tmp/aoip-rebuild-mod"
MODULE_DIR="/lib/modules/${KVER}/kernel/sound/soc/bcm"

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

[ "$EUID" -eq 0 ] || error "root 권한 필요 (sudo 로 실행)."
[ -d "/lib/modules/${KVER}/build" ] || error "커널 헤더 없음: linux-headers-${KVER}"

info "소스 다운로드 (${KBRANCH})..."
rm -rf "${BUILD_DIR}"; mkdir -p "${BUILD_DIR}"; cd "${BUILD_DIR}"
curl -fsSL "${SRC_URL}" -o rpi-simple-soundcard.c || error "소스 다운로드 실패"
grep -q "hifiberry_dac8x_init" rpi-simple-soundcard.c || error "소스 구조가 예상과 다름"

info "패치 적용 (표시명 → AoIP 2)..."
python3 - <<'PYEOF'
import re, sys
with open('rpi-simple-soundcard.c') as f:
    src = f.read()

# hifiberry_dac8x_init 헤더: GPIO 관련 변수 제거
old_pattern = re.compile(
    r'(static int hifiberry_dac8x_init\(struct snd_soc_pcm_runtime \*rtd\)\s*\{)'
    r'(\s*struct snd_soc_dai \*codec_dai = snd_soc_rtd_to_codec\(rtd, 0\);)'
    r'(\s*struct snd_soc_card \*card = rtd->card;)'
    r'(\s*struct gpio_desc \*gpio_desc;)'
    r'(\s*bool has_adc;)', re.DOTALL)
new_header = (
    'static int hifiberry_dac8x_init(struct snd_soc_pcm_runtime *rtd)\n'
    '{\n'
    '\tstruct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);')
src, n = old_pattern.subn(new_header, src)
if n == 0:
    src = src.replace(
        'struct snd_soc_card *card = rtd->card;\n\tstruct gpio_desc *gpio_desc;\n\tbool has_adc;',
        '')

# GPIO 체크 블록 → 항상 활성화 (표시명 AoIP 2)
old_gpio_block = re.compile(
    r'/\* Activate capture based on ADC8x detection \*/.*?'
    r'rtd->dai_link->playback_only = 1;.*?\}', re.DOTALL)
new_gpio_block = (
    '/* Always enable ADC capture (GPIO check removed) */\n'
    '\t{\n'
    '\t\tstruct snd_soc_dai_link *dai = rtd->dai_link;\n'
    '\n'
    '\t\tdev_info(rtd->card->dev, "ADC capture always enabled (no GPIO check)");\n'
    '\t\tcodec_dai->driver->symmetric_rate = 1;\n'
    '\t\tcodec_dai->driver->symmetric_channels = 1;\n'
    '\t\tcodec_dai->driver->symmetric_sample_bits = 1;\n'
    '\t\tcodec_dai->driver->capture.rates = SNDRV_PCM_RATE_8000_192000;\n'
    '\t\tcodec_dai->driver->capture.channels_max = 8;\n'
    '\t\tdai->name = "AoIP 2";\n'
    '\t\tdai->stream_name = "AoIP 2 HiFi";\n'
    '\t}')
src, n = old_gpio_block.subn(new_gpio_block, src)
if n == 0:
    print("ERROR: GPIO 블록 미발견"); sys.exit(1)

# card_name 은 aoip 유지, DAI/stream 표시명만 AoIP 2
src = src.replace('.card_name = "snd_rpi_hifiberry_dac8x"', '.card_name = "aoip"')
src = src.replace('.name           = "HifiBerry DAC8x"', '.name           = "AoIP 2"')
src = src.replace('.stream_name    = "HifiBerry DAC8x HiFi"', '.stream_name    = "AoIP 2 HiFi"')

# 슬레이브 dai_link + drvdata (표시명 AoIP 2)
slave_structs = (
    '\n/* AoIP slave mode: external device provides BCK/LRCLK */\n'
    'static struct snd_soc_dai_link snd_aoip_slave_dai[] = {\n'
    '\t{\n'
    '\t\t.name\t\t= "AoIP 2",\n'
    '\t\t.stream_name\t= "AoIP 2 HiFi",\n'
    '\t\t.dai_fmt\t= SND_SOC_DAIFMT_I2S |\n'
    '\t\t\t\t  SND_SOC_DAIFMT_NB_NF |\n'
    '\t\t\t\t  SND_SOC_DAIFMT_CBP_CFP,\n'
    '\t\t.init\t\t= hifiberry_dac8x_init,\n'
    '\t\tSND_SOC_DAILINK_REG(hifiberry_dac8x),\n'
    '\t},\n'
    '};\n'
    '\nstatic struct snd_rpi_simple_drvdata drvdata_aoip_slave = {\n'
    '\t.card_name = "aoip",\n'
    '\t.dai = snd_aoip_slave_dai,\n'
    '\t.fixed_bclk_ratio = 64,\n'
    '};\n\n')
idx = src.find('static const struct of_device_id snd_rpi_simple_of_match')
if idx == -1:
    print("ERROR: of_device_id 위치 미발견"); sys.exit(1)
src = src[:idx] + slave_structs + src[idx:]

# of_match: master + slave
old_match = re.compile(
    r'static const struct of_device_id snd_rpi_simple_of_match\[\]\s*=\s*\{.*?\{\},\s*\};',
    re.DOTALL)
new_match = (
    'static const struct of_device_id snd_rpi_simple_of_match[] = {\n'
    '\t{ .compatible = "aoip,aoip-dac8x",\n'
    '\t\t.data = (void *) &drvdata_hifiberry_dac8x },\n'
    '\t{ .compatible = "aoip,aoip-dac8x-slave",\n'
    '\t\t.data = (void *) &drvdata_aoip_slave },\n'
    '\t{},\n'
    '};\n')
src, n = old_match.subn(new_match, src)
if n == 0:
    print("ERROR: of_device_id 테이블 미발견"); sys.exit(1)

# 모듈 이름 (원본과 충돌 방지)
src = src.replace('.name   = "snd-rpi-simple"', '.name   = "snd-aoip"')

with open('aoip-soundcard.c', 'w') as f:
    f.write(src)
print("패치 성공 → aoip-soundcard.c")
PYEOF

info "Makefile 작성 및 빌드..."
cat > Makefile <<'EOF'
obj-m += snd-soc-aoip-soundcard.o
snd-soc-aoip-soundcard-objs := aoip-soundcard.o
KDIR := /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
EOF
make 2>&1 || error "빌드 실패"

info "모듈 설치..."
cp "${BUILD_DIR}/snd-soc-aoip-soundcard.ko" "${MODULE_DIR}/"
depmod -a
info "완료: ${MODULE_DIR}/snd-soc-aoip-soundcard.ko (표시명 AoIP 2). 재부팅 후 'aplay -l' 로 확인."

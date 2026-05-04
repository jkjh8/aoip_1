# AoIP 설치 가이드

대상 플랫폼: Raspberry Pi OS Bookworm (aarch64)

---

## 파일 구조

```
aoip_1/
├── install/
│   ├── install.sh              # 통합 설치 스크립트 ← 여기서 실행
│   ├── install-aoip-dac8x.sh  # aoip DAC8x 커널 드라이버 설치
│   ├── aes67/                  # AES67 데몬 바이너리, 설정, 스크립트
│   ├── ravenna-alsa-lkm/       # Ravenna ALSA 커널 모듈 소스
│   ├── gadget/
│   │   └── uac2-gadget.sh      # UAC2 USB Gadget 설정 스크립트
│   └── systemd/                # systemd 서비스 파일 모음
├── scripts/                    # C 소스 및 빌드 파일
└── config/
    ├── audio.json              # 오디오 브릿지 / RTP 설정
    └── channels.json           # 채널 라우팅 / DSP 설정
```

---

## 설치

```bash
sudo bash install/install.sh
```

### 수행 작업

| 단계 | 내용 |
|------|------|
| 1 | apt 패키지 설치 (linuxptp, libboost, libavahi, libasound2, libmp3lame, libsamplerate 등) |
| 2 | Node.js v20 확인 + `npm install` |
| 3 | Ravenna ALSA 커널 모듈 빌드 → `/lib/modules/$(uname -r)/extra/` 설치 |
| 4 | aoip DAC8x 커널 드라이버 설치 (Device Tree 오버레이 포함) |
| 5 | C 바이너리 빌드 (aoip_engine, rtp_recv, rtp_send) |
| 6 | `uac2-gadget.sh` → `/usr/local/bin/` 배포 |
| 7 | systemd 서비스 설치 및 자동시작 활성화 |
| 8 | 실행 권한 설정, audio 그룹 추가 |

설치 후 재부팅 권장:

```bash
sudo reboot
```

---

## 서비스 구성

서비스는 아래 순서로 의존합니다:

```
ptp4l            → PTP 클록 동기화 (eth0)
  └─ aes67-daemon  → AES67/RAVENNA 스트리밍 (MergingRavennaALSA.ko 로드)
       └─ aoip      → AoIP 백엔드 (Node.js, 웹 UI, DSP 엔진)

uac2-gadget      → USB Audio Class 2 Gadget (독립 실행)
ravenna-module   → Ravenna ALSA 모듈 (aes67-daemon에 의해 관리됨)
```

### 서비스 제어

```bash
# 시작
sudo systemctl start ptp4l aes67-daemon uac2-gadget aoip

# 상태 확인
systemctl status ptp4l aes67-daemon uac2-gadget aoip

# 로그
journalctl -u aoip -f
journalctl -u aes67-daemon -f
```

---

## 설정

### 네트워크 / AES67 (`install/aes67/daemon.conf`)

| 항목 | 기본값 | 설명 |
|------|--------|------|
| `interface_name` | `eth0` | 네트워크 인터페이스 |
| `ip_addr` | `192.168.10.97` | 장치 IP (설치 시 자동 감지) |
| `sample_rate` | `48000` | 샘플레이트 |
| `ptp_domain` | `0` | PTP 도메인 |
| `rtp_mcast_base` | `239.69.123.1` | RTP 멀티캐스트 베이스 |

### 오디오 브릿지 (`config/audio.json`)

USB Gadget, AES67, RTP 스트림 설정. 브릿지 활성화/비활성화는 `enabled` 필드로 제어.

### UAC2 볼륨 동작

`uac2-gadget.sh`에서 `p_volume_present=0`으로 설정되어 있어 PC 소프트웨어 믹서가 볼륨을 PCM에 직접 적용합니다 (PC 볼륨 슬라이더 정상 동작).

---

## 확인

```bash
# ALSA 장치 목록
aplay -l | grep -E "RAVENNA|UAC2|sndrpi"

# UAC2 믹서 컨트롤
amixer -c UAC2Gadget scontents

# PTP 상태
journalctl -u ptp4l --no-pager | tail -20
```

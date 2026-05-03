# aes67-daemon 설치 및 설정 가이드

대상: Raspberry Pi CM5 (또는 동일 환경), Raspberry Pi OS (6.12.y 커널)

---

## 1. 사전 요구사항

### 필수 패키지

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git libavahi-client-dev \
    libconfig++-dev libsoxr-dev libboost-dev
```

### MergingRavennaALSA 커널 모듈

modprobe 방식으로 로드하므로 `/lib/modules/$(uname -r)/` 에 영구 설치되어 있어야 함.

```bash
# 설치 확인
modinfo MergingRavennaALSA

# 수동 로드 테스트
sudo modprobe MergingRavennaALSA
sudo modprobe -r MergingRavennaALSA
```

---

## 2. aes67-daemon 빌드 및 설치

```bash
git clone https://github.com/bondagit/aes67-linux-daemon.git /home/kjh/aes67
cd /home/kjh/aes67

# 빌드 (systemd notify 지원 포함)
mkdir build && cd build
cmake -DWITH_SYSTEMD=OFF ..
make -j$(nproc)

# 바이너리 설치
cp daemon/aes67-daemon /home/kjh/aes67/aes67-daemon
```

---

## 3. 설정 파일

### daemon.conf

`/home/kjh/aes67/daemon.conf` — 주요 항목:

```
interface_name = "eth0"          # 네트워크 인터페이스
http_port = 8080                 # 웹UI 포트
rtsp_port = 8554                 # RTSP 포트
mdns_enabled = true              # mDNS/Avahi 사용 여부
ptp_domain = 0                   # PTP 도메인 번호
syslog_proto = "none"
```

경로가 다른 장치에서는 `interface_name` 과 포트를 실제 환경에 맞게 수정.

---

## 4. CPU 스케일링 비활성화 스크립트

`/home/kjh/aes67/scripts/disable_cpu_scaling.sh` — 실시간 오디오 지터 감소 목적:

```bash
#!/bin/bash
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | tee "$cpu" > /dev/null
done
```

```bash
chmod +x /home/kjh/aes67/scripts/disable_cpu_scaling.sh
```

---

## 5. systemd 서비스 설정

### 서비스 파일 설치

`/home/kjh/aes67/systemd/aes67-daemon.service` 를 복사:

```bash
sudo cp /home/kjh/aes67/systemd/aes67-daemon.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable aes67-daemon
```

### 서비스 파일 내용

```ini
[Unit]
Description=AES67 Linux Daemon
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/home/kjh/aes67

# 우선순위 설정
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=90
Nice=-10

# CPU 격리: JACK(CPU3)과 분리 (필요 시 주석 해제)
# CPUAffinity=2 3

# 커널 모듈 로드 (modprobe — /lib/modules 영구 설치 기반)
ExecStartPre=-/sbin/modprobe -r MergingRavennaALSA
ExecStartPre=/sbin/modprobe MergingRavennaALSA

# CPU 스케일링 비활성화
ExecStartPre=/home/kjh/aes67/scripts/disable_cpu_scaling.sh

# 데몬 실행
ExecStart=/home/kjh/aes67/aes67-daemon -c /home/kjh/aes67/daemon.conf

# 종료 시 커널 모듈 언로드
ExecStopPost=/sbin/modprobe -r MergingRavennaALSA

# 메모리 스왑 방지
LockMemory=yes

# 보안 필터
PrivateTmp=yes
PrivateMounts=yes
LockPersonality=yes
ProtectHostname=yes
ProtectKernelLogs=yes
ProtectControlGroups=yes
RestrictAddressFamilies=AF_INET AF_NETLINK AF_UNIX
RestrictNamespaces=yes
RestrictSUIDSGID=yes
SystemCallArchitectures=native
RemoveIPC=yes
UMask=077

# 재시작 설정
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### 보안 필터 설명

| 필터 | 효과 |
|------|------|
| `PrivateTmp=yes` | `/tmp` 격리 — temp 파일 경유 공격 차단 |
| `PrivateMounts=yes` | 마운트 네임스페이스 격리 |
| `LockPersonality=yes` | 프로세스 ABI 변경 차단 |
| `ProtectHostname=yes` | 호스트명 변경 차단 |
| `ProtectKernelLogs=yes` | `/dev/kmsg`, `dmesg` 접근 차단 |
| `ProtectControlGroups=yes` | cgroup 쓰기 차단 |
| `RestrictAddressFamilies=...` | IPv4, Netlink, Unix 소켓만 허용 |
| `RestrictNamespaces=yes` | 네임스페이스 생성 차단 |
| `RestrictSUIDSGID=yes` | SUID/SGID 실행 차단 |
| `SystemCallArchitectures=native` | 현재 아키텍처 시스콜만 허용 (ARM64) |
| `RemoveIPC=yes` | 종료 시 IPC 오브젝트 정리 |
| `UMask=077` | 생성 파일 소유자 외 접근 차단 |

### 적용하지 않은 필터 (이유)

| 필터 | 적용 불가 이유 |
|------|---------------|
| `ProtectKernelModules=yes` | `modprobe MergingRavennaALSA` 차단됨 |
| `ProtectKernelTunables=yes` | modprobe와 충돌 가능 |
| `RestrictRealtime=yes` | `CPUSchedulingPolicy=fifo` 와 충돌 |
| `PrivateUsers=yes` | root 실행 시 효과 없음 |
| `MemoryDenyWriteExecute=yes` | 데몬 내부 동작과 충돌 가능 |
| `ProtectHome=yes` | WorkingDirectory, 바이너리가 `/home/kjh/aes67` 에 있음 |
| `NoNewPrivileges=yes` | root + modprobe 조합에서 문제 가능 |

---

## 6. 서비스 시작 및 확인

```bash
# 시작
sudo systemctl start aes67-daemon

# 상태 확인
sudo systemctl status aes67-daemon

# 로그 확인
sudo journalctl -u aes67-daemon -f

# 커널 모듈 로드 확인
lsmod | grep MergingRavennaALSA

# ALSA 장치 확인
aplay -l
```

---

## 7. 다른 장치에 적용 시 수정 항목

| 항목 | 위치 | 수정 내용 |
|------|------|----------|
| 네트워크 인터페이스 | `daemon.conf` | `interface_name` 을 실제 인터페이스명으로 변경 |
| 설치 경로 | `aes67-daemon.service` | `WorkingDirectory`, `ExecStart*` 경로 수정 |
| CPU 격리 | `aes67-daemon.service` | `CPUAffinity` 주석 해제 및 코어 번호 조정 |
| PTP 도메인 | `daemon.conf` | 네트워크 환경에 맞게 `ptp_domain` 조정 |

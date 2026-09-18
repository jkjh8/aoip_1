# RAVENNA ALSA 커널모듈 — REF_UNIT 100µs → 1µs 패치

## 무엇을 고치나

`ravenna-alsa-lkm` 드라이버는 PTP/TIC 시각을 **100µs 단위(REF_UNIT)로 절삭**한다
(`PTP.c` 의 `NS_2_REF_UNIT`). `ktime_get()` 이 ns 해상도로 주는 값을 여기서 버리기
때문에, TIC 스케줄·PTP T1/T2·SAC 계산이 전부 100µs = **4.8프레임@48kHz** 로 양자화된다.

장기 평균 주기는 분수 누산기 덕에 정확하지만 개별 TIC 이 ±100µs 튀고, 이것이
엔진 언뮤트 게이트가 보는 `max jump 9.8~14.3fr` 의 정체이자, TIC 크기를 48 아래로
못 내리게 막는 벽이었다.

## 실측 효과 (2026-09-17, 실사용 상태)

| 지표 | 원본 | 패치 |
|---|---|---|
| TIC 간격 편차 (무부하) | ±100µs (±4.8fr) | ±14µs |
| TIC 간격 편차 (오디오 부하) | — | ±46µs |
| 5000틱 중 `>50µs` 이탈 | 3~5 | **0** |
| 엔진 `live clock 600s` max jump | 9.8 ~ 14.3 fr | **1.74 fr** |
| 같은 로그 max abs rate | 195 ~ 200 ppm | **37.8 ppm** |

남은 37.8ppm 은 양자화가 아니라 부하 상태의 TIC 지터(±46µs ≈ 46ppm/1초창)다.

## 함께 고쳐야 했던 것 — 하나라도 빠지면 TIC 이 무너진다

`NS_2_REF_UNIT` 만 바꾸면 **반드시 깨진다.** 실제로 3번 실패했다.

1. **`PTP_WATCHDOG_ELAPSE 20000`** — REF_UNIT 단위 상수. 2초가 20ms 가 되어 PTP
   워치독이 상시 발화, 락이 500ms마다 플랩. → `(2ull * REF_UNIT_PER_SEC)`
2. **`/10000` · `*10000` 4곳** (PTP.c 569,570,637,639) — REF_UNIT↔샘플 환산 상수.
   매크로를 안 쓰고 숫자로 박혀 있어 grep 으로 안 잡힌다. 위상 측정이 100배 틀림.
3. **오버플로** — `ui64T1 * rate` 의 `ui64T1` 은 PTP TAI epoch(≈1.8e9초) 기반이라
   1µs 단위에서 8.6e19 > uint64(1.8e19). → `ref_unit_to_samples()` 로 초/나머지 분할.
   SAC 식(PTP.c 908,909)도 동일.
4. **부호** — `dProportional /= (500ull * ...)` 처럼 제수를 unsigned 로 두면 통상
   산술변환으로 int64 좌변이 uint64 로 승격되어 **음수 위상오차가 2^64 근처 양수**가
   된다(`prop=36893488129`). 적분기가 포화해 주기가 1.4ms 에 고착. → `500LL * (long long)`

주석을 믿지 말 것 — `[100ns]` 라고 적힌 곳들이 실제로는 100µs 다.

## 파일

| 파일 | 설명 |
|---|---|
| `PTP.c.orig` | 패치 전 상등 원본 |
| `PTP.c.patched` | 패치본 (설치용) |
| `PTP.c.patched-debug` | 패치본 + 커널 계측 printk (진단용) |
| `ptp-ref-unit-1us.patch` | 리뷰용 unified diff |
| `MergingRavennaALSA.ko.orig` | 패치 전 빌드된 모듈 (즉시 복구용) |
| `install.sh` | 설치 / 복구 스크립트 |

## 사용법

```bash
sudo ./install.sh            # 패치본 설치 + 검증 (실패 시 자동 복구)
sudo ./install.sh --debug    # 계측 포함 빌드 — dmesg 에 TICSTAT/PLLSTAT
sudo ./install.sh --revert   # 원본 모듈로 즉시 복구 (빌드 없음)
```

드라이버 소스 경로 기본값은 `/home/kjh/aes67-linux-daemon/3rdparty/ravenna-alsa-lkm/driver`.
다르면 `RAVENNA_SRC=/path ./install.sh`.

**커널 업데이트 후에는 재설치가 필요하다** — 모듈이 `/lib/modules/$(uname -r)/` 에
설치되므로 커널이 바뀌면 사라진다.

## 진단 방법 (다음에 이 영역을 건드릴 때)

증상으로 역추적하다 3번 실패하고 라이브 무음 21분을 냈다. 판을 바꾼 두 가지:

1. **엔진의 AES67 브리지를 끈다** (`config/audio.json` 의 `AES67_in`/`AES67_out` →
   `enabled: false`). RAVENNA 카드가 `closed` 가 되어 깨진 TIC 이 아무것도 건드리지
   않는다 — 아날로그는 I2S(`hw:aoip`)로 독립 동작. 반복 실험이 무해해진다.
   단 `aoip.service` 는 `Requires=aes67-daemon.service` 라 "데몬만 재시작"은 불가능하다.

2. **커널에 계측을 넣고 원본부터 baseline 을 잡는다** (`--debug` 빌드):
   - `TICSTAT` — `timerProcess` 에서 REF_UNIT 절삭 **전** raw ns 로 TIC 간격 통계
   - `PLLSTAT` — `a/frac/T2/ticT2/dTIC/prop/IGR/adj/period` 전 단계
   원본과 패치본을 같은 계측으로 비교하면 원인이 숫자 하나로 특정된다
   (`prop=36893488129` 하나로 부호 버그가 잡혔다).

## 검증 시 주의

**`PTP locked` 만 보고 성공 판정하지 말 것.** PTP 가 locked 인데 TIC 이 무너져
18분간 입력이 무음이었던 이력이 있다. 반드시 엔진의 `clock verified`(게이트 통과)와
`live clock 600s ... max jump` 까지 확인할 것. `install.sh` 는 이 판정을 내장한다.

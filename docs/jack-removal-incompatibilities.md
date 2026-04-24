# JACK 제거 비호환 사항 정리

JACK 오디오 서버를 제거하고 `aoip_engine` 단일 C 데몬으로 전환하면서 발생하는 API·프로토콜 비호환 사항을 정리한다.

---

## 1. HTTP API 변경

### 제거된 엔드포인트

| 엔드포인트 | 이유 | 대안 |
|---|---|---|
| `GET /jack/status` | JACK 서버가 없음 | `GET /system/status` — `engine: { running: true }` 로 대체 |
| `GET /jack/ports` | JACK 포트 개념 없음 | `GET /streams` — 내부 채널 목록으로 대체 |
| `GET /jack/connections` | JACK 연결 그래프 없음 | `GET /streams/routes` — 라우팅 매트릭스로 대체 |
| `POST /jack/connect` | JACK `jack_connect()` 없음 | `POST /streams/connect` — 내부 `route add` 명령으로 대체 |
| `POST /jack/disconnect` | JACK `jack_disconnect()` 없음 | `POST /streams/disconnect` — 내부 `route remove` 명령으로 대체 |

### 변경된 엔드포인트

| 엔드포인트 | 변경 내용 |
|---|---|
| `GET /bridges` | `pid` 필드 항상 `null` 반환 (프로세스가 없음, aoip_engine 내부 스레드로 동작) |
| `POST /bridges/:name/restart` | 프로세스 재시작 대신 `bridge stop` + `bridge start` stdin 명령 전송 |

---

## 2. Socket.IO 이벤트 변경

### 제거된 이벤트

| 이벤트 | 이유 | 대안 |
|---|---|---|
| `jack:connect` | JACK 클라이언트 없음 | 라우팅 상태 변경 시 `routes:updated` 이벤트로 대체 |
| `jack:disconnect` | JACK 클라이언트 없음 | `routes:updated` 이벤트로 대체 |
| `jack:ports` | JACK 포트 목록 없음 | `channels:updated` 이벤트로 대체 |

---

## 3. 채널·포트 이름 체계 변경

### 기존 (JACK 포트 이름)

```
audio_in_1:audio_in_1       (ALSA 캡처 클라이언트)
audio_out_1:audio_out_1     (ALSA 재생 클라이언트)
dsp_engine:in_1             (DSP 입력 포트)
dsp_engine:out_1            (DSP 출력 포트)
jack_pipe_in:output_1       (RTP → JACK FIFO 입력)
jack_pipe_out:input_1       (JACK → RTP FIFO 출력)
```

### 신규 (내부 채널 ID)

JACK 포트 이름 대신 `channels.js` 가 부여한 **globalId (1-based 정수)** 를 사용한다.

```
analog:out_1   (내부 표기, aoip_engine ALSA 캡처 채널 1)
analog:sin_1   (내부 표기, aoip_engine ALSA 재생 채널 1)
```

프론트엔드에서 채널을 식별할 때는 `channel.id` (정수) 또는 `channel.name` (사람이 읽을 수 있는 레이블)을 사용한다.

---

## 4. rtp_recv / rtp_send 변경

### rtp_recv

| 항목 | 기존 | 신규 |
|---|---|---|
| 출력 대상 | JACK 포트 (`jack_connect`) | Named FIFO (`/tmp/rtp_in_<key>`) |
| stdout 메시지 `ports:` | 출력됨 | **제거** (JACK 포트가 없으므로 의미 없음) |
| JACK 클라이언트 이름 | `rtp_recv_<key>` | 없음 (pipe 모드) |
| 실행 인자 마지막 두 인자 | *(없음 또는 jack)* | `pipe <fifo_path>` |

stdout `ports:` 메시지를 파싱하는 기존 Node.js 코드(`gstreamer.js`)는 **pipe 모드에서 해당 메시지를 수신하지 않으므로** 해당 파싱 로직을 제거하였다. 준비 완료 판정은 `[rtp_recv] ready` 메시지로 대체된다.

### rtp_send

| 항목 | 기존 | 신규 |
|---|---|---|
| 입력 소스 | JACK 포트 | Named FIFO (`/tmp/rtp_out_<key>`) |
| 실행 모드 | jack 모드 (기본) | pipe 모드 (`-pipe <fifo_path>`) |

---

## 5. ALSA 브릿지 프로세스 제거

### 제거된 외부 프로세스

| 프로세스 | 역할 | 대체 |
|---|---|---|
| `audio_in` | ALSA 캡처 → JACK | aoip_engine 내부 ALSA 캡처 스레드 |
| `audio_out` | JACK → ALSA 재생 | aoip_engine 내부 ALSA 재생 스레드 |
| `zita-a2j` | ALSA → JACK (고품질 SRC) | aoip_engine 내부 libsamplerate 드리프트 보정 |
| `zita-j2a` | JACK → ALSA (고품질 SRC) | aoip_engine 내부 libsamplerate 드리프트 보정 |
| `dsp_engine` | JACK DSP 처리 | aoip_engine 내부 DSP 스레드 |
| `jack_pipe_in` | FIFO → JACK 입력 | aoip_engine `rtp_in` FIFO 리더 스레드 |
| `jack_pipe_out` | JACK 출력 → FIFO | aoip_engine `rtp_out` FIFO 라이터 스레드 |
| `jackd` | JACK 서버 | 없음 (timerfd 기반 마스터 타이밍) |

브릿지 API(`GET /bridges`, `POST /bridges/:name/restart` 등)의 응답 포맷은 유지되지만, `pid` 필드는 항상 `null`이다.

---

## 6. aoip_engine stdin 프로토콜 (신규)

Node.js (`bridges.js`, `gstreamer.js`, `dsp.js`)가 aoip_engine과 통신하는 stdin 명령 목록.

### 브릿지 관리

```
bridge add <name> <device> <rate> <period> <periods> <channels> <ch_start>
bridge add_in <name> <device> <rate> <period> <periods> <channels> <ch_start>
bridge add_out <name> <device> <rate> <period> <periods> <channels> <ch_start>
bridge stop <name>
bridge start <name>
```

- `add` : 캡처 + 재생 양방향 (analog, USB 전이중 장치)
- `add_in` : 캡처 전용 (AES67 입력 전용 장치)
- `add_out` : 재생 전용 (AES67 출력 전용 장치)
- `ch_start` : 이 장치의 DSP 버퍼 내 첫 번째 채널 인덱스 (0-based)

### RTP FIFO 관리

```
rtp_in add <key> <fifo_path> <channels> <ch_start>
rtp_in remove <key>
rtp_out add <key> <fifo_path> <channels> <ch_start>
rtp_out remove <key>
```

### 라우팅 매트릭스

```
route add <src_globalId> <dst_globalId>
route remove <src_globalId> <dst_globalId>
```

### DSP 명령 (기존 dsp_engine과 동일)

```
gain in|out <ch> <linear>
mute in|out <ch> <0|1>
bypass in|out <ch> <0|1>
hpf in <ch> enable|freq|slope <value>
eq in|out <ch> <band> enable|coeffs|freq|gain|q|type <value>
limiter out <ch> enable|threshold|attack|release|makeup <value>
```

---

## 7. aoip_engine stdout 프로토콜 (변경)

| 메시지 | 상태 | 비고 |
|---|---|---|
| `[aoip_engine] ready` | **신규** | 기존 `[dsp_engine] ready` 대체 |
| `lvl in <ch> <db>` | 유지 | 동일 |
| `lvl out <ch> <db>` | 유지 | 동일 |
| `lm out <ch> <pre> <post>` | 유지 | 동일 |
| `bridge:<name>:ready` | **신규** | 브릿지 스레드 준비 완료 |
| `bridge:<name>:stopped` | **신규** | 브릿지 스레드 중지 완료 |
| `ports: <name> ...` | **제거** | rtp_recv JACK 포트 알림, pipe 모드에서 불필요 |
| `[dsp_engine] ready` | **제거** | aoip_engine ready로 대체 |

---

## 8. 빌드 의존성 변경

### 제거된 의존성

- `libjack` / `libjack2` — JACK 클라이언트 라이브러리

### 유지되는 의존성

- `libasound2-dev` (ALSA)
- `libsamplerate-dev` (libsamplerate, SRC)
- `libgstreamer1.0-dev` (rtp_recv, rtp_send GStreamer 파이프라인)

### 빌드 명령

```bash
# aoip_engine
gcc -O2 -march=native -o scripts/aoip_engine scripts/aoip_engine.c \
    -lrt -lasound -lsamplerate -lpthread -lm

# rtp_recv (JACK 없이)
gcc -O2 -o scripts/rtp_recv scripts/rtp_recv.c \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0) -lpthread -lm

# rtp_send (기존과 동일, JACK 링크 제거)
gcc -O2 -o scripts/rtp_send scripts/rtp_send.c \
    $(pkg-config --cflags --libs gstreamer-1.0) -lpthread -lm
```

---

## 9. 프론트엔드 영향 없음 확인 목록

아래 항목은 API 시그니처가 동일하게 유지되므로 프론트엔드 수정이 **불필요**하다.

- `GET /streams` — 채널 목록 (입력/출력 배열, 동일 포맷)
- `POST /streams/connect` `{ src, dst }` — 라우팅 연결
- `POST /streams/disconnect` `{ src, dst }` — 라우팅 해제
- `GET /dsp/channel/:id` — 채널 DSP 설정 조회
- `PUT /dsp/channel/:id` — 채널 DSP 설정 저장 + 적용
- `GET /system/status` — 시스템 상태 (포맷 유지, `jack` 하위 필드는 `null`)
- Socket.IO `meters` 이벤트 — 레벨 미터 데이터 (동일 포맷)
- Socket.IO `channels:updated` — 채널 상태 변경 알림

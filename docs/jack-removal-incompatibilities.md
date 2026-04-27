# JACK 제거 비호환 사항 정리

JACK 오디오 서버를 제거하고 `aoip_engine` 단일 C 데몬으로 전환하면서 발생한 API·프로토콜 비호환 사항.

---

## 1. HTTP API 변경

### 제거된 엔드포인트

| 엔드포인트 | 이유 | 대안 |
|---|---|---|
| `GET /jack/status` | JACK 서버 없음 | `status.engine.running` (Socket.IO `status` 이벤트) |
| `GET /jack/ports` | JACK 포트 개념 없음 | 없음 (채널 목록은 `GET /channels` 로 대체) |
| `GET /jack/connections` | JACK 연결 그래프 없음 | `status.channels` 라우팅 매트릭스 |
| `POST /jack/connect` | JACK `jack_connect()` 없음 | Socket.IO `route:add { src, dst }` |
| `POST /jack/disconnect` | JACK `jack_disconnect()` 없음 | Socket.IO `route:remove { src, dst }` |

### 추가된 엔드포인트

#### `/streams` — RTP 스트림 관리

| 메서드 | 경로 | 설명 |
|---|---|---|
| `GET` | `/streams/rtp` | 전체 rtp_stream 목록 + 상태 |
| `GET` | `/streams/rtp/:client` | 특정 스트림 상세 (targets, stats 포함) |
| `POST` | `/streams/rtp/:client/start` | 스트림 시작 (rtp_in: 설정 동시 적용 가능) |
| `POST` | `/streams/rtp/:client/stop` | 스트림 정지 |
| `PUT` | `/streams/rtp/:client/config` | rtp_in 설정 변경 (재시작 없이 저장) |
| `POST` | `/streams/rtp/:client/targets` | rtp_out 전송 대상 추가 |
| `DELETE` | `/streams/rtp/:client/targets` | rtp_out 전송 대상 제거 |
| `PUT` | `/streams/rtp/:client/codec` | rtp_out 코덱/비트레이트 변경 |

### 변경된 엔드포인트

| 엔드포인트 | 변경 내용 |
|---|---|
| `GET /bridges` | `pid` 필드 항상 `null` (aoip_engine 내부 스레드로 동작) |
| `POST /bridges/:name/restart` | 프로세스 재시작 대신 `bridge stop` + `bridge start` stdin 명령 전송 |

---

## 2. Socket.IO 이벤트 변경

### 제거된 이벤트 (클라이언트→서버)

| 이벤트 | 이유 | 대안 |
|---|---|---|
| `jack:start` | JACK 없음 | 불필요 (aoip_engine이 서버 시작 시 자동 기동) |
| `jack:stop` | JACK 없음 | 불필요 |
| `jack:connect { src, dst }` | JACK `jack_connect()` 없음 | `route:add { src, dst }` |
| `jack:disconnect { src, dst }` | JACK `jack_disconnect()` 없음 | `route:remove { src, dst }` |

### 추가된 이벤트 (클라이언트→서버)

| 이벤트 | 페이로드 | 설명 |
|---|---|---|
| `route:add` | `{ src, dst }` | 라우팅 매트릭스 연결 추가 → aoip_engine `route add` |
| `route:remove` | `{ src, dst }` | 라우팅 매트릭스 연결 제거 → aoip_engine `route remove` |
| `rtp:stream:start` | `{ client, ...설정 }` | rtp_stream 시작 (rtp_in은 설정 동시 적용) |
| `rtp:stream:stop` | `{ client }` | rtp_stream 정지 |
| `rtp:stream:get` | `{ client }` | 스트림 상세 조회 |
| `rtp:streams:list` | — | 전체 스트림 목록 |
| `rtp:in:config` | `{ client, ...설정 }` | rtp_in 설정 변경 (저장만, 재시작 필요) |
| `rtp:out:target:add` | `{ client, host, port }` | rtp_out 전송 대상 추가 |
| `rtp:out:target:remove` | `{ client, host, port }` | rtp_out 전송 대상 제거 |
| `rtp:out:codec` | `{ client, codec, bitrate? }` | rtp_out 코덱 변경 |

### 변경된 서버→클라이언트 `status` 이벤트 페이로드

**이전:**
```json
{
  "jack": { "running": false, "ports": [], "connections": [] },
  ...
}
```

**현재:**
```json
{
  "engine": { "running": true },
  "bridges": { ... },
  "streams": { "rx": {...}, "tx": {...}, "rtpStreams": [...] },
  "rxStats": { ... },
  "channels": { "inputs": [...], "outputs": [...] },
  "usb": { "enabled": true, "connected": false },
  "aes67": { "running": false, "ready": false, "url": "..." }
}
```

- `jack` 키 → `engine` 키로 교체
- `engine.running` : aoip_engine 프로세스가 살아있으면 `true`
- `streams.rtpStreams` : rtp_streams 배열 추가

---

## 3. rtp_send 변경 — multiudpsink 적용

### 변경 내용

| 항목 | 이전 | 현재 |
|---|---|---|
| Sink 구성 | `tee → queue × N → udpsink × N` | `multiudpsink` 단일 엘리먼트 |
| target 추가 | 파이프라인 전체 재빌드 | `g_signal_emit_by_name("add", host, port)` 즉시 적용 |
| target 제거 | 파이프라인 전체 재빌드 | `g_signal_emit_by_name("remove", host, port)` 즉시 적용 |
| 코덱 변경 | 파이프라인 재빌드 | 파이프라인 재빌드 (코덱 변경은 불가피) |

target 추가·제거 시 오디오 중단 없음.

---

## 4. RTP 정지 시 버퍼 잔류 노이즈 수정

### 문제
`rtp:stream:stop` 호출 시 rtp_recv 프로세스를 먼저 kill하면, aoip_engine이 SHM ring buffer에 남은 데이터를 계속 읽어 노이즈 발생.

### 수정 내용

**`lib/gstreamer.js` — `stopRtpStream()`**  
엔진에 `rtp_in remove` 전송 순서를 프로세스 kill **이전**으로 변경:
```
이전: kill proc → rtp_in remove
현재: rtp_in remove → kill proc
```

**`scripts/aoip_engine.c` — DSP 루프**  
비활성(`!r->enabled || !r->shm`) rtp_in 슬롯의 해당 입력 채널을 `memset(0)` 으로 무음 처리:
```
이전: continue (이전 값 그대로 남음)
현재: 채널 버퍼 무음 → continue
```

---

## 5. 채널·포트 이름 체계

내부 포트 이름은 `channels.js`가 부여한 jackPort 문자열을 그대로 사용:

```
analog:out_1   — aoip_engine 아날로그 캡처 채널 1
analog:sin_1   — aoip_engine 아날로그 재생 채널 1
usb:out_1      — USB 캡처 채널 1
aes67:out_1    — AES67 캡처 채널 1
```

`route:add` / `route:remove` 이벤트의 `src`, `dst` 값은 이 포트 이름을 사용.

---

## 6. aoip_engine stdin 프로토콜

Node.js가 aoip_engine stdin으로 전송하는 명령 목록.

### 브릿지 관리

```
bridge add <name> <device> <rate> <period> <periods> <channels> <ch_start>
bridge add_in  <name> <device> <rate> <period> <periods> <channels> <ch_start>
bridge add_out <name> <device> <rate> <period> <periods> <channels> <ch_start>
bridge stop <name>
bridge start <name>
```

### RTP SHM 관리

```
rtp_in add <key> <shm_name> <channels> <ch_start>
rtp_in remove <key>
rtp_out add <key> <shm_name> <channels> <ch_start>
rtp_out remove <key>
```

### 라우팅 매트릭스

```
route add <src_globalId> <dst_globalId>
route remove <src_globalId> <dst_globalId>
```

### DSP

```
gain in|out <ch> <linear>
mute in|out <ch> <0|1>
bypass in|out <ch> <0|1>
hpf in <ch> enable|freq|slope <value>
eq in|out <ch> <band> enable|coeffs|freq|gain|q|type <value>
limiter out <ch> enable|threshold|attack|release|makeup <value>
```

---

## 7. 빌드

```bash
cd scripts

# aoip_engine
gcc -O3 -march=native -funroll-loops -o aoip_engine aoip_engine.c \
    -lrt -lasound -lsamplerate -lpthread -lm

# rtp_recv
gcc -O2 -o rtp_recv rtp_recv.c \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-net-1.0 gio-2.0) \
    -lpthread -lm

# rtp_send (multiudpsink 사용)
gcc -O2 -o rtp_send rtp_send.c \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0) \
    -lpthread -lm

# 또는 Makefile
make
```

---

## 8. 프론트엔드 수정 필요 사항

| 항목 | 변경 전 | 변경 후 |
|---|---|---|
| 연결 상태 확인 | `status.jack.running` | `status.engine.running` |
| 라우팅 연결 이벤트 | `jack:connect { src, dst }` | `route:add { src, dst }` |
| 라우팅 해제 이벤트 | `jack:disconnect { src, dst }` | `route:remove { src, dst }` |
| RTP 스트림 시작 | 없음 | `rtp:stream:start { client }` |
| RTP 스트림 정지 | 없음 | `rtp:stream:stop { client }` |
| RTP 전송 대상 추가 | `tx:target:add { host, port }` (legacy) | `rtp:out:target:add { client, host, port }` |

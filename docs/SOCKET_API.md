# AOIP Socket.IO API Reference

프론트엔드 ↔ 서버 간 실시간 통신 API 문서입니다.  
REST API는 더 이상 사용하지 않고 Socket.IO 이벤트로 통신합니다.

---

## 연결

```js
import { io } from 'socket.io-client';
const socket = io('http://<host>:<port>');
```

- 라이브러리: Socket.IO v4.x
- 연결 즉시 서버가 `status` 이벤트를 전송합니다.
- 연결 즉시 서버가 `aes67:sources`, `aes67:sinks` 이벤트를 전송합니다.

---

## 응답 형식 규칙

Acknowledgement callback을 사용하는 모든 이벤트는 다음 형식으로 응답합니다.

```js
// 성공
{ ok: true, ...data }

// 실패
{ ok: false, error: "error message" }
```

이벤트 발행 방법:

```js
socket.emit('event:name', payload, (response) => {
  if (!response.ok) console.error(response.error);
});
```

---

## Server → Client (서버가 자동으로 보내는 이벤트)

### `status` — 전체 상태 스냅샷 (2초 주기 + 변경 시 즉시)

시스템 전체 상태를 전송합니다. 연결 시에도 즉시 전송됩니다.

```ts
{
  engine: {
    running: boolean;        // DSP 엔진 실행 여부
  };
  bridges: {
    [name: string]: {
      running: boolean;
    };
  };
  streams: {
    rtpStreams: RtpStream[];
  };
  rxStats: {
    client: string;
    name: string;
    // RTP 수신 통계
  }[];
  channels: {
    inputs: Channel[];
    outputs: Channel[];
  };
  connections: {
    port: string;
    connections: string[];
  }[];
  aes67: {
    running: boolean;
    ready: boolean;
    url: string;
  };
}
```

**Channel 구조:**

```ts
{
  id: number;
  name: string;
  label: string;
  gain: number;           // dB
  muted: boolean;
  level: number;          // dBFS (실시간, levels 이벤트에서 갱신)
  hpf: {
    enabled: boolean;
    freq: number;
    slope: number;
  };
  eq: {
    enabled: boolean;
    bands: EqBand[];
  };
  limiter?: {             // output only
    enabled: boolean;
    threshold: number;    // dBFS
    attack: number;       // ms
    release: number;      // ms
    makeup: number;       // dB
  };
}
```

**RtpStream 구조:**

```ts
{
  client: string;
  name: string;
  type: 'rtp_in' | 'rtp_out';
  running: boolean;
  port?: number;
  address?: string;
  channels?: number;
  sampleRate?: number;
  codec?: string;
  bufferMs?: number;
  targets?: { host: string; port: number }[];  // rtp_out only
  stats?: {
    srcIp: string;
    srcPort: number;
    codec: string;
    bitrateKbps: number;
  };
}
```

---

### `levels` — 레벨 미터 (80ms 주기, ~12fps)

CPU 부하를 줄이기 위해 `status`와 별도로 빠른 주기로 전송됩니다.

```ts
{
  inputs: { id: number; level: number }[];
  outputs: {
    id: number;
    level: number;
    limiter?: number | null;  // limiter:watch 등록 시에만 포함, dBFS
  }[];
}
```

---

### `channels` — 채널 상태 변경 시 즉시

DSP/라우팅 변경 시 서버가 즉시 브로드캐스트합니다.

```ts
{
  inputs: Channel[];
  outputs: Channel[];
}
```

---

### `aes67:sources` — AES67 소스 목록 갱신

연결 시 초기 데이터 전송, 이후 소스 추가/제거 시 자동 전송됩니다.

```ts
{
  id: number;
  enabled: boolean;
  name: string;
  io: string;
  max_samples_per_packet: number;
  codec: string;
  address: string;         // multicast IP
  ttl: number;
  payload_type: number;
  dscp: number;
  refclk_ptp_traceable: boolean;
  map: number[];           // 채널 매핑
}[]
```

---

### `aes67:sinks` — AES67 싱크 목록 갱신

연결 시 초기 데이터 전송, 이후 싱크 추가/제거/SDP변경 시 자동 전송됩니다.

```ts
{
  id: number;
  name: string;
  io: string;
  use_sdp: boolean;
  source: string;          // SDP URL or SDP body
  delay: number;
  ignore_refclk_gmid: boolean;
  map: number[];
}[]
```

---

### `aes67:ptp:status` — PTP 상태 변경 시

PTP 클락 상태가 변경될 때 자동 전송됩니다.

```ts
{
  status: 'locked' | 'locking' | 'unlocked' | string;
}
```

---

## Client → Server (클라이언트가 요청하는 이벤트)

---

### AES67 / Daemon

#### `aes67:status` — 데몬 상태 조회

```js
socket.emit('aes67:status', (res) => {
  // res: { running: boolean, ready: boolean, url: string }
});
```

---

#### `aes67:config:get` — 데몬 설정 조회

```js
socket.emit('aes67:config:get', (res) => {
  // res: { ok: true, config: { http_port, sample_rate, ptp_domain, ... } }
});
```

---

#### `aes67:config:set` — 데몬 설정 변경

```js
socket.emit('aes67:config:set', {
  sample_rate: 48000,
  playout_delay: 3,
  // ...변경할 필드만
}, (res) => {
  // res: { ok: true }
});
```

---

#### `aes67:ptp:config:get` — PTP 설정 조회

```js
socket.emit('aes67:ptp:config:get', (res) => {
  // res: { ok: true, config: { domain: number, dscp: number } }
});
```

---

#### `aes67:ptp:config:set` — PTP 설정 변경

```js
socket.emit('aes67:ptp:config:set', {
  domain: 0,
  dscp: 48,
}, (res) => {
  // res: { ok: true }
});
```

---

#### `aes67:ptp:status` — PTP 현재 상태 조회 (on-demand)

> 자동 push(`aes67:ptp:status` 브로드캐스트)로 대부분 처리되므로 초기 로드 시에만 사용.

```js
socket.emit('aes67:ptp:status', (res) => {
  // res: { ok: true, status: { status: string, ... } }
});
```

---

#### `aes67:source:add` — AES67 소스 추가/수정

성공 시 모든 클라이언트에 `aes67:sources` 브로드캐스트됩니다.

```js
socket.emit('aes67:source:add', {
  id: 0,                          // 필수
  enabled: true,
  name: 'ALSA Source 0',
  io: 'Audio Device',
  max_samples_per_packet: 48,
  codec: 'L24',
  address: '239.69.0.126',
  ttl: 15,
  payload_type: 98,
  dscp: 34,
  refclk_ptp_traceable: false,
  map: [0, 1],
}, (res) => {
  // res: { ok: true }
});
```

---

#### `aes67:source:remove` — AES67 소스 제거

성공 시 모든 클라이언트에 `aes67:sources` 브로드캐스트됩니다.

```js
socket.emit('aes67:source:remove', { id: 0 }, (res) => {
  // res: { ok: true }
});
```

---

#### `aes67:source:sdp` — 소스 SDP 조회

```js
socket.emit('aes67:source:sdp', { id: 0 }, (res) => {
  // res: { ok: true, sdp: "v=0\r\no=- ..." }
});
```

---

#### `aes67:sink:add` — AES67 싱크 추가/수정

성공 시 모든 클라이언트에 `aes67:sinks` 브로드캐스트됩니다.

```js
socket.emit('aes67:sink:add', {
  id: 0,                           // 필수
  name: 'ALSA Sink 0',
  io: 'Audio Device',
  use_sdp: true,
  source: 'http://192.168.1.x:987/api/source/sdp/0',
  delay: 576,
  ignore_refclk_gmid: true,
  map: [0, 1],
}, (res) => {
  // res: { ok: true }
});
```

---

#### `aes67:sink:remove` — AES67 싱크 제거

성공 시 모든 클라이언트에 `aes67:sinks` 브로드캐스트됩니다.

```js
socket.emit('aes67:sink:remove', { id: 0 }, (res) => {
  // res: { ok: true }
});
```

---

#### `aes67:sink:status` — 싱크 실시간 수신 상태 조회

```js
socket.emit('aes67:sink:status', { id: 0 }, (res) => {
  // res: {
  //   ok: true,
  //   status: {
  //     sink_flags: {
  //       receiving_rtp_packet: boolean,
  //       rtp_seq_id_error: boolean,
  //       rtp_ssrc_error: boolean,
  //       rtp_payload_type_error: boolean,
  //       muted: boolean,
  //     }
  //   }
  // }
});
```

---

#### `aes67:browse` — 네트워크 AES67 소스 탐색

```js
socket.emit('aes67:browse', {
  type: 'all',    // 'all' | 'mdns' | 'sap'  (기본값: 'all')
}, (res) => {
  // res: { ok: true, sources: [...] }
});
```

---

### 라우팅 매트릭스

#### `route:add` — 채널 연결

```js
socket.emit('route:add', {
  src: 'input:0',    // 'input:<id>' 또는 'rtp:<client>:<ch>'
  dst: 'output:0',   // 'output:<id>'
}, (res) => {
  // res: { ok: true }
  // 성공 시 status 브로드캐스트
});
```

---

#### `route:remove` — 채널 연결 해제

```js
socket.emit('route:remove', {
  src: 'input:0',
  dst: 'output:0',
}, (res) => {
  // res: { ok: true }
  // 성공 시 status 브로드캐스트
});
```

---

### 채널 제어

#### `ch:gain` — 채널 게인 설정

```js
socket.emit('ch:gain', {
  type: 'input',   // 'input' | 'output'
  id: 0,
  gain: -6.0,      // dB
}, (res) => {
  // res: { ok: true }
  // 성공 시 status 브로드캐스트
});
```

---

#### `ch:mute` — 채널 뮤트

```js
socket.emit('ch:mute', {
  type: 'input',   // 'input' | 'output'
  id: 0,
  muted: true,
}, (res) => {
  // res: { ok: true }
  // 성공 시 status 브로드캐스트
});
```

---

#### `ch:label` — 채널 레이블 변경

```js
socket.emit('ch:label', {
  type: 'input',   // 'input' | 'output'
  id: 0,
  label: 'Mic 1',
}, (res) => {
  // res: { ok: true }
});
```

---

#### `dsp:bypass` — DSP 일시 바이패스

모든 채널의 EQ/HPF/리미터를 무시하고 신호를 통과시킵니다.

```js
socket.emit('dsp:bypass', (res) => {
  // res: { ok: true }
});
```

---

#### `dsp:restore` — DSP 설정 복원

바이패스 상태에서 저장된 DSP 설정을 다시 적용합니다.

```js
socket.emit('dsp:restore', (res) => {
  // res: { ok: true }
});
```

---

### DSP 파라미터

#### `dsp:hpf` — HPF (High Pass Filter) 설정

60ms 디바운스 적용 후 DSP 엔진에 전송됩니다.

```js
socket.emit('dsp:hpf', {
  id: 0,              // 채널 id
  freq: 80,           // Hz
  slope: 12,          // dB/oct: 12 | 24
  enabled: true,
}, (res) => {
  // res: { ok: true }
  // 즉시 channels 브로드캐스트
});
```

---

#### `dsp:eq` — EQ 밴드 파라미터 설정

60ms 디바운스 적용 후 DSP 엔진에 전송됩니다.

```js
socket.emit('dsp:eq', {
  type: 'input',     // 'input' | 'output'
  id: 0,             // 채널 id
  band: 0,           // 밴드 인덱스 (0-based)
  freq: 1000,        // Hz
  gain: 0.0,         // dB
  q: 0.707,
  type_: 'peak',    // 'peak' | 'lowshelf' | 'highshelf' | 'lowpass' | 'highpass'
  enabled: true,
}, (res) => {
  // res: { ok: true }
  // 즉시 channels 브로드캐스트
});
```

---

#### `dsp:eq_toggle` — EQ 전체 활성화/비활성화

```js
socket.emit('dsp:eq_toggle', {
  type: 'input',     // 'input' | 'output'
  id: 0,
  enabled: false,
}, (res) => {
  // res: { ok: true }
  // 즉시 channels 브로드캐스트
});
```

---

#### `dsp:limiter` — 리미터 설정 (output only)

```js
socket.emit('dsp:limiter', {
  id: 0,
  enabled: true,
  threshold: -6.0,   // dBFS
  attack: 1.0,       // ms
  release: 100.0,    // ms
  makeup: 0.0,       // dB
}, (res) => {
  // res: { ok: true }
  // 즉시 channels 브로드캐스트
});
```

---

#### `limiter:watch` — 리미터 미터 모니터링 등록/해제

등록하면 `levels` 이벤트의 `outputs[i].limiter` 필드가 채워집니다.

```js
// 모니터링 시작
socket.emit('limiter:watch', { id: 0, watch: true });

// 모니터링 중지
socket.emit('limiter:watch', { id: 0, watch: false });
```

---

### RTP 스트림

#### `rtp:streams:list` — 전체 RTP 스트림 목록

```js
socket.emit('rtp:streams:list', (res) => {
  // res: { ok: true, streams: RtpStream[] }
});
```

---

#### `rtp:stream:get` — 특정 RTP 스트림 상세

```js
socket.emit('rtp:stream:get', { client: 'rtp_in_0' }, (res) => {
  // res: { ok: true, stream: RtpStream }
});
```

---

#### `rtp:stream:start` — RTP 스트림 시작

`rtp_in`의 경우 config 필드를 함께 전달하면 시작 전에 적용됩니다.

```js
socket.emit('rtp:stream:start', {
  client: 'rtp_in_0',
  // rtp_in 전용 옵션 (선택):
  port: 5004,
  address: '0.0.0.0',
  channels: 2,
  sampleRate: 48000,
  bufferMs: 100,
}, (res) => {
  // res: { ok: true, stream: RtpStream }
  // 성공 시 status 브로드캐스트
});
```

---

#### `rtp:stream:stop` — RTP 스트림 중지

```js
socket.emit('rtp:stream:stop', { client: 'rtp_in_0' }, (res) => {
  // res: { ok: true }
  // 성공 시 status 브로드캐스트
});
```

---

#### `rtp:in:config` — RTP 수신 설정 변경 (실행 중 가능)

```js
socket.emit('rtp:in:config', {
  client: 'rtp_in_0',
  port: 5004,
  address: '0.0.0.0',
  channels: 2,
  sampleRate: 48000,
  bufferMs: 100,
}, (res) => {
  // res: { ok: true, stream: RtpStream }
  // 성공 시 status 브로드캐스트
});
```

---

#### `rtp:out:target:add` — RTP 송신 대상 추가

```js
socket.emit('rtp:out:target:add', {
  client: 'rtp_out_0',
  host: '192.168.1.100',
  port: 5004,
}, (res) => {
  // res: { ok: true, stream: RtpStream }
  // 성공 시 status 브로드캐스트
});
```

---

#### `rtp:out:target:remove` — RTP 송신 대상 제거

```js
socket.emit('rtp:out:target:remove', {
  client: 'rtp_out_0',
  host: '192.168.1.100',
  port: 5004,
}, (res) => {
  // res: { ok: true, stream: RtpStream }
  // 성공 시 status 브로드캐스트
});
```

---

#### `rtp:out:codec` — RTP 송신 코덱 변경

```js
socket.emit('rtp:out:codec', {
  client: 'rtp_out_0',
  codec: 'mp3',        // 'mp3' | 'raw'
  bitrate: 320,        // kbps (mp3 전용)
}, (res) => {
  // res: { ok: true, stream: RtpStream }
  // 성공 시 status 브로드캐스트
});
```

---

### 시스템

#### `system:network:get` — 네트워크 정보 조회

```js
socket.emit('system:network:get', { iface: 'eth0' }, (res) => {
  // res: {
  //   iface: string,
  //   ip: string,
  //   subnet: string,
  //   gateway: string,
  //   dns: string,
  //   mode: 'dhcp' | 'static',
  //   mac: string,
  // }
});
```

---

#### `system:network:set` — 네트워크 설정 변경

```js
// DHCP
socket.emit('system:network:set', {
  iface: 'eth0',
  mode: 'dhcp',
}, (res) => {
  // res: { ok: true }
});

// Static IP
socket.emit('system:network:set', {
  iface: 'eth0',
  mode: 'static',
  ip: '192.168.1.100',
  subnet: '255.255.255.0',
  gateway: '192.168.1.1',
  dns: '8.8.8.8',
}, (res) => {
  // res: { ok: true }
});
```

---

#### `system:reboot` — 시스템 재시작

```js
socket.emit('system:reboot', (res) => {
  // res: { ok: true }
  // 이후 연결이 끊어집니다
});
```

---

## 프론트엔드 초기화 패턴

```js
const socket = io('http://<host>:<port>');

// 전체 상태 — 초기 로드 및 주기적 갱신
socket.on('status', (state) => {
  store.setEngine(state.engine);
  store.setChannels(state.channels);
  store.setConnections(state.connections);
  store.setStreams(state.streams);
  store.setAes67(state.aes67);
});

// 레벨 미터 — 빠른 갱신
socket.on('levels', ({ inputs, outputs }) => {
  store.updateLevels(inputs, outputs);
});

// 채널 즉시 갱신 (DSP 변경 시)
socket.on('channels', (channels) => {
  store.setChannels(channels);
});

// AES67 — 이벤트 기반 push
socket.on('aes67:sources', (sources) => {
  store.setAes67Sources(sources);
});
socket.on('aes67:sinks', (sinks) => {
  store.setAes67Sinks(sinks);
});
socket.on('aes67:ptp:status', ({ status }) => {
  store.setPtpStatus(status);
});
```

---

## 기존 REST API와의 매핑

| 기존 REST | Socket 이벤트 |
|-----------|---------------|
| `GET /aes67/status` | `aes67:status` |
| `GET /aes67/config` | `aes67:config:get` |
| `POST /aes67/config` | `aes67:config:set` |
| `GET /aes67/ptp/config` | `aes67:ptp:config:get` |
| `POST /aes67/ptp/config` | `aes67:ptp:config:set` |
| `GET /aes67/ptp/status` | `aes67:ptp:status` (요청) 또는 자동 push |
| `GET /aes67/sources` | 연결 시 자동 push `aes67:sources` |
| `PUT /aes67/sources/:id` | `aes67:source:add` |
| `DELETE /aes67/sources/:id` | `aes67:source:remove` |
| `GET /aes67/sources/:id/sdp` | `aes67:source:sdp` |
| `GET /aes67/sinks` | 연결 시 자동 push `aes67:sinks` |
| `PUT /aes67/sinks/:id` | `aes67:sink:add` |
| `DELETE /aes67/sinks/:id` | `aes67:sink:remove` |
| `GET /aes67/sinks/:id/status` | `aes67:sink:status` |
| `GET /aes67/browse` | `aes67:browse` |
| `GET /streams` | `status.streams` |
| `POST /streams/rtp/:client/start` | `rtp:stream:start` |
| `POST /streams/rtp/:client/stop` | `rtp:stream:stop` |
| `PUT /streams/rtp/:client/config` | `rtp:in:config` |
| `POST /streams/rtp/:client/targets` | `rtp:out:target:add` |
| `DELETE /streams/rtp/:client/targets` | `rtp:out:target:remove` |
| `PUT /streams/rtp/:client/codec` | `rtp:out:codec` |
| `GET /system/network` | `system:network:get` |
| `POST /system/network` | `system:network:set` |
| `POST /system/reboot` | `system:reboot` |

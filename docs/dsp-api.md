# DSP API Reference

Complete reference for the DSP processing chain Socket.IO API and engine protocol.

## DSP Processing Chains

**Input channel:** fader → trim (±20 dB) → HPF (12/24/48 dB/oct) → 4-band EQ → Gate → Compressor

**Output channel:** fader → 4-band EQ → Compressor → Limiter

All DSP modules default to disabled (`enabled: false`). Disabled modules pass audio through unprocessed.

---

## Socket.IO Events — Client → Server

All events accept an optional acknowledgement callback `(response) => {}` where `response = { ok: true }` on success or `{ ok: false, error: string }` on failure.

### `dsp:trim`
Input channel analog trim gain.

```typescript
interface DspTrimPayload {
  type: 'input'
  id: number          // channel id (1-based)
  db: number          // trim gain, range [-20, +20] dB
}
```

### `dsp:hpf`
Input channel high-pass filter (Butterworth).

```typescript
interface DspHpfPayload {
  id: number          // input channel id
  params: {
    enabled: boolean
    slope: 12 | 24 | 48   // dB/octave (1, 2, or 4 cascaded biquad sections)
    fc: number             // cutoff frequency, Hz (range: 20–20000)
  }
}
```

### `dsp:eq:band`
Parametric EQ band (4 bands per channel, input and output).

```typescript
interface DspEqBandPayload {
  type: 'input' | 'output'
  id: number
  band: 1 | 2 | 3 | 4
  params: {
    enabled: boolean
    type: 'peak' | 'low_shelf' | 'high_shelf' | 'notch'
    fc: number       // center/corner frequency, Hz (range: 20–20000)
    q: number        // Q factor (range: 0.1–10.0)
    gainDb: number   // boost/cut in dB (range: -20–+20); ignored for notch
  }
}
```

**Default band assignments:**

| Band | Type | Frequency |
|------|------|-----------|
| 1 | peak | 100 Hz |
| 2 | peak | 500 Hz |
| 3 | peak | 2000 Hz |
| 4 | high_shelf | 8000 Hz |

### `dsp:gate`
Input channel noise gate.

```typescript
interface DspGatePayload {
  id: number          // input channel id
  params: {
    enabled: boolean
    threshold: number   // open threshold, dB (range: -80–0)
    attackMs: number    // attack time (range: 0.1–500)
    releaseMs: number   // release time (range: 1–5000)
    holdMs: number      // hold time before release (range: 0–2000)
    rangeDb: number     // attenuation when closed, dB (range: -80–0)
  }
}
```

### `dsp:comp`
Compressor (input and output channels).

```typescript
interface DspCompPayload {
  type: 'input' | 'output'
  id: number
  params: {
    enabled: boolean
    threshold: number   // dB (range: -60–0)
    ratio: number       // compression ratio (range: 1.0–∞, e.g. 4 = 4:1)
    knee: number        // soft-knee width, dB (range: 0–24)
    attackMs: number    // attack time (range: 0.1–500)
    releaseMs: number   // release time (range: 1–5000)
    makeupDb: number    // makeup gain, dB (range: 0–40)
  }
}
```

### `dsp:lim`
Output channel limiter (instantaneous peak limiting, zero latency).

```typescript
interface DspLimPayload {
  id: number          // output channel id
  params: {
    enabled: boolean
    threshold: number   // ceiling, dBFS (range: -20–0, typical: -0.5)
    releaseMs: number   // release time (range: 1–5000)
  }
}
```

### `dsp:gr:enable` / `dsp:gr:disable`
Enable or disable GR (Gain Reduction) metering broadcast. When enabled, the server emits `gr` events at ~12 Hz alongside `levels`.

```typescript
// no payload required
socket.emit('dsp:gr:enable', (res) => { ... })
socket.emit('dsp:gr:disable', (res) => { ... })
```

---

## Socket.IO Events — Server → Client

### `levels`
Audio level meters, emitted every 100 ms (~10 fps).

```typescript
interface LevelsEvent {
  inputs:  Array<{ id: number; level: number }>   // dBFS, -120 = silence
  outputs: Array<{ id: number; level: number }>
}
```

### `gr`
Gain Reduction meters, emitted every 100 ms when GR reporting is enabled.

```typescript
interface GrEvent {
  inputs:  Array<{ ch: number; gate: number; comp: number }>
  outputs: Array<{ ch: number; gate: number; comp: number; lim: number }>
}
// all values in dB: 0.0 = no reduction, -6.0 = 6 dB GR applied
```

### `status`
Full system snapshot, emitted every 2000 ms and on connection.

### `channels`
Channel list with current state, emitted on connection and on RTP stream state changes.

### `dsp:changed`
Emitted to all clients whenever a DSP parameter is updated. Contains only the changed parameter.

```typescript
interface DspChangedEvent {
  type:   'input' | 'output'
  id:     number
  key:    'trim' | 'hpf' | 'eq' | 'gate' | 'comp' | 'lim'
  params: object   // same shape as the corresponding dsp:* command payload
}
```

---

## I2S Mono / Stereo Processing Mode

The on-board I2S codec (`config/audio.json` → `jack`, defaults to Analog 1/2) supports a runtime-selectable **mono** or **stereo** DSP processing mode. The mode is set independently for the input and output direction.

- **mono** (default): channels 1 and 2 are processed independently. Each channel keeps its own DSP parameters, gain, and mute state.
- **stereo**: channels 1 and 2 are linked. Any DSP parameter, gain, or mute change made to either channel is mirrored to the other so the pair processes identically.

**Linked parameters:** `trim`, `hpf`, `eq` (all 4 bands), `gate`, `comp`, `lim`, `gain`, `mute`.
**Not linked:** `label` (each channel keeps its own name), routing, `active`.

**Mode-change semantics:**
- **mono → stereo:** channel 2 is overwritten with channel 1's current values (channel 1 is the master). Engine commands are re-issued for channel 2 to apply the mirrored state immediately.
- **stereo → mono:** current values on both channels are preserved as-is. Subsequent edits diverge again.

**Availability:** stereo mode is only valid when `jack.channels === 2`. Other configurations are forced to `mono`.

### Socket.IO Events

```typescript
// Client → Server: query current mode
socket.emit('dsp:mode:get', (res) => { /* res = { ok: true, mode: { input, output } } */ })

// Client → Server: set mode. Either { direction, mode } or { input, output } shape.
interface DspModeSetPayload {
  direction?: 'input' | 'output'
  mode?:      'mono'  | 'stereo'
  input?:     'mono'  | 'stereo'  // shortcut, may be combined with output
  output?:    'mono'  | 'stereo'
}
socket.emit('dsp:mode:set', { input: 'stereo' }, (res) => { /* res = { ok, mode } */ })

// Server → Client: broadcast on mode change and on initial connection.
interface DspModeEvent {
  input:  'mono' | 'stereo'
  output: 'mono' | 'stereo'
}
socket.on('dsp:mode', (mode: DspModeEvent) => { ... })
```

### REST API

| Method | Path | Body | Response |
|--------|------|------|----------|
| GET    | `/api/dsp/mode` | — | `{ ok: true, mode: { input, output } }` |
| PUT    | `/api/dsp/mode` | `{ input?: 'mono'\|'stereo', output?: 'mono'\|'stereo' }` | `{ ok: true, mode }` on success, `{ ok: false, error }` on validation failure |

**Examples:**

```bash
# Read current mode
curl http://localhost:3000/api/dsp/mode
# → { "ok": true, "mode": { "input": "mono", "output": "mono" } }

# Enable stereo link on inputs only
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"input":"stereo"}' \
  http://localhost:3000/api/dsp/mode
# → { "ok": true, "mode": { "input": "stereo", "output": "mono" } }

# Enable stereo on both directions
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"input":"stereo","output":"stereo"}' \
  http://localhost:3000/api/dsp/mode
```

### Persistence

The mode is persisted in `config/channels.json` under the top-level `i2s` field:

```json
{
  "inputs": [ ... ],
  "outputs": [ ... ],
  "routing": [ ... ],
  "i2s": { "input": "mono", "output": "mono" }
}
```

On server restart the saved mode is restored before `restoreDspState()` runs, so channel 1's saved parameters (which equal channel 2's in stereo mode) are re-applied to both channels.

---

## DSP State Persistence

DSP parameters are persisted per-channel in `config/channels.json` under a `dsp` key. On engine restart, `restoreDspState()` replays all saved parameters automatically.

**Input channel schema:**
```json
{
  "dsp": {
    "trim": 0,
    "hpf": { "enabled": false, "slope": 12, "fc": 80.0 },
    "eq": [
      { "band": 1, "enabled": false, "type": "peak",       "fc": 100,  "q": 0.707, "gainDb": 0 },
      { "band": 2, "enabled": false, "type": "peak",       "fc": 500,  "q": 0.707, "gainDb": 0 },
      { "band": 3, "enabled": false, "type": "peak",       "fc": 2000, "q": 0.707, "gainDb": 0 },
      { "band": 4, "enabled": false, "type": "high_shelf", "fc": 8000, "q": 0.707, "gainDb": 0 }
    ],
    "gate": { "enabled": false, "threshold": -40, "attackMs": 5, "releaseMs": 100, "holdMs": 50, "rangeDb": -80 },
    "comp": { "enabled": false, "threshold": -20, "ratio": 4, "knee": 6, "attackMs": 10, "releaseMs": 100, "makeupDb": 0 }
  }
}
```

**Output channel schema:**
```json
{
  "dsp": {
    "eq": [
      { "band": 1, "enabled": false, "type": "peak",       "fc": 100,  "q": 0.707, "gainDb": 0 },
      { "band": 2, "enabled": false, "type": "peak",       "fc": 500,  "q": 0.707, "gainDb": 0 },
      { "band": 3, "enabled": false, "type": "peak",       "fc": 2000, "q": 0.707, "gainDb": 0 },
      { "band": 4, "enabled": false, "type": "high_shelf", "fc": 8000, "q": 0.707, "gainDb": 0 }
    ],
    "comp": { "enabled": false, "threshold": -20, "ratio": 4, "knee": 6, "attackMs": 10, "releaseMs": 100, "makeupDb": 0 },
    "lim":  { "enabled": false, "threshold": -0.5, "releaseMs": 200 }
  }
}
```

---

## Engine Socket Protocol (C engine direct commands)

These are the low-level text commands sent over the Unix socket to `aoip_engine`. Under normal operation these are generated by the Node.js layer — documented here for debugging.

```
# Gain / mute
gain in|out <ch> <linear>           # 0.0–2.0
mute in|out <ch> 0|1
bypass in|out <ch> 0|1

# Trim (input only)
trim in <ch> <db>                   # -20 to +20

# HPF (input only) — C engine computes Butterworth coefficients
hpf in <ch> set slope 12|24|48 fc <hz>
hpf in <ch> disable

# EQ band (biquad coefficients computed by JS layer)
eq in|out <ch> band 1|2|3|4 coef <b0> <b1> <b2> <a1> <a2>
eq in|out <ch> band 1|2|3|4 disable

# Gate (input only)
gate in <ch> set thr <db> attack <ms> release <ms> hold <ms> range <db>
gate in <ch> disable

# Compressor
comp in|out <ch> set thr <db> ratio <r> knee <db> attack <ms> release <ms> makeup <db>
comp in|out <ch> disable

# Limiter (output only)
lim out <ch> set thr <db> release <ms>
lim out <ch> disable

# GR metering enable/disable
gr enable
gr disable
```

**Engine → Node.js (stdout lines):**
```
lvl in <ch> <dbfs>
lvl out <ch> <dbfs>
gr in <ch> gate <db> comp <db>       # when gr enabled, ~12 Hz
gr out <ch> comp <db> lim <db>
rtp_buf <key> fillMs=<n> targetMs=<n> pct=<n> underrun=<n> ...
clk2 aoip_rate=<hz> ravenna_rate=<hz> drift_ppm=<n> elapsed=<s> ...
[aoip_engine] ready
```

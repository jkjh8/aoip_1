#!/usr/bin/env python3
"""
Per-channel JACK gain/mute processor (inputs + outputs).

Port layout:
  Input  channels: gainer:in_1..N_in  → gainer:out_1..N_in
  Output channels: gainer:sin_1..N_out → gainer:sout_1..N_out

Commands via stdin:
  gain in  <ch> <0.0-2.0>
  gain out <ch> <0.0-2.0>
  mute in  <ch> <0|1>
  mute out <ch> <0|1>

Usage: python3 gainer.py <n_in> <n_out>
"""
import sys, time, threading
import jack
import numpy as np

if len(sys.argv) < 3:
    print("Usage: gainer.py <n_in> <n_out>", file=sys.stderr)
    sys.exit(1)

n_in  = int(sys.argv[1])
n_out = int(sys.argv[2])

client = jack.Client('gainer', no_start_server=True)

in_ports   = [client.inports.register(f'in_{i+1}')   for i in range(n_in)]
out_ports  = [client.outports.register(f'out_{i+1}')  for i in range(n_in)]
sin_ports  = [client.inports.register(f'sin_{i+1}')  for i in range(n_out)]
sout_ports = [client.outports.register(f'sout_{i+1}') for i in range(n_out)]

# target gain / mute — written by cmd_loop (GIL keeps list assignment atomic)
in_tgt   = [1.0]   * n_in
out_tgt  = [1.0]   * n_out
in_mute  = [False] * n_in
out_mute = [False] * n_out

# current (smoothed) gain — only touched in process callback
in_cur  = [1.0] * n_in
out_cur = [1.0] * n_out

@client.set_process_callback
def process(frames):
    # ── input channels ──────────────────────────────
    for i, (ip, op) in enumerate(zip(in_ports, out_ports)):
        dst = op.get_array()
        if in_mute[i]:
            dst[:] = 0.0
            in_cur[i] = in_tgt[i]
        else:
            tgt = in_tgt[i]
            cur = in_cur[i]
            src = ip.get_array()
            if abs(cur - tgt) < 1e-6:
                np.multiply(src, tgt, out=dst)
            else:
                ramp = np.linspace(cur, tgt, frames, dtype=np.float32)
                np.multiply(src, ramp, out=dst)
                in_cur[i] = tgt

    # ── output channels ─────────────────────────────
    for i, (ip, op) in enumerate(zip(sin_ports, sout_ports)):
        dst = op.get_array()
        if out_mute[i]:
            dst[:] = 0.0
            out_cur[i] = out_tgt[i]
        else:
            tgt = out_tgt[i]
            cur = out_cur[i]
            src = ip.get_array()
            if abs(cur - tgt) < 1e-6:
                np.multiply(src, tgt, out=dst)
            else:
                ramp = np.linspace(cur, tgt, frames, dtype=np.float32)
                np.multiply(src, ramp, out=dst)
                out_cur[i] = tgt

def cmd_loop():
    for line in sys.stdin:
        parts = line.strip().split()
        if len(parts) != 4:
            continue
        cmd, direction, ch_s, val_s = parts
        try:
            idx = int(ch_s) - 1
            if direction == 'in':
                n = n_in; tgt_list = in_tgt; mute_list = in_mute
            elif direction == 'out':
                n = n_out; tgt_list = out_tgt; mute_list = out_mute
            else:
                continue
            if not (0 <= idx < n):
                continue
            if cmd == 'gain':
                tgt_list[idx] = max(0.0, min(2.0, float(val_s)))
            elif cmd == 'mute':
                mute_list[idx] = (val_s == '1')
        except (ValueError, IndexError):
            pass

threading.Thread(target=cmd_loop, daemon=True).start()

with client:
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass

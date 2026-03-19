#!/usr/bin/env python3
"""
Multi-port JACK level meter — outputs portname:dBFS to stdout at ~12 Hz.
Usage: python3 meter.py <port1> [port2] ...
"""
import sys, time, math, threading
import jack
import numpy as np

if len(sys.argv) < 2:
    print("Usage: meter.py <port1> [port2] ...", file=sys.stderr)
    sys.exit(1)

src_ports = sys.argv[1:]
n = len(src_ports)

client = jack.Client('mtr_multi', no_start_server=True)
inports = [client.inports.register(f'in_{i}') for i in range(n)]

peaks = [0.0] * n
lock = threading.Lock()

@client.set_process_callback
def process(frames):
    local = []
    for ip in inports:
        buf = ip.get_array()
        rms = math.sqrt(float(np.dot(buf, buf)) / len(buf)) if len(buf) else 0.0
        local.append(rms)
    with lock:
        for i, rms in enumerate(local):
            if rms > peaks[i]:
                peaks[i] = rms

def print_loop():
    while True:
        time.sleep(0.08)  # ~12 Hz
        with lock:
            vals = peaks[:]
            for i in range(n):
                peaks[i] = 0.0
        for i, rms in enumerate(vals):
            db = 20.0 * math.log10(max(rms, 1e-10))
            db = max(-100.0, min(0.0, db))
            sys.stdout.write(f'{src_ports[i]}:{db:.2f}\n')
        sys.stdout.flush()

t = threading.Thread(target=print_loop, daemon=True)
t.start()

with client:
    for i, sp in enumerate(src_ports):
        try:
            client.connect(sp, inports[i].name)
        except Exception as e:
            print(f'connect failed for {sp}: {e}', file=sys.stderr)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass

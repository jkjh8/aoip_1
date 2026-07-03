import os, time, ctypes, struct

def phc_clockid(fd):
    return ((~fd) << 3) | 3

librt = ctypes.CDLL('librt.so.1', use_errno=True)
class timespec(ctypes.Structure):
    _fields_ = [('tv_sec', ctypes.c_long), ('tv_nsec', ctypes.c_long)]

fd = os.open('/dev/ptp0', os.O_RDONLY)
cid = phc_clockid(fd)
out = open('/var/tmp/phc_drift.csv', 'a', buffering=1)
out.write("# epoch_real, phc_minus_real_ns\n")
while True:
    ts_p = timespec(); ts_r = timespec()
    # sandwich read to reduce jitter: real, phc, real
    librt.clock_gettime(0, ctypes.byref(ts_r))
    r1 = ts_r.tv_sec * 10**9 + ts_r.tv_nsec
    librt.clock_gettime(cid, ctypes.byref(ts_p))
    p = ts_p.tv_sec * 10**9 + ts_p.tv_nsec
    librt.clock_gettime(0, ctypes.byref(ts_r))
    r2 = ts_r.tv_sec * 10**9 + ts_r.tv_nsec
    r = (r1 + r2) // 2
    out.write(f"{r/1e9:.6f},{p - r}\n")
    time.sleep(30)

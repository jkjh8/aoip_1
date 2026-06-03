#!/usr/bin/env python3
import ctypes
import ctypes.util

ADJ_TICK = 0x4000
TICK_NOMINAL = 10000

class _Timex(ctypes.Structure):
    _fields_ = [
        ('modes',     ctypes.c_uint),
        ('offset',    ctypes.c_long),
        ('freq',      ctypes.c_long),
        ('maxerror',  ctypes.c_long),
        ('esterror',  ctypes.c_long),
        ('status',    ctypes.c_int),
        ('constant',  ctypes.c_long),
        ('precision', ctypes.c_long),
        ('tolerance', ctypes.c_long),
        ('tv_sec',    ctypes.c_long),
        ('tv_usec',   ctypes.c_long),
        ('tick',      ctypes.c_long),
        ('_pad',      ctypes.c_long * 20),
    ]

libc = ctypes.CDLL(ctypes.util.find_library('c'))

t = _Timex()
t.modes = ADJ_TICK
t.tick = TICK_NOMINAL
libc.adjtimex(ctypes.byref(t))

t2 = _Timex()
libc.adjtimex(ctypes.byref(t2))
print(f'tick reset: {t2.tick}us')

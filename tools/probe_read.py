"""Decode sws_probe .bin logs (136-byte records). Read-only.

Record: u32 type, u32 tid, i64 qpc, u64 state, u32 seq, f32[27]
types: 1 TICK, 2 DRIFT_PRE, 3 DRIFT_POST, 4 DRIVE, 5 MARK, 6 INFO
TICK/DRIFT f: 0 cmdSpeed, 1 cmdScale, 2 abActive, 3 aboveCruise, 4 abEnergy, 5 f280, 6 boostTimer, 7 drifting,
              8 f28a, 9 f28b, 10-13 driftQuat, 14 f2a0, 15 power, 16 maxFactor, 17 dbForce, 18 dbTime, 19 topCruise,
              20-21 cfg ptr, 22 dt (TICK) / pressed (DRIFT)
DRIVE f: 0-3 quat, 4-6 target, 7-9 vel, 10-13 result, 14-17 p3, 18 dt, 19-21 gains(x,y,fwd), 22 boostTimer, 23 drifting
"""
import struct, sys, collections
import numpy as np

DT = np.dtype([('type', '<u4'), ('tid', '<u4'), ('qpc', '<i8'), ('state', '<u8'), ('seq', '<u4'), ('f', '<f4', 27)])
assert DT.itemsize == 136
NAMES = {1: 'TICK', 2: 'DRIFT_PRE', 3: 'DRIFT_POST', 4: 'DRIVE', 5: 'MARK', 6: 'INFO'}

def load(path):
    a = np.fromfile(path, dtype=DT)
    info = a[a['type'] == 6]
    freq = struct.unpack('<q', info[0]['f'][2:4].tobytes())[0] if len(info) else 10_000_000
    t0 = a['qpc'].min() if len(a) else 0
    return a, freq, t0

if __name__ == '__main__':
    a, freq, t0 = load(sys.argv[1])
    print(len(a), 'records; qpc freq', freq, '; span', round((a['qpc'].max() - t0) / freq, 3), 's')
    print({NAMES.get(int(k), k): int(v) for k, v in zip(*np.unique(a['type'], return_counts=True))})
    print('states seen:', {hex(int(s)): int(c) for s, c in zip(*np.unique(a['state'][a['type'] == 1], return_counts=True))})
    seqs = a['seq'].astype(np.int64)
    print('seq monotonic gaps (dropped/reordered):', int((np.diff(np.sort(seqs)) != 1).sum()))
    for t in (1, 2, 3, 4):
        r = a[a['type'] == t]
        if len(r): print(NAMES[t], 'first f[0:23]:', np.round(r[0]['f'][:24], 4).tolist())

# Direct call/jmp sites (E8/E9 rel32) to a target, in a code range of either build.
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from imgs import RESULTS
os.chdir(RESULTS)
from imgs import *
import verify as V


def sites(img, lo, hi, target):
    b = np.frombuffer(img.read(lo, hi - lo), dtype=np.uint8)
    rel = b[1:-3].astype(np.int64) | (b[2:-2].astype(np.int64) << 8) | (b[3:-1].astype(np.int64) << 16) | (b[4:].astype(np.int64) << 24)
    rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
    pos = np.arange(len(rel)) + lo
    hit = (pos + 5 + rel == target) & ((b[:-4] == 0xE8) | (b[:-4] == 0xE9))
    return [int(p) for p in pos[hit]]


if __name__ == '__main__':
    which, t = sys.argv[1], int(sys.argv[2], 16) - BASE
    img, hi = (V.S, 0x2bb1c00) if which == 'steam' else (V.D, 0x2c1f000)
    for p in sites(img, 0x1000, hi, t): print(which, hex(p + BASE), img.read(p, 1).hex())

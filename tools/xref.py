"""Find RIP-relative references (lea/mov/cmp etc. with disp32) to target VAs in the code sections."""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pemap import PE

def rip_refs(pe, targets, sections=('.text1', '.data2')):
    """Return {target: [va_of_disp_field,...]}; the instruction ends at disp_va+4 (+imm size, if any)."""
    out = {t: [] for t in targets}
    for s in pe.sections:
        if s[0] not in sections: continue
        name, va, vsz, rptr, rsz = s
        buf = np.frombuffer(pe.m, dtype=np.uint8, count=rsz, offset=rptr)
        n = rsz - 4
        d = (buf[0:n].astype(np.int64) | (buf[1:n+1].astype(np.int64) << 8) |
             (buf[2:n+2].astype(np.int64) << 16) | (buf[3:n+3].astype(np.int64) << 24))
        d = np.where(d >= 2**31, d - 2**32, d)
        pos_va = pe.base + va + np.arange(n, dtype=np.int64)
        for extra in (0, 1, 4):  # imm8 / imm32 after disp shift the RIP base
            tgt = pos_va + 4 + extra + d
            for t in targets:
                hits = np.nonzero(tgt == t)[0]
                out[t].extend(int(pe.base + va + h) for h in hits)
    return out

if __name__ == '__main__':
    pe = PE(sys.argv[1])
    ts = [int(x, 16) for x in sys.argv[2:]]
    for t, hs in rip_refs(pe, ts).items():
        print(hex(t), len(hs), [hex(h) for h in hs[:20]])

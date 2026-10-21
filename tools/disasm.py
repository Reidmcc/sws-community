"""Disassemble the function containing a VA (bounds from .pdata), read-only."""
import sys, os, struct, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pemap import PE
import capstone

class Funcs:
    def __init__(self, pe):
        self.pe = pe
        pd = [s for s in pe.sections if s[0] == '.pdata'][0]
        raw = pe.m[pd[3]:pd[3] + pd[4]]
        fs = []
        for i in range(len(raw) // 12):
            b, e, u = struct.unpack_from('<III', raw, 12 * i)
            if b: fs.append((pe.base + b, pe.base + e))
        fs.sort(); self.starts = [f[0] for f in fs]; self.fs = fs
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); self.cs.detail = False
    def containing(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        if i >= 0 and self.fs[i][0] <= va < self.fs[i][1]: return self.fs[i]
        return None
    def dis(self, start, end):
        code = self.pe.read(start, end - start)
        return list(self.cs.disasm(code, start))

if __name__ == '__main__':
    pe = PE(sys.argv[1]); F = Funcs(pe)
    for a in sys.argv[2:]:
        va = int(a, 16); f = F.containing(va)
        print(f'== {a} in func {hex(f[0])}-{hex(f[1])} ({f[1]-f[0]} bytes)' if f else f'== {a}: no pdata func')
        if f and f[1] - f[0] < 400:
            for ins in F.dis(*f): print(f'   {ins.address:#x}: {ins.mnemonic} {ins.op_str}')

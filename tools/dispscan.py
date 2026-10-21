"""Find functions whose instructions use ALL of the given memory displacements (e.g. config field offsets)."""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pemap import PE
from disasm import Funcs
import capstone
from capstone import x86

def candidates(pe, F, disps):
    """Byte prefilter: functions whose bytes contain every disp32 encoding."""
    sets = []
    for d in disps:
        pat = struct.pack('<i', d)
        hits = set()
        for s in pe.sections:
            if s[0] not in ('.text1', '.data2'): continue
            lo, hi = s[3], s[3] + s[4]
            i = pe.m.find(pat, lo, hi)
            while i >= 0:
                f = F.containing(pe.va(i))
                if f: hits.add(f)
                i = pe.m.find(pat, i + 1, hi)
        sets.append(hits)
    return set.intersection(*sets)

def confirm(pe, F, f, disps):
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); cs.detail = True
    found = collections.defaultdict(list)
    for ins in cs.disasm(pe.read(f[0], f[1] - f[0]), f[0]):
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.disp in disps:
                found[op.mem.disp].append((ins.address, f'{ins.mnemonic} {ins.op_str}'))
    return found if all(d in found for d in disps) else None

if __name__ == '__main__':
    pe = PE(sys.argv[1]); F = Funcs(pe)
    disps = [int(x, 16) for x in sys.argv[2:]]
    c = candidates(pe, F, disps)
    print('byte candidates:', len(c))
    for f in sorted(c):
        r = confirm(pe, F, f, disps)
        if r:
            print(f'== func {f[0]:#x}-{f[1]:#x} ({f[1]-f[0]} bytes)')
            for d in disps:
                for a, t in r[d][:4]: print(f'     {a:#x}: {t}')

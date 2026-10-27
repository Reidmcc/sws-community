# Instruction-level check of the size-aligned function pairs. Two functions match when every instruction has the
# same length and text, except RIP-relative memory operands and relative branch/call targets, whose targets are
# collected instead: code targets must agree with the function map, data targets are tallied as evidence for the
# data map (steam data rva -> ea data rva). Writes verified.pkl {steam_start: ea_start} and dataev.pkl.
import collections, pickle, re, sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgs import *
os.chdir(RESULTS)
import capstone

S = Steam(); D = Dump()
fmap = pickle.load(open('fmap.pkl', 'rb'))
fs = pickle.load(open('funcs_steam.pkl', 'rb'))
cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
RIP = re.compile(r'rip ([+-]) (0x[0-9a-f]+|\d+)')
HEX = re.compile(r'0x[0-9a-f]+')
BR = {'call', 'jmp'}


def targets(ins):
    """Normalized operand text and the address the instruction refers to (None if none)."""
    op = ins.op_str
    m = RIP.search(op)
    if m:
        disp = int(m.group(2), 0) * (1 if m.group(1) == '+' else -1)
        return RIP.sub('rip+D', op), ins.address + ins.size + disp
    if ins.mnemonic in BR or ins.mnemonic.startswith('j') or ins.mnemonic.startswith('loop'):
        h = HEX.fullmatch(op)
        if h: return 'T', int(op, 16)
    return op, None


def check_pair(s, e, size):
    a = list(cs.disasm(S.read(s, size), s)); b = list(cs.disasm(D.read(e, size), e))
    if len(a) != len(b) or sum(i.size for i in a) != size: return None
    pend = []
    for x, y in zip(a, b):
        if x.size != y.size or x.mnemonic != y.mnemonic: return None
        ox, tx = targets(x); oy, ty = targets(y)
        if ox != oy: return None
        if tx is None:
            if x.bytes != y.bytes: return None
            continue
        pend.append((tx, ty, ox == 'T'))
    return pend


def main(limit=None):
    starts = sorted(fmap)
    if limit: starts = starts[:limit]
    ok, bad = {}, []
    dataev = collections.defaultdict(collections.Counter)
    codeev = collections.Counter()
    for n_, s in enumerate(starts):
        e, size = fmap[s]
        a = list(cs.disasm(S.read(s, size), s)); b = list(cs.disasm(D.read(e, size), e))
        good = len(a) == len(b) and sum(i.size for i in a) == size
        pend = []
        if good:
            for x, y in zip(a, b):
                if x.size != y.size or x.mnemonic != y.mnemonic: good = False; break
                ox, tx = targets(x); oy, ty = targets(y)
                if ox != oy: good = False; break
                if tx is None:
                    if x.bytes != y.bytes: good = False; break
                    continue
                pend.append((tx, ty, ox == 'T'))
        if not good: bad.append(s); continue
        ok[s] = e
        for tx, ty, direct in pend:
            if direct:
                codeev[(tx, ty)] += 1
            else:
                dataev[tx][ty] += 1
        if n_ % 20000 == 0: print(n_, len(ok), len(bad), flush=True)
    print('verified', len(ok), 'rejected', len(bad))
    # code targets that land on function starts must agree with the map
    agree = disagree = 0
    for (tx, ty), c in codeev.items():
        if tx in fmap:
            if fmap[tx][0] == ty: agree += c
            else: disagree += c
    print('branch targets on mapped starts: agree', agree, 'disagree', disagree)
    pickle.dump(ok, open('verified.pkl', 'wb'))
    pickle.dump(dict(dataev), open('dataev.pkl', 'wb'))
    pickle.dump(bad, open('rejected.pkl', 'wb'))
    pickle.dump(dict(codeev), open('codeev.pkl', 'wb'))


if __name__ == '__main__':
    main(int(sys.argv[1]) if len(sys.argv) > 1 else None)

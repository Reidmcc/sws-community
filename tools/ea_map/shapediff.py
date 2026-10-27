# Diff two functions by shape: targets reduced to their kind, so only real code differences show.
import difflib, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from imgs import RESULTS
os.chdir(RESULTS)
from imgs import BASE
import verify as V
import verify2 as W


def lines(img, start, size):
    out = []
    for ins in V.cs.disasm(img.read(start, size), start + BASE):
        op, t, k = W.norm(ins)
        if k == 'T': op = 'T'
        out.append((ins.address, '%s %s' % (ins.mnemonic, op)))
    return out


s, ss, e, es = (int(x, 16) if i % 2 == 0 else int(x) for i, x in enumerate(sys.argv[1:5]))
a = lines(V.S, s - BASE, ss); b = lines(V.D, e - BASE, es)
sm = difflib.SequenceMatcher(None, [x[1] for x in a], [x[1] for x in b], autojunk=False)
print('steam %x %d ins, ea %x %d ins, ratio %.3f' % (s, len(a), e, len(b), sm.ratio()))
for tag, i1, i2, j1, j2 in sm.get_opcodes():
    if tag == 'equal': continue
    print('--- %s S[%d:%d] E[%d:%d]' % (tag, i1, i2, j1, j2))
    for x in a[i1:i2][:12]: print('  S %x  %s' % x)
    for y in b[j1:j2][:12]: print('  E %x  %s' % y)

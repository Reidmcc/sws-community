# For a pair of functions that differ in places: in the instruction runs that match by shape, do the code and data
# targets they imply agree with the rest of the map?
import difflib, os, pickle, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from imgs import RESULTS
os.chdir(RESULTS)
from imgs import BASE
import verify as V
import verify2 as W
ok = pickle.load(open('verified_all.pkl', 'rb')); dataev = pickle.load(open('dataev.pkl', 'rb'))


def ins(img, s, n):
    out = []
    for i in V.cs.disasm(img.read(s - BASE, n), s):
        op, t, k = W.norm(i)
        out.append(('%s %s' % (i.mnemonic, 'T' if k == 'T' else op), None if t is None else (t - BASE if t >= BASE else t)))
    return out


s, ss, e, es = (int(x, 16) if i % 2 == 0 else int(x) for i, x in enumerate(sys.argv[1:5]))
a, b = ins(V.S, s, ss), ins(V.D, e, es)
sm = difflib.SequenceMatcher(None, [x[0] for x in a], [x[0] for x in b], autojunk=False)
agree = dis = unk = inner = 0; bad = []
for tag, i1, i2, j1, j2 in sm.get_opcodes():
    if tag != 'equal': continue
    for (_, tx), (_, ty) in zip(a[i1:i2], b[j1:j2]):
        if tx is None: continue
        if s - BASE <= tx < s - BASE + ss: inner += 1; continue
        ref = ok.get(tx)
        if ref is None and tx in dataev: ref = dataev[tx].most_common(1)[0][0]
        if ref is None: unk += 1
        elif ref == ty: agree += 1
        else: dis += 1; bad.append((hex(tx + BASE), hex(ty + BASE), hex(ref + BASE)))
print('%x vs %x: shape ratio %.3f; outside targets agree %d, disagree %d, unknown %d; inner branches %d' % (s, e, sm.ratio(), agree, dis, unk, inner), bad[:6])

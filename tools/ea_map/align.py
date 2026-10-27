# Align the two builds' function lists by size, patience-style: functions whose size is unique in both lists
# anchor the alignment (longest increasing run), then gaps between anchors are filled where both sides hold the
# same sequence of sizes. Writes fmap.pkl: steam_start -> (ea_start, size).
import bisect, collections, pickle, sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgs import RESULTS
os.chdir(RESULTS)

fs = pickle.load(open('funcs_steam.pkl', 'rb'))
fe = pickle.load(open('funcs_ea.pkl', 'rb'))
a = [e - b for b, e in fs]
b = [e - b_ for b_, e in fe]

ca = collections.Counter(a); cb = collections.Counter(b)
posb = {s: i for i, s in enumerate(b) if cb[s] == 1}
pairs = [(i, posb[s]) for i, s in enumerate(a) if ca[s] == 1 and s in posb]
# longest increasing subsequence on the ea index
tails, prev, idx = [], [-1] * len(pairs), []
for k, (i, j) in enumerate(pairs):
    p = bisect.bisect_left(tails, j)
    if p == len(tails): tails.append(j); idx.append(k)
    else: tails[p] = j; idx[p] = k
    prev[k] = idx[p - 1] if p else -1
k = idx[-1]; anchors = []
while k >= 0: anchors.append(pairs[k]); k = prev[k]
anchors.reverse()
print('unique-size pairs', len(pairs), 'anchors', len(anchors))

m = {}
for i, j in anchors: m[i] = j
filled = 0
ext = [(-1, -1)] + anchors + [(len(a), len(b))]
for (i0, j0), (i1, j1) in zip(ext, ext[1:]):
    # walk forward from the left anchor and back from the right while sizes agree
    x, y = i0 + 1, j0 + 1
    while x < i1 and y < j1 and a[x] == b[y]: m[x] = y; x += 1; y += 1; filled += 1
    x2, y2 = i1 - 1, j1 - 1
    while x2 >= x and y2 >= y and a[x2] == b[y2]: m[x2] = y2; x2 -= 1; y2 -= 1; filled += 1
print('filled', filled, 'mapped', len(m), 'of', len(a))
fmap = {fs[i][0]: (fe[j][0], a[i]) for i, j in m.items()}
pickle.dump(fmap, open('fmap.pkl', 'wb'))

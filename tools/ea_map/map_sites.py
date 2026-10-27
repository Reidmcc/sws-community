# Map every game address the mod uses from the Steam build to the EA build, and say how each was found.
import bisect, collections, pickle, re, sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgs import *
os.chdir(RESULTS)
import verify as V
import verify2 as W

fs = pickle.load(open('funcs_steam.pkl', 'rb')); fstarts = [f[0] for f in fs]
fe = pickle.load(open('funcs_ea.pkl', 'rb')); estarts = {f[0]: f[1] - f[0] for f in fe}
ok = pickle.load(open('verified.pkl', 'rb'))
codeev = pickle.load(open('codeev.pkl', 'rb'))
dataev = pickle.load(open('dataev.pkl', 'rb'))

sites = []
for line in open(r'E:\sws_community\mod\check_sites.h'):
    m = re.search(r'\{ (0x[0-9a-f]+)ull, 0x[0-9a-f]+u, 0x[0-9a-f]+ull, "([^"]*)" \}', line)
    if m: sites.append((int(m.group(1), 16) - BASE, m.group(2)))


def func_of(r):
    i = bisect.bisect_right(fstarts, r) - 1
    return fs[i] if i >= 0 and fs[i][0] <= r < fs[i][1] else None


# Extend the verified map with functions reached by direct calls/jumps from verified code, then verify them.
by_target = collections.defaultdict(collections.Counter)
for (tx, ty), c in codeev.items(): by_target[tx][ty] += c
added = 0
for _round in range(3):
    for tx, cands in list(by_target.items()):
        if tx in ok: continue
        f = func_of(tx)
        if not f or f[0] != tx: continue
        ty, c = cands.most_common(1)[0]
        if ty not in estarts or estarts[ty] != f[1] - f[0]: continue
        pend = V.check_pair(tx, ty, f[1] - f[0])
        if pend is None: continue
        ok[tx] = ty; added += 1
        for a, b, direct in pend:
            if direct: by_target[a][b] += 1
            else: dataev.setdefault(a, collections.Counter())[b] += 1
# Second pass: rejected size-aligned pairs, and same-size functions in the same slot between verified neighbours,
# checked with switch tables and image-relative operands allowed.
fmap = pickle.load(open('fmap.pkl', 'rb'))
fes = [f[0] for f in fe]
second = 0
for s, (e, size) in fmap.items():
    if s in ok: continue
    pend = W.check_pair2(s, e, size)
    if pend is None: continue
    ok[s] = e; second += 1
    for a, b, k in pend:
        if k == 'T': by_target[a][b] += 1
        else: dataev.setdefault(a, collections.Counter())[b] += 1
vs = sorted(ok); 
for i, (s, e_) in enumerate(fs):
    if s in ok: continue
    j = bisect.bisect_left(vs, s)
    if j == 0 or j >= len(vs): continue
    ls, rs = vs[j - 1], vs[j]
    li = bisect.bisect_left(fstarts, ls); ri = bisect.bisect_left(fstarts, rs)
    le = bisect.bisect_left(fes, ok[ls]); re_ = bisect.bisect_left(fes, ok[rs])
    if ri - li != re_ - le: continue
    k = le + (i - li)
    e, ee = fe[k]
    if ee - e != e_ - s: continue
    pend = W.check_pair2(s, e, e_ - s)
    if pend is None: continue
    ok[s] = e; second += 1
    for a, b, kk in pend:
        if kk != 'T': dataev.setdefault(a, collections.Counter())[b] += 1
print('functions verified:', len(ok), '(added through call targets:', added, ', second pass:', second, ')')
pickle.dump(ok, open('verified_all.pkl', 'wb'))

# The data map: references from verified code, as (steam rva, ea rva) pairs with their majority target.
pairs = sorted((tx, c.most_common(1)[0][0]) for tx, c in dataev.items())
ptx = [p[0] for p in pairs]
S = V.S; D = V.D
ssecs = sections(S)


def sec_of(r):
    for name, vs, va, ch in ssecs:
        if va <= r < va + vs: return name, va, vs
    return None


def data_delta(r):
    """Delta (ea - steam) for a data rva, from the nearest code references around it in the same section."""
    sec = sec_of(r)
    i = bisect.bisect_right(ptx, r) - 1
    left = pairs[i] if i >= 0 else None
    right = pairs[i + 1] if i + 1 < len(pairs) else None
    cand = []
    for p in (left, right):
        if p and sec_of(p[0]) == sec: cand.append((abs(p[0] - r), p[1] - p[0], p[0]))
    if not cand: return None, None
    cand.sort()
    exact = [c for c in cand if c[0] == 0]
    if exact: return exact[0][1], 'referenced'
    if len(cand) == 2 and cand[0][1] == cand[1][1]: return cand[0][1], 'between refs %d/%d bytes away' % (cand[0][0], cand[1][0])
    return cand[0][1], 'nearest ref %d bytes away (other side differs)' % cand[0][0]


results = []
for r, hook in sites:
    f = func_of(r)
    if f and f[0] in ok:
        results.append((r, hook, ok[f[0]] + (r - f[0]), 'code: function verified (+0x%x)' % (r - f[0])))
        continue
    if f:
        results.append((r, hook, None, 'code: function %x not matched (size %d)' % (f[0] + BASE, f[1] - f[0])))
        continue
    d, how = data_delta(r)
    results.append((r, hook, None if d is None else r + d, 'data %s: %s' % (sec_of(r)[0] if sec_of(r) else '?', how)))

pickle.dump(results, open('site_map.pkl', 'wb'))
c = collections.Counter(x[3].split(':')[0] + (':ok' if x[2] is not None else ':none') for x in results)
print(c)
for x in results:
    if x[2] is None or not x[3].startswith('code: function verified'):
        print(hex(x[0] + BASE), '->', hex(x[2] + BASE) if x[2] is not None else '-', x[1][:26], '|', x[3])

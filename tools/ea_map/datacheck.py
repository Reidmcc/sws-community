# Content check for data addresses: compare a window at the Steam address with candidates near the estimated EA
# address. Each 8-byte slot must agree: code pointers must map through verified functions, data pointers must land
# where the data map puts them (within the same delta region), anything else must be byte-identical.
import bisect, pickle, struct, sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgs import *
os.chdir(RESULTS)

S = Steam(); D = Dump()
fs = pickle.load(open('funcs_steam.pkl', 'rb')); fst = [f[0] for f in fs]
ok = pickle.load(open('verified_all.pkl', 'rb'))
dataev = pickle.load(open('dataev.pkl', 'rb'))
pairs = sorted((tx, c.most_common(1)[0][0]) for tx, c in dataev.items()); ptx = [p[0] for p in pairs]
IMG_S = (0x140000000, 0x140000000 + 0x1f200000)


def map_code(r):
    i = bisect.bisect_right(fst, r) - 1
    if i >= 0 and fs[i][0] <= r < fs[i][1] and fs[i][0] in ok: return ok[fs[i][0]] + r - fs[i][0]
    return None


def data_deltas(r):
    i = bisect.bisect_right(ptx, r) - 1
    out = set()
    for p in pairs[max(0, i - 1):i + 3]: out.add(p[1] - p[0])
    return out


def slot_check(sq, eq):
    """'eq' identical, 'code' mapped code pointer, 'data' plausible data pointer, None mismatch."""
    if sq == eq: return 'eq'
    if IMG_S[0] <= sq < IMG_S[1]:
        r = sq - BASE
        mc = map_code(r)
        if mc is not None: return 'code' if mc == eq - BASE else None
        if any(r + d == eq - BASE for d in data_deltas(r)): return 'data'
        return 'ptr?' if IMG_S[0] <= eq < IMG_S[1] else None
    return None


def score(sr, er, n=0x40):
    sb = S.read(sr, n); eb = D.read(er, n)
    res = []
    for k in range(0, n, 8):
        sq = struct.unpack_from('<Q', sb, k)[0]; eq = struct.unpack_from('<Q', eb, k)[0]
        res.append(slot_check(sq, eq))
    return res


def best(sr, er_est, radius=0x400, n=0x40):
    cands = []
    for d in range(-radius, radius + 8, 8):
        r = score(sr, er_est + d, n)
        bad = r.count(None); weak = r.count('ptr?')
        strong = sum(1 for x in r if x in ('code', 'data'))
        cands.append((bad, weak, -strong, abs(d), er_est + d, r))
    cands.sort()
    return cands[0], cands[1]

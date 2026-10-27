# Check a Steam/EA function pair with the strict and second-pass checks, then test every target the pair implies
# against what the rest of the map already says (code targets: verified functions; data targets: the majority
# reference from verified code).
import os, pickle, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from imgs import RESULTS
os.chdir(RESULTS)
from imgs import BASE
import verify as V
import verify2 as W

ok = pickle.load(open('verified_all.pkl', 'rb'))
dataev = pickle.load(open('dataev.pkl', 'rb'))


def check(s, e, size):
    pend = V.check_pair(s, e, size)
    how = 'strict'
    if pend is None:
        pend = W.check_pair2(s, e, size); how = 'second pass'
    if pend is None: return None
    agree = disagree = unknown = 0; bad = []
    for item in pend:
        tx, ty = item[0], item[1]
        if s <= tx < s + size:  # inside the function itself
            good = ty - e == tx - s
        elif tx in ok: good = ok[tx] == ty
        elif tx in dataev: good = dataev[tx].most_common(1)[0][0] == ty
        else: unknown += 1; continue
        if good: agree += 1
        else: disagree += 1; bad.append((hex(tx + BASE), hex(ty + BASE)))
    return how, len(pend), agree, disagree, unknown, bad[:8]


if __name__ == '__main__':
    for arg in sys.argv[1:]:
        s, e, n = arg.split(':')
        print(arg, check(int(s, 16) - BASE, int(e, 16) - BASE, int(n)))

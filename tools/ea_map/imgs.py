# The two images the EA map compares: the Steam exe (work/starwarssquadrons.exe, a copy of the install) and the EA
# build's decrypted image (work/ea/ea_image.bin, captured by sws_dump.dll). Both stay out of the repo (work/ is ignored);
# SWS_WORK names the folder when it isn't the main checkout's. The map's intermediate results live in work/ea/results.
import mmap, os, pickle, struct, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from pemap import PE

WORK = os.environ.get('SWS_WORK', r'E:\sws_community\work')
RESULTS = os.path.join(WORK, 'ea', 'results')
BASE = 0x140000000


class Steam:
    def __init__(self):
        self.pe = PE(os.path.join(WORK, 'starwarssquadrons.exe'))

    def read(self, rva, n):
        if rva < 0x1000:
            return self.pe.m[rva:rva + n]
        b = self.pe.read(BASE + rva, n)
        return b if b is not None and len(b) == n else (b or b'') + b'\0' * (n - len(b or b''))


class Dump:
    def __init__(self, path=os.path.join(WORK, 'ea', 'ea_image.bin')):
        self.f = open(path, 'rb')
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)

    def read(self, rva, n):
        return self.m[rva:rva + n]


def ddir(img, idx):
    pe = struct.unpack_from('<I', img.read(0, 0x40), 0x3c)[0]
    return struct.unpack_from('<II', img.read(pe, 0x200), 24 + 112 + 8 * idx)


def sections(img):
    pe = struct.unpack_from('<I', img.read(0, 0x40), 0x3c)[0]
    h = img.read(pe, 0x1000)
    n = struct.unpack_from('<H', h, 6)[0]
    opt = struct.unpack_from('<H', h, 20)[0]
    out = []
    for i in range(n):
        o = 24 + opt + 40 * i
        out.append((h[o:o + 8].rstrip(b'\0').decode(), *struct.unpack_from('<IIIII', h, o + 8)[:2], struct.unpack_from('<I', h, o + 36)[0]))
    return out  # (name, vsize, va, characteristics)


def funcs(img):
    va, sz = ddir(img, 3)
    raw = img.read(va, sz)
    return sorted((b, e) for b, e, u in struct.iter_unpack('<III', raw[:len(raw) // 12 * 12]) if b)


def load(name):
    return pickle.load(open(os.path.join(RESULTS, name), 'rb'))

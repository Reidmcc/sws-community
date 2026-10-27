# Second pass for functions the strict check rejected: also accept image-relative displacements (switch tables
# addressed off the image base) and a tail of jump-table data after an unconditional jump, when every 4-byte entry is
# an image RVA on both sides (or the tail bytes are equal).
import re, struct, sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify as V

IMG = 0x1f200000
DISP = re.compile(r'([+-]) (0x[0-9a-f]+)\]')


def norm(ins):
    op, t = V.targets(ins)
    if t is not None: return op, t, 'T' if op == 'T' else 'R'
    m = DISP.search(op)
    if m and 'rip' not in op:
        v = int(m.group(2), 16)
        if 0x1000 <= v < IMG: return DISP.sub('+ RVA]', op), v, 'A'
    return op, None, None


def tail_ok(sb, eb):
    if sb == eb: return True
    if len(sb) % 4 or len(sb) != len(eb): return False
    for (x,), (y,) in zip(struct.iter_unpack('<I', sb), struct.iter_unpack('<I', eb)):
        if x == y: continue
        if not (0x1000 <= x < IMG and 0x1000 <= y < IMG): return False
    return True


def check_pair2(s, e, size, esize=None):
    esize = esize or size
    if esize != size: return None
    sb = V.S.read(s, size); eb = V.D.read(e, size)
    pend = []; off = 0; prev = None; tables = set()
    while off < size:
        a = V.cs.disasm(sb[off:], s + off); b = V.cs.disasm(eb[off:], e + off)
        restarted = False
        for x, y in zip(a, b):
            if x.address - s in tables: prev = 'jmp'; ok = False
            else: ok = x.size == y.size and x.mnemonic == y.mnemonic
            if ok:
                ox, tx, kx = norm(x); oy, ty, ky = norm(y)
                ok = ox == oy and (tx is not None or x.bytes == y.bytes)
            if not ok:
                if prev not in ('jmp', 'ret', 'int3'): return None
                # a data table after an unconditional jump: skip 4-byte entries that are RVAs on both sides
                n = 0
                while off + 4 <= size:
                    xv = struct.unpack_from('<I', sb, off)[0]; yv = struct.unpack_from('<I', eb, off)[0]
                    if xv == yv or (0x1000 <= xv < IMG and 0x1000 <= yv < IMG): off += 4; n += 1
                    else: break
                if n == 0:
                    # byte tables: equal bytes up to the next difference that decodes alike
                    while off < size and sb[off] == eb[off]: off += 1
                    if n == 0 and off < size and sb[off] != eb[off]: return None
                prev = None; restarted = True
                break
            if tx is not None:
                pend.append((tx, ty, kx))
                if kx == 'A' and s <= tx < s + size: tables.add(tx - s)
            off += x.size; prev = x.mnemonic
        if not restarted:
            if off >= size: return pend
            # the disassembler stopped (undecodable bytes): treat like a table
            if sb[off:] == eb[off:] or (prev in ('jmp', 'ret', 'int3') and tail_ok(sb[off:], eb[off:])): return pend
            return None
    return pend

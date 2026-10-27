"""Generate mod/build_ea.h: the EA app build's address for every game address the mod's source names, and the bytes the
mod expects there where they differ from the Steam build's.

The mod's source names game addresses as the Steam build's (the build it was analysed on); at run time G(a) gives the
running build's address (mod/game_build.h). The EA app's exe is encrypted on disk, so this works from its decrypted
image (work/ea/ea_image.bin, captured once by sws_dump.dll) and the function map between the builds that
tools/ea_map builds from it (work/ea/results; see tools/ea_map/README.md). Every address gets evidence, strongest first:

  range    a bound the mod compares pointers against in its logging, set from the EA build's section layout
  import   a slot of the game's own import table, found by the function address the wrapper wrote there
  code     inside a function the map verified instruction by instruction (same offset), or one checked by hand
  vtentry  a function with no unwind entry, read from the matching slot of an already mapped vtable
  type     a ClassInfo, confirmed by its type name (ClassInfo +0 -> type data +0 -> the name)
  vtable   a vtable, confirmed by its entries mapping through verified functions
  ref      data that verified code references, every reference agreeing

Anything without evidence stops the generator. Expected bytes: for each hook site and byte check in the source, the EA
bytes at the mapped address; where they differ, they are written out, after checking that a hook's stolen bytes are still
whole instructions with no RIP-relative operand or relative branch (the trampoline copies them as they are).

Usage: .venv/Scripts/python.exe tools/gen_build_ea.py
"""
import bisect
import collections
import ctypes
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, "ea_map"))
from imgs import BASE, RESULTS, Dump, Steam, load, sections  # noqa: E402
import capstone  # noqa: E402

SOURCES = [os.path.join(PROJECT, "mod", "sws_probe.cpp"), os.path.join(PROJECT, "mod", "config_types.h")]
OUT = os.path.join(PROJECT, "mod", "build_ea.h")
ADDR = re.compile(r"\b0x(14[0-9a-fA-F]{7})(?:ull|ULL)?\b")
cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

S, D = Steam(), Dump()
fs = load("funcs_steam.pkl"); fst = [f[0] for f in fs]
ok = load("verified_all.pkl")
dataev = load("dataev.pkl")

# Bounds the mod compares pointers against (diagnostic logging only), from each build's section layout.
RANGE = {
    0x140000000: (0x140000000, "the exe's base, the same in both"),
    0x140001000: (0x140001000, "start of code, the same in both"),
    0x14307e400: (0x1430ebe00, "end of code: Steam .text1+.data2 end, EA .xdata+.sdata end"),
    0x143000000: (0x143000000, "looks_vtable lower bound, as loose in EA as in Steam (EA code ends 0x1430ebe00)"),
    0x146000000: (0x146000000, "looks_vtable upper bound; EA vtables end below it (.srdata ends 0x144667000)"),
    0x1459b0000: (0x1459b0000, "pointer-into-image upper bound; still above EA code and vtables"),
}

# Functions the alignment missed or that EA rebuilt, checked by hand (2026-10-27, tools/ea_map/*.py and the notes in
# work/ea/results/ea_site_map.tsv). Only the entry may be used for the last two: their bodies differ.
HAND = {
    0x1418f2930: (0x1418f5f90, "same code (strict check, 16/16 targets agree); missed by the size alignment"),
    0x1412e5e50: (0x1412e8100, "same code (strict check, 38 targets agree, none disagree); missed by the size alignment"),
    0x14169cf00: (0x14169f090, "entry only: EA drops one member's SteamAPI_RegisterCallback, the rest alike"),
    0x1412df8a0: (0x1412e1ce0, "entry only: rebuilt; identity from its only caller (a verified pair, both at +0xa7)"),
}

# Slots of the game's own import table (the protection wrappers fill it at start, so the PE import directory doesn't
# list it): (dll, name or ordinal). The Steam slot's identity is read from the exe (an ordinal or a hint/name entry);
# the EA slot is the one holding that function's address in the dump. System DLLs sit at the same address in every
# process until a reboot, so this is looked up live only while the machine hasn't rebooted since the dump was taken,
# and kept in work/ea/results/imports_ea.json for later runs.
IMPORTS = {
    0x14307fef0: ("ws2_32.dll", 2, "bind"),
    0x14307fee0: ("ws2_32.dll", 23, "socket"),
    0x14307ff10: ("ws2_32.dll", 20, "sendto"),
    0x14307ff70: ("ws2_32.dll", "WSASocketW", "WSASocketW"),
    0x14307f388: ("kernel32.dll", "EnterCriticalSection", "EnterCriticalSection"),
    0x14307f2e8: ("kernel32.dll", "LeaveCriticalSection", "LeaveCriticalSection"),
}

# Byte checks in the source other than the hook table: (Steam address, length, what).
CHECKS = [
    (0x14025c3f0, 12, "string assign prologue"),
    (0x141191750, 12, "set option prologue"),
    (0x140ba0c20, 6, "post-load-level prologue"),
    (0x140b9dd89, 7, "3b: server-init job load (patched to a jmp)"),
    (0x1418f2930, 6, "backend chooser prologue (patched to return Local)"),
    (0x140696cb0, 4, "net tick prologue"),
]
# Relative jumps the mod writes: (at, length, target) must keep their distance.
REL_PATCHES = [(0x140b9dd89, 5, 0x140b9dd89 + 5 + 0x13d)]


def code_only(text):
    """The source without comments and string literals (addresses there are notes, not uses)."""
    out = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    out = re.sub(r"/\*.*?\*/", " ", out, flags=re.S)
    return re.sub(r"//[^\n]*", "", out)


def literals():
    found = set()
    for f in SOURCES:
        found |= {int(m.group(1), 16) for m in ADDR.finditer(code_only(open(f, encoding="utf-8").read()))}
    return found


def q(img, va):
    return struct.unpack("<Q", img.read(va - BASE, 8))[0]


def func_of(va):
    r = va - BASE
    i = bisect.bisect_right(fst, r) - 1
    return fs[i] if i >= 0 and fs[i][0] <= r < fs[i][1] else None


def in_code(img_secs, va):
    return any(ch & 0x20000000 and va_ <= va - BASE < va_ + vs for _, vs, va_, ch in img_secs)


def type_name(img, va):
    p = q(img, va)
    if not BASE <= p < BASE + 0x20000000: return None
    p = q(img, p)
    if not BASE <= p < BASE + 0x20000000: return None
    b = img.read(p - BASE, 96).split(b"\0")[0]
    return b.decode() if len(b) >= 3 and all(32 <= c < 127 for c in b) else None


def ref(va):
    c = dataev.get(va - BASE)
    if not c: return None
    (best, n), = c.most_common(1)
    return (best + BASE, n) if n == sum(c.values()) else None


def map_code(va):
    f = func_of(va)
    if f and f[0] in ok: return ok[f[0]] + (va - BASE - f[0]) + BASE
    return None


okk = sorted(ok)


RIPD = re.compile(r"rip ([+-]) (0x[0-9a-f]+)")


def leaf_body(img, va):
    """A leaf function up to its first ret or jmp: its instructions with RIP-relative operands as a placeholder, and
    their targets. None if it calls anything or runs past 64 bytes."""
    shape, targets = [], []
    for ins in cs.disasm(img.read(va - BASE, 64), va):
        if ins.mnemonic == "call" or (ins.mnemonic.startswith("j") and "rip" not in ins.op_str and "[" not in ins.op_str): return None
        m = RIPD.search(ins.op_str)
        if m:
            targets.append(ins.address + ins.size + int(m.group(2), 16) * (1 if m.group(1) == "+" else -1))
            shape.append((ins.mnemonic, RIPD.sub("rip+D", ins.op_str), ins.size))
        else:
            shape.append((ins.mnemonic, ins.op_str, bytes(ins.bytes)))
        if ins.mnemonic in ("ret", "jmp", "int3"): return shape, targets
    return None


def same_target(st, et):
    """A RIP-relative target in the Steam build and its counterpart in EA agree on the evidence we have."""
    if map_code(st) is not None: return map_code(st) == et
    n = type_name(S, st)
    if n: return type_name(D, et) == n
    r = ref(st)
    return r is not None and r[0] == et


def map_leaf(va):
    """A function with no unwind entry (a leaf): shifted like the verified functions on either side, if it has the same
    instructions there and any RIP-relative targets agree."""
    r = va - BASE
    if func_of(va): return None
    i = bisect.bisect_right(okk, r)
    body = leaf_body(S, va)
    if not body or i == 0 or i >= len(okk): return None
    for d in {ok[okk[i - 1]] - okk[i - 1], ok[okk[i]] - okk[i]}:
        eb = leaf_body(D, va + d)
        if eb and eb[0] == body[0] and all(same_target(s, e) for s, e in zip(body[1], eb[1])): return va + d
    return None


def shape(img, va, n=6):
    out = []
    for ins in cs.disasm(img.read(va - BASE, 96), va):
        op = RIPD.sub("rip+D", ins.op_str)
        if ins.mnemonic == "call" or ins.mnemonic.startswith("j"): op = "T" if "[" not in op else op
        out.append((ins.mnemonic, op))
        if len(out) == n: break
    return out


def same_shape(sva, eva, n=6):
    """The first n instructions of two functions alike, apart from relative targets."""
    a = shape(S, sva, n)
    return len(a) == n and a == shape(D, eva, n)


def map_any(va):
    return map_code(va) or map_leaf(va)


def imports_ea():
    cache = os.path.join(RESULTS, "imports_ea.json")
    if os.path.exists(cache):
        return {int(k, 16): v for k, v in json.load(open(cache)).items()}
    boot = ctypes.windll.kernel32.GetTickCount64() / 1000.0
    import time
    if time.time() - boot > os.path.getmtime(os.path.join(RESULTS, "..", "ea_image.bin")):
        sys.exit("the machine has rebooted since the dump: import slots can't be looked up live (and no cache)")
    k32 = ctypes.WinDLL("kernel32")
    k32.LoadLibraryW.restype = ctypes.c_void_p
    k32.GetProcAddress.restype = ctypes.c_void_p
    k32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    secs = {n: (va, vs) for n, vs, va, ch in sections(D)}
    lo, size = secs[".didata"]
    blob = D.read(lo, size)
    out = {}
    for steam, (dll, what, name) in IMPORTS.items():
        h = k32.LoadLibraryW(dll)
        fn = k32.GetProcAddress(h, ctypes.c_void_p(what) if isinstance(what, int) else ctypes.c_char_p(what.encode()))
        pat = struct.pack("<Q", fn); hits = []; p = 0
        while (p := blob.find(pat, p)) >= 0:
            if p % 8 == 0: hits.append(lo + p + BASE)
            p += 1
        if len(hits) != 1: sys.exit("import %s!%s: %d slots hold its address" % (dll, name, len(hits)))
        out[steam] = hits[0]
    json.dump({"%x" % k: v for k, v in out.items()}, open(cache, "w"), indent=1)
    return out


def steam_import(slot, what):
    v = q(S, slot)
    if isinstance(what, int): return v == (1 << 63) | what
    return S.read((v & 0x7FFFFFFF) + 2, len(what) + 1) == what.encode() + b"\0"


def resolve(lits):
    ssecs, esecs = sections(S), sections(D)
    iat = imports_ea()
    table = {}
    vt_lits = [a for a in lits if a in (0x1433fa218, 0x1433fa240)]
    for a in sorted(lits):
        if a in RANGE: table[a] = RANGE[a][0], "range: " + RANGE[a][1]; continue
        if a in HAND: table[a] = HAND[a][0], "code: " + HAND[a][1]; continue
        if a in IMPORTS:
            dll, what, name = IMPORTS[a]
            if not steam_import(a, what): sys.exit("%x is not %s!%s in the Steam exe" % (a, dll, name))
            table[a] = iat[a], "import: %s!%s, by its address in the dump" % (dll.split(".")[0], name); continue
        m = map_code(a)
        if m is not None:
            table[a] = m, "code: verified function +0x%x" % (a - BASE - func_of(a)[0]); continue
        if in_code(ssecs, a):
            # a function with no unwind entry: find it in the mod's vtables
            for vt in vt_lits:
                for k in range(32):
                    if q(S, vt + 8 * k) == a:
                        evt = resolve_vtable(vt); e = q(D, evt + 8 * k)
                        if not same_shape(a, e, 8): sys.exit("%x: vtable slot %d holds %x, a different shape" % (a, k, e))
                        table[a] = e, "vtentry: %x slot %d, same first 8 instructions" % (vt, k)
                        break
                if a in table: break
            if a in table: continue
            sys.exit("%x: code with no verified function and no vtable slot" % a)
        name = type_name(S, a)
        if name:
            e = ref(a)
            cands = ([e[0]] if e else []) + [x for x in data_guess(a) if not e or x != e[0]]
            hit = next((c for c in cands if type_name(D, c) == name), None)
            if hit is None: hit = search_type(a, name)
            if hit is None: sys.exit("%x: ClassInfo %s not found in EA" % (a, name))
            table[a] = hit, "type: %s" % name; continue
        vt = resolve_vtable(a, required=False)
        if vt is not None:
            table[a] = vt, resolve_vtable.how[a]; continue
        e = ref(a)
        if e: table[a] = e[0], "ref: referenced %d times from verified code, all agreeing" % e[1]; continue
        sys.exit("%x: no evidence" % a)
    return table


def data_guess(a):
    """Addresses the data map would place a at: the nearest references on either side, by their deltas."""
    r = a - BASE
    pairs = data_guess.pairs
    i = bisect.bisect_right(data_guess.keys, r) - 1
    out = []
    for p in pairs[max(0, i - 2):i + 3]:
        out.append(r + p[1] - p[0] + BASE)
    return sorted(set(out), key=lambda x: abs(x - a))


data_guess.pairs = sorted((tx, c.most_common(1)[0][0]) for tx, c in dataev.items())
data_guess.keys = [p[0] for p in data_guess.pairs]


def search_type(a, name, radius=0x40000):
    for g in data_guess(a)[:1]:
        for d in range(0, radius, 8):
            for c in (g + d, g - d):
                if type_name(D, c) == name: return c
    return None


def vt_prefix(svt, evt, n=32):
    """How many entries, from the first, map to the EA candidate's (vtables can be a few entries long, and EA orders
    them differently, so only the run from the start says anything)."""
    for k in range(n):
        m = map_any(q(S, svt + 8 * k))
        if m is None or m != q(D, evt + 8 * k): return k
    return n


def resolve_vtable(a, required=True):
    ssecs = sections(S)
    if sum(in_code(ssecs, q(S, a + 8 * k)) for k in range(8)) < 4:
        if required: sys.exit("%x: not a vtable" % a)
        return None
    r = ref(a)
    # Search: EA places its vtables in a different order, so nearby references say nothing. Every EA position whose
    # first entry is the mapped first entry is a candidate, scored by its run of matching entries; the best must be
    # unique, at least 2 long, and agree with the references from verified code if there are any.
    m = map_any(q(S, a))
    if m is None and r:
        # entries we can't map (no unwind entries, not leaves): take the references, if no entry that maps disagrees
        # and the first entries have the same shape
        n = sum(1 for k in range(20) if map_any(q(S, a + 8 * k)) is not None)
        if all(map_any(q(S, a + 8 * k)) in (None, q(D, r[0] + 8 * k)) for k in range(20)) and \
                all(same_shape(q(S, a + 8 * k), q(D, r[0] + 8 * k)) for k in range(4)):
            resolve_vtable.how[a] = "vtable: referenced %d times from verified code; %d entries that map agree, first 4 alike" % (r[1], n)
            return r[0]
        sys.exit("%x: referenced vtable %x disagrees with its entries" % (a, r[0]))
    if m is None:
        if required: sys.exit("%x: the vtable's first entry doesn't map" % a)
        return None
    pat = struct.pack("<Q", m); blob = resolve_vtable.blob; lo = resolve_vtable.lo
    scores = collections.Counter(); p = 0
    while (p := blob.find(pat, p)) >= 0:
        if p % 8 == 0:
            c = lo + p + BASE
            scores[c] = vt_prefix(a, c)
        p += 1
    top = scores.most_common(2)
    if top and top[0][1] >= 2 and (len(top) == 1 or top[0][1] > top[1][1]):
        if r and r[0] != top[0][0]: sys.exit("%x: vtable found at %x (run %d) but referenced as %x (run %d)" % (a, top[0][0], top[0][1], r[0], scores[r[0]]))
        resolve_vtable.how[a] = "vtable: the only one whose first %d entries map%s" % (top[0][1], ", as referenced" if r else "")
        return top[0][0]
    if required or (top and top[0][1] >= 2): sys.exit("%x: vtable %s" % (a, "ambiguous: %s" % top[:2] if top else "not found"))
    return None


def _vt_region():
    secs = {n: (va, vs) for n, vs, va, ch in sections(D)}
    lo = secs[".didata"][0]; hi = secs[".srdata"][0] + secs[".srdata"][1]
    return lo, D.read(lo, hi - lo)


resolve_vtable.lo, resolve_vtable.blob = _vt_region()
resolve_vtable.how = {}


def hook_sites():
    """The hook table's entries: (address, stolen length, expected bytes) from the source."""
    text = open(SOURCES[0], encoding="utf-8").read()
    out = []
    for m in re.finditer(r'\{"([^"]*)", 0x([0-9a-f]+), (\d+),\s*\{([^}]*)\}', text):
        bs = bytes(int(x, 16) for x in m.group(4).replace(" ", "").split(",") if x)
        out.append((int(m.group(2), 16), int(m.group(3)), bs, m.group(1)))
    return out


def relocatable(code, at):
    """The instructions in code, whole, with no RIP-relative operand or relative branch."""
    n = 0
    for ins in cs.disasm(code, at):
        if "rip" in ins.op_str: return False
        if ins.mnemonic == "call" or ins.mnemonic.startswith("j") or ins.mnemonic.startswith("loop"): return False
        n += ins.size
    return n == len(code)


def expects(table):
    out = []
    for a, steal, src, name in hook_sites():
        sb = S.read(a - BASE, steal)
        if sb != src: sys.exit("%s: the source's expected bytes differ from the Steam exe" % name)
        eb = D.read(table[a][0] - BASE, steal)
        if not relocatable(eb, table[a][0]): sys.exit("%s: the EA bytes to steal are not whole, position-free instructions" % name)
        if eb != sb: out.append((a, eb, name))
    for a, n, what in CHECKS:
        sb, eb = S.read(a - BASE, n), D.read(table[a][0] - BASE, n)
        if eb != sb: out.append((a, eb, what))
    for at, n, target in REL_PATCHES:
        if table[target][0] - (table[at][0] + n) != target - (at + n): sys.exit("%x: the patched jump's distance changes in EA" % at)
    return out


def main():
    lits = literals()
    # targets of the relative patches must be mapped too
    lits |= {t for _, _, t in REL_PATCHES}
    table = resolve(lits)
    exp = expects(table)
    pe = struct.unpack_from("<I", D.read(0, 0x40), 0x3C)[0]
    h = D.read(pe, 0x100)
    stamp, size = struct.unpack_from("<I", h, 8)[0], struct.unpack_from("<I", h, 24 + 56)[0]
    kinds = collections.Counter(v[1].split(":")[0] for v in table.values())
    with open(OUT, "w", newline="\n") as f:
        f.write("// Generated by tools/gen_build_ea.py from the EA app build's decrypted image. Do not edit by hand.\n")
        f.write("// The EA app build's address for every game address the mod names (by its Steam address), with the evidence;\n")
        f.write("// and the bytes the mod expects at an address where the EA build's differ.\n")
        f.write("// %s\n" % ", ".join("%s %d" % kv for kv in sorted(kinds.items())))
        f.write("#pragma once\n")
        f.write("static const uint32_t kEaTimeDateStamp = 0x%08x, kEaSizeOfImage = 0x%08x;\n" % (stamp, size))
        f.write("static const BuildAddr kEaAddrs[] = {\n")
        for a in sorted(table):
            e, how = table[a]
            f.write("    { 0x%xull, 0x%xull },   // %s\n" % (a, e, how))
        f.write("};\n")
        f.write("static const BuildExpect kEaExpect[] = {\n")
        for a, eb, what in exp:
            f.write("    { 0x%xull, %d, {%s} },   // %s\n" % (a, len(eb), ",".join("0x%02x" % x for x in eb), what))
        f.write("};\n")
    print("%d addresses (%s); %d expected-byte overrides -> %s" % (len(table), dict(kinds), len(exp), OUT))
    for a, eb, what in exp: print("  differs: %x %s" % (a, what))


if __name__ == "__main__":
    main()

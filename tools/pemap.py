"""Minimal read-only PE image mapper: file bytes, VA<->file-offset, section lookup."""
import struct, mmap

class PE:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        h = self.m
        pe = struct.unpack_from('<I', h, 0x3C)[0]
        nsec = struct.unpack_from('<H', h, pe + 6)[0]
        optsz = struct.unpack_from('<H', h, pe + 20)[0]
        self.base = struct.unpack_from('<Q', h, pe + 24 + 24)[0]
        self.sections = []
        for i in range(nsec):
            o = pe + 24 + optsz + 40 * i
            name = h[o:o + 8].rstrip(b'\0').decode()
            vsz, va, rsz, rptr = struct.unpack_from('<IIII', h, o + 8)
            self.sections.append((name, va, vsz, rptr, rsz))

    def sec_of_va(self, va):
        rva = va - self.base
        for s in self.sections:
            if s[1] <= rva < s[1] + max(s[2], s[4]):
                return s
        return None

    def off(self, va):
        s = self.sec_of_va(va)
        if not s: return None
        rva = va - self.base
        if rva - s[1] >= s[4]: return None  # in virtual-only tail (bss)
        return s[3] + (rva - s[1])

    def va(self, off):
        for s in self.sections:
            if s[3] <= off < s[3] + s[4]:
                return self.base + s[1] + (off - s[3])
        return None

    def read(self, va, n):
        o = self.off(va)
        return None if o is None else self.m[o:o + n]

    def q(self, va):
        b = self.read(va, 8); return None if b is None else struct.unpack('<Q', b)[0]

    def cstr(self, va, maxlen=256):
        b = self.read(va, maxlen)
        if b is None: return None
        i = b.find(b'\0')
        return b[:i].decode('latin1') if i >= 0 else None

    def find_all(self, needle, start=0):
        i = self.m.find(needle, start)
        while i >= 0:
            yield i
            i = self.m.find(needle, i + 1)

"""Read-only PE header summary: image base, ASLR flag, relocation dir, sections, imports of interest."""
import struct, sys
p = sys.argv[1]
with open(p, 'rb') as f:
    h = f.read(4096)
    pe = struct.unpack_from('<I', h, 0x3C)[0]
    assert h[pe:pe+4] == b'PE\0\0'
    nsec = struct.unpack_from('<H', h, pe+6)[0]
    tds = struct.unpack_from('<I', h, pe+8)[0]
    optsz = struct.unpack_from('<H', h, pe+20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', h, opt)[0]
    base = struct.unpack_from('<Q', h, opt+24)[0]
    dllch = struct.unpack_from('<H', h, opt+70)[0]
    ndd = struct.unpack_from('<I', h, opt+108)[0]
    dd = [struct.unpack_from('<II', h, opt+112+8*i) for i in range(ndd)]
    import datetime
    print('timestamp', datetime.datetime.utcfromtimestamp(tds))
    print('magic', hex(magic), 'ImageBase', hex(base))
    print('DllCharacteristics', hex(dllch), 'DYNAMIC_BASE(ASLR)=', bool(dllch & 0x40), 'HIGH_ENTROPY_VA=', bool(dllch & 0x20))
    print('reloc dir', dd[5], 'TLS dir', dd[9])
    secs = pe + 24 + optsz
    for i in range(nsec):
        s = h[secs+40*i: secs+40*i+40]
        name = s[:8].rstrip(b'\0').decode(errors='replace')
        vsz, va, rsz, rptr, _, _, _, _, ch = struct.unpack_from('<IIIIIIHHI', s, 8)
        print(f'  {name:10s} va={va:#x} vsize={vsz:#x} rawsize={rsz:#x} ch={ch:#x}')

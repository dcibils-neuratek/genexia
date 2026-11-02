#!/usr/bin/env python3
"""
psp_inspect.py - what a decrypted PSP program is made of, before recompiling it.

  psp_inspect.py PROGRAM        an ELF or PRX; encrypted ~PSP files are refused

Prints its code and data sizes, the system functions it imports by module, and
how often each kind of CPU instruction appears, VFPU included.
"""
import struct
import sys

# primary opcodes that are the PSP's vector unit
VFPU = {0x18: 'arithmetic', 0x19: 'arithmetic', 0x1B: 'arithmetic', 0x34: 'prefix/unary', 0x37: 'matrix', 0x3C: 'matrix/prefix',
        0x32: 'load', 0x36: 'load', 0x3A: 'store', 0x3E: 'store', 0x3D: 'control'}


def segments(data):
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        sys.exit('Not a 32-bit little-endian ELF: ' + ('this one is still encrypted' if data[:4] == b'~PSP' else repr(data[:4])))
    kind = struct.unpack_from('<H', data, 16)[0]
    phoff, = struct.unpack_from('<I', data, 28)
    size, count = struct.unpack_from('<HH', data, 42)
    out = []
    for i in range(count):
        t, off, va, pa, fsz, msz, flags, _ = struct.unpack_from('<8I', data, phoff + i * size)
        if t == 1:
            out.append((off, va, pa, fsz, msz, flags))
    return kind, out


def main():
    data = open(sys.argv[1], 'rb').read()
    kind, segs = segments(data)

    def at(va):
        for off, v, _, fsz, _, _ in segs:
            if v <= va < v + fsz:
                return off + va - v
        return None

    def text(va):
        o = at(va)
        return data[o:data.index(b'\0', o)].decode('latin1') if o is not None else '?'
    code = [s for s in segs if s[5] & 1]
    print('kind %s, code %.2f MB, loaded %.2f MB' % ('PRX' if kind == 0xFFA0 else 'ELF', sum(s[3] for s in code) / 1048576, sum(s[4] for s in segs) / 1048576))

    # import tables: found by their shape, since a game's module info can sit anywhere
    imports, seen = [], set()
    for off, v, _, fsz, _, _ in segs:
        for o in range(off, off + fsz - 20, 4):
            nm, _, esize, _, funcs, nids, stubs = struct.unpack_from('<IIBBHII', data, o)
            if esize != 5 or not 0 < funcs < 1000 or o in seen or None in (at(nm), at(nids), at(stubs)):
                continue
            name = text(nm)
            if len(name) > 3 and name.isascii() and name.replace('_', '').isalnum():
                imports.append((name, funcs))
                seen.add(o)
    for name, funcs in sorted(imports, key=lambda x: -x[1]):
        print('  %-26s %3d functions' % (name, funcs))
    print('system modules %d, functions imported %d' % (len(imports), sum(f for _, f in imports)))

    counts, vfpu, words = {}, {}, 0
    for off, _, _, fsz, _, _ in code:
        for (w,) in struct.iter_unpack('<I', data[off:off + (fsz & ~3)]):
            op = w >> 26
            words += 1
            if op in VFPU:
                vfpu[VFPU[op]] = vfpu.get(VFPU[op], 0) + 1
            elif op == 0x11:
                counts['fpu'] = counts.get('fpu', 0) + 1
            elif op == 0x1F:
                counts['allegrex bit ops'] = counts.get('allegrex bit ops', 0) + 1
    n = sum(vfpu.values())
    print('code words %d, VFPU %d (%.1f%%)' % (words, n, 100 * n / max(1, words)), 'fpu', counts.get('fpu', 0), 'bit ops', counts.get('allegrex bit ops', 0))
    for k, v in sorted(vfpu.items(), key=lambda x: -x[1]):
        print('  VFPU %-16s %d' % (k, v))


main()

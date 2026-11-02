#!/usr/bin/env python3
"""Places a relocatable PSP program at the address the console would load it.

    psp_relocate.py BOOT.BIN OUT

Some early games are PRX modules based at 0. This does what the PSP's loader
does and writes the fixed-address ELF that recompile_psp.py and the runtime
take. Only for programs already unencrypted on the disc. OUT holds the game's
code: it stays out of this repository.
"""
import struct
import sys

BASE = 0x08804000
ET_PRX = 0xFFA0
SHT_PRXREL = 0x700000A0
R_32, R_26, R_HI16, R_LO16 = 2, 4, 5, 6


def word(image, at):
    return struct.unpack_from('<I', image, at)[0]


def relocate(data):
    e_type = struct.unpack_from('<H', data, 16)[0]
    if data[:4] != b'\x7fELF' or e_type != ET_PRX:
        sys.exit('Not an unencrypted PRX.')
    entry, phoff, shoff = struct.unpack_from('<III', data, 24)
    phnum = struct.unpack_from('<H', data, 44)[0]
    shnum = struct.unpack_from('<H', data, 48)[0]
    segments = [struct.unpack_from('<8I', data, phoff + i * 32) for i in range(phnum)]
    size = max(s[2] + s[5] for s in segments)
    image = bytearray(max(s[2] + s[4] for s in segments))
    for s in segments:
        image[s[2]:s[2] + s[4]] = data[s[1]:s[1] + s[4]]

    relocs = []
    for i in range(shnum):
        s = struct.unpack_from('<10I', data, shoff + i * 40)
        if s[1] == SHT_PRXREL:
            relocs += struct.iter_unpack('<II', data[s[4]:s[4] + s[5]])

    done, unknown = 0, {}
    for n, (offset, info) in enumerate(relocs):
        kind, from_seg, to_seg = info & 0xFF, info >> 8 & 0xFF, info >> 16 & 0xFF
        at = segments[from_seg][2] + offset
        to = BASE + segments[to_seg][2]
        op = word(image, at)
        if kind == R_32:
            op = (op + to) & 0xFFFFFFFF
        elif kind == R_26:
            op = (op & 0xFC000000) | ((((op & 0x03FFFFFF) << 2) + to) >> 2 & 0x03FFFFFF)
        elif kind == R_HI16:
            # the carry depends on the low half, in the LO16 that follows
            lo = 0
            for later, later_info in relocs[n + 1:n + 64]:
                if later_info & 0xFF == R_LO16:
                    lo = word(image, segments[later_info >> 8 & 0xFF][2] + later) & 0xFFFF
                    lo -= 0x10000 if lo & 0x8000 else 0
                    break
            full = ((op & 0xFFFF) << 16) + lo + to
            op = (op & 0xFFFF0000) | ((full + 0x8000) >> 16 & 0xFFFF)
        elif kind == R_LO16:
            op = (op & 0xFFFF0000) | ((op + to) & 0xFFFF)
        else:
            unknown[kind] = unknown.get(kind, 0) + 1
            continue
        struct.pack_into('<I', image, at, op)
        done += 1
    print('%d relocations applied at %08X' % (done, BASE), 'not understood: %s' % unknown if unknown else '')
    return bytes(image), BASE + entry, size


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    image, entry, size = relocate(open(sys.argv[1], 'rb').read())
    header = b'\x7fELF\1\1\1' + bytes(9) + struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, entry, 52, 0, 0x10a23001, 52, 32, 1, 40, 0, 0)
    program = struct.pack('<8I', 1, 0x1000, BASE, BASE, len(image), size, 7, 0x1000)
    with open(sys.argv[2], 'wb') as f:
        f.write((header + program).ljust(0x1000, b'\0') + image)


if __name__ == '__main__':
    main()

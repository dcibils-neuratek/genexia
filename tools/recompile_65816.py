#!/usr/bin/env python3
"""
recompile_65816.py - static recompiler: 65C816 ROM -> C source.

  recompile_65816.py ROM OUTDIR --prefix S_ --register recomp_register_smk \
      [--entries FILE] [--listing FILE]

Same pipeline as the 68000 recompiler: entry points come from coverage logs,
code is found by following static control flow, and each basic block becomes a
C function looked up by the runtime dispatcher. The difference is the CPU's
mode flags: an immediate's size depends on M and X, so every block belongs to a
(pc, mx) pair, and REP/SEP are tracked along each path.
"""
import os
import sys

a = sys.argv[1:]
rom_path, outdir = a[0], a[1]
prefix, register_fn, entry_files, listing = 'S_', 'recomp_register', [], None
i = 2
while i < len(a):
    k, v = a[i], a[i + 1]; i += 2
    if k == '--prefix': prefix = v
    elif k == '--register': register_fn = v
    elif k == '--entries': entry_files.append(v)
    elif k == '--listing': listing = v
    else: sys.exit('bad arg ' + k)

rom = open(rom_path, 'rb').read()
if len(rom) % 1024 == 512:
    rom = rom[512:]
SZ = len(rom)


def score(base):
    if base + 32 > SZ:
        return -1
    h = rom[base:base + 32]
    s = 8 if ((h[0x1E] | h[0x1F] << 8) ^ (h[0x1C] | h[0x1D] << 8)) == 0xFFFF else 0
    s += sum(32 <= c < 127 for c in h[:21])
    return s


hirom = score(0xFFC0) > score(0x7FC0)


def off(pc):
    """file offset of a 24-bit address, or None if it isn't ROM"""
    b, o = pc >> 16, pc & 0xFFFF
    if b in (0x7E, 0x7F):
        return None
    if hirom:
        if (b & 0x7F) >= 0x40:
            return (pc & 0x3FFFFF) % SZ
        return (((b & 0x3F) << 16) | o) % SZ if o >= 0x8000 else None
    return ((b & 0x7F) << 15 | (o & 0x7FFF)) % SZ if o >= 0x8000 else None


def rb(pc):
    o = off(pc)
    return None if o is None else rom[o]


# instruction lengths; 'm' / 'x' depend on the mode flags
L = ("2222222213113334" "2222222213113334"
     "3242222213113334" "2222222213113334"
     "1223222213113334" "2223222213113434"  # 5C is 4
     "1323222213113334" "2222222213113334"
     "2323222213113334" "2222222213113334"
     "x2x2222213113334" "2222222213113334"
     "x222222213113334" "2222222213113334"
     "x222222213113334" "2222322213113334")
L = list(L)
for op in (0x09, 0x29, 0x49, 0x69, 0x89, 0xA9, 0xC9, 0xE9):
    L[op] = 'm'
L[0x20] = '3'; L[0x22] = '4'; L[0x5C] = '4'; L[0x4C] = '3'; L[0x82] = '3'; L[0x62] = '3'
L[0x44] = L[0x54] = '3'; L[0xF4] = '3'; L[0xD4] = '2'; L[0x6C] = L[0x7C] = L[0xDC] = L[0xFC] = '3'


def length(op, mx):
    c = L[op]
    if c == 'm':
        return 2 if mx & 2 else 3
    if c == 'x':
        return 2 if mx & 1 else 3
    return int(c)


ALU = {0x00: 'ora', 0x20: 'and', 0x40: 'eor', 0x60: 'adc', 0x80: 'sta', 0xA0: 'lda', 0xC0: 'cmp', 0xE0: 'sbc'}
MODES = {0x01: 'a_inx_v({b})', 0x03: 'a_sr_v({b})', 0x05: 'a_dp_v({b})', 0x07: 'a_inl_v({b})',
         0x0D: 'a_abs_v({w})', 0x0F: '{l}', 0x11: 'a_iny_v({b},{W})', 0x12: 'a_ind_v({b})',
         0x13: 'a_sry_v({b})', 0x15: 'a_dpx_v({b})', 0x17: 'a_ily_v({b})',
         0x19: 'a_idx_v({w},w65.y,{W})', 0x1D: 'a_idx_v({w},w65.x,{W})', 0x1F: 'a_alx_v({l})'}
SIMPLE = {0xA6: 'op_ldx(a_dp_v({b}))', 0xAE: 'op_ldx(a_abs_v({w}))', 0xA4: 'op_ldy(a_dp_v({b}))',
          0xAC: 'op_ldy(a_abs_v({w}))', 0x86: 'op_stx(a_dp_v({b}))', 0x8E: 'op_stx(a_abs_v({w}))',
          0x84: 'op_sty(a_dp_v({b}))', 0x8C: 'op_sty(a_abs_v({w}))', 0x64: 'op_stz(a_dp_v({b}))',
          0x9C: 'op_stz(a_abs_v({w}))', 0xBD: None}
SIMPLE.pop(0xBD)
BR = {0x10: '!(w65.p & NF)', 0x30: '(w65.p & NF)', 0x50: '!(w65.p & VF)', 0x70: '(w65.p & VF)',
      0x90: '!(w65.p & CF)', 0xB0: '(w65.p & CF)', 0xD0: '!(w65.p & ZF)', 0xF0: '(w65.p & ZF)'}
# run through the interpreter core, and the block must end (PC or flags unknown)
END = {0x00, 0x02, 0x28, 0x40, 0x44, 0x54, 0x60, 0x6B, 0x6C, 0x7C, 0xCB, 0xDB, 0xDC, 0xFB, 0xFC}
STOP_PATH = {0x00, 0x02, 0xDB}


class Ins:
    pass


def s8(v): return v - 256 if v & 0x80 else v
def s16(v): return v - 65536 if v & 0x8000 else v


def decode(pc, mx):
    op = rb(pc)
    if op is None:
        return None
    n = length(op, mx)
    ops = [rb(((pc & 0xFF0000) | ((pc + i) & 0xFFFF))) for i in range(1, n)]
    if any(v is None for v in ops):
        return None
    c = Ins(); c.pc, c.mx, c.op, c.n, c.targets, c.flow, c.mx_after = pc, mx, op, n, [], None, mx
    bank = pc & 0xFF0000
    nxt = bank | ((pc + n) & 0xFFFF)
    c.next = nxt
    b = '0x%02X' % ops[0] if n > 1 else ''
    w = '0x%04X' % (ops[0] | ops[1] << 8) if n > 2 else ''
    l = '0x%06X' % (ops[0] | ops[1] << 8 | ops[2] << 16) if n > 3 else ''
    F = 'F(%d); ' % n
    low, high = op & 0x1F, op & 0xE0
    if op in BR or op == 0x80:
        t = bank | ((pc + 2 + s8(ops[0])) & 0xFFFF)
        c.targets = [(t, mx)]
        if op == 0x80:
            c.c = F + 'IDLE(); w65.pc = 0x%04X;' % (t & 0xFFFF); c.flow = 'jump'
        else:
            c.c = F + 'if (%s) { IDLE(); w65.pc = 0x%04X; } else w65.pc = 0x%04X;' % (BR[op], t & 0xFFFF, nxt & 0xFFFF)
            c.flow = 'branch'
    elif op == 0x82:
        t = bank | ((pc + 3 + s16(ops[0] | ops[1] << 8)) & 0xFFFF)
        c.targets = [(t, mx)]; c.flow = 'jump'
        c.c = F + 'IDLE(); w65.pc = 0x%04X;' % (t & 0xFFFF)
    elif op == 0x4C:
        t = bank | (ops[0] | ops[1] << 8)
        c.targets = [(t, mx)]; c.flow = 'jump'; c.c = F + 'w65.pc = %s;' % w
    elif op == 0x5C:
        t = ops[0] | ops[1] << 8 | ops[2] << 16
        c.targets = [(t, mx)]; c.flow = 'jump'
        c.c = F + 'w65.pc = 0x%04X; w65.pb = 0x%02X;' % (t & 0xFFFF, t >> 16)
    elif op == 0x20:
        t = bank | (ops[0] | ops[1] << 8)
        c.targets = [(t, mx)]; c.flow = 'call'
        c.c = F + 'IDLE(); push16(0x%04X); w65.pc = %s;' % ((pc + 2) & 0xFFFF, w)
    elif op == 0x22:
        t = ops[0] | ops[1] << 8 | ops[2] << 16
        c.targets = [(t, mx)]; c.flow = 'call'
        c.c = F + 'push8(w65.pb); IDLE(); push16(0x%04X); w65.pc = 0x%04X; w65.pb = 0x%02X;' % ((pc + 3) & 0xFFFF, t & 0xFFFF, t >> 16)
    elif op in END:
        c.flow = 'end'
        c.c = 'w65.pc = 0x%04X; w65_exec();' % (pc & 0xFFFF)
    elif op in (0xC2, 0xE2):
        bits = (ops[0] >> 4) & 3          # P bit5=M, bit4=X -> mx bits 1,0
        c.mx_after = (mx & ~bits) if op == 0xC2 else (mx | bits)
        c.c = 'w65.pc = 0x%04X; w65_exec();' % (pc & 0xFFFF)
    elif high in ALU and low in MODES:
        name = ALU[high]
        ea = MODES[low].format(b=b, w=w, l=l, W=1 if name == 'sta' else 0)
        c.c = F + 'op_%s(%s);' % (name, ea)
    elif op in SIMPLE:
        c.c = F + SIMPLE[op].format(b=b, w=w) + ';'
    elif op in (0x09, 0x29, 0x49, 0xA9, 0xC9, 0x69, 0xE9):
        nm = {0x09: 'ora', 0x29: 'and', 0x49: 'eor', 0xA9: 'lda', 0xC9: 'cmp', 0x69: 'adc', 0xE9: 'sbc'}[op]
        o = {'ora': '|', 'and': '&', 'eor': '^'}.get(nm)
        if mx & 2:
            if nm == 'lda': c.c = F + '{ seta8(%s); nz8(%s); }' % (b, b)
            elif nm == 'cmp': c.c = F + 'cmp8((u8)w65.a, %s);' % b
            elif o: c.c = F + '{ u8 v = (u8)(w65.a %s %s); seta8(v); nz8(v); }' % (o, b)
            else: c.c = F + '%s8(%s);' % (nm, b)
        else:
            if nm == 'lda': c.c = F + '{ w65.a = %s; nz16(w65.a); }' % w
            elif nm == 'cmp': c.c = F + 'cmp16(w65.a, %s);' % w
            elif o: c.c = F + '{ w65.a %s= %s; nz16(w65.a); }' % (o, w)
            else: c.c = F + '%s16(%s);' % (nm, w)
    elif op == 0xA2:
        c.c = F + ('{ w65.x = %s; nz8((u8)w65.x); }' % b if mx & 1 else '{ w65.x = %s; nz16(w65.x); }' % w)
    elif op == 0xA0:
        c.c = F + ('{ w65.y = %s; nz8((u8)w65.y); }' % b if mx & 1 else '{ w65.y = %s; nz16(w65.y); }' % w)
    elif op == 0xE0:
        c.c = F + ('cmp8((u8)w65.x, %s);' % b if mx & 1 else 'cmp16(w65.x, %s);' % w)
    elif op == 0xC0:
        c.c = F + ('cmp8((u8)w65.y, %s);' % b if mx & 1 else 'cmp16(w65.y, %s);' % w)
    elif op == 0x18: c.c = 'F(1); IDLE(); w65.p &= ~CF;'
    elif op == 0x38: c.c = 'F(1); IDLE(); w65.p |= CF;'
    else:
        c.c = 'w65.pc = 0x%04X; w65_exec();' % (pc & 0xFFFF)   # shared interpreter core
    return c


# ---------------------------------------------------------- discovery
entries = set()
for ef in entry_files:
    if os.path.exists(ef):
        for line in open(ef):
            p = line.split()
            if len(p) == 2:
                entries.add((int(p[0], 16), int(p[1])))
ins, starts, work = {}, set(entries), list(entries)
while work:
    pc, mx = work.pop()
    while True:
        k = (pc, mx)
        if k in ins:
            break
        c = decode(pc, mx)
        if c is None or c.op in STOP_PATH and k not in entries:
            break
        ins[k] = c
        for t in c.targets:
            if off(t[0]) is not None:
                starts.add(t); work.append(t)
        if c.flow in ('jump', 'end'):
            break
        if c.flow in ('branch', 'call'):
            starts.add((c.next, c.mx_after))
        pc, mx = c.next, c.mx_after

# --------------------------------------------------------------- emit
blocks = []
for s in sorted(starts):
    if s not in ins:
        continue
    seq, k = [], s
    while k in ins:
        c = ins[k]; seq.append(c)
        k = (c.next, c.mx_after)
        if c.flow or k in starts:
            break
    blocks.append((s, seq, k))

os.makedirs(outdir, exist_ok=True)
for fn in os.listdir(outdir):
    if fn.startswith('recomp_'):
        os.remove(os.path.join(outdir, fn))
HEAD = '''/* GENERATED by tools/recompile_65816.py - do not edit */
#include "w65816_ops.h"
#include "machine.h"
#define FAST(n) (snes_master += (n) * snes_rom_speed)
#define SLOW(n) (snes_master += (n) * 8)
'''
CHUNK = 1500
nfiles = 0
for fi in range(0, len(blocks), CHUNK):
    with open(os.path.join(outdir, 'recomp_%03d.c' % nfiles), 'w') as f:
        f.write(HEAD)
        for (pc, mx), seq, end in blocks[fi:fi + CHUNK]:
            f.write('#undef F\n#define F(n) %s(n)\n' % ('FAST' if pc & 0x800000 else 'SLOW'))
            f.write('void %s%06X_%d(void) {\n' % (prefix, pc, mx))
            for c in seq:
                f.write('    %s  /* %06X op %02X */\n' % (c.c, c.pc, c.op))
            if not seq[-1].flow:
                f.write('    w65.pc = 0x%04X;\n' % (end[0] & 0xFFFF))
            f.write('}\n')
    nfiles += 1
with open(os.path.join(outdir, 'recomp_register.c'), 'w') as f:
    f.write('/* GENERATED by tools/recompile_65816.py - do not edit */\n#include "machine.h"\n')
    for (pc, mx), _, _ in blocks:
        f.write('void %s%06X_%d(void);\n' % (prefix, pc, mx))
    f.write('void %s(void) {\n' % register_fn)
    for (pc, mx), _, _ in blocks:
        f.write('    snes_recomp_add(0x%06X, %d, %s%06X_%d);\n' % (pc, mx, prefix, pc, mx))
    f.write('}\n')
if listing:
    with open(listing, 'w') as f:
        for k in sorted(ins):
            c = ins[k]
            f.write('%06X mx%d %s\n' % (c.pc, c.mx, ' '.join('%02X' % rb((c.pc & 0xFF0000) | ((c.pc + i) & 0xFFFF)) for i in range(c.n))))
print('%s, instructions %d (%d bytes), blocks %d, files %d' % ('HiROM' if hirom else 'LoROM', len(ins), sum(c.n for c in ins.values()), len(blocks), nfiles), file=sys.stderr)

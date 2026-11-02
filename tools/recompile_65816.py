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

Past that, candidates are guessed: jump tables, the code after PLP, and routines
sitting after the end of a known one. A wrong guess costs only code size. A block
runs only when the CPU is at exactly that (pc, mx), and ROM can't change, so what
it does is right whether or not the guess was.
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
COLUMN = [2, 2, 2, 2, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 4]
L = [COLUMN[op & 15] for op in range(256)]
L[0x20] = L[0x44] = L[0x54] = L[0x62] = L[0x82] = L[0xF4] = 3
L[0x40] = L[0x60] = 1
L[0x22] = L[0x5C] = 4
for op in (0x09, 0x29, 0x49, 0x69, 0x89, 0xA9, 0xC9, 0xE9):
    L[op] = 'm'
for op in (0xA0, 0xA2, 0xC0, 0xE0):
    L[op] = 'x'


def length(op, mx):
    c = L[op]
    if c == 'm':
        return 2 if mx & 2 else 3
    if c == 'x':
        return 2 if mx & 1 else 3
    return c


ALU = {0x00: 'ora', 0x20: 'and', 0x40: 'eor', 0x60: 'adc', 0x80: 'sta', 0xA0: 'lda', 0xC0: 'cmp', 0xE0: 'sbc'}
MODES = {0x01: 'a_inx_v({b})', 0x03: 'a_sr_v({b})', 0x05: 'a_dp_v({b})', 0x07: 'a_inl_v({b})',
         0x0D: 'a_abs_v({w})', 0x0F: '{l}', 0x11: 'a_iny_({X},{b},{W})', 0x12: 'a_ind_v({b})',
         0x13: 'a_sry_v({b})', 0x15: 'a_dpx_v({b})', 0x17: 'a_ily_v({b})',
         0x19: 'a_idx_({X},{w},w65.y,{W})', 0x1D: 'a_idx_({X},{w},w65.x,{W})', 0x1F: 'a_alx_v({l})'}
DP, AB, DX, DY = 'a_dp_v({b})', 'a_abs_v({w})', 'a_dpx_v({b})', 'a_dpy_v({b})'
AXW, AXR, AYR = 'a_idx_({X},{w},w65.x,1)', 'a_idx_({X},{w},w65.x,0)', 'a_idx_({X},{w},w65.y,0)'
SIMPLE = {0xA6: 'op_ldx_({X},%s)' % DP, 0xAE: 'op_ldx_({X},%s)' % AB, 0xB6: 'op_ldx_({X},%s)' % DY, 0xBE: 'op_ldx_({X},%s)' % AYR,
          0xA4: 'op_ldy_({X},%s)' % DP, 0xAC: 'op_ldy_({X},%s)' % AB, 0xB4: 'op_ldy_({X},%s)' % DX, 0xBC: 'op_ldy_({X},%s)' % AXR,
          0x86: 'op_stx_({X},%s)' % DP, 0x8E: 'op_stx_({X},%s)' % AB, 0x96: 'op_stx_({X},%s)' % DY,
          0x84: 'op_sty_({X},%s)' % DP, 0x8C: 'op_sty_({X},%s)' % AB, 0x94: 'op_sty_({X},%s)' % DX,
          0x64: 'op_stz_({M},%s)' % DP, 0x74: 'op_stz_({M},%s)' % DX, 0x9C: 'op_stz_({M},%s)' % AB, 0x9E: 'op_stz_({M},%s)' % AXW,
          0xE4: 'op_cpx_({X},%s)' % DP, 0xEC: 'op_cpx_({X},%s)' % AB, 0xC4: 'op_cpy_({X},%s)' % DP, 0xCC: 'op_cpy_({X},%s)' % AB,
          0x24: 'op_bit_({M},%s)' % DP, 0x2C: 'op_bit_({M},%s)' % AB, 0x34: 'op_bit_({M},%s)' % DX, 0x3C: 'op_bit_({M},%s)' % AXR,
          0xD4: 'op_pei({b})', 0xF4: 'push16({w})'}
for name, dp, ab, dx, ax, acc in (('ASL', 0x06, 0x0E, 0x16, 0x1E, 0x0A), ('LSR', 0x46, 0x4E, 0x56, 0x5E, 0x4A),
                                  ('ROL', 0x26, 0x2E, 0x36, 0x3E, 0x2A), ('ROR', 0x66, 0x6E, 0x76, 0x7E, 0x6A),
                                  ('INC', 0xE6, 0xEE, 0xF6, 0xFE, 0x1A), ('DEC', 0xC6, 0xCE, 0xD6, 0xDE, 0x3A)):
    SIMPLE[dp] = 'rmw_({M},R_%s,%s)' % (name, DP)
    SIMPLE[ab] = 'rmw_({M},R_%s,%s)' % (name, AB)
    SIMPLE[dx] = 'rmw_({M},R_%s,%s)' % (name, DX)
    SIMPLE[ax] = 'rmw_({M},R_%s,%s)' % (name, AXW)
    SIMPLE[acc] = 'rmw_a_({M},R_%s)' % name
SIMPLE.update({0x04: 'rmw_({M},R_TSB,%s)' % DP, 0x0C: 'rmw_({M},R_TSB,%s)' % AB,
               0x14: 'rmw_({M},R_TRB,%s)' % DP, 0x1C: 'rmw_({M},R_TRB,%s)' % AB})
IMPLIED = {0x48: 'op_pha_({M})', 0x68: 'op_pla_({M})', 0xDA: 'op_phx_({X})', 0xFA: 'op_plx_({X})', 0x5A: 'op_phy_({X})',
           0x7A: 'op_ply_({X})', 0x08: 'op_php()', 0x8B: 'op_phb()', 0xAB: 'op_plb()', 0x0B: 'op_phd()', 0x2B: 'op_pld()',
           0x4B: 'op_phk()', 0xAA: 'op_tax_({X})', 0xA8: 'op_tay_({X})', 0x8A: 'op_txa_({M})', 0x98: 'op_tya_({M})',
           0x9B: 'op_txy_({X})', 0xBB: 'op_tyx_({X})', 0xBA: 'op_tsx_({X})', 0x9A: 'op_txs()', 0x5B: 'op_tcd()',
           0x7B: 'op_tdc()', 0x1B: 'op_tcs()', 0x3B: 'op_tsc()', 0xEB: 'op_xba()', 0xE8: 'op_inx_({X})', 0xCA: 'op_dex_({X})',
           0xC8: 'op_iny_({X})', 0x88: 'op_dey_({X})', 0x18: 'IDLE(); w65.p &= ~CF', 0x38: 'IDLE(); w65.p |= CF',
           0x58: 'IDLE(); w65.p &= ~IF', 0x78: 'IDLE(); w65.p |= IF', 0xB8: 'IDLE(); w65.p &= ~VF',
           0xD8: 'IDLE(); w65.p &= ~DF', 0xF8: 'IDLE(); w65.p |= DF', 0xEA: 'IDLE()'}
BR = {0x10: '!(w65.p & NF)', 0x30: '(w65.p & NF)', 0x50: '!(w65.p & VF)', 0x70: '(w65.p & VF)',
      0x90: '!(w65.p & CF)', 0xB0: '(w65.p & CF)', 0xD0: '!(w65.p & ZF)', 0xF0: '(w65.p & ZF)'}
# run through the interpreter core, and the block must end (PC or flags unknown)
END = {0x00, 0x02, 0x28, 0x40, 0x44, 0x54, 0xCB, 0xDB, 0xFB}
# the target is only known at run time, so these end the block too
RETURN = {0x60: 'F(1); op_rts();', 0x6B: 'F(1); op_rtl();'}
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
    c = Ins(); c.pc, c.mx, c.op, c.n, c.targets, c.flow, c.mx_after, c.tail, c.cond = pc, mx, op, n, [], None, mx, None, None
    c.more = []
    bank = pc & 0xFF0000
    nxt = bank | ((pc + n) & 0xFFFF)
    c.next = nxt
    b = '0x%02X' % ops[0] if n > 1 else ''
    w = '0x%04X' % (ops[0] | ops[1] << 8) if n > 2 else ''
    l = '0x%06X' % (ops[0] | ops[1] << 8 | ops[2] << 16) if n > 3 else ''
    F = 'F(%d); ' % n
    low, high = op & 0x1F, op & 0xE0
    M, X = 1 if mx & 2 else 0, 1 if mx & 1 else 0
    if op in BR or op == 0x80:
        t = bank | ((pc + 2 + s8(ops[0])) & 0xFFFF)
        c.targets = [(t, mx)]
        c.tail = (t, mx)
        if op == 0x80:
            c.c = F + 'IDLE();'; c.flow = 'jump'
        else:
            c.c = F; c.cond = BR[op]; c.flow = 'branch'
    elif op == 0x82:
        t = bank | ((pc + 3 + s16(ops[0] | ops[1] << 8)) & 0xFFFF)
        c.targets = [(t, mx)]; c.flow = 'jump'; c.tail = (t, mx)
        c.c = F + 'IDLE();'
    elif op == 0x4C:
        t = bank | (ops[0] | ops[1] << 8)
        c.targets = [(t, mx)]; c.flow = 'jump'; c.tail = (t, mx); c.c = F.rstrip()
    elif op == 0x5C:
        t = ops[0] | ops[1] << 8 | ops[2] << 16
        c.targets = [(t, mx)]; c.flow = 'jump'; c.tail = (t, mx)
        c.c = F + 'w65.pb = 0x%02X;' % (t >> 16)
    elif op == 0x20:
        t = bank | (ops[0] | ops[1] << 8)
        c.targets = [(t, mx)]; c.flow = 'call'; c.tail = (t, mx)
        c.c = F + 'IDLE(); push16(0x%04X);' % ((pc + 2) & 0xFFFF)
    elif op == 0x22:
        t = ops[0] | ops[1] << 8 | ops[2] << 16
        c.targets = [(t, mx)]; c.flow = 'call'; c.tail = (t, mx)
        c.c = F + 'push8(w65.pb); IDLE(); push16(0x%04X); w65.pb = 0x%02X;' % ((pc + 3) & 0xFFFF, t >> 16)
    elif op in RETURN:
        c.flow = 'end'; c.c = RETURN[op]
    elif op == 0x6C:
        c.flow = 'end'; c.c = F + 'w65.pc = rd16z(%s);' % w
    elif op == 0xDC:
        c.flow = 'end'; c.c = F + '{ u16 p = %s; w65.pc = rd16z(p); w65.pb = RD((u16)(p + 2)); }' % w
    elif op == 0x7C:
        c.flow = 'end'; c.c = F + 'IDLE(); w65.pc = rd16(0x%06X | (u16)(%s + w65.x));' % (bank, w)
    elif op == 0xFC:
        c.flow = 'end'
        c.c = F + 'push16(0x%04X); IDLE(); w65.pc = rd16(0x%06X | (u16)(%s + w65.x));' % ((pc + 2) & 0xFFFF, bank, w)
    elif op in END:
        c.flow = 'end'
        c.c = 'w65.pc = 0x%04X; w65_exec();' % (pc & 0xFFFF)
        if op in (0x44, 0x54, 0xCB):     # block moves and WAI come back to the next instruction
            c.more = [(nxt, mx)]
        elif op == 0xFB:                 # XCE into native mode starts with 8-bit registers
            c.more = [(nxt, 3)]
    elif op in (0xC2, 0xE2):
        bits = (ops[0] >> 4) & 3          # P bit5=M, bit4=X -> mx bits 1,0
        c.mx_after = (mx & ~bits) if op == 0xC2 else (mx | bits)
        c.c = F + '%s(%s);' % ('op_rep' if op == 0xC2 else 'op_sep', b)
    elif high in ALU and low in MODES:
        name = ALU[high]
        ea = MODES[low].format(b=b, w=w, l=l, W=1 if name == 'sta' else 0, X=X)
        c.c = F + 'op_%s_(%d,%s);' % (name, M, ea)
    elif op in SIMPLE:
        c.c = F + SIMPLE[op].format(b=b, w=w, M=M, X=X) + ';'
    elif op in IMPLIED:
        c.c = F + IMPLIED[op].format(M=M, X=X) + ';'
    elif op == 0x62:
        c.c = F + 'IDLE(); push16(0x%04X);' % ((pc + 3 + s16(ops[0] | ops[1] << 8)) & 0xFFFF)
    elif op == 0x42:
        c.c = F.rstrip()
    elif op == 0x89:
        c.c = F + ('op_bit_imm8(%s);' % b if M else 'op_bit_imm16(%s);' % w)
    elif op in (0x09, 0x29, 0x49, 0xA9, 0xC9, 0x69, 0xE9):
        nm = {0x09: 'ora', 0x29: 'and', 0x49: 'eor', 0xA9: 'lda', 0xC9: 'cmp', 0x69: 'adc', 0xE9: 'sbc'}[op]
        o = {'ora': '|', 'and': '&', 'eor': '^'}.get(nm)
        if M:
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
        c.c = F + ('{ w65.x = %s; nz8((u8)w65.x); }' % b if X else '{ w65.x = %s; nz16(w65.x); }' % w)
    elif op == 0xA0:
        c.c = F + ('{ w65.y = %s; nz8((u8)w65.y); }' % b if X else '{ w65.y = %s; nz16(w65.y); }' % w)
    elif op == 0xE0:
        c.c = F + ('cmp8((u8)w65.x, %s);' % b if X else 'cmp16(w65.x, %s);' % w)
    elif op == 0xC0:
        c.c = F + ('cmp8((u8)w65.y, %s);' % b if X else 'cmp16(w65.y, %s);' % w)
    else:
        sys.exit('no translation for opcode %02X at %06X' % (op, pc))
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
BAD = {0x00, 0x02, 0x42, 0xDB}       # BRK, COP, WDM and STP don't appear in real code paths


def walk():
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
            for t in c.targets + c.more:
                if off(t[0]) is not None:
                    starts.add(t); work.append(t)
            if c.flow in ('jump', 'end'):
                break
            if c.flow in ('branch', 'call'):
                starts.add((c.next, c.mx_after))
            pc, mx = c.next, c.mx_after


def plausible(k0):
    """does every path from here decode as code, as far as the known code or a return?"""
    seen, todo = set(), [k0]
    while todo:
        k = todo.pop()
        if k in ins or k in seen:
            continue
        if len(seen) > 3000:
            return False
        c = decode(*k)
        if c is None or c.op in BAD:
            return False
        seen.add(k)
        for t in c.targets:
            if off(t[0]) is not None:
                todo.append(t)
            elif c.flow != 'call':
                return False
        todo += [t for t in c.more if off(t[0]) is not None]
        if c.flow not in ('jump', 'end'):
            todo.append((c.next, c.mx_after))
    return len(seen) >= 3 or k0 in ins


def rom16(a):
    lo, hi = rb(a), rb((a & 0xFF0000) | ((a + 1) & 0xFFFF))
    return None if lo is None or hi is None else lo | hi << 8


def table(a, bank, mx):
    """a list of 16-bit code addresses at a; returns the entries that look like code"""
    found = []
    for i in range(256):
        t = a + i * 2
        v = rom16(t)
        if v is None or v < 0x8000 or (t, 0) in ins or (t, 1) in ins or (t, 2) in ins or (t, 3) in ins:
            break
        k = (bank | v, mx)
        if not plausible(k):
            break
        found.append(k)
    return found


def guesses():
    new = []
    by_addr = sorted(ins)
    for (pc, mx), c in ((k, ins[k]) for k in by_addr):
        bank = pc & 0xFF0000
        w = rom16(bank | ((pc + 1) & 0xFFFF)) if c.n >= 3 else None
        if c.op in (0x7C, 0xFC) and w is not None:              # JMP/JSR (table,X)
            new += table(bank | w, bank, mx)
        elif c.op in (0x6C, 0xDC):                              # table,X loaded just before
            p = pc
            for _ in range(10):
                p = next((x for x in range(p - 1, p - 5, -1) if any((x, m) in ins and ins[(x, m)].next == p for m in range(4))), None)
                if p is None:
                    break
                d = next((ins[(p, m)] for m in range(4) if (p, m) in ins), None)
                if d.op in (0xB9, 0xBD):
                    new += table(bank | rom16(bank | (p + 1 & 0xFFFF)), bank, mx)
                    break
        elif c.op == 0xF4 and w is not None and w >= 0x8000:    # PEA of a return address
            new.append((bank | ((w + 1) & 0xFFFF), mx))
        elif c.op == 0x28:                                      # PLP: try each mode that follows
            new += [(c.next, m) for m in range(4)]
    # a routine usually starts right after the end of the one before it
    used = {(pc & 0xFF0000) | ((pc + i) & 0xFFFF) for (pc, mx), c in ins.items() for i in range(c.n)}
    for (pc, mx), c in ((k, ins[k]) for k in by_addr):
        if c.flow in ('jump', 'end') and not c.more and c.next not in used:
            p = c.next
            for _ in range(32):                                 # padding between routines
                if rb(p) not in (0x00, 0xFF):
                    break
                p += 1
            new += [(p, m) for m in dict.fromkeys((mx, 3, 0, 2, 1))]
    return new


walk()
known = len(ins)
for round_ in range(40):
    tried = set()
    n0 = len(ins)
    for k in guesses():
        if k in ins or k in tried or off(k[0]) is None:
            continue
        tried.add(k)
        if plausible(k) and k not in ins:
            if any((k[0], m) in ins for m in range(4)):         # that address already has a mode
                continue
            starts.add(k); entries.add(k); work.append(k)
            walk()
    if len(ins) == n0:
        break
print('followed %d instructions, guessed %d more in %d rounds' % (known, len(ins) - known, round_ + 1), file=sys.stderr)

# An entry from a coverage log may only be the place an interrupt happened to
# return to. When the instruction before it simply runs into it, it needs no
# block of its own: interrupts are taken between blocks, so they can't land there.
falls_into = {(c.next, c.mx_after) for c in ins.values() if not c.flow}
hard = set()
for c in ins.values():
    hard.update(c.targets)
    hard.update(c.more)
    if c.flow in ('branch', 'call'):
        hard.add((c.next, c.mx_after))
starts = hard | {e for e in entries if e not in falls_into}

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
#include "machine.h"
#ifndef SNES_LOCKSTEP
#define RD(a) snes_rd((a) & 0xFFFFFF)
#define WR(a, v) snes_wr((a) & 0xFFFFFF, (u8)(v))
#define IDLE() (snes_master += 6)
#endif
#define W65_NATIVE
#include "w65816_ops.h"
#define FAST(n) (snes_master += (n) * snes_rom_speed)
#define SLOW(n) (snes_master += (n) * 8)
#define GO(f) do { if (snes_master < snes_slice_end) f(); return; } while (0)
'''
CHUNK = 1500
name = lambda k: '%s%06X_%d' % (prefix, k[0], k[1])
nfiles = 0
for fi in range(0, len(blocks), CHUNK):
    part = blocks[fi:fi + CHUNK]
    here = {b[0] for b in part}

    def go(k):
        """leave the block for k: straight there when it is in this file"""
        t = 'w65.pc = 0x%04X;' % (k[0] & 0xFFFF)
        return t + (' GO(%s);' % name(k) if k in here else ' return;')

    with open(os.path.join(outdir, 'recomp_%03d.c' % nfiles), 'w') as f:
        f.write(HEAD)
        for k, _, _ in part:
            f.write('static void %s(void);\n' % name(k))
        for (pc, mx), seq, end in part:
            f.write('#undef F\n#define F(n) %s(n)\n' % ('FAST' if pc & 0x800000 else 'SLOW'))
            f.write('static void %s(void) {\n' % name((pc, mx)))
            for c in seq[:-1]:
                f.write('    %s  /* %06X op %02X */\n' % (c.c, c.pc, c.op))
            c = seq[-1]
            f.write('    %s  /* %06X op %02X */\n' % (c.c, c.pc, c.op))
            if c.flow == 'branch':
                f.write('    if (%s) { IDLE(); %s }\n    %s\n' % (c.cond, go(c.tail), go((c.next, c.mx_after))))
            elif c.flow in ('jump', 'call'):
                f.write('    %s\n' % go(c.tail))
            elif not c.flow:
                f.write('    %s\n' % go(end))
            f.write('}\n')
        f.write('static const SnesBlock table[] = {\n')
        for k, _, _ in part:
            f.write('    { 0x%06Xu, %d, %s },\n' % (k[0], k[1], name(k)))
        f.write('};\nvoid %s_%d(void) { snes_recomp_add(table, sizeof table / sizeof table[0]); }\n' % (register_fn, nfiles))
    nfiles += 1
with open(os.path.join(outdir, 'recomp_register.c'), 'w') as f:
    f.write('/* GENERATED by tools/recompile_65816.py - do not edit */\n#include "machine.h"\n')
    for n in range(nfiles):
        f.write('void %s_%d(void);\n' % (register_fn, n))
    f.write('void %s(void) {\n' % register_fn)
    for n in range(nfiles):
        f.write('    %s_%d();\n' % (register_fn, n))
    f.write('}\n')
if listing:
    with open(listing, 'w') as f:
        for k in sorted(ins):
            c = ins[k]
            f.write('%06X mx%d %s\n' % (c.pc, c.mx, ' '.join('%02X' % rb((c.pc & 0xFF0000) | ((c.pc + i) & 0xFFFF)) for i in range(c.n))))
print('%s, instructions %d (%d bytes), blocks %d, files %d' % ('HiROM' if hirom else 'LoROM', len(ins), sum(c.n for c in ins.values()), len(blocks), nfiles), file=sys.stderr)

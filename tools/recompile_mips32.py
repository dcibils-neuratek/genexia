#!/usr/bin/env python3
"""Compact N64 recompiler, for machines that can't afford the exact one.

    recompile_mips32.py ROM OUTDIR --entries recomps/n64/game/coverage.txt --register fn

recompile_mips.py mirrors the interpreter cycle for cycle. This one gives that up
to be small and fast on a 32-bit CPU; see src/chips/r4300_compact.h for the rules:

  - registers are 32 bits. A MIPS register holds the sign extension of its low
    word unless real 64-bit code ran, and instructions that need more go to the
    interpreter;
  - a block keeps registers in locals and stores the changed ones at its end;
  - blocks start only where control can arrive, so nothing is generated twice;
  - interrupts are taken between blocks, and cycle counts are only an estimate;
  - no bail-outs: anything a block can't do itself ends it just before that
    instruction. Hardware access goes through r4_io_*.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recompile_mips import branch_of, Unsupported, Invalid, COST_MEM, COST_MULT, COST_DMULT, COST_DIV, COST_DDIV, \
    COST_FADD, COST_FMUL_S, COST_FMUL_D, COST_FDIV_S, COST_FDIV_D, COST_FCVT

MAX_INSTS = 80
NEAR_64 = 12
BLOCKS_PER_FILE = 2500


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def fw(n):
    return (n & ~1) * 2 + (n & 1)


def fd64(n):
    return (n & ~1) * 2


class Block:
    """Code for one block, with the register locals it has in use."""

    def __init__(self):
        self.out = []
        self.state = {}        # register -> 'clean' or 'dirty'
        self.used = set()
        self.mem = False

    def emit(self, s):
        if s:
            self.out.append('    ' + s)

    def rd(self, r):
        if r == 0:
            return '0u'
        if r not in self.state:
            self.used.add(r)
            self.state[r] = 'clean'
            self.emit('r%d = g[L(%d)];' % (r, r))
        return 'r%d' % r

    def wr(self, r, e):
        if r == 0:
            return
        self.used.add(r)
        self.state[r] = 'dirty'
        self.emit('r%d = %s;' % (r, e))

    def flush_text(self):
        return ' '.join('SET(%d, r%d);' % (r, r) for r, s in sorted(self.state.items()) if s == 'dirty')

    def forget(self, r):
        """the register changed in memory, or is about to be read there"""
        if self.state.get(r) == 'dirty':
            self.emit('SET(%d, r%d);' % (r, r))
        self.state.pop(r, None)


def alu(b, pc, inst):
    """Writes one non-branch instruction into block b, returns its extra cycles."""
    op = inst >> 26
    rs, rt, rd, sa = (inst >> 21) & 31, (inst >> 16) & 31, (inst >> 11) & 31, (inst >> 6) & 31
    imm = inst & 0xFFFF
    simm = sx16(imm)
    fn = inst & 63
    R, W = b.rd, b.wr
    if op == 0:
        if fn in (8, 9):
            raise Invalid()
        if fn == 0:
            if inst:
                W(rd, '%s << %d' % (R(rt), sa))
            return 0
        if fn == 2:  W(rd, '%s >> %d' % (R(rt), sa)); return 0
        if fn == 3:  W(rd, '(u32)((s32)%s >> %d)' % (R(rt), sa)); return 0
        if fn == 4:  W(rd, '%s << (%s & 31)' % (R(rt), R(rs))); return 0
        if fn == 6:  W(rd, '%s >> (%s & 31)' % (R(rt), R(rs))); return 0
        if fn == 7:  W(rd, '(u32)((s32)%s >> (%s & 31))' % (R(rt), R(rs))); return 0
        if fn == 15: return 0
        if fn == 16: W(rd, 'r4_low(&s_hi)'); return 0
        if fn == 17: x = R(rs); b.emit('s_hi = SX(%s);' % x); return 0
        if fn == 18: W(rd, 'r4_low(&s_lo)'); return 0
        if fn == 19: x = R(rs); b.emit('s_lo = SX(%s);' % x); return 0
        if fn == 24: x, y = R(rs), R(rt); b.emit('r4_mult((s32)%s, (s32)%s);' % (x, y)); return COST_MULT
        if fn == 25: x, y = R(rs), R(rt); b.emit('r4_multu(%s, %s);' % (x, y)); return COST_MULT
        if fn == 26: x, y = R(rs), R(rt); b.emit('r4_div((s32)%s, (s32)%s);' % (x, y)); return COST_DIV
        if fn == 27: x, y = R(rs), R(rt); b.emit('r4_divu(%s, %s);' % (x, y)); return COST_DIV
        if fn in (32, 33): W(rd, '%s + %s' % (R(rs), R(rt))); return 0
        if fn in (34, 35): W(rd, '%s - %s' % (R(rs), R(rt))); return 0
        if fn == 36: W(rd, '%s & %s' % (R(rs), R(rt))); return 0
        if fn == 37: W(rd, '%s | %s' % (R(rs), R(rt))); return 0
        if fn == 38: W(rd, '%s ^ %s' % (R(rs), R(rt))); return 0
        if fn == 39: W(rd, '~(%s | %s)' % (R(rs), R(rt))); return 0
        if fn == 42: W(rd, '(s32)%s < (s32)%s' % (R(rs), R(rt))); return 0
        if fn == 43: W(rd, '%s < %s' % (R(rs), R(rt))); return 0
        if fn in (12, 13) or fn in (28, 29, 30, 31) or fn >= 44 or fn in (20, 22, 23):
            raise Unsupported()
        raise Invalid()
    if op == 1:
        if rt in (8, 9, 10, 11, 12, 14):
            raise Unsupported()
        raise Invalid()
    if op in (8, 9): W(rt, '%s + (u32)(%d)' % (R(rs), simm)); return 0
    if op == 10: W(rt, '(s32)%s < (%d)' % (R(rs), simm)); return 0
    if op == 11: W(rt, '%s < (u32)(%d)' % (R(rs), simm)); return 0
    if op == 12: W(rt, '%s & %du' % (R(rs), imm)); return 0
    if op == 13: W(rt, '%s | %du' % (R(rs), imm)); return 0
    if op == 14: W(rt, '%s ^ %du' % (R(rs), imm)); return 0
    if op == 15: W(rt, '0x%08Xu' % ((inst << 16) & 0xFFFFFFFF)); return 0
    if op in (16, 18, 24, 25):
        raise Unsupported()
    if op == 17:
        return cop1(b, inst)
    if op == 47:
        return 0
    if op in (26, 27, 44, 45, 48, 50, 52, 54, 56, 58, 60, 62):
        raise Unsupported()

    a = '%s + (u32)(%d)' % (R(rs), simm)
    if op in (32, 33, 35, 36, 37):
        name = {32: 'r4_lb', 33: 'r4_lh', 35: 'r4_lw', 36: 'r4_lbu', 37: 'r4_lhu'}[op]
        W(rt, '%s(%s)' % (name, a))
        return COST_MEM
    if op in (34, 38):
        W(rt, '%s(%s, %s)' % ('r4_lwl' if op == 34 else 'r4_lwr', R(rt), a))
        return COST_MEM
    if op == 39: W(rt, 'r4_lw(%s)' % a); return COST_MEM
    if op in (40, 41, 43):
        name = {40: 'r4_sb', 41: 'r4_sh', 43: 'r4_sw'}[op]
        b.emit('%s(%s, %s);' % (name, a, R(rt)))
        return COST_MEM
    if op in (42, 46):
        b.emit('%s(%s, %s);' % ('r4_swl' if op == 42 else 'r4_swr', a, R(rt)))
        return COST_MEM
    if op == 49: b.emit('s_fpw[%d] = r4_lw(%s);' % (fw(rt), a)); return COST_MEM
    if op == 57: b.emit('r4_sw(%s, s_fpw[%d]);' % (a, fw(rt))); return COST_MEM
    if op == 53: b.emit('r4_fsl(%d, r4_ld(%s));' % (fd64(rt), a)); return COST_MEM
    if op == 61: b.emit('r4_sd(%s, r4_fgl(%d));' % (a, fd64(rt))); return COST_MEM
    if op == 55:                # all 64 bits: the context switch saves registers this way
        b.state.pop(rt, None)
        if rt:
            b.emit('r4_gpr64(%d, r4_ld(%s));' % (rt, a))
        return COST_MEM
    if op == 63:
        b.forget(rt)
        b.emit('r4_sd(%s, %s);' % (a, 'r4_get64(%d)' % rt if rt else '0ull'))
        return COST_MEM
    raise Invalid()


def cop1(b, inst):
    rs, rt, rd = (inst >> 21) & 31, (inst >> 16) & 31, (inst >> 11) & 31
    fs, ft, fd, fn = rd, rt, (inst >> 6) & 31, inst & 63
    if rs == 0: b.wr(rt, 's_fpw[%d]' % fw(fs)); return 0
    if rs == 2:
        b.wr(rt, 's_fcr31' if rd == 31 else '0xA00u' if rd == 0 else '0u')
        return 0
    if rs == 4: x = b.rd(rt); b.emit('s_fpw[%d] = %s;' % (fw(fs), x)); return 0
    if rs in (1, 5, 6):
        raise Unsupported()
    if rs in (16, 17):
        s = rs == 16
        typ, get, put = ('float', 'r4_fgs', 'r4_fss') if s else ('double', 'r4_fgd', 'r4_fsd')
        reg = fw if s else fd64
        x, y, d = reg(fs), reg(ft), reg(fd)
        arith = {0: ('+', COST_FADD), 1: ('-', COST_FADD), 2: ('*', COST_FMUL_S if s else COST_FMUL_D),
                 3: ('/', COST_FDIV_S if s else COST_FDIV_D)}
        if fn in arith:
            o, c = arith[fn]
            b.emit('%s(%d, %s(%d) %s %s(%d));' % (put, d, get, x, o, get, y))
            return c
        if fn == 5:
            b.emit('%s(%d, %s(%s(%d)));' % (put, d, '__builtin_fabsf' if s else '__builtin_fabs', get, x))
            return 0
        if fn == 7: b.emit('%s(%d, -%s(%d));' % (put, d, get, x)); return 0
        if fn == 6:
            b.emit('s_fpw[%d] = s_fpw[%d];' % (d, x) if s else 'r4_fsl(%d, r4_fgl(%d));' % (d, x))
            return 0
        if fn >= 48:
            cond = fn & 15
            terms = []
            if cond & 1: terms.append('a_ != a_ || b_ != b_')
            if cond & 2: terms.append('a_ == b_')
            if cond & 4: terms.append('a_ < b_')
            b.emit('{ %s a_ = %s(%d), b_ = %s(%d); s_fcr31 = (s_fcr31 & ~(1u << 23)) | (u32)(%s) << 23; }' % (
                typ, get, x, get, y, ' || '.join(terms) or '0'))
            return 0
        if fn == 4:
            b.emit('%s(%d, %s(%s(%d)));' % (put, d, 'r4_sqrtf' if s else 'r4_sqrt', get, x))
            return COST_FDIV_S if s else COST_FDIV_D
        if fn == 13:             # TRUNC.W
            b.emit('s_fpw[%d] = (u32)(s32)%s(%d);' % (fw(fd), get, x))
            return COST_FCVT
        if s and fn == 33: b.emit('r4_fsd(%d, (double)r4_fgs(%d));' % (fd64(fd), x)); return 0
        if not s and fn == 32: b.emit('r4_fss(%d, (float)r4_fgd(%d));' % (fw(fd), x)); return COST_FCVT
        if 8 <= fn <= 15 or fn in (36, 37):
            b.emit('r4300_cop1(0x%08Xu);' % inst)
            return COST_FCVT
        raise Unsupported()
    if rs in (20, 21):
        if rs == 20 and fn == 32:
            b.emit('r4_fss(%d, (float)(s32)s_fpw[%d]);' % (fw(fd), fw(fs)))
            return COST_FCVT
        if rs == 20 and fn == 33:
            b.emit('r4_fsd(%d, (double)(s32)s_fpw[%d]);' % (fd64(fd), fw(fs)))
            return COST_FCVT
        raise Unsupported()
    raise Invalid()


def condition(b, inst, br):
    """The branch test on 32-bit values, from the instruction itself."""
    op = inst >> 26
    rs, rt = (inst >> 21) & 31, (inst >> 16) & 31
    if op == 1:
        if rs == 0:
            return '0' if not rt & 1 else '1'
        return '(s32)%s %s 0' % (b.rd(rs), '<' if not rt & 1 else '>=')
    if op in (2, 3):
        return '1'
    if op == 17:
        return br['c']
    if op in (4, 20):
        return '1' if rs == rt else '%s == %s' % (b.rd(rs), b.rd(rt))
    if op in (5, 21):
        return '0' if rs == rt else '%s != %s' % (b.rd(rs), b.rd(rt))
    if op in (6, 22):
        return '(s32)%s <= 0' % b.rd(rs)
    return '(s32)%s > 0' % b.rd(rs)


def needs_64(inst):
    op, fn = inst >> 26, inst & 63
    if op == 0:
        return fn in (20, 22, 23, 28, 29, 30, 31) or fn >= 44
    return op in (24, 25, 26, 27, 44, 45)


class Recompiler:
    def __init__(self, rom, regions):
        self.rom, self.regions = rom, regions
        self.wide = set()       # (pc, delta) near real 64-bit code: all the interpreter's
        self.starts = set()
        self.reached = {}       # (pc, delta) -> falls through to the next instruction
        self.blocks = {}

    def region(self, pc, delta):
        return any(ram <= pc < ram + n and off - ram == delta for ram, off, n in self.regions)

    def word(self, pc, delta):
        i = (pc + delta) & 0xFFFFFFFF
        return struct.unpack('>I', self.rom[i:i + 4])[0]

    def valid(self, pc, delta):
        return 0x80000000 <= pc < 0x80800000 and not pc & 3 and self.region(pc, delta)

    def find_wide(self):
        """64-bit values only live for a few instructions, in the runtime's long
        long helpers. An AND or a branch next to them can look 32-bit and isn't."""
        for ram, off, n in self.regions:
            d = off - ram
            for pc in range(ram, ram + n & ~3, 4):
                if pc + d + 4 <= len(self.rom) and needs_64(self.word(pc, d)):
                    for k in range(-NEAR_64, NEAR_64 + 1):
                        self.wide.add((pc + k * 4, d))

    def trial(self, pc, inst, d=None):
        """0 ok, 1 interpreter's, 2 not code"""
        if (pc, d) in self.wide:
            return 1
        try:
            alu(Block(), pc, inst)
        except Unsupported:
            return 1
        except Invalid:
            return 2
        return 0

    def scan(self, seeds):
        """Everything reachable, and which addresses something really jumps to."""
        work, seen = list(seeds), set()
        hard = set()
        while work:
            pc, d = work.pop()
            while (pc, d) not in seen and self.valid(pc, d):
                seen.add((pc, d))
                inst = self.word(pc, d)
                br = branch_of(pc, inst)
                if br:
                    self.reached[(pc, d)] = False
                    if not self.valid(pc + 4, d) or branch_of(pc + 4, self.word(pc + 4, d)) or self.trial(pc + 4, self.word(pc + 4, d), d) or (pc, d) in self.wide:
                        hard.add((pc, d))   # the interpreter runs both, then comes back
                        for t in [br['t'], pc + 8] if 't' in br else []:
                            hard.add((t, d))
                            work.append((t, d))
                        break
                    seen.add((pc + 4, d))
                    self.reached[(pc + 4, d)] = False
                    for t in ([br['t']] if 't' in br else []) + ([pc + 8] if 'reg' not in br and (br['c'] != '1' or br.get('link')) or br.get('link') else []):
                        hard.add((t, d))
                        work.append((t, d))
                    break
                k = self.trial(pc, inst, d)
                if k == 2:
                    break
                self.reached[(pc, d)] = k == 0
                if k == 1:           # the interpreter runs it, then lands here
                    hard.add((pc, d))
                    hard.add((pc + 4, d))
                pc += 4
        return hard

    def block(self, entry, d, name_of):
        b = Block()
        words, k, pc, end = [], 0, entry, None
        while True:
            if len(words) >= MAX_INSTS or ((pc, d) in self.starts and pc != entry) or not self.valid(pc, d):
                b.emit(b.flush_text())
                end = 'GO(%s, 0x%08Xu, %d);' % (name_of(pc, d), pc, k)
                break
            inst = self.word(pc, d)
            br = branch_of(pc, inst)
            if br:
                slot = pc + 4
                si = self.word(slot, d) if self.valid(slot, d) else None
                if si is None or branch_of(slot, si) or self.trial(slot, si, d) or (pc, d) in self.wide:
                    b.emit(b.flush_text())
                    end = 'STEP(0x%08Xu, %d);' % (pc, k)
                    break
                words += [inst, si]
                link = br.get('link')
                k0 = k + 1
                if 'reg' in br:
                    b.emit('{ u32 t_ = %s;' % b.rd(br['reg']))
                    if link:
                        b.wr(link, '0x%08Xu' % (pc + 8))
                    x = alu(b, slot, si)
                    b.emit(b.flush_text())
                    end = 'return r4_jump(t_, used + %d); }' % (k0 + 1 + x)
                    break
                t, c = br['t'], condition(b, inst, br)
                if link:
                    b.wr(link, '0x%08Xu' % (pc + 8))
                after = pc + 8
                # waiting for an interrupt: the time goes by without being spent
                if t == pc and si == 0 and c == '1' and not link and entry == pc:
                    end = 'IDLE(0x%08Xu);' % pc
                    break
                if br['likely']:
                    b.emit('{ int c_ = %s; if (!c_) { %s GO(%s, 0x%08Xu, %d); } }' % (
                        c, b.flush_text(), name_of(after, d), after, k0))
                    x = alu(b, slot, si)
                    b.emit(b.flush_text())
                    end = 'GO(%s, 0x%08Xu, %d);' % (name_of(t, d), t, k0 + 1 + x)
                    break
                b.emit('{ int c_ = %s;' % c)
                x = alu(b, slot, si)
                k1 = k0 + 1 + x
                b.emit(b.flush_text())
                if c == '1':
                    end = 'GO(%s, 0x%08Xu, %d); }' % (name_of(t, d), t, k1)
                else:
                    end = 'if (c_) GO(%s, 0x%08Xu, %d); GO(%s, 0x%08Xu, %d); }' % (
                        name_of(t, d), t, k1, name_of(after, d), after, k1)
                break
            try:
                if (pc, d) in self.wide:
                    raise Unsupported()
                x = alu(b, pc, inst)
            except (Unsupported, Invalid):
                b.emit(b.flush_text())
                end = 'STEP(0x%08Xu, %d);' % (pc, k)
                break
            words.append(inst)
            k += 1 + x
            pc += 4
        return dict(b=b, words=words, end=end)


def main():
    a = sys.argv[1:]
    rom_path, out = a[0], a[1]
    opt = {a[i]: a[i + 1] for i in range(2, len(a), 2)}
    rom = open(rom_path, 'rb').read()
    regions, seeds = [], []
    for line in open(opt['--entries']):
        f = line.split()
        if f and f[0] == 'D':
            regions.append((int(f[1], 16) | 0x80000000, int(f[2], 16), int(f[3], 16)))
        elif f and f[0] == 'B':
            seeds.append((int(f[1], 16), int(f[2], 16)))
    seeds = {(pc, off - pc) for pc, off in seeds}
    wanted = {d for _, d in seeds}
    regions = sorted(r for r in set(regions) if r[1] - r[0] in wanted)
    r = Recompiler(rom, regions)
    r.find_wide()
    hard = r.scan(seeds)
    # A logged entry the instruction before simply runs into was an interrupt's
    # return address; interrupts only happen between blocks now.
    r.starts = hard | {(pc, d) for pc, d in seeds if not r.reached.get((pc - 4, d))}
    r.starts = {s for s in r.starts if r.valid(*s)}

    order = sorted(r.starts, key=lambda s: (s[0], s[1] & 0xFFFFFFFF))
    prefix = opt.get('--prefix', 'N_')
    full = lambda pc, d: '%s%08X_%X' % (prefix, pc, (pc + d) & 0xFFFFFFFF)
    file_of = {s: i // BLOCKS_PER_FILE for i, s in enumerate(order)}
    known = set(order)

    def name_of_in(n):
        return lambda pc, d: full(pc, d) if file_of.get((pc, d)) == n else 'NULL'

    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.startswith('recomp_'):
            os.remove(os.path.join(out, f))
    insts = nfiles = 0
    for i in range(0, len(order), BLOCKS_PER_FILE):
        part, n = order[i:i + BLOCKS_PER_FILE], i // BLOCKS_PER_FILE
        made = [(s, r.block(s[0], s[1], name_of_in(n))) for s in part]
        made = [(s, m) for s, m in made if m['words'] or m['end'].startswith('IDLE')]
        with open(os.path.join(out, 'recomp_%03d.c' % n), 'w') as f:
            f.write('/* GENERATED by tools/recompile_mips32.py - do not edit */\n#include "r4300_compact.h"\n\n')
            have = {s for s, _ in made}
            for s in part:
                if s in have:
                    f.write('static int %s(int used);\n' % full(*s))
                else:
                    f.write('#define %s NULL\n' % full(*s))
            for (pc, d), m in made:
                b = m['b']
                f.write('static const u32 %s_w[] = {%s};\n' % (full(pc, d), ','.join('0x%08Xu' % w for w in m['words'])))
                f.write('static int %s(int used) {\n' % full(pc, d))
                f.write('    u32 *const g = r4_low_words;\n')
                # The OS turns the FPU on for a thread at its first fault, which is
                # also what tells it to save that thread's FP registers.
                if any(w >> 26 in (0x11, 0x31, 0x35, 0x39, 0x3D) for w in m['words']):
                    f.write('    if (unlikely((r4300_cop0[12] & 0x24000000u) != 0x20000000u)) STEP(0x%08Xu, 0);\n' % pc)
                if b.used:
                    f.write('    u32 %s;\n' % ', '.join('r%d' % x for x in sorted(b.used)))
                f.write('\n'.join(b.out))
                f.write('\n    %s\n}\n' % m['end'])
                insts += len(m['words'])
            f.write('static const R4Compact table[] = {\n')
            for (pc, d), m in made:
                f.write('    { 0x%08Xu, %s, %s_w, %d },\n' % (pc, full(pc, d), full(pc, d), len(m['words'])))
            f.write('};\nvoid %s_%d(void) { r4300_compact_add(table, sizeof table / sizeof table[0]); }\n' % (opt['--register'], n))
        nfiles += 1
    with open(os.path.join(out, 'recomp_register.c'), 'w') as f:
        f.write('/* GENERATED by tools/recompile_mips32.py - do not edit */\n')
        for n in range(nfiles):
            f.write('void %s_%d(void);\n' % (opt['--register'], n))
        f.write('void %s(void) {\n' % opt['--register'])
        for n in range(nfiles):
            f.write('    %s_%d();\n' % (opt['--register'], n))
        f.write('}\n')
    print('%d blocks, %d instructions, %d files, %d seeds' % (len(order), insts, nfiles, len(seeds)), file=sys.stderr)


if __name__ == '__main__':
    main()

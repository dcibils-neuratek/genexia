#!/usr/bin/env python3
"""Static recompiler for N64 (MIPS III) games.

    recompile_mips.py ROM OUTDIR --entries recomps/n64/game/coverage.txt --prefix M_ --register fn

Coverage lines are `D ram rom len` (the game copied ROM to RAM) and
`B pc rom` (code at pc, from that ROM offset); they hold addresses only. Code is
read from the ROM, so the same RAM address can have several overlays.

Each block mirrors the interpreter in src/chips/r4300.c exactly, including its
cycle costs; see src/chips/r4300_recomp.h for the contract. Whatever a block
does not handle leaves it for the interpreter.
"""
import os
import struct
import sys

COST_MEM, COST_MULT, COST_DMULT, COST_DIV, COST_DDIV = 1, 4, 7, 36, 68
COST_FADD, COST_FMUL_S, COST_FMUL_D, COST_FDIV_S, COST_FDIV_D, COST_FCVT = 2, 4, 7, 28, 57, 3
MAX_INSTS = 40
BLOCKS_PER_FILE = 3000


def G(n):
    return '0ull' if n == 0 else 'r4300_gpr[%d]' % n


def S32(e):
    return '(u32)(%s)' % e


class Unsupported(Exception):
    """A real instruction the interpreter must run."""


class Invalid(Exception):
    """Not an instruction: this path is data."""


class Gen:
    def __init__(self, fr):
        self.fr = fr

    def fw(self, n):
        return n * 2 if self.fr else (n & ~1) * 2 + (n & 1)

    def fd(self, n):
        return n * 2 if self.fr else (n & ~1) * 2

    def set(self, r, e):
        return '' if r == 0 else '%s = %s;' % (G(r), e)

    def mem(self, inst, size, wr, body, bail, mask=None):
        """Address in `a`, RDRAM offset in `o`; `body` runs on the fast path."""
        rs = (inst >> 21) & 31
        imm = struct.unpack('>h', struct.pack('>H', inst & 0xFFFF))[0]
        if mask is None:
            bad = 'o >= N64_RDRAM_SIZE' + (' || (a & %d)' % (size - 1) if size > 1 else '')
            off = 'a - 0x80000000u'
        else:
            bad = 'o >= N64_RDRAM_SIZE'
            off = '(a & ~%du) - 0x80000000u' % mask
        return '{ u32 a = (u32)%s + (u32)(%d); u32 o = %s; if (unlikely(%s)) %s; %s }' % (
            G(rs), imm, off, bad, bail, body)

    def alu(self, pc, inst, bail):
        """Returns (code, extra cycles, uses fpu) of a non-branch instruction."""
        op = inst >> 26
        rs, rt, rd, sa = (inst >> 21) & 31, (inst >> 16) & 31, (inst >> 11) & 31, (inst >> 6) & 31
        imm = inst & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        fn = inst & 63
        a, b = G(rs), G(rt)
        ua, ub = S32(a), S32(b)
        st = self.set
        if op == 0:
            if fn in (8, 9):
                raise Invalid()
            if fn == 0:  return st(rd, 'SX(%s << %d)' % (ub, sa)), 0, 0
            if fn == 2:  return st(rd, 'SX(%s >> %d)' % (ub, sa)), 0, 0
            if fn == 3:  return st(rd, 'SX((s64)%s >> %d)' % (b, sa)), 0, 0
            if fn == 4:  return st(rd, 'SX(%s << (%s & 31))' % (ub, a)), 0, 0
            if fn == 6:  return st(rd, 'SX(%s >> (%s & 31))' % (ub, a)), 0, 0
            if fn == 7:  return st(rd, 'SX((s64)%s >> (%s & 31))' % (b, a)), 0, 0
            if fn in (12, 13): raise Unsupported()
            if fn == 15: return '', 0, 0
            if fn == 16: return st(rd, 's_hi'), 0, 0
            if fn == 17: return 's_hi = %s;' % a, 0, 0
            if fn == 18: return st(rd, 's_lo'), 0, 0
            if fn == 19: return 's_lo = %s;' % a, 0, 0
            if fn == 20: return st(rd, '%s << (%s & 63)' % (b, a)), 0, 0
            if fn == 22: return st(rd, '%s >> (%s & 63)' % (b, a)), 0, 0
            if fn == 23: return st(rd, '(u64)((s64)%s >> (%s & 63))' % (b, a)), 0, 0
            if fn == 24: return 'r4_mult((s32)%s, (s32)%s);' % (a, b), COST_MULT, 0
            if fn == 25: return 'r4_multu(%s, %s);' % (ua, ub), COST_MULT, 0
            if fn == 26: return 'r4_div((s32)%s, (s32)%s);' % (a, b), COST_DIV, 0
            if fn == 27: return 'r4_divu(%s, %s);' % (ua, ub), COST_DIV, 0
            if fn == 28: return 'r4_dmult((s64)%s, (s64)%s);' % (a, b), COST_DMULT, 0
            if fn == 29: return 'r4_dmultu(%s, %s);' % (a, b), COST_DMULT, 0
            if fn == 30: return 'r4_ddiv((s64)%s, (s64)%s);' % (a, b), COST_DDIV, 0
            if fn == 31: return 'r4_ddivu(%s, %s);' % (a, b), COST_DDIV, 0
            if fn in (32, 34):
                f = 'sadd' if fn == 32 else 'ssub'
                return '{ s32 r; if (unlikely(__builtin_%s_overflow((s32)%s, (s32)%s, &r))) %s; %s }' % (
                    f, a, b, bail, st(rd, 'SX(r)')), 0, 0
            if fn == 33: return st(rd, 'SX(%s + %s)' % (ua, ub)), 0, 0
            if fn == 35: return st(rd, 'SX(%s - %s)' % (ua, ub)), 0, 0
            if fn == 36: return st(rd, '%s & %s' % (a, b)), 0, 0
            if fn == 37: return st(rd, '%s | %s' % (a, b)), 0, 0
            if fn == 38: return st(rd, '%s ^ %s' % (a, b)), 0, 0
            if fn == 39: return st(rd, '~(%s | %s)' % (a, b)), 0, 0
            if fn == 42: return st(rd, '(s64)%s < (s64)%s' % (a, b)), 0, 0
            if fn == 43: return st(rd, '(u64)%s < (u64)%s' % (a, b)), 0, 0
            if fn in (44, 46):
                f = 'saddll' if fn == 44 else 'ssubll'
                return '{ long long r; if (unlikely(__builtin_%s_overflow((long long)%s, (long long)%s, &r))) %s; %s }' % (
                    f, a, b, bail, st(rd, '(u64)r')), 0, 0
            if fn == 45: return st(rd, '%s + %s' % (a, b)), 0, 0
            if fn == 47: return st(rd, '%s - %s' % (a, b)), 0, 0
            if fn in (48, 49, 50, 51, 52, 54): raise Unsupported()
            if fn == 56: return st(rd, '%s << %d' % (b, sa)), 0, 0
            if fn == 58: return st(rd, '%s >> %d' % (b, sa)), 0, 0
            if fn == 59: return st(rd, '(u64)((s64)%s >> %d)' % (b, sa)), 0, 0
            if fn == 60: return st(rd, '%s << %d' % (b, sa + 32)), 0, 0
            if fn == 62: return st(rd, '%s >> %d' % (b, sa + 32)), 0, 0
            if fn == 63: return st(rd, '(u64)((s64)%s >> %d)' % (b, sa + 32)), 0, 0
            raise Invalid()
        if op == 1:
            if rt in (8, 9, 10, 11, 12, 14):
                raise Unsupported()
            raise Invalid()
        if op == 8:
            return '{ s32 r; if (unlikely(__builtin_sadd_overflow((s32)%s, %d, &r))) %s; %s }' % (
                a, simm, bail, st(rt, 'SX(r)')), 0, 0
        if op == 9:  return st(rt, 'SX(%s + (u32)(%d))' % (ua, simm)), 0, 0
        if op == 10: return st(rt, '(s64)%s < (s64)(%d)' % (a, simm)), 0, 0
        if op == 11: return st(rt, '(u64)%s < (u64)(s64)(%d)' % (a, simm)), 0, 0
        if op == 12: return st(rt, '%s & %du' % (a, imm)), 0, 0
        if op == 13: return st(rt, '%s | %du' % (a, imm)), 0, 0
        if op == 14: return st(rt, '%s ^ %du' % (a, imm)), 0, 0
        if op == 15: return st(rt, 'SX(0x%08Xu)' % ((inst << 16) & 0xFFFFFFFF)), 0, 0
        if op == 16:
            raise Unsupported()
        if op == 17:
            return self.cop1(inst, bail)
        if op == 18:
            raise Unsupported()
        if op == 24:
            return '{ long long r; if (unlikely(__builtin_saddll_overflow((long long)%s, (long long)(%d), &r))) %s; %s }' % (
                a, simm, bail, st(rt, '(u64)r')), 0, 0
        if op == 25: return st(rt, '%s + (u64)(s64)(%d)' % (a, simm)), 0, 0
        M = lambda size, wr, body: self.mem(inst, size, wr, body, bail)
        U = lambda size, body: self.mem(inst, size, 0, body, bail, size - 1)
        if op == 26:
            return U(8, 'u32 sh = (a & 7) * 8; u64 m = r4_rd64(n64_rdram + o); %s' % st(
                rt, '(%s & ((1ull << sh) - 1)) | (m << sh)' % b)), COST_MEM, 0
        if op == 27:
            return U(8, 'u32 sh = (7 - (a & 7)) * 8; u64 m = r4_rd64(n64_rdram + o); %s' % st(
                rt, '(%s & ~(~0ull >> sh)) | (m >> sh)' % b)), COST_MEM, 0
        if op == 32: return M(1, 0, st(rt, '(u64)(s64)(s8)n64_rdram[o]')), COST_MEM, 0
        if op == 33: return M(2, 0, st(rt, '(u64)(s64)(s16)r4_rd16(n64_rdram + o)')), COST_MEM, 0
        if op == 34:
            return U(4, 'u32 sh = (a & 3) * 8; u32 m = r4_rd32(n64_rdram + o); %s' % st(
                rt, 'SX((%s & ((1u << sh) - 1)) | (m << sh))' % ub)), COST_MEM, 0
        if op == 35: return M(4, 0, st(rt, 'SX(r4_rd32(n64_rdram + o))')), COST_MEM, 0
        if op == 36: return M(1, 0, st(rt, 'n64_rdram[o]')), COST_MEM, 0
        if op == 37: return M(2, 0, st(rt, 'r4_rd16(n64_rdram + o)')), COST_MEM, 0
        if op == 38:
            return U(4, 'u32 sh = (3 - (a & 3)) * 8; u32 m = r4_rd32(n64_rdram + o); %s' % st(
                rt, 'SX((%s & ~(0xFFFFFFFFu >> sh)) | (m >> sh))' % ub)), COST_MEM, 0
        if op == 39: return M(4, 0, st(rt, 'r4_rd32(n64_rdram + o)')), COST_MEM, 0
        if op == 40: return M(1, 1, 'n64_rdram[o] = (u8)%s;' % b), COST_MEM, 0
        if op == 41: return M(2, 1, 'r4_wr16(n64_rdram + o, (u16)%s);' % b), COST_MEM, 0
        if op == 42:
            return U(4, 'u32 sh = (a & 3) * 8; u32 mk = 0xFFFFFFFFu >> sh; '
                     'r4_wr32(n64_rdram + o, (r4_rd32(n64_rdram + o) & ~mk) | (%s >> sh));' % ub), COST_MEM, 0
        if op == 43: return M(4, 1, 'r4_wr32(n64_rdram + o, %s);' % ub), COST_MEM, 0
        if op == 44:
            return U(8, 'u32 sh = (a & 7) * 8; u64 mk = ~0ull >> sh; '
                     'r4_wr64(n64_rdram + o, (r4_rd64(n64_rdram + o) & ~mk) | (%s >> sh));' % b), COST_MEM, 0
        if op == 45:
            return U(8, 'u32 sh = (7 - (a & 7)) * 8; '
                     'r4_wr64(n64_rdram + o, (r4_rd64(n64_rdram + o) & ((1ull << sh) - 1)) | (%s << sh));' % b), COST_MEM, 0
        if op == 46:
            return U(4, 'u32 sh = (3 - (a & 3)) * 8; '
                     'r4_wr32(n64_rdram + o, (r4_rd32(n64_rdram + o) & ((1u << sh) - 1)) | (%s << sh));' % ub), COST_MEM, 0
        if op == 47: return '', 0, 0
        if op in (48, 52, 56, 60):
            raise Unsupported()
        if op == 49: return M(4, 0, 's_fpw[%d] = r4_rd32(n64_rdram + o);' % self.fw(rt)), COST_MEM, 1
        if op == 53: return M(8, 0, 'r4_fsl(%d, r4_rd64(n64_rdram + o));' % self.fd(rt)), COST_MEM, 1
        if op == 55: return M(8, 0, st(rt, 'r4_rd64(n64_rdram + o)')), COST_MEM, 0
        if op == 57: return M(4, 1, 'r4_wr32(n64_rdram + o, s_fpw[%d]);' % self.fw(rt)), COST_MEM, 1
        if op == 61: return M(8, 1, 'r4_wr64(n64_rdram + o, r4_fgl(%d));' % self.fd(rt)), COST_MEM, 1
        if op == 63: return M(8, 1, 'r4_wr64(n64_rdram + o, %s);' % b), COST_MEM, 0
        if op in (50, 54, 58, 62):
            raise Unsupported()
        raise Invalid()

    def cop1(self, inst, bail):
        rs, rt, rd = (inst >> 21) & 31, (inst >> 16) & 31, (inst >> 11) & 31
        fs, ft, fd, fn = rd, rt, (inst >> 6) & 31, inst & 63
        st = self.set
        if rs == 0: return st(rt, 'SX(s_fpw[%d])' % self.fw(fs)), 0, 1
        if rs == 1: return st(rt, 'r4_fgl(%d)' % self.fd(fs)), 0, 1
        if rs == 2:
            return st(rt, 'SX(s_fcr31)' if rd == 31 else '0xA00u' if rd == 0 else '0'), 0, 1
        if rs == 4: return 's_fpw[%d] = %s;' % (self.fw(fs), S32(G(rt))), 0, 1
        if rs == 5: return 'r4_fsl(%d, %s);' % (self.fd(fs), G(rt)), 0, 1
        if rs == 6:
            raise Unsupported()
        if rs in (16, 17):
            s = rs == 16
            typ, get, put, odd = ('float', 'r4_fgs', 'r4_fss', 'r4_odd_s') if s else ('double', 'r4_fgd', 'r4_fsd', 'r4_odd_d')
            idx = self.fw if s else self.fd
            x, y, d = idx(fs), idx(ft), idx(fd)
            arith = {0: ('+', COST_FADD), 1: ('-', COST_FADD),
                     2: ('*', COST_FMUL_S if s else COST_FMUL_D),
                     3: ('/', COST_FDIV_S if s else COST_FDIV_D)}
            if fn in arith:
                o, c = arith[fn]
                return '{ %s a_ = %s(%d), b_ = %s(%d), r = a_ %s b_; if (unlikely(%s(r))) %s; FC_CLEAR; %s(%d, r); }' % (
                    typ, get, x, get, y, o, odd, bail, put, d), c, 1
            if fn in (5, 7):
                e = ('__builtin_fabsf(a_)' if s else '__builtin_fabs(a_)') if fn == 5 else '-a_'
                return '{ %s a_ = %s(%d), r = %s; if (unlikely(%s(r))) %s; FC_CLEAR; %s(%d, r); }' % (
                    typ, get, x, e, odd, bail, put, d), 0, 1
            if fn == 6:
                c = 's_fpw[%d] = s_fpw[%d];' % (d, x) if s else 'r4_fsl(%d, r4_fgl(%d));' % (d, x)
                return 'FC_CLEAR; ' + c, 0, 1
            if fn >= 48:
                cond = fn & 15
                eq = '(a_ == b_)' if cond & 2 else '0'
                lt = '(a_ < b_)' if cond & 4 else '0'
                return ('{ %s a_ = %s(%d), b_ = %s(%d); if (unlikely(a_ != a_ || b_ != b_)) %s; '
                        'int c_ = %s || %s; s_fcr31 = (s_fcr31 & ~0x3F000u & ~(1u << 23)) | (u32)c_ << 23; }') % (
                    typ, get, x, get, y, bail, eq, lt), 0, 1
            if fn == 4:
                extra = COST_FDIV_S if s else COST_FDIV_D
            elif 8 <= fn <= 15 or fn in (36, 37):
                extra = COST_FCVT
            elif s and fn == 33:
                extra = 0
            elif not s and fn == 32:
                extra = COST_FCVT
            else:
                raise Unsupported()
        elif rs in (20, 21):
            if fn not in (32, 33):
                raise Unsupported()
            extra = COST_FCVT
        else:
            raise Invalid()
        return '{ u32 f_ = s_fcr31; if (unlikely(r4300_cop1(0x%08Xu) < 0)) { s_fcr31 = f_; %s; } }' % (inst, bail), extra, 1


def branch_of(pc, inst):
    """(condition C, likely, link, target or None, target register C) or None."""
    op = inst >> 26
    rs, rt = (inst >> 21) & 31, (inst >> 16) & 31
    imm = inst & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    t = (pc + 4 + (simm << 2)) & 0xFFFFFFFF
    a, b = G(rs), G(rt)
    if op == 0 and (inst & 63) == 8:
        return dict(reg=rs, link=None)
    if op == 0 and (inst & 63) == 9:
        return dict(reg=rs, link=(inst >> 11) & 31)
    if op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19):
        c = '(s64)%s %s 0' % (a, '<' if not rt & 1 else '>=')
        if rs == 0:
            c = '0' if not rt & 1 else '1'
        return dict(c=c, likely=bool(rt & 2), link=31 if rt & 16 else None, t=t)
    if op in (2, 3):
        return dict(c='1', likely=False, link=31 if op == 3 else None,
                    t=((pc + 4) & 0xF0000000) | ((inst & 0x03FFFFFF) << 2))
    eq = '%s == %s' % (a, b)
    ne = '%s != %s' % (a, b)
    if rs == rt:
        eq, ne = '1', '0'
    tbl = {4: (eq, 0), 5: (ne, 0), 6: ('(s64)%s <= 0' % a, 0), 7: ('(s64)%s > 0' % a, 0),
           20: (eq, 1), 21: (ne, 1), 22: ('(s64)%s <= 0' % a, 1), 23: ('(s64)%s > 0' % a, 1)}
    if op in tbl:
        c, lk = tbl[op]
        return dict(c=c, likely=bool(lk), link=None, t=t)
    if op == 17 and rs == 8:
        want = (inst >> 16) & 1
        return dict(c='((s_fcr31 >> 23) & 1) == %d' % want, likely=bool(inst & (1 << 17)), link=None, t=t, fpu=1)
    return None


class Recompiler:
    def __init__(self, rom, regions, gen):
        self.rom, self.regions, self.gen = rom, regions, gen
        self.blocks = {}      # (pc, rom offset) -> dict
        self.todo = []

    def region(self, pc, delta):
        for ram, off, n in self.regions:
            if ram <= pc < ram + n and off - ram == delta:
                return True
        return False

    def seed(self, pc, delta):
        if (pc, delta) not in self.blocks and 0x80000000 <= pc < 0x80800000 and not pc & 3 and self.region(pc, delta):
            self.blocks[(pc, delta)] = None
            self.todo.append((pc, delta))

    def word(self, pc, delta):
        i = (pc + delta) & 0xFFFFFFFF
        return struct.unpack('>I', self.rom[i:i + 4])[0]

    def block(self, entry, delta):
        gen = self.gen
        lines, words, k, last_k, fpu = [], [], 0, 0, 0
        pc = entry
        follow = []
        end = None
        while True:
            if len(words) >= MAX_INSTS or not self.region(pc, delta):
                end = 'EXIT(0x%08Xu, %d);' % (pc, k)
                follow.append(pc)
                break
            inst = self.word(pc, delta)
            br = branch_of(pc, inst)
            if br:
                slot = pc + 4
                if not self.region(slot, delta):
                    end = None
                    break
                si = self.word(slot, delta)
                if branch_of(slot, si):
                    end = None             # branch in a delay slot: interpreter
                    break
                words += [inst, si]
                last_k = k
                link = br.get('link')
                pre = ''
                k0 = k + 1
                if 'reg' in br:
                    target_expr = 't_'
                    pre = 'u32 t_ = %s; ' % S32(G(br['reg']))
                    if link:
                        pre += '%s = SX(0x%08Xu); ' % (G(link), pc + 8)
                    bail = 'BAIL(0x%08Xu, t_, 1, %d)' % (slot, k0)
                    try:
                        code, x, f = gen.alu(slot, si, bail)
                    except Unsupported:
                        words = words[:-2]
                        end = None
                        break
                    except Invalid:
                        return self.finish(entry, delta, lines, words[:-2], last_k, fpu, None, follow, stop=True)
                    fpu |= f
                    last_k = k0
                    end = '{ %s%s EXITR(t_, %d); }' % (pre, code, k0 + 1 + x)
                    break
                t, c = br['t'], br['c']
                if link:
                    pre += '%s = SX(0x%08Xu); ' % (G(link), pc + 8)
                fpu |= br.get('fpu', 0)
                after = pc + 8
                if br['likely']:
                    bail = 'BAIL(0x%08Xu, 0x%08Xu, 1, %d)' % (slot, t, k0)
                else:
                    bail = 'BAIL(0x%08Xu, c_ ? 0x%08Xu : 0x%08Xu, 1, %d)' % (slot, t, after, k0)
                try:
                    code, x, f = gen.alu(slot, si, bail)
                except Unsupported:
                    words = words[:-2]
                    end = None
                    break
                except Invalid:
                    return self.finish(entry, delta, lines, words[:-2], last_k, fpu, None, follow, stop=True)
                fpu |= f
                last_k = k0
                k1 = k0 + 1 + x
                if t == entry and delta is not None:
                    jump = '{ used += %d; goto top; }' % k1
                    self.loops = getattr(self, 'loops', 0) + 1
                else:
                    jump = 'EXIT(0x%08Xu, %d);' % (t, k1)
                if br['likely']:
                    end = '{ %sint c_ = %s; if (!c_) EXIT(0x%08Xu, %d); %s %s }' % (pre, c, after, k0, code, jump)
                else:
                    end = '{ %sint c_ = %s; %s if (c_) %s EXIT(0x%08Xu, %d); }' % (pre, c, code, jump, after, k1)
                follow += [t, after]
                break
            bail = 'BAIL(0x%08Xu, 0x%08Xu, 0, %d)' % (pc, pc + 4, k)
            try:
                code, x, f = gen.alu(pc, inst, bail)
            except Unsupported:
                follow.append(pc + 4)
                break
            except Invalid:
                break
            words.append(inst)
            fpu |= f
            last_k = k
            if code:
                lines.append('    ' + code)
            k += 1 + x
            pc += 4
        if end is None and words:
            end = 'BAIL(0x%08Xu, 0x%08Xu, 0, %d);' % (pc, pc + 4, k)
        return self.finish(entry, delta, lines, words, last_k, fpu, end, follow)

    def finish(self, entry, delta, lines, words, last_k, fpu, end, follow, stop=False):
        for f in follow:
            self.seed(f, delta)
        if not words:
            return None
        if end is None:
            pc = entry + len(words) * 4
            end = 'BAIL(0x%08Xu, 0x%08Xu, 0, %d);' % (pc, pc + 4, last_k + 1)
            return None if stop else dict(lines=lines, words=words, last_k=last_k, fpu=fpu, end=end)
        return dict(lines=lines, words=words, last_k=last_k, fpu=fpu, end=end)

    def run(self):
        while self.todo:
            pc, delta = self.todo.pop()
            self.blocks[(pc, delta)] = self.block(pc, delta)


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
    wanted = {off - pc for pc, off in seeds}
    regions = sorted(r for r in set(regions) if r[1] - r[0] in wanted)
    gen = Gen(int(opt.get('--fr', '1')))
    r = Recompiler(rom, regions, gen)
    for pc, off in seeds:
        r.seed(pc, off - pc)
    r.run()

    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.startswith('recomp_'):
            os.remove(os.path.join(out, f))
    prefix = opt.get('--prefix', 'M_')
    blocks = [(pc, d, b) for (pc, d), b in sorted(r.blocks.items(), key=lambda kv: (kv[0][0], kv[0][1] & 0xFFFFFFFF)) if b]
    names = {}
    for n in range(0, len(blocks), BLOCKS_PER_FILE):
        with open(os.path.join(out, 'recomp_%03d.c' % (n // BLOCKS_PER_FILE)), 'w') as f:
            f.write('/* GENERATED by tools/recompile_mips.py - do not edit */\n#include "r4300_recomp.h"\n\n')
            for pc, d, b in blocks[n:n + BLOCKS_PER_FILE]:
                name = '%s%08X_%X' % (prefix, pc, (pc + d) & 0xFFFFFFFF)
                names[(pc, d)] = name
                f.write('const u32 %s_w[] = {%s};\n' % (name, ','.join('0x%08Xu' % w for w in b['words'])))
                f.write('int %s(int used) {\ntop:\n' % name)
                f.write('    if (unlikely(used + %d >= s_limit)) STALL(0x%08Xu);\n' % (b['last_k'], pc))
                if b['fpu']:
                    f.write('    if (unlikely((r4300_cop0[12] & 0x24000000u) != 0x%08Xu)) STALL(0x%08Xu);\n' % (
                        0x20000000 | (0x04000000 if gen.fr else 0), pc))
                f.write('\n'.join(b['lines']))
                f.write('\n    %s\n}\n' % b['end'])
    with open(os.path.join(out, 'recomp_register.c'), 'w') as f:
        f.write('/* GENERATED by tools/recompile_mips.py - do not edit */\n#include "r4300_recomp.h"\n\n')
        for name in names.values():
            f.write('extern const u32 %s_w[]; int %s(int);\n' % (name, name))
        f.write('void %s(void) {\n' % opt['--register'])
        for (pc, d), name in names.items():
            b = r.blocks[(pc, d)]
            f.write('    r4300_recomp_add(0x%08Xu, %s, %s_w, %d);\n' % (pc, name, name, len(b['words'])))
        f.write('}\n')
    insts = sum(len(b['words']) for _, _, b in blocks)
    print('%d blocks, %d instructions, %d self loops' % (len(blocks), insts, getattr(r, 'loops', 0)), file=sys.stderr)


main()

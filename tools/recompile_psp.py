#!/usr/bin/env python3
"""Static recompiler for PSP games (Allegrex: 32-bit MIPS with the VFPU).

    recompile_psp.py PROGRAM OUTDIR [--extra file of hex pcs]

PROGRAM is a decrypted, fixed-address ELF. Code is found by following it from
the entry point and from every value in the program that could be a code
address. Each block is a C function returning the next pc; see
src/chips/allegrex.h. VFPU instructions call the VFPU core one at a time.

The output holds the game's code: it stays out of this repository.
"""
import collections
import os
import struct
import sys

BLOCKS_PER_FILE = 4000


class Invalid(Exception):
    """Not an instruction: this path is data."""


def R(n):
    return '0u' if n == 0 else 'p.r[%d]' % n


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def set_r(r, e):
    return '' if r == 0 else 'p.r[%d] = %s;' % (r, e)


def special(inst):
    rs, rt, rd, sa, fn = (inst >> 21) & 31, (inst >> 16) & 31, (inst >> 11) & 31, (inst >> 6) & 31, inst & 63
    a, b = R(rs), R(rt)
    s = lambda x: '(s32)' + x
    t = {
        32: a + ' + ' + b, 33: a + ' + ' + b, 34: a + ' - ' + b, 35: a + ' - ' + b,
        36: a + ' & ' + b, 37: a + ' | ' + b, 38: a + ' ^ ' + b, 39: '~(%s | %s)' % (a, b),
        42: '%s < %s' % (s(a), s(b)), 43: '%s < %s' % (a, b),
        44: '%s > %s ? %s : %s' % (s(a), s(b), a, b), 45: '%s < %s ? %s : %s' % (s(a), s(b), a, b),
        4: '%s << (%s & 31)' % (b, a), 7: '(u32)(%s >> (%s & 31))' % (s(b), a),
        22: 'psp_clz(%s)' % a, 23: 'psp_clz(~%s)' % a, 16: 'p.hi', 18: 'p.lo',
    }
    if fn in t:
        return set_r(rd, t[fn])
    if fn == 0:
        return set_r(rd, '%s << %d' % (b, sa))
    if fn == 2:
        if rs == 1:
            return set_r(rd, 'psp_ror(%s, %d)' % (b, sa))
        if rs == 0:
            return set_r(rd, '%s >> %d' % (b, sa))
        raise Invalid()
    if fn == 3:
        return set_r(rd, '(u32)(%s >> %d)' % (s(b), sa))
    if fn == 6:
        return set_r(rd, ('psp_ror(%s, %s)' if sa == 1 else '%s >> (%s & 31)') % (b, a))
    if fn == 10:
        return 'if (%s == 0) { %s }' % (b, set_r(rd, a))
    if fn == 11:
        return 'if (%s != 0) { %s }' % (b, set_r(rd, a))
    if fn == 15:
        return ''
    if fn == 17:
        return 'p.hi = %s;' % a
    if fn == 19:
        return 'p.lo = %s;' % a
    if fn in (24, 25, 26, 27, 28, 29, 46, 47):
        q = '(s64)(s32)%s * (s32)%s' % (a, b)
        u = '(u64)%s * %s' % (a, b)
        return {24: '{ s64 m_ = %s; p.lo = (u32)m_; p.hi = (u32)(m_ >> 32); }' % q,
                25: '{ u64 m_ = %s; p.lo = (u32)m_; p.hi = (u32)(m_ >> 32); }' % u,
                26: 'psp_div((s32)%s, (s32)%s);' % (a, b), 27: 'psp_divu(%s, %s);' % (a, b),
                28: 'psp_acc(%s);' % q, 29: 'psp_acc((s64)(%s));' % u,
                46: 'psp_acc(-(%s));' % q, 47: 'psp_acc(-(s64)(%s));' % u}[fn]
    raise Invalid()


def cop1(pc, inst):
    rs, rt, rd, fn = (inst >> 21) & 31, (inst >> 16) & 31, (inst >> 11) & 31, inst & 63
    fs, ft, fd = rd, rt, (inst >> 6) & 31
    S = lambda n: 'p.f[%d].s' % n
    if rs == 0: return set_r(rt, 'p.f[%d].w' % fs)
    if rs == 2: return set_r(rt, 'p.fcr31' if rd == 31 else '0x3351u' if rd == 0 else '0')
    if rs == 4: return 'p.f[%d].w = %s;' % (fs, R(rt))
    if rs == 6: return 'p.fcr31 = %s;' % R(rt) if rd == 31 else ''
    if rs == 20:
        if fn == 32:
            return '%s = (float)p.f[%d].i;' % (S(fd), fs)
        raise Invalid()
    if rs != 16:
        raise Invalid()
    if fn < 4:
        return '%s = %s %s %s;' % (S(fd), S(fs), '+-*/'[fn], S(ft))
    u = {4: 'sqrtf(%s)', 5: 'fabsf(%s)', 6: '%s', 7: '-%s'}
    if fn in u:
        return '%s = %s;' % (S(fd), u[fn] % S(fs))
    c = {12: 'roundf(rintf(%s))', 13: 'truncf(%s)', 14: 'ceilf(%s)', 15: 'floorf(%s)', 36: 'rintf(%s)'}
    if fn in c:
        return 'p.f[%d].i = psp_f2i(%s);' % (fd, c[fn] % S(fs))
    if fn >= 48:
        a, b = S(fs), S(ft)
        parts = ['(%s != %s || %s != %s)' % (a, a, b, b)] if fn & 1 else []
        if fn & 2: parts.append('%s == %s' % (a, b))
        if fn & 4: parts.append('%s < %s' % (a, b))
        return 'p.fcr31 = (p.fcr31 & ~(1u << 23)) | (u32)(%s) << 23;' % (' || '.join(parts) or '0')
    raise Invalid()


def alu(pc, inst):
    """C for one instruction that isn't a branch."""
    op, rs, rt = inst >> 26, (inst >> 21) & 31, (inst >> 16) & 31
    imm, simm = inst & 0xFFFF, sx(inst & 0xFFFF)
    a = R(rs)
    if op == 0:
        fn = inst & 63
        if fn == 12:
            raise Invalid()               # the game's own syscalls come through import stubs
        if fn == 13:
            return 'return psp_trap(0x%08Xu, 0x%08Xu);' % (pc, inst)
        return special(inst)
    if op == 8 or op == 9: return set_r(rt, '%s + (u32)%d' % (a, simm))
    if op == 10: return set_r(rt, '(s32)%s < %d' % (a, simm))
    if op == 11: return set_r(rt, '%s < (u32)%d' % (a, simm))
    if op == 12: return set_r(rt, '%s & %du' % (a, imm))
    if op == 13: return set_r(rt, '%s | %du' % (a, imm))
    if op == 14: return set_r(rt, '%s ^ %du' % (a, imm))
    if op == 15: return set_r(rt, '0x%08Xu' % (imm << 16))
    if op == 17: return cop1(pc, inst)
    if op == 31:
        rd, sa, fn = (inst >> 11) & 31, (inst >> 6) & 31, inst & 63
        b = R(rt)
        if fn == 0:                       # ext
            return set_r(rt, '(%s >> %d) & 0x%Xu' % (a, sa, (1 << (rd + 1)) - 1))
        if fn == 4:                       # ins
            m = ((1 << (rd - sa + 1)) - 1) << sa
            return set_r(rt, '(%s & ~0x%Xu) | ((%s << %d) & 0x%Xu)' % (b, m & 0xFFFFFFFF, a, sa, m & 0xFFFFFFFF))
        if fn == 32:
            e = {16: '(u32)(s32)(s8)%s', 24: '(u32)(s32)(s16)%s', 20: 'psp_bitrev(%s)'}.get(sa)
            if sa == 2: e = 'psp_ror(__builtin_bswap32(%s), 16)'
            if sa == 3: e = '__builtin_bswap32(%s)'
            if e:
                return set_r(rd, e % b)
        raise Invalid()
    ea = '%s + (u32)%d' % (a, simm)
    addr = '{ u32 a_ = %s; ' % ea
    m = {32: '(u32)(s32)(s8)RD8(a_)', 33: '(u32)(s32)(s16)RD16(a_)', 35: 'RD32(a_)', 36: 'RD8(a_)', 37: 'RD16(a_)',
         34: 'psp_lwl(%s, a_)' % R(rt), 38: 'psp_lwr(%s, a_)' % R(rt), 48: 'RD32(a_)'}
    if op in m:
        return addr + set_r(rt, m[op]) + ' }'
    w = {40: 'WR8(a_, (u8)%s);', 41: 'WR16(a_, (u16)%s);', 43: 'WR32(a_, %s);', 42: 'psp_swl(%s, a_);', 46: 'psp_swr(%s, a_);'}
    if op in w:
        return addr + w[op] % R(rt) + ' }'
    if op == 56:                          # sc: one CPU, threads switch only at system calls
        return addr + 'WR32(a_, %s); %s }' % (R(rt), set_r(rt, '1'))
    if op == 47: return ''                # cache
    if op == 63 and inst >> 16 == 0xFFFF: return ''   # vnop, vflush, vsync
    if op == 49: return addr + 'p.f[%d].w = RD32(a_); }' % rt
    if op == 57: return addr + 'WR32(a_, p.f[%d].w); }' % rt
    if op in (50, 54, 58, 62, 53, 61):
        return 'psp_vfpu_load(0x%08Xu);' % inst
    if op in (24, 25, 27, 52, 55, 60, 18):
        return 'psp_vfpu(0x%08Xu);' % inst
    raise Invalid()


def branch_of(pc, inst):
    op, rs, rt = inst >> 26, (inst >> 21) & 31, (inst >> 16) & 31
    t = (pc + 4 + (sx(inst & 0xFFFF) << 2)) & 0xFFFFFFFF
    a, b = R(rs), R(rt)
    if op == 0 and inst & 63 == 8:
        return dict(reg=rs, link=None)
    if op == 0 and inst & 63 == 9:
        return dict(reg=rs, link=(inst >> 11) & 31)
    if op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19):
        c = '(s32)%s %s 0' % (a, '<' if not rt & 1 else '>=')
        return dict(c=c, likely=bool(rt & 2), link=31 if rt & 16 else None, t=t)
    if op in (2, 3):
        return dict(c='1', likely=False, link=31 if op == 3 else None, t=((pc + 4) & 0xF0000000) | ((inst & 0x03FFFFFF) << 2))
    tbl = {4: '%s == %s' % (a, b), 5: '%s != %s' % (a, b), 6: '(s32)%s <= 0' % a, 7: '(s32)%s > 0' % a}
    if op in tbl: return dict(c=tbl[op], likely=False, link=None, t=t)
    if op - 16 in tbl and 20 <= op <= 23: return dict(c=tbl[op - 16], likely=True, link=None, t=t)
    if op == 17 and rs == 8:
        return dict(c='((p.fcr31 >> 23) & 1) == %d' % (rt & 1), likely=bool(rt & 2), link=None, t=t)
    if op == 18 and rs == 8:
        cc = (inst >> 18) & 7
        return dict(c='((p.vcc >> %d) & 1) == %d' % (cc, rt & 1), likely=bool(rt & 2), link=None, t=t)
    return None


class Program:
    def __init__(self, path):
        d = open(path, 'rb').read()
        if d[:4] != b'\x7fELF':
            sys.exit('Needs a decrypted ELF.')
        self.entry, phoff = struct.unpack_from('<II', d, 24)
        size, count = struct.unpack_from('<HH', d, 42)
        loads = [struct.unpack_from('<8I', d, phoff + i * size) for i in range(count)]
        loads = [l for l in loads if l[0] == 1]
        if struct.unpack_from('<H', d, 16)[0] != 2 or len(loads) != 1:
            sys.exit('Only a single-segment, fixed-address program so far.')
        _, off, self.base, _, fsz, self.memsz, _, _ = loads[0]
        self.image = d[off:off + fsz]
        self.end = self.base + fsz
        self.words = struct.unpack('<%dI' % (fsz // 4), self.image[:fsz & ~3])
        self.imports = self.find_imports()

    def word(self, pc):
        return self.words[(pc - self.base) >> 2]

    def inside(self, pc):
        return self.base <= pc < self.end - 4 and not pc & 3

    def text(self, va):
        o = va - self.base
        return self.image[o:self.image.index(b'\0', o)].decode('latin1')

    def find_imports(self):
        out = {}
        for i in range(len(self.words) - 5):
            nm, _, e, nids, stubs = self.words[i:i + 5]
            if e & 0xFF != 5 or e >> 16 == 0 or e >> 16 > 999 or not all(self.inside(x & ~3) for x in (nm, nids, stubs)):
                continue
            name = self.text(nm)
            if len(name) > 3 and name.isascii() and name.replace('_', '').isalnum():
                for k in range(e >> 16):
                    out[stubs + 8 * k] = (name, self.word(nids + 4 * k))
        return out


class Recompiler:
    def __init__(self, prog):
        self.prog = prog
        self.blocks = {}
        self.todo = []
        self.covered = set()
        self.bad = collections.Counter()

    def seed(self, pc):
        if pc not in self.blocks and self.prog.inside(pc) and pc not in self.prog.imports:
            self.blocks[pc] = None
            self.todo.append(pc)

    def block(self, entry):
        prog, lines, pc = self.prog, [], entry
        seen = []
        while prog.inside(pc):
            if pc in prog.imports:
                lines.append('return 0x%08Xu;' % pc)
                return lines, seen
            inst = prog.word(pc)
            br = branch_of(pc, inst)
            if not br:
                try:
                    code = alu(pc, inst)
                except Invalid:
                    self.bad[inst >> 26] += 1
                    return None, seen
                seen.append(pc)
                if code:
                    lines.append(code)
                if 'return' in code:
                    return lines, seen
                pc += 4
                continue
            slot = prog.word(pc + 4)
            if branch_of(pc + 4, slot):
                return None, seen             # a branch in a delay slot is data, or a trick worth seeing
            try:
                code = alu(pc + 4, slot)
            except Invalid:
                self.bad[slot >> 26] += 1
                return None, seen
            seen += [pc, pc + 4]
            link = br.get('link')
            pre = '%s ' % set_r(link, '0x%08Xu' % (pc + 8)) if link else ''
            if 'reg' in br:
                lines.append('{ u32 t_ = %s; %s%s return t_; }' % (R(br['reg']), pre, code))
                if link:
                    self.seed(pc + 8)
                return lines, seen
            t, after = br['t'], pc + 8
            self.seed(t)
            if br['c'] == '1':
                lines.append('{ %s%s return 0x%08Xu; }' % (pre, code, t))
                if link:
                    self.seed(after)
            elif br['likely']:
                lines.append('{ %sif (!(%s)) return 0x%08Xu; %s return 0x%08Xu; }' % (pre, br['c'], after, code, t))
                self.seed(after)
            else:
                lines.append('{ %sint c_ = %s; %s return c_ ? 0x%08Xu : 0x%08Xu; }' % (pre, br['c'], code, t, after))
                self.seed(after)
            return lines, seen
        return None, seen

    def run(self):
        while self.todo:
            pc = self.todo.pop()
            lines, seen = self.block(pc)
            if lines is not None:
                self.blocks[pc] = lines
                self.covered.update(seen)


def main():
    a = sys.argv[1:]
    prog = Program(a[0])
    out = a[1]
    r = Recompiler(prog)
    r.seed(prog.entry)
    direct = 0
    for w in prog.words:                  # every value that could be a code address
        if prog.inside(w):
            r.seed(w)
            direct += 1
    hi = {}
    for i, w in enumerate(prog.words):    # lui/addiu pairs building an address
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 15:
            hi[rt] = (w & 0xFFFF) << 16
        elif op in (9, 13) and rs in hi:
            pc = (hi[rs] + sx(w & 0xFFFF) if op == 9 else hi[rs] | (w & 0xFFFF)) & 0xFFFFFFFF
            r.seed(pc)
    if '--extra' in a:
        for l in open(a[a.index('--extra') + 1]):
            r.seed(int(l, 16))
    r.run()
    blocks = sorted((pc, b) for pc, b in r.blocks.items() if b)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.startswith('recomp_'):
            os.remove(os.path.join(out, f))
    parts = range(0, len(blocks), BLOCKS_PER_FILE)
    for n in parts:
        with open(os.path.join(out, 'recomp_%03d.c' % (n // BLOCKS_PER_FILE)), 'w') as f:
            f.write('/* GENERATED by tools/recompile_psp.py - do not edit */\n#include "allegrex.h"\n\n')
            for pc, b in blocks[n:n + BLOCKS_PER_FILE]:
                f.write('static u32 B%08X(void) {\n    %s\n    return 0x%08Xu;\n}\n' % (pc, '\n    '.join(b), 0))
            f.write('void psp_register_%03d(void) {\n' % (n // BLOCKS_PER_FILE))
            for pc, _ in blocks[n:n + BLOCKS_PER_FILE]:
                f.write('    psp_block_add(0x%08Xu, B%08X);\n' % (pc, pc))
            f.write('}\n')
    with open(os.path.join(out, 'recomp_register.c'), 'w') as f:
        f.write('/* GENERATED by tools/recompile_psp.py - do not edit */\n#include "allegrex.h"\n\n')
        for n in parts:
            f.write('void psp_register_%03d(void);\n' % (n // BLOCKS_PER_FILE))
        f.write('void psp_register_game(void) {\n')
        for n in parts:
            f.write('    psp_register_%03d();\n' % (n // BLOCKS_PER_FILE))
        for pc, (mod, nid) in sorted(prog.imports.items()):
            f.write('    psp_import_add(0x%08Xu, "%s", 0x%08Xu);\n' % (pc, mod, nid))
        f.write('}\n')
    total = len(prog.words)
    print('entry %08X, %d words, %d imports' % (prog.entry, total, len(prog.imports)))
    print('%d blocks, %d instructions reached (%.1f%% of the program, the rest is data or unreached)' % (
        len(blocks), len(r.covered), 100 * len(r.covered) / total))
    print('paths stopped by undecoded words:', sum(r.bad.values()), 'top opcodes', r.bad.most_common(8))


if __name__ == '__main__':
    main()

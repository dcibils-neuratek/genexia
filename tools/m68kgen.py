#!/usr/bin/env python3
"""
m68kgen.py - Motorola 68000 decoder + C emitter.

A single description of every 68000 instruction, used in two modes:

  * interp : emits one C handler per opcode for the fallback interpreter.
             Register numbers / small immediates are read from cpu.ir at
             run time so identical handlers are de-duplicated, extension
             words are fetched with fetch16().
  * recomp : emits straight-line C for one concrete instruction at a known
             ROM address. Extension words, PC-relative addresses, branch
             targets and register numbers become constants.

Ctx collects the emitted statements and control-flow facts (flow kind,
static targets) that the recompiler uses for code discovery.
"""

MASK = {1: '0xFFu', 2: '0xFFFFu', 4: '0xFFFFFFFFu'}
CT = {1: 'u8', 2: 'u16', 4: 'u32'}
SFX = {1: '.b', 2: '.w', 4: '.l'}
CC = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']
CCEXPR = [
    '1', '0',
    '(!cpu.c && !cpu.z)', '(cpu.c || cpu.z)',
    '(!cpu.c)', '(cpu.c)',
    '(!cpu.z)', '(cpu.z)',
    '(!cpu.v)', '(cpu.v)',
    '(!cpu.n)', '(cpu.n)',
    '(cpu.n == cpu.v)', '(cpu.n != cpu.v)',
    '(!cpu.z && cpu.n == cpu.v)', '(cpu.z || cpu.n != cpu.v)',
]


class Illegal(Exception):
    pass


class Fld:
    """A bit field of the opcode word."""
    def __init__(s, op, shift, mask):
        s.v = (op >> shift) & mask
        s.shift, s.mask = shift, mask


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


class Ctx:
    def __init__(s, mode, op, rom=None, addr=0):
        s.mode = mode
        s.op = op
        s.rom = rom
        s.addr = addr            # instruction address (recomp)
        s.pc = addr + 2          # next extension word address (recomp)
        s.out = []
        s.ntmp = 0
        s.cycles = 0
        s.dyn_cycles = False
        s.mn = ''
        s.ops = []
        # control flow facts for the recompiler
        s.flow = None            # None | 'jump' | 'branch' | 'call' | 'ret' | 'indirect' | 'stop' | 'exc' | 'sr'
        s.targets = []           # static successor targets
        s.dyn_targets = []       # targets computed at run time (indirect)

    # -- plumbing ----------------------------------------------------------
    @property
    def rc(s):
        return s.mode == 'recomp'

    def emit(s, line):
        s.out.append(line)

    def tmp(s, p='t'):
        s.ntmp += 1
        return '%s%d' % (p, s.ntmp)

    def let(s, ctype, expr, p='t'):
        t = s.tmp(p)
        s.emit('%s %s = %s;' % (ctype, t, expr))
        return t

    def f(s, fld):
        if s.rc:
            return str(fld.v)
        if fld.shift == 0:
            return '(cpu.ir & %d)' % fld.mask
        return '((cpu.ir >> %d) & %d)' % (fld.shift, fld.mask)

    def rom16(s, a):
        if a + 1 >= len(s.rom):
            return 0
        return s.rom[a] << 8 | s.rom[a + 1]

    def ext16(s):
        """returns (value_expr, address_expr, python_value_or_None)"""
        if s.rc:
            w = s.rom16(s.pc)
            a = s.pc
            s.pc += 2
            return ('0x%04Xu' % w, '0x%06Xu' % a, w)
        p = s.tmp('p')
        w = s.tmp('w')
        s.emit('u32 %s = cpu.pc; u32 %s = fetch16(); (void)%s;' % (p, w, p))
        return (w, p, None)

    def next_pc(s):
        return '0x%06Xu' % s.pc if s.rc else 'cpu.pc'

    def this_pc(s):
        return '0x%06Xu' % s.addr if s.rc else 'cpu.ppc'

    def set_pc(s, expr):
        s.emit('cpu.pc = %s;' % expr)

    def sync_pc(s):
        """make cpu.pc hold the next instruction address (recomp only needed)"""
        if s.rc:
            s.set_pc(s.next_pc())

    def opnd(s, text):
        s.ops.append(text)

    def cyc(s, n):
        s.cycles += n

    def exception(s, vec, ret_expr):
        s.emit('m68k_exception(%d, %s);' % (vec, ret_expr))

    def disasm(s):
        return (s.mn + ' ' + ','.join(s.ops)).strip()


# ---------------------------------------------------------------- EAs
EA_KINDS = ['dn', 'an', 'ind', 'postinc', 'predec', 'd16', 'd8x']
EA7 = ['absw', 'absl', 'pcd16', 'pcd8x', 'imm']

ALL = {'dn', 'an', 'ind', 'postinc', 'predec', 'd16', 'd8x', 'absw', 'absl', 'pcd16', 'pcd8x', 'imm'}
DATA = ALL - {'an'}
MEM = ALL - {'dn', 'an'}
CTRL = {'ind', 'd16', 'd8x', 'absw', 'absl', 'pcd16', 'pcd8x'}
ALT = {'dn', 'an', 'ind', 'postinc', 'predec', 'd16', 'd8x', 'absw', 'absl'}
DALT = DATA & ALT
MALT = MEM & ALT
CALT = CTRL & ALT

# EA calculation time (byte/word, long)
EA_TIME = {'dn': (0, 0), 'an': (0, 0), 'ind': (4, 8), 'postinc': (4, 8), 'predec': (6, 10),
           'd16': (8, 12), 'd8x': (10, 14), 'absw': (8, 12), 'absl': (12, 16),
           'pcd16': (8, 12), 'pcd8x': (10, 14), 'imm': (4, 8)}


class EA:
    def __init__(s, op, mode_shift, reg_shift):
        s.mode = (op >> mode_shift) & 7
        s.reg = Fld(op, reg_shift, 7)
        if s.mode < 7:
            s.kind = EA_KINDS[s.mode]
        else:
            if s.reg.v > 4:
                raise Illegal()
            s.kind = EA7[s.reg.v]

    def check(s, allowed):
        if s.kind not in allowed:
            raise Illegal()
        return s

    def time(s, sz):
        return EA_TIME[s.kind][1 if sz == 4 else 0]


def mkea(op, mode_shift=3, reg_shift=0, allowed=ALL):
    return EA(op, mode_shift, reg_shift).check(allowed)


def ea_addr(c, ea, sz):
    """emit address computation for memory EA; returns name of a u32 temp"""
    k = ea.kind
    r = c.f(ea.reg)
    rv = ea.reg.v
    if k == 'ind':
        c.opnd('(a%s)' % rv)
        return c.let('u32', 'cpu.a[%s]' % r, 'ea')
    if k == 'postinc':
        c.opnd('(a%s)+' % rv)
        t = c.let('u32', 'cpu.a[%s]' % r, 'ea')
        inc = 'BINC(%s)' % r if sz == 1 else str(sz)
        c.emit('cpu.a[%s] += %s;' % (r, inc))
        return t
    if k == 'predec':
        c.opnd('-(a%s)' % rv)
        dec = 'BINC(%s)' % r if sz == 1 else str(sz)
        c.emit('cpu.a[%s] -= %s;' % (r, dec))
        return c.let('u32', 'cpu.a[%s]' % r, 'ea')
    if k == 'd16':
        w, _, wv = c.ext16()
        if c.rc:
            c.opnd('%s(a%d)' % (hex16s(wv), rv))
        return c.let('u32', 'cpu.a[%s] + SX16(%s)' % (r, w), 'ea')
    if k == 'd8x':
        w, _, wv = c.ext16()
        if c.rc:
            c.opnd('%s(a%d,%s)' % (hex8s(wv), rv, idxname(wv)))
            return c.let('u32', 'cpu.a[%s] + %s' % (r, idx_static(wv)), 'ea')
        return c.let('u32', 'cpu.a[%s] + m68k_index(%s)' % (r, w), 'ea')
    if k == 'absw':
        w, _, wv = c.ext16()
        if c.rc:
            a = s16(wv) & 0xFFFFFFFF
            c.opnd('($%X).w' % (a & 0xFFFFFF))
            return c.let('u32', '0x%08Xu' % a, 'ea')
        return c.let('u32', 'SX16(%s)' % w, 'ea')
    if k == 'absl':
        hi, _, hv = c.ext16()
        lo, _, lv = c.ext16()
        if c.rc:
            a = hv << 16 | lv
            c.opnd('($%X).l' % a)
            return c.let('u32', '0x%08Xu' % a, 'ea')
        return c.let('u32', '(%s << 16) | %s' % (hi, lo), 'ea')
    if k == 'pcd16':
        w, p, wv = c.ext16()
        if c.rc:
            a = (c.pc - 2 + s16(wv)) & 0xFFFFFFFF
            c.opnd('$%X(pc)' % a)
            c.pcref = a
            return c.let('u32', '0x%08Xu' % a, 'ea')
        return c.let('u32', '%s + SX16(%s)' % (p, w), 'ea')
    if k == 'pcd8x':
        w, p, wv = c.ext16()
        if c.rc:
            base = c.pc - 2
            c.opnd('%s(pc,%s)' % (hex8s(wv), idxname(wv)))
            c.pcindex = (base + s8(wv)) & 0xFFFFFFFF
            return c.let('u32', '0x%08Xu + %s' % (base, idx_static(wv)), 'ea')
        return c.let('u32', '%s + m68k_index(%s)' % (p, w), 'ea')
    raise Illegal()


def hex16s(v):
    v = s16(v)
    return '-$%X' % -v if v < 0 else '$%X' % v


def hex8s(v):
    v = s8(v)
    return '-$%X' % -v if v < 0 else '$%X' % v


def idxname(w):
    return '%s%d.%s' % ('a' if w & 0x8000 else 'd', (w >> 12) & 7, 'l' if w & 0x800 else 'w')


def idx_static(w):
    r = (w >> 12) & 7
    x = 'cpu.%s[%d]' % ('a' if w & 0x8000 else 'd', r)
    if not w & 0x800:
        x = 'SX16(%s)' % x
    d = s8(w)
    if d:
        return '%s + (u32)%d' % (x, d)
    return x


def ea_read(c, ea, sz):
    """emit read of EA; returns name of u32 temp (zero-extended to size)"""
    k = ea.kind
    r = c.f(ea.reg)
    if k == 'dn':
        c.opnd('d%d' % ea.reg.v)
        return c.let('u32', '(%s)cpu.d[%s]' % (CT[sz], r), 's')
    if k == 'an':
        c.opnd('a%d' % ea.reg.v)
        return c.let('u32', '(%s)cpu.a[%s]' % (CT[sz], r), 's')
    if k == 'imm':
        if sz == 4:
            hi, _, hv = c.ext16()
            lo, _, lv = c.ext16()
            if c.rc:
                c.opnd('#$%X' % (hv << 16 | lv))
                return c.let('u32', '0x%08Xu' % (hv << 16 | lv), 's')
            return c.let('u32', '(%s << 16) | %s' % (hi, lo), 's')
        w, _, wv = c.ext16()
        if c.rc:
            v = wv & (0xFF if sz == 1 else 0xFFFF)
            c.opnd('#$%X' % v)
            return c.let('u32', '0x%Xu' % v, 's')
        return c.let('u32', '%s & %s' % (w, MASK[sz]), 's')
    a = ea_addr(c, ea, sz)
    return c.let('u32', 'rd%d(%s)' % (sz * 8, a), 's')


def reg_write(c, kind, rexpr, sz, v):
    if kind == 'dn':
        c.emit('SETD%d(%s, %s);' % (sz * 8, rexpr, v))
    else:
        c.emit('cpu.a[%s] = %s;' % (rexpr, v))


class Loc:
    """resolved read-modify-write location"""
    def __init__(s, c, ea, sz, read=True):
        s.c, s.ea, s.sz = c, ea, sz
        s.r = c.f(ea.reg)
        if ea.kind == 'dn':
            c.opnd('d%d' % ea.reg.v)
            s.val = c.let('u32', '(%s)cpu.d[%s]' % (CT[sz], s.r), 'd') if read else None
        elif ea.kind == 'an':
            c.opnd('a%d' % ea.reg.v)
            s.val = c.let('u32', 'cpu.a[%s]' % s.r, 'd') if read else None
        else:
            s.addr = ea_addr(c, ea, sz)
            s.val = c.let('u32', 'rd%d(%s)' % (sz * 8, s.addr), 'd') if read else None

    def write(s, v):
        if s.ea.kind in ('dn', 'an'):
            reg_write(s.c, s.ea.kind, s.r, s.sz, v)
        else:
            s.c.emit('wr%d(%s, %s);' % (s.sz * 8, s.addr, v))


def size2(bits):
    """standard size field 00/01/10"""
    if bits == 3:
        raise Illegal()
    return [1, 2, 4][bits]


# ======================================================== instructions

def gen(c):
    """decode c.op and emit C for it into c. Raises Illegal."""
    op = c.op
    line = op >> 12
    c.pcref = None
    c.pcindex = None
    return LINES[line](c, op)


# ---- line 0: immediate ops, bit ops, movep
def line0(c, op):
    if op & 0x0100:
        if (op >> 3) & 7 == 1:
            return movep(c, op)
        return bitop(c, op, dynamic=True)
    sub = (op >> 9) & 7
    if sub == 4:
        return bitop(c, op, dynamic=False)
    if sub == 7:
        raise Illegal()
    names = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}
    if sub not in names:
        raise Illegal()
    name = names[sub]
    szb = (op >> 6) & 3
    # to CCR / SR
    if (op & 0x3F) == 0x3C and sub in (0, 1, 5):
        if szb == 0:
            c.mn = name
            w, _, wv = c.ext16()
            if c.rc:
                c.opnd('#$%X' % (wv & 0xFF))
            c.opnd('ccr')
            o = {0: '|', 1: '&', 5: '^'}[sub]
            c.emit('set_ccr(get_ccr() %s (%s & 0xFF));' % (o, w))
            c.cyc(20)
            return
        if szb == 1:
            c.mn = name
            w, _, wv = c.ext16()
            if c.rc:
                c.opnd('#$%X' % wv)
            c.opnd('sr')
            privileged(c)
            o = {0: '|', 1: '&', 5: '^'}[sub]
            c.emit('set_sr(get_sr() %s %s);' % (o, w))
            c.cyc(20)
            c.flow = 'sr'
            return
        raise Illegal()
    sz = size2(szb)
    c.mn = name + SFX[sz]
    # immediate source
    if sz == 4:
        hi, _, hv = c.ext16()
        lo, _, lv = c.ext16()
        imm = '0x%08Xu' % (hv << 16 | lv) if c.rc else '((%s << 16) | %s)' % (hi, lo)
        if c.rc:
            c.opnd('#$%X' % (hv << 16 | lv))
    else:
        w, _, wv = c.ext16()
        imm = ('0x%Xu' % (wv & (0xFF if sz == 1 else 0xFFFF))) if c.rc else '(%s & %s)' % (w, MASK[sz])
        if c.rc:
            c.opnd('#$%X' % (wv & (0xFF if sz == 1 else 0xFFFF)))
    s = c.let('u32', imm, 'i')
    if name == 'cmpi':
        ea = mkea(op, allowed=DALT)
        d = ea_read(c, ea, sz)
        c.emit('f_cmp(%d, %s, %s);' % (sz, s, d))
        c.cyc((14 if ea.kind == 'dn' else 12 + ea.time(sz)) if sz == 4 else (8 if ea.kind == 'dn' else 8 + ea.time(sz)))
        return
    ea = mkea(op, allowed=DALT)
    loc = Loc(c, ea, sz)
    expr = {
        'ori': 'f_logic(%d, %s | %s)' % (sz, loc.val, s),
        'andi': 'f_logic(%d, %s & %s)' % (sz, loc.val, s),
        'eori': 'f_logic(%d, %s ^ %s)' % (sz, loc.val, s),
        'addi': 'f_add(%d, %s, %s)' % (sz, s, loc.val),
        'subi': 'f_sub(%d, %s, %s)' % (sz, s, loc.val),
    }[name]
    loc.write(c.let('u32', expr, 'r'))
    if ea.kind == 'dn':
        c.cyc(16 if sz == 4 else 8)
    else:
        c.cyc((20 if sz == 4 else 12) + ea.time(sz))


def privileged(c):
    c.emit('if (!cpu.s) { m68k_exception(8, %s); %s }' % (c.this_pc(), 'return;'))


def bitop(c, op, dynamic):
    kind = (op >> 6) & 3
    name = ['btst', 'bchg', 'bclr', 'bset'][kind]
    c.mn = name
    if dynamic:
        rn = Fld(op, 9, 7)
        c.opnd('d%d' % rn.v)
        bit = c.let('u32', 'cpu.d[%s]' % c.f(rn), 'b')
    else:
        w, _, wv = c.ext16()
        if c.rc:
            c.opnd('#%d' % (wv & 0xFF))
        bit = c.let('u32', '%s' % w, 'b')
    allowed = DATA if kind == 0 else DALT
    if not dynamic and kind == 0:
        allowed = DATA - {'imm'}
    ea = mkea(op, allowed=allowed)
    sz = 4 if ea.kind == 'dn' else 1
    if ea.kind == 'imm':
        val = ea_read(c, ea, 1)
        loc = None
    else:
        loc = Loc(c, ea, sz)
        val = loc.val
    m = c.let('u32', '1u << (%s & %d)' % (bit, 31 if sz == 4 else 7), 'm')
    c.emit('cpu.z = (%s & %s) == 0;' % (val, m))
    if kind == 1:
        loc.write('%s ^ %s' % (val, m))
    elif kind == 2:
        loc.write('%s & ~%s' % (val, m))
    elif kind == 3:
        loc.write('%s | %s' % (val, m))
    base = {0: 4, 1: 8, 2: 10, 3: 8}[kind] if dynamic else {0: 8, 1: 12, 2: 14, 3: 12}[kind]
    if sz == 4:
        c.cyc(base + 2)
    else:
        c.cyc(base + ea.time(1))


def movep(c, op):
    dr = Fld(op, 9, 7)
    ar = Fld(op, 0, 7)
    mode = (op >> 6) & 3
    sz = 4 if mode & 1 else 2
    c.mn = 'movep' + SFX[sz]
    w, _, wv = c.ext16()
    a = c.let('u32', 'cpu.a[%s] + SX16(%s)' % (c.f(ar), w), 'ea')
    dn = c.f(dr)
    if mode < 2:
        if c.rc:
            c.opnd('%s(a%d)' % (hex16s(wv), ar.v))
            c.opnd('d%d' % dr.v)
        if sz == 2:
            c.emit('SETD16(%s, rd8(%s) << 8 | rd8(%s + 2));' % (dn, a, a))
        else:
            c.emit('cpu.d[%s] = rd8(%s) << 24 | rd8(%s + 2) << 16 | rd8(%s + 4) << 8 | rd8(%s + 6);' % (dn, a, a, a, a))
    else:
        if c.rc:
            c.opnd('d%d' % dr.v)
            c.opnd('%s(a%d)' % (hex16s(wv), ar.v))
        v = c.let('u32', 'cpu.d[%s]' % dn, 'v')
        if sz == 2:
            c.emit('wr8(%s, %s >> 8); wr8(%s + 2, %s);' % (a, v, a, v))
        else:
            c.emit('wr8(%s, %s >> 24); wr8(%s + 2, %s >> 16); wr8(%s + 4, %s >> 8); wr8(%s + 6, %s);' % (a, v, a, v, a, v, a, v))
    c.cyc(24 if sz == 4 else 16)


# ---- lines 1-3: move
def move(c, op):
    sz = {1: 1, 3: 2, 2: 4}[op >> 12]
    src = mkea(op, 3, 0, ALL if sz != 1 else DATA)
    dmode = (op >> 6) & 7
    if dmode == 1:
        if sz == 1:
            raise Illegal()
        c.mn = 'movea' + SFX[sz]
        v = ea_read(c, src, sz)
        ar = Fld(op, 9, 7)
        c.opnd('a%d' % ar.v)
        c.emit('cpu.a[%s] = %s;' % (c.f(ar), 'SX16(%s)' % v if sz == 2 else v))
        c.cyc(4 + src.time(sz))
        return
    dst = mkea(op, 6, 9, DALT)
    c.mn = 'move' + SFX[sz]
    v = ea_read(c, src, sz)
    c.emit('f_logic(%d, %s);' % (sz, v))
    if dst.kind == 'dn':
        c.opnd('d%d' % dst.reg.v)
        reg_write(c, 'dn', c.f(dst.reg), sz, v)
    else:
        a = ea_addr(c, dst, sz)
        c.emit('wr%d(%s, %s);' % (sz * 8, a, v))
    dt = dst.time(sz)
    if dst.kind == 'predec':
        dt = 8 if sz == 4 else 4
    c.cyc(4 + src.time(sz) + dt)


# ---- line 4: misc
def line4(c, op):
    if op & 0x0100:
        if (op >> 6) & 7 == 7:
            # LEA
            ea = mkea(op, allowed=CTRL)
            ar = Fld(op, 9, 7)
            c.mn = 'lea'
            a = ea_addr(c, ea, 4)
            c.opnd('a%d' % ar.v)
            c.emit('cpu.a[%s] = %s;' % (c.f(ar), a))
            c.cyc({'ind': 4, 'd16': 8, 'd8x': 12, 'absw': 8, 'absl': 12, 'pcd16': 8, 'pcd8x': 12}[ea.kind])
            return
        if (op >> 6) & 7 == 6:
            # CHK.W
            ea = mkea(op, allowed=DATA)
            dr = Fld(op, 9, 7)
            c.mn = 'chk.w'
            s = ea_read(c, ea, 2)
            c.opnd('d%d' % dr.v)
            d = c.let('s32', '(s16)cpu.d[%s]' % c.f(dr), 'v')
            c.cyc(10 + ea.time(2))
            c.emit('if (%s < 0 || %s > (s16)%s) { cpu.n = %s < 0; m68k_exception(6, %s); cpu.cyc += %d; return; }'
                   % (d, d, s, d, c.next_pc(), c.cycles + 30))
            return
        raise Illegal()
    sub = (op >> 8) & 0xF
    szb = (op >> 6) & 3
    if sub == 0x0:
        if szb == 3:
            c.mn = 'move'
            c.opnd('sr')
            ea = mkea(op, allowed=DALT)
            loc = Loc(c, ea, 2, read=False)
            loc.write('get_sr()')
            c.cyc(6 if ea.kind == 'dn' else 8 + ea.time(2))
            return
        sz = size2(szb)
        c.mn = 'negx' + SFX[sz]
        ea = mkea(op, allowed=DALT)
        loc = Loc(c, ea, sz)
        loc.write(c.let('u32', 'f_subx(%d, %s, 0)' % (sz, loc.val), 'r'))
        c.cyc((6 if sz == 4 else 4) if ea.kind == 'dn' else (12 if sz == 4 else 8) + ea.time(sz))
        return
    if sub == 0x2:
        if szb == 3:
            raise Illegal()
        sz = size2(szb)
        c.mn = 'clr' + SFX[sz]
        ea = mkea(op, allowed=DALT)
        # 68000 CLR does a read before the write; harmless to skip except on IO
        loc = Loc(c, ea, sz, read=False)
        loc.write('0')
        c.emit('cpu.n = 0; cpu.z = 1; cpu.v = 0; cpu.c = 0;')
        c.cyc((6 if sz == 4 else 4) if ea.kind == 'dn' else (12 if sz == 4 else 8) + ea.time(sz))
        return
    if sub == 0x4:
        if szb == 3:
            c.mn = 'move'
            ea = mkea(op, allowed=DATA)
            v = ea_read(c, ea, 2)
            c.opnd('ccr')
            c.emit('set_ccr(%s);' % v)
            c.cyc(12 + ea.time(2))
            return
        sz = size2(szb)
        c.mn = 'neg' + SFX[sz]
        ea = mkea(op, allowed=DALT)
        loc = Loc(c, ea, sz)
        loc.write(c.let('u32', 'f_sub(%d, %s, 0)' % (sz, loc.val), 'r'))
        c.cyc((6 if sz == 4 else 4) if ea.kind == 'dn' else (12 if sz == 4 else 8) + ea.time(sz))
        return
    if sub == 0x6:
        if szb == 3:
            c.mn = 'move'
            ea = mkea(op, allowed=DATA)
            privileged(c)
            v = ea_read(c, ea, 2)
            c.opnd('sr')
            c.emit('set_sr(%s);' % v)
            c.cyc(12 + ea.time(2))
            c.flow = 'sr'
            return
        sz = size2(szb)
        c.mn = 'not' + SFX[sz]
        ea = mkea(op, allowed=DALT)
        loc = Loc(c, ea, sz)
        loc.write(c.let('u32', 'f_logic(%d, ~%s)' % (sz, loc.val), 'r'))
        c.cyc((6 if sz == 4 else 4) if ea.kind == 'dn' else (12 if sz == 4 else 8) + ea.time(sz))
        return
    if sub == 0x8:
        mode = (op >> 3) & 7
        if szb == 0:
            c.mn = 'nbcd'
            ea = mkea(op, allowed=DALT)
            loc = Loc(c, ea, 1)
            loc.write(c.let('u32', 'f_sbcd(%s, 0)' % loc.val, 'r'))
            c.cyc(6 if ea.kind == 'dn' else 8 + ea.time(1))
            return
        if szb == 1:
            if mode == 0:
                r = Fld(op, 0, 7)
                c.mn = 'swap'
                c.opnd('d%d' % r.v)
                rr = c.f(r)
                c.emit('cpu.d[%s] = f_logic(4, cpu.d[%s] >> 16 | cpu.d[%s] << 16);' % (rr, rr, rr))
                c.cyc(4)
                return
            c.mn = 'pea'
            ea = mkea(op, allowed=CTRL)
            a = ea_addr(c, ea, 4)
            c.emit('push32(%s);' % a)
            c.cyc({'ind': 12, 'd16': 16, 'd8x': 20, 'absw': 16, 'absl': 20, 'pcd16': 16, 'pcd8x': 20}[ea.kind])
            return
        if mode == 0:
            r = Fld(op, 0, 7)
            rr = c.f(r)
            c.opnd('d%d' % r.v)
            if szb == 2:
                c.mn = 'ext.w'
                c.emit('SETD16(%s, f_logic(2, SX8(cpu.d[%s])));' % (rr, rr))
            else:
                c.mn = 'ext.l'
                c.emit('cpu.d[%s] = f_logic(4, SX16(cpu.d[%s]));' % (rr, rr))
            c.cyc(4)
            return
        return movem(c, op, to_mem=True)
    if sub == 0xA:
        if op == 0x4AFC:
            c.mn = 'illegal'
            c.exception(4, c.this_pc())
            c.emit('cpu.cyc += 34; return;')
            c.flow = 'exc'
            return
        if szb == 3:
            c.mn = 'tas'
            ea = mkea(op, allowed=DALT)
            loc = Loc(c, ea, 1)
            c.emit('f_logic(1, %s);' % loc.val)
            loc.write('%s | 0x80' % loc.val)
            c.cyc(4 if ea.kind == 'dn' else 14 + ea.time(1))
            return
        sz = size2(szb)
        c.mn = 'tst' + SFX[sz]
        ea = mkea(op, allowed=DALT)
        v = ea_read(c, ea, sz)
        c.emit('f_logic(%d, %s);' % (sz, v))
        c.cyc(4 + ea.time(sz))
        return
    if sub == 0xC:
        if szb in (2, 3):
            return movem(c, op, to_mem=False)
        raise Illegal()
    if sub == 0xE:
        if szb == 1:
            low = op & 0x3F
            if low < 0x10:
                v = Fld(op, 0, 15)
                c.mn = 'trap'
                c.opnd('#%d' % v.v)
                c.emit('m68k_exception(32 + %s, %s);' % (c.f(v), c.next_pc()))
                c.emit('cpu.cyc += 34; return;')
                c.flow = 'exc'
                return
            if low < 0x18:
                r = Fld(op, 0, 7)
                rr = c.f(r)
                c.mn = 'link'
                w, _, wv = c.ext16()
                c.opnd('a%d' % r.v)
                if c.rc:
                    c.opnd('#%s' % hex16s(wv))
                c.emit('push32(cpu.a[%s]);' % rr)
                c.emit('cpu.a[%s] = cpu.a[7];' % rr)
                c.emit('cpu.a[7] += SX16(%s);' % w)
                c.cyc(16)
                return
            if low < 0x20:
                r = Fld(op, 0, 7)
                rr = c.f(r)
                c.mn = 'unlk'
                c.opnd('a%d' % r.v)
                c.emit('cpu.a[7] = cpu.a[%s];' % rr)
                c.emit('cpu.a[%s] = pop32();' % rr)
                c.cyc(12)
                return
            if low < 0x30:
                r = Fld(op, 0, 7)
                rr = c.f(r)
                c.mn = 'move'
                privileged(c)
                if low < 0x28:
                    c.opnd('a%d' % r.v)
                    c.opnd('usp')
                    c.emit('cpu.usp = cpu.a[%s];' % rr)
                else:
                    c.opnd('usp')
                    c.opnd('a%d' % r.v)
                    c.emit('cpu.a[%s] = cpu.usp;' % rr)
                c.cyc(4)
                return
            if op == 0x4E70:
                c.mn = 'reset'
                privileged(c)
                c.cyc(132)
                return
            if op == 0x4E71:
                c.mn = 'nop'
                c.cyc(4)
                return
            if op == 0x4E72:
                c.mn = 'stop'
                w, _, wv = c.ext16()
                if c.rc:
                    c.opnd('#$%X' % wv)
                privileged(c)
                c.emit('set_sr(%s); cpu.stopped = 1;' % w)
                c.sync_pc()
                c.cyc(4)
                c.flow = 'stop'
                return
            if op == 0x4E73:
                c.mn = 'rte'
                privileged(c)
                c.emit('{ u32 sr = pop16(); cpu.pc = pop32(); set_sr(sr); }')
                c.cyc(20)
                c.flow = 'ret'
                return
            if op == 0x4E75:
                c.mn = 'rts'
                c.emit('cpu.pc = pop32();')
                c.cyc(16)
                c.flow = 'ret'
                return
            if op == 0x4E76:
                c.mn = 'trapv'
                c.cyc(4)
                c.emit('if (cpu.v) { m68k_exception(7, %s); cpu.cyc += 34; return; }' % c.next_pc())
                return
            if op == 0x4E77:
                c.mn = 'rtr'
                c.emit('set_ccr(pop16()); cpu.pc = pop32();')
                c.cyc(20)
                c.flow = 'ret'
                return
            raise Illegal()
        if szb == 2:
            c.mn = 'jsr'
            ea = mkea(op, allowed=CTRL)
            a = ea_addr(c, ea, 4)
            c.emit('push32(%s);' % c.next_pc())
            c.set_pc(a)
            c.cyc({'ind': 16, 'd16': 18, 'd8x': 22, 'absw': 18, 'absl': 20, 'pcd16': 18, 'pcd8x': 22}[ea.kind])
            static_target(c, ea, 'call')
            return
        if szb == 3:
            c.mn = 'jmp'
            ea = mkea(op, allowed=CTRL)
            a = ea_addr(c, ea, 4)
            c.set_pc(a)
            c.cyc({'ind': 8, 'd16': 10, 'd8x': 14, 'absw': 10, 'absl': 12, 'pcd16': 10, 'pcd8x': 14}[ea.kind])
            static_target(c, ea, 'jump')
            return
    raise Illegal()


def static_target(c, ea, kind):
    """record static jump/call targets for code discovery"""
    if not c.rc:
        return
    t = None
    if ea.kind == 'absw':
        t = s16(c.rom16(c.pc - 2)) & 0xFFFFFF
    elif ea.kind == 'absl':
        t = (c.rom16(c.pc - 4) << 16 | c.rom16(c.pc - 2)) & 0xFFFFFF
    elif ea.kind == 'pcd16':
        t = c.pcref & 0xFFFFFF
    if t is not None:
        c.flow = kind
        c.targets.append(t)
    else:
        c.flow = 'call_ind' if kind == 'call' else 'indirect'


def movem(c, op, to_mem):
    sz = 4 if op & 0x40 else 2
    c.mn = 'movem' + SFX[sz]
    w, _, wv = c.ext16()
    if to_mem:
        ea = mkea(op, allowed=CALT | {'predec'})
    else:
        ea = mkea(op, allowed=CTRL | {'postinc'})
    r = c.f(ea.reg)

    def reglist(m, rev):
        regs = []
        for i in range(16):
            if m & (1 << i):
                regs.append(15 - i if rev else i)
        return '/'.join(('d%d' % x) if x < 8 else ('a%d' % (x - 8)) for x in sorted(regs))

    if c.rc:
        n = bin(wv).count('1')
        c.cycles += (8 if to_mem else 12) + n * (8 if sz == 4 else 4)
    else:
        c.dyn_cycles = True
        c.emit('cpu.cyc += %d + popcount16(%s) * %d;' % (8 if to_mem else 12, w, 8 if sz == 4 else 4))

    if to_mem:
        if ea.kind == 'predec':
            if c.rc:
                c.opnd(reglist(wv, True))
                c.opnd('-(a%d)' % ea.reg.v)
                # unrolled: highest register stored at highest address
                c.emit('{ u32 a = cpu.a[%s];' % r)
                for i in range(16):
                    if wv & (1 << i):
                        rr = 15 - i
                        src = 'cpu.d[%d]' % rr if rr < 8 else 'cpu.a[%d]' % (rr - 8)
                        c.emit('  a -= %d; wr%d(a, %s);' % (sz, sz * 8, src))
                c.emit('  cpu.a[%s] = a; }' % r)
            else:
                c.emit('cpu.a[%s] = movem_store_predec(%s, cpu.a[%s], %d);' % (r, w, r, sz))
            return
        if c.rc:
            c.opnd(reglist(wv, False))
        a = ea_addr(c, ea, sz)
        if c.rc:
            off = 0
            for i in range(16):
                if wv & (1 << i):
                    src = 'cpu.d[%d]' % i if i < 8 else 'cpu.a[%d]' % (i - 8)
                    c.emit('wr%d(%s + %d, %s);' % (sz * 8, a, off, src))
                    off += sz
        else:
            c.emit('movem_store(%s, %s, %d);' % (w, a, sz))
        return
    # mem -> regs
    if ea.kind == 'postinc':
        if c.rc:
            c.opnd('(a%d)+' % ea.reg.v)
            c.opnd(reglist(wv, False))
            c.emit('{ u32 a = cpu.a[%s];' % r)
            for i in range(16):
                if wv & (1 << i):
                    dst = 'cpu.d[%d]' % i if i < 8 else 'cpu.a[%d]' % (i - 8)
                    rd = 'SX16(rd16(a))' if sz == 2 else 'rd32(a)'
                    c.emit('  %s = %s; a += %d;' % (dst, rd, sz))
            # the base register itself is overwritten by the post-increment
            c.emit('  cpu.a[%s] = a; }' % r)
        else:
            c.emit('cpu.a[%s] = movem_load(%s, cpu.a[%s], %d);' % (r, w, r, sz))
        return
    a = ea_addr(c, ea, sz)
    if c.rc:
        c.opnd(reglist(wv, False))
        off = 0
        c.emit('{ u32 a = %s;' % a)
        for i in range(16):
            if wv & (1 << i):
                dst = 'cpu.d[%d]' % i if i < 8 else 'cpu.a[%d]' % (i - 8)
                rd = 'SX16(rd16(a + %d))' % off if sz == 2 else 'rd32(a + %d)' % off
                c.emit('  %s = %s;' % (dst, rd))
                off += sz
        c.emit('}')
    else:
        c.emit('movem_load(%s, %s, %d);' % (w, a, sz))


# ---- line 5: addq/subq/scc/dbcc
def line5(c, op):
    szb = (op >> 6) & 3
    if szb == 3:
        cc = Fld(op, 8, 15)
        if (op >> 3) & 7 == 1:
            r = Fld(op, 0, 7)
            rr = c.f(r)
            c.mn = 'db' + CC[cc.v]
            c.opnd('d%d' % r.v)
            w, p, wv = c.ext16()
            if c.rc:
                tgt = '0x%06Xu' % ((p_int(c) + s16(wv)) & 0xFFFFFF)
                c.opnd('$%X' % ((p_int(c) + s16(wv)) & 0xFFFFFF))
                c.targets.append((p_int(c) + s16(wv)) & 0xFFFFFF)
            else:
                tgt = '%s + SX16(%s)' % (p, w)
            cond = CCEXPR[cc.v] if c.rc else 'm68k_cond(%s)' % c.f(cc)
            c.flow = 'branch'
            c.emit('if (!%s) {' % cond)
            c.emit('  u32 cnt = (u16)(cpu.d[%s] - 1); SETD16(%s, cnt);' % (rr, rr))
            c.emit('  if (cnt != 0xFFFF) { cpu.pc = %s; cpu.cyc += 10; } else { cpu.pc = %s; cpu.cyc += 14; }'
                   % (tgt, c.next_pc()))
            c.emit('} else { cpu.pc = %s; cpu.cyc += 12; }' % c.next_pc())
            c.dyn_cycles = True
            return
        c.mn = 's' + CC[cc.v]
        ea = mkea(op, allowed=DALT)
        cond = CCEXPR[cc.v] if c.rc else 'm68k_cond(%s)' % c.f(cc)
        loc = Loc(c, ea, 1, read=False)
        loc.write('(%s) ? 0xFF : 0' % cond)
        c.cyc(6 if ea.kind == 'dn' else 8 + ea.time(1))
        return
    sz = size2(szb)
    q = Fld(op, 9, 7)
    sub = op & 0x100
    c.mn = ('subq' if sub else 'addq') + SFX[sz]
    data = c.let('u32', '((%s - 1) & 7) + 1' % c.f(q), 'q') if not c.rc else c.let('u32', str(q.v or 8), 'q')
    c.opnd('#%d' % (q.v or 8))
    ea = mkea(op, allowed=ALT)
    if ea.kind == 'an':
        if sz == 1:
            raise Illegal()
        r = c.f(ea.reg)
        c.opnd('a%d' % ea.reg.v)
        c.emit('cpu.a[%s] %s= %s;' % (r, '-' if sub else '+', data))
        c.cyc(8)
        return
    loc = Loc(c, ea, sz)
    loc.write(c.let('u32', 'f_%s(%d, %s, %s)' % ('sub' if sub else 'add', sz, data, loc.val), 'r'))
    if ea.kind == 'dn':
        c.cyc(8 if sz == 4 else 4)
    else:
        c.cyc((12 if sz == 4 else 8) + ea.time(sz))


def p_int(c):
    """address of the most recently fetched extension word (recomp)"""
    return c.pc - 2


# ---- line 6: branches
def line6(c, op):
    cc = (op >> 8) & 15
    d8 = op & 0xFF
    c.mn = ['bra', 'bsr'][cc] if cc < 2 else 'b' + CC[cc]
    base_c = c.pc if c.rc else None
    if d8 == 0:
        c.mn += '.w'
        w, p, wv = c.ext16()
        if c.rc:
            t = (base_c + s16(wv)) & 0xFFFFFF
            tgt = '0x%06Xu' % t
        else:
            tgt = '%s + SX16(%s)' % (p, w)
    else:
        c.mn += '.s'
        if d8 == 0xFF:
            # 68020 32-bit displacement: not on 68000, treat as illegal-ish
            raise Illegal()
        if c.rc:
            t = (base_c + s8(d8)) & 0xFFFFFF
            tgt = '0x%06Xu' % t
        else:
            tgt = 'cpu.pc + SX8(cpu.ir)'
    if c.rc:
        c.opnd('$%X' % t)
        c.targets.append(t)
    if cc == 0:
        c.set_pc(tgt)
        c.cyc(10)
        c.flow = 'jump'
        return
    if cc == 1:
        c.emit('push32(%s);' % c.next_pc())
        c.set_pc(tgt)
        c.cyc(18)
        c.flow = 'call'
        return
    cond = CCEXPR[cc] if c.rc else 'm68k_cond(%d)' % cc
    c.dyn_cycles = True
    c.emit('if (%s) { cpu.pc = %s; cpu.cyc += 10; } else { cpu.pc = %s; cpu.cyc += %d; }'
           % (cond, tgt, c.next_pc(), 12 if d8 == 0 else 8))
    c.flow = 'branch'


def line7(c, op):
    if op & 0x100:
        raise Illegal()
    r = Fld(op, 9, 7)
    c.mn = 'moveq'
    c.opnd('#%s' % hex8s(op))
    c.opnd('d%d' % r.v)
    v = ('0x%08Xu' % (s8(op) & 0xFFFFFFFF)) if c.rc else 'SX8(cpu.ir)'
    c.emit('cpu.d[%s] = f_logic(4, %s);' % (c.f(r), v))
    c.cyc(4)


# ---- lines 8, 9, B, C, D: arithmetic / logic
def bcd(c, op, name):
    rx = Fld(op, 9, 7)
    ry = Fld(op, 0, 7)
    c.mn = name
    fn = 'f_abcd' if name == 'abcd' else 'f_sbcd'
    if op & 8:
        # -(Ay),-(Ax)
        c.opnd('-(a%d)' % ry.v)
        c.opnd('-(a%d)' % rx.v)
        ya, xa = c.f(ry), c.f(rx)
        c.emit('cpu.a[%s] -= BINC(%s);' % (ya, ya))
        s = c.let('u32', 'rd8(cpu.a[%s])' % ya, 's')
        c.emit('cpu.a[%s] -= BINC(%s);' % (xa, xa))
        a = c.let('u32', 'cpu.a[%s]' % xa, 'ea')
        d = c.let('u32', 'rd8(%s)' % a, 'd')
        c.emit('wr8(%s, %s(%s, %s));' % (a, fn, s, d))
        c.cyc(18)
    else:
        c.opnd('d%d' % ry.v)
        c.opnd('d%d' % rx.v)
        c.emit('SETD8(%s, %s((u8)cpu.d[%s], (u8)cpu.d[%s]));' % (c.f(rx), fn, c.f(ry), c.f(rx)))
        c.cyc(6)


def addsubx(c, op, name, sz):
    rx = Fld(op, 9, 7)
    ry = Fld(op, 0, 7)
    c.mn = name + SFX[sz]
    fn = 'f_addx' if name == 'addx' else 'f_subx'
    if op & 8:
        c.opnd('-(a%d)' % ry.v)
        c.opnd('-(a%d)' % rx.v)
        ya, xa = c.f(ry), c.f(rx)
        dec = lambda r: ('BINC(%s)' % r) if sz == 1 else str(sz)
        c.emit('cpu.a[%s] -= %s;' % (ya, dec(ya)))
        s = c.let('u32', 'rd%d(cpu.a[%s])' % (sz * 8, ya), 's')
        c.emit('cpu.a[%s] -= %s;' % (xa, dec(xa)))
        a = c.let('u32', 'cpu.a[%s]' % xa, 'ea')
        d = c.let('u32', 'rd%d(%s)' % (sz * 8, a), 'd')
        c.emit('wr%d(%s, %s(%d, %s, %s));' % (sz * 8, a, fn, sz, s, d))
        c.cyc(30 if sz == 4 else 18)
    else:
        c.opnd('d%d' % ry.v)
        c.opnd('d%d' % rx.v)
        c.emit('SETD%d(%s, %s(%d, (%s)cpu.d[%s], (%s)cpu.d[%s]));'
               % (sz * 8, c.f(rx), fn, sz, CT[sz], c.f(ry), CT[sz], c.f(rx)))
        c.cyc(8 if sz == 4 else 4)


def arith(c, op, name):
    """or/and/sub/add/cmp/eor with Dn"""
    dr = Fld(op, 9, 7)
    szb = (op >> 6) & 3
    sz = size2(szb)
    to_ea = bool(op & 0x100)
    c.mn = name + SFX[sz]
    fnexpr = {
        'or': lambda s, d: 'f_logic(%d, %s | %s)' % (sz, d, s),
        'and': lambda s, d: 'f_logic(%d, %s & %s)' % (sz, d, s),
        'eor': lambda s, d: 'f_logic(%d, %s ^ %s)' % (sz, d, s),
        'add': lambda s, d: 'f_add(%d, %s, %s)' % (sz, s, d),
        'sub': lambda s, d: 'f_sub(%d, %s, %s)' % (sz, s, d),
    }
    dn = c.f(dr)
    if not to_ea:
        allowed = ALL if name in ('add', 'sub', 'cmp') else DATA
        ea = mkea(op, allowed=allowed)
        if ea.kind == 'an' and sz == 1:
            raise Illegal()
        s = ea_read(c, ea, sz)
        c.opnd('d%d' % dr.v)
        d = c.let('u32', '(%s)cpu.d[%s]' % (CT[sz], dn), 'd')
        if name == 'cmp':
            c.emit('f_cmp(%d, %s, %s);' % (sz, s, d))
            c.cyc((6 if sz == 4 else 4) + ea.time(sz))
            return
        c.emit('SETD%d(%s, %s);' % (sz * 8, dn, fnexpr[name](s, d)))
        if sz == 4:
            c.cyc((8 if ea.kind in ('dn', 'an', 'imm') else 6) + ea.time(sz))
        else:
            c.cyc(4 + ea.time(sz))
        return
    allowed = DALT if name == 'eor' else MALT
    ea = mkea(op, allowed=allowed)
    c.opnd('d%d' % dr.v)
    s = c.let('u32', '(%s)cpu.d[%s]' % (CT[sz], dn), 's')
    loc = Loc(c, ea, sz)
    loc.write(c.let('u32', fnexpr[name](s, loc.val), 'r'))
    if ea.kind == 'dn':
        c.cyc(8 if sz == 4 else 4)
    else:
        c.cyc((12 if sz == 4 else 8) + ea.time(sz))


def addra(c, op, name):
    ar = Fld(op, 9, 7)
    sz = 4 if op & 0x100 else 2
    c.mn = name + SFX[sz]
    ea = mkea(op, allowed=ALL)
    s = ea_read(c, ea, sz)
    if sz == 2:
        s = 'SX16(%s)' % s
    c.opnd('a%d' % ar.v)
    o = {'adda': '+', 'suba': '-'}.get(name)
    if name == 'cmpa':
        c.emit('f_cmp(4, %s, cpu.a[%s]);' % (s, c.f(ar)))
        c.cyc(6 + ea.time(sz))
        return
    c.emit('cpu.a[%s] %s= %s;' % (c.f(ar), o, s))
    c.cyc((8 if sz == 2 or ea.kind in ('dn', 'an', 'imm') else 6) + ea.time(sz))


def muldiv(c, op, name):
    dr = Fld(op, 9, 7)
    ea = mkea(op, allowed=DATA)
    c.mn = name
    s = ea_read(c, ea, 2)
    c.opnd('d%d' % dr.v)
    dn = c.f(dr)
    if name in ('mulu', 'muls'):
        c.emit('cpu.d[%s] = f_%s(%s, cpu.d[%s]);' % (dn, name, s, dn))
        c.cyc((54 if name == 'mulu' else 58) + ea.time(2))
        return
    c.emit('if (!(%s & 0xFFFF)) { m68k_exception(5, %s); cpu.cyc += 38; return; }' % (s, c.next_pc()))
    c.emit('cpu.d[%s] = f_%s(%s, cpu.d[%s]);' % (dn, name, s, dn))
    c.cyc((120 if name == 'divu' else 140) + ea.time(2))


def line8(c, op):
    if (op >> 6) & 7 == 3:
        return muldiv(c, op, 'divu')
    if (op >> 6) & 7 == 7:
        return muldiv(c, op, 'divs')
    if (op & 0x1F0) == 0x100:
        return bcd(c, op, 'sbcd')
    return arith(c, op, 'or')


def line9(c, op):
    if (op >> 6) & 3 == 3:
        return addra(c, op, 'suba')
    if (op & 0x130) == 0x100:
        return addsubx(c, op, 'subx', size2((op >> 6) & 3))
    return arith(c, op, 'sub')


def lineB(c, op):
    if (op >> 6) & 3 == 3:
        return addra(c, op, 'cmpa')
    if op & 0x100:
        if (op >> 3) & 7 == 1:
            sz = size2((op >> 6) & 3)
            ax = Fld(op, 9, 7)
            ay = Fld(op, 0, 7)
            c.mn = 'cmpm' + SFX[sz]
            c.opnd('(a%d)+' % ay.v)
            c.opnd('(a%d)+' % ax.v)
            ya, xa = c.f(ay), c.f(ax)
            inc = lambda r: ('BINC(%s)' % r) if sz == 1 else str(sz)
            s = c.let('u32', 'rd%d(cpu.a[%s])' % (sz * 8, ya), 's')
            c.emit('cpu.a[%s] += %s;' % (ya, inc(ya)))
            d = c.let('u32', 'rd%d(cpu.a[%s])' % (sz * 8, xa), 'd')
            c.emit('cpu.a[%s] += %s;' % (xa, inc(xa)))
            c.emit('f_cmp(%d, %s, %s);' % (sz, s, d))
            c.cyc(20 if sz == 4 else 12)
            return
        return arith(c, op, 'eor')
    return arith(c, op, 'cmp')


def lineC(c, op):
    if (op >> 6) & 7 == 3:
        return muldiv(c, op, 'mulu')
    if (op >> 6) & 7 == 7:
        return muldiv(c, op, 'muls')
    if (op & 0x1F0) == 0x100:
        return bcd(c, op, 'abcd')
    if op & 0x100 and (op >> 3) & 0x1F in (0x08, 0x09, 0x11):
        rx = Fld(op, 9, 7)
        ry = Fld(op, 0, 7)
        m = (op >> 3) & 0x1F
        c.mn = 'exg'
        x, y = c.f(rx), c.f(ry)
        if m == 0x08:
            a, b = 'cpu.d[%s]' % x, 'cpu.d[%s]' % y
            c.opnd('d%d' % rx.v); c.opnd('d%d' % ry.v)
        elif m == 0x09:
            a, b = 'cpu.a[%s]' % x, 'cpu.a[%s]' % y
            c.opnd('a%d' % rx.v); c.opnd('a%d' % ry.v)
        else:
            a, b = 'cpu.d[%s]' % x, 'cpu.a[%s]' % y
            c.opnd('d%d' % rx.v); c.opnd('a%d' % ry.v)
        c.emit('{ u32 t = %s; %s = %s; %s = t; }' % (a, a, b, b))
        c.cyc(6)
        return
    return arith(c, op, 'and')


def lineD(c, op):
    if (op >> 6) & 3 == 3:
        return addra(c, op, 'adda')
    if (op & 0x130) == 0x100:
        return addsubx(c, op, 'addx', size2((op >> 6) & 3))
    return arith(c, op, 'add')


# ---- line E: shifts
SHN = ['as', 'ls', 'rox', 'ro']


def lineE(c, op):
    szb = (op >> 6) & 3
    left = bool(op & 0x100)
    if szb == 3:
        # memory shift by one
        if op & 0x800:
            raise Illegal()
        t = (op >> 9) & 3
        c.mn = SHN[t] + ('l' if left else 'r')
        ea = mkea(op, allowed=MALT)
        loc = Loc(c, ea, 2)
        loc.write(c.let('u32', 'f_%s(2, %s, 1)' % (c.mn, loc.val), 'r'))
        c.cyc(8 + ea.time(2))
        return
    sz = size2(szb)
    t = (op >> 3) & 3
    name = SHN[t] + ('l' if left else 'r')
    c.mn = name + SFX[sz]
    cr = Fld(op, 9, 7)
    r = Fld(op, 0, 7)
    rr = c.f(r)
    if op & 0x20:
        c.opnd('d%d' % cr.v)
        cnt = c.let('u32', 'cpu.d[%s] & 63' % c.f(cr), 'n')
    else:
        c.opnd('#%d' % (cr.v or 8))
        cnt = c.let('u32', str(cr.v or 8) if c.rc else '((%s - 1) & 7) + 1' % c.f(cr), 'n')
    c.opnd('d%d' % r.v)
    c.emit('SETD%d(%s, f_%s(%d, cpu.d[%s], %s));' % (sz * 8, rr, name, sz, rr, cnt))
    c.emit('cpu.cyc += 2 * %s;' % cnt)
    c.cyc(8 if sz == 4 else 6)


def lineA(c, op):
    c.mn = 'dc.w $%04X ; line-A' % op
    c.exception(10, c.this_pc())
    c.emit('cpu.cyc += 34; return;')
    c.flow = 'exc'


def lineF(c, op):
    c.mn = 'dc.w $%04X ; line-F' % op
    c.exception(11, c.this_pc())
    c.emit('cpu.cyc += 34; return;')
    c.flow = 'exc'


LINES = [line0, move, move, move, line4, line5, line6, line7,
         line8, line9, lineA, lineB, lineC, lineD, lineE, lineF]


def decode(op, mode='interp', rom=None, addr=0):
    c = Ctx(mode, op, rom, addr)
    try:
        gen(c)
    except Illegal:
        return None
    return c

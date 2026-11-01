#!/usr/bin/env python3
"""
recompile.py - static recompiler: 68000 ROM -> C source.

  recompile.py ROM OUTDIR [--entries FILE ...] [--listing FILE]

Code discovery is recursive descent from the exception vectors, static
branch/call targets and Capcom-style PC-relative jump tables, plus any
extra entry points listed in --entries files (addresses the runtime logged
when it had to fall back to the interpreter, one hex address per line).

Every discovered basic block becomes a C function B_xxxxxx(); the runtime
dispatcher looks blocks up by PC. Blocks end at any control transfer, at
SR writes (so pending interrupts are taken) and before another block start.
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m68kgen import decode, s16

args = sys.argv[1:]
rom_path, outdir = args[0], args[1]
entry_files, listing = [], None
prefix, register_fn = 'B_', 'recomp_register'
i = 2
while i < len(args):
    if args[i] == '--entries':
        entry_files.append(args[i + 1]); i += 2
    elif args[i] == '--listing':
        listing = args[i + 1]; i += 2
    elif args[i] == '--prefix':          # function prefix, one per game
        prefix = args[i + 1]; i += 2
    elif args[i] == '--register':        # name of the generated registration function
        register_fn = args[i + 1]; i += 2
    else:
        sys.exit('bad arg ' + args[i])

rom = open(rom_path, 'rb').read()
ROMSZ = len(rom)


def r16(a):
    return rom[a] << 8 | rom[a + 1]


def r32(a):
    return r16(a) << 16 | r16(a + 2)


def valid_code_addr(a):
    return 0x200 <= a < ROMSZ - 1 and not (a & 1)


# ------------------------------------------------------------ discovery
VEC_NAMES = {1: 'reset', 2: 'bus_error', 3: 'address_error', 4: 'illegal_insn', 5: 'zero_divide',
             6: 'chk', 7: 'trapv', 8: 'privilege', 9: 'trace', 10: 'line_a', 11: 'line_f',
             24: 'spurious_irq', 25: 'irq1', 26: 'irq2_ext', 27: 'irq3', 28: 'hblank_irq4',
             29: 'irq5', 30: 'vblank_irq6', 31: 'irq7'}
entries = set()
labels = {}
for v in range(1, 64):
    a = r32(v * 4) & 0xFFFFFF
    if valid_code_addr(a):
        entries.add(a)
        labels.setdefault(a, VEC_NAMES.get(v, 'trap%d' % (v - 32) if 32 <= v < 48 else 'vec%02d' % v))
for ef in entry_files:
    if os.path.exists(ef):
        for line in open(ef):
            line = line.strip()
            if line:
                a = int(line, 16)
                if valid_code_addr(a):
                    entries.add(a)

insns = {}         # addr -> Ctx
block_starts = set(entries)
work = list(entries)
bad = set()
call_targets = set()
jump_tables = []


def jump_table(c, prev):
    """Capcom pattern: move.w (d8,pc,Dn.w),Dn ; jmp/jsr (d8,pc,Dn.w)
    table of 16-bit offsets relative to the table base."""
    if prev is None or c.pcindex is None or prev.pcindex is None:
        return []
    if not prev.mn.startswith('move.w'):
        return []
    base = prev.pcindex
    if base != c.pcindex:
        return []
    targets = []
    lim = None
    i = 0
    while True:
        ea = base + 2 * i
        if lim is not None and ea >= lim:
            break
        if ea + 1 >= ROMSZ or i > 256:
            break
        t = (base + s16(r16(ea))) & 0xFFFFFF
        if not valid_code_addr(t) or decode(r16(t), 'recomp', rom, t) is None:
            break
        targets.append(t)
        if t > base and (lim is None or t < lim):
            lim = t
        i += 1
    jump_tables.append((c.addr, base, len(targets)))
    return targets


while work:
    a = work.pop()
    prev = None
    while True:
        if a in insns or a in bad:
            break
        if not valid_code_addr(a):
            bad.add(a)
            break
        c = decode(r16(a), 'recomp', rom, a)
        if c is None:
            bad.add(a)
            break
        insns[a] = c
        c.size = c.pc - a
        for t in c.targets:
            if valid_code_addr(t):
                block_starts.add(t)
                work.append(t)
                if c.flow == 'call':
                    call_targets.add(t)
        nxt = a + c.size
        if c.flow in ('indirect', 'call_ind'):
            for t in jump_table(c, prev):
                block_starts.add(t)
                work.append(t)
        if c.flow in ('jump', 'ret', 'indirect', 'exc'):
            break
        if c.flow in ('branch', 'call', 'call_ind', 'sr', 'stop'):
            block_starts.add(nxt)
        prev = c
        a = nxt

for t in call_targets:
    labels.setdefault(t, 'sub_%06X' % t)

# ------------------------------------------------------------- blocks
blocks = []
for start in sorted(block_starts):
    if start not in insns:
        continue
    seq = []
    a = start
    while a in insns:
        c = insns[a]
        seq.append(c)
        a += c.size
        if c.flow is not None or a in block_starts:
            break
    blocks.append((start, seq, a))

# ------------------------------------------------------------- emit C
os.makedirs(outdir, exist_ok=True)
for fn in os.listdir(outdir):
    if fn.startswith('recomp_') and fn.endswith('.c'):
        os.remove(os.path.join(outdir, fn))

CHUNK = 1500
files = []
for fi in range(0, len(blocks), CHUNK):
    part = blocks[fi:fi + CHUNK]
    name = os.path.join(outdir, 'recomp_%03d.c' % (fi // CHUNK))
    files.append(name)
    with open(name, 'w') as f:
        f.write('/* GENERATED by tools/recompile.py from the SF2 ROM - do not edit */\n')
        f.write('#include "m68k.h"\n\n')
        for start, seq, end in part:
            lab = labels.get(start)
            f.write('/* ---- %06X%s */\n' % (start, ('  ' + lab) if lab else ''))
            f.write('void %s%06X(void) {\n' % (prefix, start))
            for c in seq:
                f.write('    /* %06X: %s */\n' % (c.addr, c.disasm()))
                f.write('    {\n')
                for line in c.out:
                    f.write('        %s\n' % line)
                if c.cycles:
                    f.write('        cpu.cyc += %d;\n' % c.cycles)
                f.write('    }\n')
            last = seq[-1]
            if last.flow in (None, 'sr', 'stop'):
                f.write('    cpu.pc = 0x%06Xu;\n' % end)
            f.write('}\n\n')

with open(os.path.join(outdir, 'recomp_register.c'), 'w') as f:
    f.write('/* GENERATED by tools/recompile.py - do not edit */\n#include "m68k.h"\n\n')
    for start, _, _ in blocks:
        f.write('void %s%06X(void);\n' % (prefix, start))
    # a table, not a call per block: the PowerPC TOC has no room for that many
    f.write('\nstatic const struct { u32 pc; blockfn f; } table[] = {\n')
    for start, _, _ in blocks:
        f.write('    { 0x%06Xu, %s%06X },\n' % (start, prefix, start))
    f.write('};\n\nvoid %s(void) {\n' % register_fn)
    f.write('    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) recomp_add(table[i].pc, table[i].f);\n')
    f.write('}\n')

# ------------------------------------------------------------ listing
if listing:
    with open(listing, 'w') as f:
        f.write('; Street Fighter II\' Special Champion Edition (J) - recovered 68000 code\n')
        f.write('; %d instructions, %d blocks, %d subroutines, %d jump tables\n\n'
                % (len(insns), len(blocks), len(call_targets), len(jump_tables)))
        last = None
        for a in sorted(insns):
            c = insns[a]
            if last is not None and a != last:
                f.write('\n; ... data %06X-%06X (%d bytes)\n\n' % (last, a, a - last))
            if a in labels:
                f.write('\n%s:\n' % labels[a])
            elif a in block_starts:
                f.write('loc_%06X:\n' % a)
            words = ' '.join('%04X' % r16(x) for x in range(a, a + c.size, 2))
            f.write('  %06X  %-24s %s\n' % (a, words, c.disasm()))
            last = a + c.size

code_bytes = sum(c.size for c in insns.values())
lo = min(insns) if insns else 0
hi = max(insns) if insns else 0
print('instructions %d (%d bytes, range %06X-%06X), blocks %d, subroutines %d, jump tables %d, files %d'
      % (len(insns), code_bytes, lo, hi, len(blocks), len(call_targets), len(jump_tables), len(files)),
      file=sys.stderr)

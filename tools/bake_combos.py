#!/usr/bin/env python3
"""bake_combos.py TUNE_DIR : write tuned combo scripts (from SF2_COMBO_TUNE runs)
back into src/combo.c, replacing each combo's script literal in order
(character 0..11, fire then thunder)."""
import glob, re, sys, textwrap
tuned = {}
for fn in glob.glob(sys.argv[1] + '/s*w*.txt'):
    lines = open(fn).read().splitlines()
    for i, l in enumerate(lines):
        m = re.match(r'TUNED slot \d+ char (\d+) which (\d+)', l)
        if m and i + 1 < len(lines):
            tuned[(int(m.group(1)), int(m.group(2)))] = lines[i + 1].strip().strip('"').strip()
src = open('src/combo.c').read()
entry = re.compile(r'(\{ "[^"]+", FX_\w+,\s*)((?:"[^"]*"\s*)+)(\})')
out, pos, k = [], 0, 0
for m in entry.finditer(src):
    ch, which = k // 2, k % 2
    k += 1
    out.append(src[pos:m.start()])
    if (ch, which) in tuned:
        parts = textwrap.wrap(tuned[(ch, which)], 96, break_on_hyphens=False)
        lit = ''.join('"%s%s"\n          ' % (p, ' ' if i < len(parts) - 1 else '') for i, p in enumerate(parts)).rstrip() + ' '
        out.append(m.group(1) + lit + m.group(3))
    else:
        out.append(m.group(0))
    pos = m.end()
out.append(src[pos:])
open('src/combo.c', 'w').write(''.join(out))
print('baked %d tuned combos of %d entries' % (len(tuned), k))

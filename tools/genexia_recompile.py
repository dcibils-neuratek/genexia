#!/usr/bin/env python3
"""
genexia_recompile.py - one ROM in, one native game library out.

  genexia_recompile.py ROM [--rounds N] [--frames N] [--no-os9] [--push HOST]

Inspects the ROM, finds its code (the interpreter, then bots playing, then
static analysis in the recompiler), compiles it into a library, and checks every
block it ran against the interpreter. Everything lands in
  ~/Library/Application Support/Genexia/games/<CRC32>/
where Genexia finds it by the ROM's fingerprint. Progress goes to stdout as one
JSON object per line, for the Genexia Recompiler app.
"""
import binascii
import concurrent.futures as cf
import json
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = os.path.expanduser('~/Library/Application Support/Genexia/games')
RETRO68 = os.path.expanduser('~/retro68-work/Retro68-build/toolchain')
INCS = ['-I' + os.path.join(REPO, d) for d in ('build', 'src', 'src/core', 'src/chips', 'src/backends/genesis',
                                              'src/backends/snes', 'src/backends/n64')]
GAMELIB = os.path.join(REPO, 'src/backends/snes/gamelib.c')
REGISTER = 'recomp_register_game'
# SNES cartridge chips Genexia doesn't have, by the header's type byte
SNES_CHIPS = {0x13: 'Super FX', 0x14: 'Super FX', 0x15: 'Super FX', 0x1A: 'Super FX', 0x23: 'OBC-1', 0x34: 'SA-1', 0x35: 'SA-1',
              0x43: 'S-DD1', 0x45: 'S-DD1', 0x55: 'S-RTC', 0xE3: 'Super Game Boy', 0xE5: 'Satellaview', 0xF3: 'CX4', 0xF5: 'custom chip', 0xF6: 'custom chip', 0xF9: 'SPC7110'}


def say(**e):
    print(json.dumps(e), flush=True)


def step(name, detail=''):
    say(event='step', step=name, detail=detail)


def fail(msg):
    say(event='failed', message=msg)
    sys.exit(1)


def snes_score(rom, base):
    if base + 32 > len(rom):
        return -1
    h = rom[base:base + 32]
    s = 8 if ((h[0x1E] | h[0x1F] << 8) ^ (h[0x1C] | h[0x1D] << 8)) == 0xFFFF else 0
    s += sum(32 <= c < 127 for c in h[:21])
    if (base == 0xFFC0 and h[0x15] & 15 in (1, 5)) or (base == 0x7FC0 and h[0x15] & 15 in (0, 2, 3)):
        s += 4
    return s


def inspect(path):
    rom = open(path, 'rb').read()
    info = {'rom': os.path.abspath(path), 'size': len(rom), 'console': None, 'title': os.path.splitext(os.path.basename(path))[0],
            'supported': False, 'problem': None}
    if len(rom) % 1024 == 512:
        rom = rom[512:]
    if rom[:4] in (b'\x80\x37\x12\x40', b'\x37\x80\x40\x12', b'\x40\x12\x37\x80'):
        info['console'] = 'Nintendo 64'
        if rom[:4] == b'\x80\x37\x12\x40':
            info['title'] = rom[0x20:0x34].decode('ascii', 'replace').strip() or info['title']
        info['problem'] = 'Nintendo 64 games can\'t be built as libraries yet.'
    elif rom[0x100:0x104] == b'SEGA':
        info['console'] = 'Genesis'
        info['title'] = rom[0x150:0x180].decode('ascii', 'replace').strip() or info['title']
        info['problem'] = 'Genesis games can\'t be built as libraries yet.'
    else:
        lo, hi = snes_score(rom, 0x7FC0), snes_score(rom, 0xFFC0)
        if max(lo, hi) >= 16:
            base = 0xFFC0 if hi > lo else 0x7FC0
            info['console'] = 'SNES'
            info['title'] = rom[base:base + 21].decode('ascii', 'replace').strip() or info['title']
            chip = rom[base + 0x16]
            if chip in SNES_CHIPS:
                info['problem'] = 'This cartridge uses a %s chip, which Genexia doesn\'t emulate yet.' % SNES_CHIPS[chip]
            else:
                info['supported'] = True
    if not info['console']:
        info['problem'] = 'This doesn\'t look like a Genesis, SNES or Nintendo 64 ROM.'
    info['id'] = '%08X' % binascii.crc32(rom)
    return info


def run(cmd, env=None, **kw):
    e = dict(os.environ, SF2_NOSAVE='1')
    e.update(env or {})
    return subprocess.run(cmd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', **kw)


def play(tool, rom, frames, seed, miss, lib=None, interp=False, runner=False):
    env = {'SF2_MISSLOG': miss} if miss else {}
    if lib:
        env['SF2_GAMELIB'] = lib
    if runner:
        env['SF2_MONKEY_RUN'] = '1'
    cmd = [tool, '--frames', str(frames), '--monkey', str(seed), '--rom', rom]
    if interp:
        cmd.insert(1, '--interp')
    return run(cmd, env).stdout


def read_cov(path):
    if not os.path.exists(path):
        return set()
    return {l.strip() for l in open(path) if len(l.split()) == 2}


def recompile(rom, d):
    r = run([sys.executable, os.path.join(REPO, 'tools/recompile_65816.py'), rom, os.path.join(d, 'code'), '--entries', os.path.join(d, 'coverage.txt'),
             '--prefix', 'G_', '--register', REGISTER])
    m = re.search(r'instructions (\d+) \((\d+) bytes\), blocks (\d+)', r.stdout)
    if r.returncode or not m:
        fail('The recompiler stopped: ' + r.stdout[-300:])
    g = re.search(r'followed (\d+) instructions, guessed (\d+)', r.stdout)
    return {'instructions': int(m[1]), 'code_bytes': int(m[2]), 'blocks': int(m[3]), 'followed': int(g[1]), 'guessed': int(g[2])}


def sources(d):
    c = os.path.join(d, 'code')
    return [GAMELIB] + sorted(os.path.join(c, f) for f in os.listdir(c) if f.endswith('.c'))


def compile_all(cc, flags, objdir, srcs):
    os.makedirs(objdir, exist_ok=True)

    def one(s):
        o = os.path.join(objdir, os.path.basename(s)[:-2] + '.o')
        if os.path.exists(o) and os.path.getmtime(o) >= os.path.getmtime(s):
            r = None
        else:
            r = run([cc] + flags + ['-c', s, '-o', o])
        if r and r.returncode:
            fail('Compiling failed: ' + r.stdout[-400:])
        return o
    with cf.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        return list(ex.map(one, srcs))


def link_host(d, name='game.dylib', extra=(), opt='-O2'):
    objs = compile_all('clang', [opt, '-std=gnu11', '-w', '-fPIC', '-DGAME_LIB', '-DGAME_REGISTER=' + REGISTER] + list(extra) + INCS,
                       os.path.join(d, 'obj-' + name), sources(d))
    out = os.path.join(d, name)
    r = run(['clang', '-dynamiclib', '-o', out] + objs)
    if r.returncode:
        fail('Linking failed: ' + r.stdout[-400:])
    return out


def link_os9(d, game_id):
    cc = os.path.join(RETRO68, 'bin/powerpc-apple-macos-gcc')
    objs = compile_all(cc, ['-std=gnu11', '-O2', '-w', '-mminimal-toc', '-mcpu=750', '-mtune=7450', '-fno-strict-aliasing', '-DGENEXIA_OS9',
                            '-DGAME_LIB', '-DGAME_REGISTER=' + REGISTER, '-DROM_MAX=0x800000'] + INCS, os.path.join(d, 'obj-os9'), sources(d))
    so, pef, out = os.path.join(d, 'libgame.so'), os.path.join(d, 'game.pef'), os.path.join(d, game_id + '.lib.bin')
    r = run([cc, '-shared', '-Wl,-bE:' + os.path.join(REPO, 'tools/game_lib.exp'), '-o', so] + objs)
    if r.returncode:
        fail('Linking for OS 9 failed: ' + r.stdout[-400:])
    r = run([os.path.join(RETRO68, 'bin/MakePEF'), so, '-o', pef])
    if r.returncode:
        fail('MakePEF failed: ' + r.stdout[-400:])
    r = run([os.path.join(RETRO68, 'bin/Rez'), os.path.join(REPO, 'tools/game_lib.r'), '-I', os.path.join(RETRO68, 'RIncludes'),
             '-o', out, '-t', 'shlb', '-c', '????', '--data', pef])
    if r.returncode:
        fail('Rez failed: ' + r.stdout[-400:])
    os.remove(so)
    return out


def share(outputs):
    blocks = interp = 0
    for t in outputs:
        m = re.search(r'blocks (\d+)\s+interp-steps (\d+)', t)
        if m:
            blocks += int(m[1]); interp += int(m[2])
    return round(100.0 * interp / max(1, blocks + interp), 2)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    rom, rounds, frames, os9, host = a[0], 4, 30000, True, None
    i = 1
    while i < len(a):
        if a[i] == '--no-os9': os9 = False; i += 1; continue
        k, v = a[i], a[i + 1]; i += 2
        if k == '--rounds': rounds = int(v)
        elif k == '--frames': frames = int(v)
        elif k == '--push': host = v
        else: sys.exit('bad option ' + k)
    t0 = time.time()
    step('inspect', 'Reading the cartridge')
    info = inspect(rom)
    say(event='inspected', **info)
    if not info['supported']:
        fail(info['problem'])
    d = os.path.join(GAMES, info['id'])
    os.makedirs(d, exist_ok=True)
    cov = os.path.join(d, 'coverage.txt')

    def save(**more):
        info.update(more)
        with open(os.path.join(d, 'info.json'), 'w') as f:
            json.dump(info, f, indent=1)
    save(status='building')
    tool = os.path.join(REPO, 'genexia')
    lockstep = os.path.join(REPO, 'genexia-lockstep')
    if not os.path.exists(tool) or not os.path.exists(lockstep):
        fail('Build Genexia first: make genexia smk-lockstep')
    tmp = os.path.join(d, 'tmp')
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)

    step('discover', 'Following the game from power-on in the interpreter')
    if not read_cov(cov):
        boot = os.path.join(tmp, 'boot.miss')
        play(tool, rom, 1500, 1, boot, interp=True)
        known = read_cov(boot)
        if not known:
            fail('The game never started in the interpreter.')
        open(cov, 'w').write('\n'.join(sorted(known)) + '\n')

    bots = os.cpu_count() or 4
    bots = min(8, max(2, bots))
    outputs = []
    for r in range(rounds):
        step('compile', 'Compiling what is known (round %d)' % (r + 1))
        st = recompile(rom, d)
        lib = link_host(d)
        say(event='stats', round=r + 1, **st)
        step('play', 'Bots playing round %d of %d' % (r + 1, rounds))
        seeds = [(1000 * (r + 1) + b, b < bots * 3 // 4) for b in range(bots)]
        miss = lambda s: os.path.join(tmp, 'r%d.miss' % s)
        with cf.ThreadPoolExecutor(bots) as ex:
            outputs = list(ex.map(lambda p: play(tool, rom, frames, p[0], miss(p[0]), lib=lib, runner=p[1]), seeds))
        known = read_cov(cov)
        new = set()
        for s, _ in seeds:
            new |= read_cov(miss(s))
        new -= known
        open(cov, 'w').write('\n'.join(sorted(known | new)) + '\n')
        say(event='round', round=r + 1, new=len(new), interpreter_percent=share(outputs))
        if len(new) < 3:
            break

    step('compile', 'Final compile')
    st = recompile(rom, d)
    lib = link_host(d)
    say(event='stats', **st)

    step('verify', 'Checking each block against the interpreter')
    ls = link_host(d, 'game-check.dylib', ['-DSNES_LOCKSTEP'], '-O1')
    out = run([lockstep, '--frames', '3000', '--monkey', '7', '--rom', rom], {'SF2_GAMELIB': ls, 'SF2_MONKEY_RUN': '1'}).stdout
    bad = len(re.findall(r'^block ', out, re.M))
    m = re.search(r'blocks (\d+)', out[out.rfind('frames '):]) if 'frames ' in out else None
    checked = int(m[1]) if m else 0
    if not checked:
        fail('The check didn\'t finish: ' + out[-300:])
    say(event='verified', blocks=checked, mismatches=bad)

    native = None
    if os9 and os.path.exists(os.path.join(RETRO68, 'bin/MakePEF')):
        step('os9', 'Compiling for the iMac G4')
        native = link_os9(d, info['id'])
        say(event='os9', file=native, bytes=os.path.getsize(native))
    if host and native:
        step('push', 'Sending to the iMac')
        r = run([sys.executable, os.path.join(REPO, 'tools/os9_share.py'), host, 'push', native, info['id'] + '.lib'])
        if r.returncode:
            fail('Couldn\'t reach the iMac at %s.' % host)

    os.remove(ls)
    shutil.rmtree(os.path.join(d, 'obj-game-check.dylib'), ignore_errors=True)
    shutil.rmtree(tmp, ignore_errors=True)
    save(status='ready' if not bad else 'mismatch', instructions=st['instructions'], blocks=st['blocks'], addresses=len(read_cov(cov)),
         interpreter_percent=share(outputs), checked_blocks=checked, mismatches=bad, library=lib, os9_library=native, built=int(time.time()))
    say(event='done', id=info['id'], title=info['title'], seconds=round(time.time() - t0), mismatches=bad)
    sys.exit(0 if not bad else 2)


main()

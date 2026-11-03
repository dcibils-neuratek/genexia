#!/usr/bin/env python3
"""PSP games for the Genexia Recompiler app.

    psp_game.py scan          puts every PSP disc in Downloads and on the Desktop on the shelf
    psp_game.py build DISC    compiles one, with the app's progress events

Each game lives in the app's games folder under its disc ID. Nothing here goes
in the repository. Genexia holds no decryption code: a disc whose program is
encrypted builds only from a dump the player made with PPSSPP.
"""
import concurrent.futures
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUPPORT = os.path.expanduser('~/Library/Application Support/Genexia')
GAMES = os.path.join(SUPPORT, 'games')
OLD_GAMES = os.path.join(SUPPORT, 'games-psp')
KEPT_SAVES = os.path.join(SUPPORT, 'psp-saves')
REMOVED = os.path.join(SUPPORT, 'psp-removed.txt')
SEARCH = [os.path.expanduser('~/Downloads'), os.path.expanduser('~/Desktop')]
DUMPS = [os.path.expanduser('~/.config/ppsspp/PSP/SYSTEM/DUMP'), os.path.expanduser('~/Downloads')]
CONSOLE = 'PSP'
ENCRYPTED = ("This disc's program is encrypted, and Genexia can't read it. In PPSSPP, turn on Dump decrypted EBOOT.BIN "
             "in the developer tools and start the game once, then press Compile.")


building = None        # (folder, info) of the game being built, so a failure is kept with it


def say(**e):
    print(json.dumps(e), flush=True)


def step(name, detail=''):
    say(event='step', step=name, detail=detail)


def fail(message):
    if building:
        building[1].update(status='ready', problem=message)
        write_info(*building)
    say(event='failed', message=message)
    sys.exit(1)


class Disc:
    """Just enough of ISO 9660 to find a file by its path."""

    def __init__(self, path):
        self.f = open(path, 'rb')

    def sector(self, lba, size):
        self.f.seek(lba * 2048)
        return self.f.read(size)

    def find(self, path):
        head = self.sector(16, 2048)
        if head[1:6] != b'CD001':
            return None
        root = head[156:190]
        lba, size = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
        for part in path.split('/'):
            data, at, found = self.sector(lba, size), 0, False
            while at < len(data):
                n = data[at]
                if n == 0:
                    at = (at // 2048 + 1) * 2048
                    continue
                e = data[at:at + n]
                at += n
                if e[33:33 + e[32]].decode('latin1').split(';')[0] == part:
                    lba, size = struct.unpack_from('<I', e, 2)[0], struct.unpack_from('<I', e, 10)[0]
                    found = True
                    break
            if not found:
                return None
        return lba, size

    def read(self, path):
        where = self.find(path)
        return self.sector(*where) if where else None


def param_sfo(data):
    keys, values, count = struct.unpack_from('<III', data, 8)
    facts = {}
    for i in range(count):
        name, kind, used, _, at = struct.unpack_from('<HHIII', data, 20 + i * 16)
        key = data[keys + name:data.index(b'\0', keys + name)].decode('ascii', 'replace')
        raw = data[values + at:values + at + used]
        facts[key] = raw.rstrip(b'\0').decode('utf8', 'replace') if kind == 0x0204 else raw
    return facts


def identify(path):
    """The disc's ID and title, or None when it isn't a PSP game."""
    try:
        sfo = Disc(path).read('PSP_GAME/PARAM.SFO')
        facts = param_sfo(sfo) if sfo else {}
    except (OSError, struct.error, ValueError, IndexError):
        return None
    disc_id, title = facts.get('DISC_ID'), facts.get('TITLE')
    if not isinstance(disc_id, str) or not re.fullmatch(r'[A-Z0-9]{9}', disc_id):
        return None
    return disc_id, title if isinstance(title, str) and title else disc_id


def adopt(disc_id):
    """Games built before the app knew PSP stay reachable from their old folder."""
    new, old = os.path.join(GAMES, disc_id), os.path.join(OLD_GAMES, disc_id)
    if not os.path.exists(new) and os.path.isdir(old) and not os.path.islink(old):
        os.rename(old, new)
        os.symlink(new, old)
    os.makedirs(new, exist_ok=True)
    return new


def write_info(d, info):
    temporary = os.path.join(d, 'info.json.new')
    with open(temporary, 'w') as f:
        json.dump(info, f, indent=1)
    os.replace(temporary, os.path.join(d, 'info.json'))


def read_info(d):
    try:
        with open(os.path.join(d, 'info.json')) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def pictures(disc, d):
    """The disc's own backdrop is the game's picture."""
    if os.path.exists(os.path.join(d, 'shot.png')):
        return
    image = disc.read('PSP_GAME/PIC1.PNG') or disc.read('PSP_GAME/ICON0.PNG')
    if image:
        with open(os.path.join(d, 'shot.png'), 'wb') as f:
            f.write(image)
    open(os.path.join(d, 'art.none'), 'w').close()


def removed():
    try:
        with open(REMOVED) as f:
            return set(f.read().split())
    except OSError:
        return set()


def recompile_stats(elf, out):
    r = subprocess.run([sys.executable, os.path.join(REPO, 'tools/recompile_psp.py'), elf, out], capture_output=True, text=True)
    m = re.search(r'(\d+) blocks, (\d+) instructions reached \(([\d.]+)%', r.stdout + r.stderr)
    return (int(m.group(1)), int(m.group(2)), float(m.group(3))) if m else None


def dump_of(disc_id):
    for folder in DUMPS:
        for path in glob.glob(os.path.join(folder, disc_id + '*.BIN')):
            with open(path, 'rb') as f:
                if f.read(4) == b'\x7fELF':
                    return path
    return None


def boot_program(disc, disc_id):
    """The program as an ELF: from the disc if it's readable, or the player's dump."""
    boot = disc.read('PSP_GAME/SYSDIR/BOOT.BIN')
    if boot and boot[:4] == b'\x7fELF':
        return boot
    dump = dump_of(disc_id)
    if dump:
        with open(dump, 'rb') as f:
            return f.read()
    return None


def scan():
    known = {}
    for name in os.listdir(GAMES) if os.path.isdir(GAMES) else []:
        info = read_info(os.path.join(GAMES, name))
        if info and info.get('console') == CONSOLE:
            known[info.get('rom')] = name
    gone = removed()
    new = 0
    discs = []
    for top in SEARCH:
        base = top.count(os.sep)
        for root, dirs, files in os.walk(top):
            if root.count(os.sep) - base >= 2:
                dirs[:] = []
            discs += [os.path.join(root, n) for n in files if n.lower().endswith('.iso')]
    for path in discs:
        if path in known:
            continue
        found = identify(path)
        if not found or found[0] in gone:
            continue
        disc_id, title = found
        d = adopt(disc_id)
        info = read_info(d) or {}
        if os.path.exists(info.get('rom', '')):
            continue                    # a second copy of a disc already on the shelf
        disc = Disc(path)
        pictures(disc, d)
        elf = os.path.join(d, 'game.elf')
        readable = boot_program(disc, disc_id) or os.path.exists(elf)
        info.update(rom=path, console=CONSOLE, title=title.upper(), id=disc_id, supported=True, problem=None if readable else ENCRYPTED)
        info.setdefault('status', 'ready')
        if not info.get('instructions') and os.path.exists(os.path.join(d, 'play_metal')) and os.path.exists(elf):
            stats = recompile_stats(elf, os.path.join(d, 'stats.tmp'))
            shutil.rmtree(os.path.join(d, 'stats.tmp'), ignore_errors=True)
            if stats:
                info.update(blocks=stats[0], instructions=stats[1], reached_percent=stats[2])
            info['library_bytes'] = os.path.getsize(os.path.join(d, 'play_metal'))
        write_info(d, info)
        new += 1
    if new:
        print('new')


def extract_program(disc, d, disc_id):
    """BOOT.BIN to the fixed-address program the recompiler takes."""
    boot = boot_program(disc, disc_id)
    elf = os.path.join(d, 'game.elf')
    if not boot:
        if os.path.exists(elf):
            return elf
        fail(ENCRYPTED)
    path = os.path.join(d, 'BOOT.BIN')
    with open(path, 'wb') as f:
        f.write(boot)
    if struct.unpack_from('<H', boot, 16)[0] == 0xFFA0:
        r = subprocess.run([sys.executable, os.path.join(REPO, 'tools/psp_relocate.py'), path, elf], capture_output=True, text=True)
        if r.returncode:
            fail('Could not place the program in memory: ' + (r.stderr or r.stdout).strip()[:200])
    else:
        shutil.copyfile(path, elf)
    return elf


def compile_game(d, cc):
    code, objects = os.path.join(d, 'code'), os.path.join(d, 'obj')
    shutil.rmtree(objects, ignore_errors=True)
    os.makedirs(objects)
    sources = sorted(n for n in os.listdir(code) if n.endswith('.c'))
    start = time.time()

    def one(name):
        out = os.path.join(objects, name[:-2] + '.o')
        return subprocess.run(cc + ['-O1', '-c', os.path.join(code, name), '-o', out], capture_output=True, text=True)

    finished = 0
    say(event='progress', label='Compiling', done=0, total=len(sources))
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for r in pool.map(one, sources):
            if r.returncode:
                fail('The compiler stopped: ' + r.stderr.strip()[:300])
            finished += 1
            say(event='progress', label='Compiling', done=finished, total=len(sources))
    say(event='timing', detail='Compiling', seconds=round(time.time() - start, 1))


def tool_flags(command):
    return subprocess.run(command, capture_output=True, text=True).stdout.split()


def link(d):
    src = os.path.join(REPO, 'src/backends/psp')
    build = os.path.join(REPO, 'build')
    sdl_c, sdl_l = tool_flags(['sdl2-config', '--cflags']), tool_flags(['sdl2-config', '--libs'])
    av_c, av_l = tool_flags(['pkg-config', '--cflags', 'libavcodec']), tool_flags(['pkg-config', '--libs', 'libavcodec', 'libavutil'])
    if not av_l:
        fail('Compiling PSP games needs FFmpeg for their music and movies: brew install ffmpeg')
    subprocess.run([sys.executable, os.path.join(REPO, 'tools/psp_nids.py'), os.path.join(build, 'psp_nids.h')], check=True, capture_output=True)
    metal = os.path.join(build, 'psp_metal.o')
    r = subprocess.run(['clang', '-O2', '-w', '-c', '-fobjc-arc', '-I' + src] + sdl_c + [os.path.join(src, 'psp_metal.m'), '-o', metal], capture_output=True, text=True)
    if r.returncode:
        fail('The picture code would not compile: ' + r.stderr.strip()[:300])
    objects = sorted(os.path.join(d, 'obj', n) for n in os.listdir(os.path.join(d, 'obj')))
    target = os.path.join(d, 'play_metal')
    cmd = ['clang', '-O2', '-w', '-std=gnu11', '-DPSP_WINDOW', '-DPSP_METAL', '-I' + os.path.join(REPO, 'src/chips'), '-I' + build, '-I' + src]
    cmd += sdl_c + av_c + [os.path.join(src, n) for n in ('psp_runtime.c', 'psp_vfpu.c', 'psp_ge.c')] + [metal] + objects
    cmd += sdl_l + av_l + ['-lm', '-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'Foundation', '-o', target + '.new']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        fail('Linking stopped: ' + r.stderr.strip()[:300])
    os.replace(target + '.new', target)
    return os.path.getsize(target)


def build(path):
    global building
    start = time.time()
    step('inspect', 'Reading the disc')
    found = identify(path)
    if not found:
        fail("This isn't a PSP game disc Genexia can read.")
    disc_id, title = found
    say(event='inspected', id=disc_id, title=title.upper(), console=CONSOLE)
    d = adopt(disc_id)
    disc = Disc(path)
    pictures(disc, d)
    info = read_info(d) or {}
    info.update(rom=path, console=CONSOLE, title=title.upper(), id=disc_id, supported=True, problem=None, status='building')
    write_info(d, info)
    building = d, info
    if disc_id in removed():
        with open(REMOVED, 'w') as f:
            f.write('\n'.join(removed() - {disc_id}) + '\n')
    kept = os.path.join(KEPT_SAVES, disc_id)
    if os.path.isdir(kept) and not os.path.exists(os.path.join(d, 'saves')):
        os.rename(kept, os.path.join(d, 'saves'))

    elf = extract_program(disc, d, disc_id)
    step('discover', 'Finding the code in the program')
    shutil.rmtree(os.path.join(d, 'code'), ignore_errors=True)
    stats = recompile_stats(elf, os.path.join(d, 'code'))
    if not stats:
        fail('The recompiler stopped on this program.')
    say(event='stats', instructions=stats[1])
    info.update(blocks=stats[0], instructions=stats[1], reached_percent=stats[2])

    step('compile', 'Compiling to native code')
    compile_game(d, ['clang', '-w', '-std=gnu11', '-I' + os.path.join(REPO, 'src/chips')])
    step('link', 'Adding the graphics, sound and system code')
    info['library_bytes'] = link(d)
    info['status'] = 'ready'
    write_info(d, info)
    say(event='done', seconds=round(time.time() - start, 1))


if __name__ == '__main__':
    if sys.argv[1:] == ['scan']:
        scan()
    elif len(sys.argv) == 3 and sys.argv[1] == 'build':
        build(sys.argv[2])
    else:
        sys.exit(__doc__)

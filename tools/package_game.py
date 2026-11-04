#!/usr/bin/env python3
"""Package a built game as a double-clickable Mac app.

    tools/package_game.py GAME_ID [--target arm|intel|ppc|windows|linux] [--out DIR]

The app holds the emulators with no game built in, the game's ROM, its native
library, its options and SDL, so it runs with neither Genexia nor this project
around. It contains the ROM: it is for your own use, and it lands outside the
repository so it can't be committed.

arm and intel make a Mac app; a PSP game packages for arm, with the Homebrew
libraries its player uses inside, or as a folder for x86-64 windows or linux,
built with zig and drawn with OpenGL. A PSP package holds no part of the disc:
the player reads the user's own .iso, put beside it. ppc makes an HFS disk image for Mac OS 9 holding
the app, library and ROM side by side. Prints one line of JSON per event, like
genexia_recompile.py.
"""
import concurrent.futures as cf
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = os.path.expanduser('~/Library/Application Support/Genexia/games')
OUT = os.path.expanduser('~/Applications/Genexia Games')
RUNTIME = os.path.join(REPO, 'genexia-runtime')
RUNTIME_X86 = os.path.join(REPO, 'genexia-runtime-x86_64')
RETRO68 = os.path.expanduser('~/retro68-work/Retro68-build/toolchain')
INCS = ['-I' + os.path.join(REPO, d) for d in ('build', 'src', 'src/core', 'src/chips', 'src/backends/genesis',
                                              'src/backends/snes', 'src/backends/n64')]
X86 = ['-arch', 'x86_64', '-mmacosx-version-min=10.15']
# Homebrew's SDL has no Intel half. Any universal SDL2 does: it's the same library.
SDL_X86 = ['/Applications/love.app/Contents/Frameworks/SDL2.framework/SDL2']
SDL_LOCAL = os.path.join(REPO, 'build/x86_64/libSDL2-2.0.0.dylib')


PSPDEV = os.path.expanduser('~/pspdev')


def say(**e):
    print(json.dumps(e), flush=True)


def fail(msg):
    say(event='failed', message=msg)
    sys.exit(1)


def run(cmd, env=None):
    return subprocess.run(cmd, cwd=REPO, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def must(cmd):
    r = run(cmd)
    if r.returncode:
        fail(' '.join(cmd[:2]) + ' failed: ' + r.stdout[-300:])
    return r.stdout


def app_name(info):
    """The ROM's file name reads better than the shouted cartridge title."""
    stem = os.path.splitext(os.path.basename(info['rom']))[0]
    name = re.sub(r'\s*[(\[].*?[)\]]', '', stem).strip()
    name = re.sub(r'^\d+ - ', '', name)            # a collection's number, as in "0017 - "
    return re.sub(r'[/:]', ' ', name) or info['title'].title()


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
    return int(m[1]), int(m[2]), data[m.end():]


def screenshot(rom, lib, d):
    """The liveliest of a few moments of attract mode, for the icon."""
    best, score = None, -1
    for frames in (2400, 1500, 800):
        shot = os.path.join(d, f'{frames}.ppm')
        env = dict(os.environ, SF2_GAMELIB=lib, SF2_NOSAVE='1')
        run([RUNTIME, '--frames', str(frames), '--rom', rom, '--shot', shot], env)
        if not os.path.exists(shot):
            continue
        w, h, px = read_ppm(shot)
        colours = len({px[i:i + 3] for i in range(0, len(px), 3)})
        if colours > score:
            best, score = shot, colours
    return best


def png(path, size, rgba, height=None):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    height = height or size
    rows = b''.join(b'\0' + rgba[y * size * 4:(y + 1) * size * 4] for y in range(height))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, height, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def icon_image(shot, size):
    """The middle of the frame in sharp pixels, on a rounded Mac icon shape."""
    w, h, px = read_ppm(shot)
    side = min(w, h)
    x0, y0 = (w - side) // 2, (h - side) // 2
    inset = size // 10                  # macOS icons leave a margin
    body = size - 2 * inset
    radius = body * 0.225
    out = bytearray(size * size * 4)
    for y in range(body):
        sy = y0 + y * side // body
        for x in range(body):
            # corner distance decides the rounded edge
            cx = max(radius - x, x - (body - 1 - radius), 0)
            cy = max(radius - y, y - (body - 1 - radius), 0)
            alpha = min(1, max(0, radius + 0.5 - (cx * cx + cy * cy) ** 0.5))
            if not alpha:
                continue
            s = (sy * w + x0 + x * side // body) * 3
            o = ((y + inset) * size + x + inset) * 4
            out[o:o + 4] = bytes((px[s], px[s + 1], px[s + 2], int(alpha * 255)))
    return bytes(out)


def make_icon(shot, target, d, cartridge=None):
    iconset = os.path.join(d, 'game.iconset')
    os.makedirs(iconset)
    for size, names in ((16, ['16x16']), (32, ['16x16@2x', '32x32']), (64, ['32x32@2x']),
                        (128, ['128x128']), (256, ['128x128@2x', '256x256']),
                        (512, ['256x256@2x', '512x512'])):
        for n in names:
            file = os.path.join(iconset, f'icon_{n}.png')
            if cartridge:
                must(['sips', '-z', str(size), str(size), cartridge, '--out', file])
            else:
                png(file, size, icon_image(shot, size))
    must(['iconutil', '-c', 'icns', '-o', target, iconset])


def intel_sdl():
    if os.path.exists(SDL_LOCAL):
        return SDL_LOCAL
    src = next((p for p in SDL_X86 if os.path.exists(p)), None)
    if not src:
        fail('Intel apps need a universal SDL2, and none was found on this Mac.')
    os.makedirs(os.path.dirname(SDL_LOCAL), exist_ok=True)
    must(['lipo', '-thin', 'x86_64', src, '-output', SDL_LOCAL])
    must(['install_name_tool', '-id', '@executable_path/../Frameworks/libSDL2-2.0.0.dylib', SDL_LOCAL])
    return SDL_LOCAL


def intel_runtime():
    sdl = intel_sdl()
    must(['make', '-j8', 'BUILD=build/x86_64', 'RT_OUT=genexia-runtime-x86_64', 'CC=clang ' + ' '.join(X86),
          'SDL_LD=' + sdl + ' -Wl,-framework,Cocoa', 'genexia-runtime-x86_64'])


def intel_library(d, console):
    """The game's generated C, compiled again for an Intel CPU."""
    gamelib = os.path.join(REPO, 'src/backends', 'n64' if console == 'Nintendo 64' else 'snes', 'gamelib.c')
    code = os.path.join(d, 'code')
    srcs = [gamelib] + sorted(os.path.join(code, f) for f in os.listdir(code) if f.endswith('.c'))
    objdir = os.path.join(d, 'obj-intel')
    os.makedirs(objdir, exist_ok=True)
    flags = X86 + ['-O2', '-std=gnu11', '-w', '-fPIC', '-DGAME_LIB', '-DGAME_REGISTER=recomp_register_game'] + INCS
    done = [0]

    def one(s):
        o = os.path.join(objdir, os.path.basename(s)[:-2] + '.o')
        if not (os.path.exists(o) and os.path.getmtime(o) >= os.path.getmtime(s)):
            extra = ['-O0'] if s.endswith('recomp_register.c') else []
            must(['clang'] + flags + extra + ['-c', s, '-o', o])
        done[0] += 1
        say(event='packaging', id='compile', done=done[0], total=len(srcs))
        return o
    with cf.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        objs = list(ex.map(one, srcs))
    say(event='packaging', id='link')
    lib = os.path.join(objdir, 'game.dylib')
    must(['clang'] + X86 + ['-dynamiclib', '-Wl,-exported_symbol,_genexia_game', '-o', lib] + objs)
    return lib


def package_psp(info, game, name, out, d):
    """A folder for PSP/GAME on a memory stick: the game's native code in the EBOOT, the ROM beside it."""
    if info['console'] != 'SNES':
        fail('Only SNES games can be built for the PSP so far.')
    if not os.path.exists(os.path.join(PSPDEV, 'bin/psp-gcc')):
        fail('The PSP compiler is missing: it belongs in ~/pspdev.')
    must(['make', '-j8', 'genexia-runtime'])
    work = tempfile.mkdtemp(prefix='genexia-package')
    try:
        os.symlink(d, os.path.join(work, 'game'))      # make can't take a path with spaces
        icon = os.path.join(work, 'ICON0.PNG')
        cartridge = os.path.join(d, 'cartridge.png')
        shot = None if os.path.exists(cartridge) else screenshot(info['rom'], os.path.join(d, 'game.dylib'), work)
        if not shot and os.path.exists(cartridge):
            square = os.path.join(work, 'square.png')
            must(['sips', '-z', '80', '80', cartridge, '--out', square])
            must(['sips', '-p', '80', '144', '--padColor', '000000', square, '--out', icon])
        elif shot:
            w, h, px = read_ppm(shot)
            image = bytearray()
            for y in range(80):
                for x in range(144):
                    s = (y * h // 80 * w + x * w // 144) * 3
                    image += px[s:s + 3] + b'\xff'
            png(icon, 144, bytes(image), 80)
        else:
            open(icon, 'wb').close()
        say(event='packaging', id='compile')
        build = os.path.join(work, 'out')
        must(['make', '-j8', '-f', os.path.join(REPO, 'src/platforms/psp/Makefile'), 'GAME=' + os.path.join(work, 'game'),
              'OUT=' + build, 'ICON=' + icon, 'TITLE=' + re.sub(r"['\"$`\\\\]", '', name)])
        size = run([os.path.join(PSPDEV, 'bin/psp-size'), os.path.join(d, 'obj-psp/game.elf')]).stdout.split('\n')[1].split()
        memory = int(size[3]) - 0x600000 + os.path.getsize(info['rom'])
        if memory > 52 << 20:
            fail('This game needs %d MB and a PSP has at most 64.' % (memory >> 20))
        say(event='packaging', id='assemble')
        shutil.copy(info['rom'], os.path.join(build, 'game.rom'))
        folder = os.path.join(out, name + ' (PSP)')
        os.makedirs(out, exist_ok=True)
        shutil.rmtree(folder, ignore_errors=True)
        shutil.move(build, folder)
        say(event='packaged', app=folder, name=name + (' (needs a PSP-2000 or later)' if memory > 22 << 20 else ''),
            bytes=sum(os.path.getsize(os.path.join(folder, f)) for f in os.listdir(folder)))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def package_ppc(info, game, name, out):
    """A disk for Mac OS 9: the app starts the game.rom beside it."""
    if not info.get('os9_library') or not os.path.exists(info['os9_library']):
        fail('This game has no PowerPC library yet. Only SNES games can be built for Mac OS 9 so far.')
    say(event='packaging', id='runtime')
    must(['make', 'os9-runtime'])
    app = os.path.join(REPO, 'build/os9/runtime/Genexia.bin')
    sizes = sum(os.path.getsize(p) for p in (app, info['os9_library'], info['rom']))
    say(event='packaging', id='disk')
    os.makedirs(out, exist_ok=True)
    disk = os.path.join(out, name + ' (PowerPC).dsk')
    with open(disk, 'wb') as f:
        f.truncate((sizes * 11 // 10 + (4 << 20)) // 512 * 512)
    env = dict(os.environ, PATH=os.path.join(RETRO68, 'bin') + ':' + os.environ['PATH'])
    hfs_name = re.sub(r'[:]', ' ', name)[:27]
    for cmd in (['hformat', '-l', hfs_name, disk], ['hcopy', '-m', app, ':' + hfs_name],
                ['hcopy', '-m', info['os9_library'], ':%s.lib' % game], ['hcopy', '-r', info['rom'], ':game.rom'], ['humount']):
        r = run(cmd, env)
        if r.returncode:
            fail(cmd[0] + ' failed: ' + r.stdout[-300:])
    say(event='packaged', app=disk, name=name, bytes=os.path.getsize(disk))


def bundle_sdl(app, intel=False):
    frameworks = os.path.join(app, 'Contents/Frameworks')
    os.makedirs(frameworks)
    if intel:
        shutil.copy(SDL_LOCAL, frameworks)
        return
    exe = os.path.join(app, 'Contents/MacOS/genexia')
    prefix = must(['sdl2-config', '--prefix']).strip()
    sdl2 = next(l.split()[0] for l in must(['otool', '-L', exe]).splitlines() if 'libSDL2' in l)
    shutil.copy(os.path.join(prefix, 'lib/libSDL2-2.0.0.dylib'), frameworks)
    must(['install_name_tool', '-change', sdl2, '@executable_path/../Frameworks/libSDL2-2.0.0.dylib', exe])
    # Homebrew's SDL2 is sdl2-compat, which loads SDL3 from beside itself
    for p in (os.path.join(prefix, 'lib/libSDL3.dylib'), '/opt/homebrew/lib/libSDL3.dylib'):
        if os.path.exists(p):
            sdl3 = os.path.join(frameworks, 'libSDL3.dylib')
            shutil.copy(p, sdl3)
            must(['install_name_tool', '-id', '@loader_path/libSDL3.dylib', sdl3])
            break


def bundle_dylibs(exe, frameworks):
    """Copies every Homebrew library exe needs, and theirs, beside it and points each at its copy."""
    os.makedirs(frameworks, exist_ok=True)
    todo, done = [exe], set()
    while todo:
        f = todo.pop()
        for line in must(['otool', '-L', f]).splitlines()[1:]:
            dep = line.split()[0]
            if not dep.startswith('/opt/homebrew/'):
                continue
            base = os.path.basename(dep)
            copy = os.path.join(frameworks, base)
            if base not in done:
                done.add(base)
                shutil.copy(os.path.realpath(dep), copy)
                os.chmod(copy, 0o755)
                must(['install_name_tool', '-id', '@loader_path/' + base, copy])
                todo.append(copy)
            new = ('@executable_path/../Frameworks/' if f == exe else '@loader_path/') + base
            must(['install_name_tool', '-change', dep, new, f])
    # Homebrew's SDL2 is sdl2-compat, which loads SDL3 from beside itself
    if 'libSDL2-2.0.0.dylib' in done and os.path.exists('/opt/homebrew/lib/libSDL3.dylib'):
        sdl3 = os.path.join(frameworks, 'libSDL3.dylib')
        shutil.copy(os.path.realpath('/opt/homebrew/lib/libSDL3.dylib'), sdl3)
        os.chmod(sdl3, 0o755)
        must(['install_name_tool', '-id', '@loader_path/libSDL3.dylib', sdl3])


def psp_game_txt(game, d):
    """game.txt: which disc the player looks for, the program it must hold, and the settings.
    A package carries neither: the user puts their own disc beside it."""
    with open(os.path.join(d, 'game.elf'), 'rb') as f:
        elf = f.read()
    ph = struct.unpack_from('<I', elf, 28)[0]
    off, _, _, size = struct.unpack_from('<4I', elf, ph + 4)
    crc = zlib.crc32(elf[off:off + size])
    return f'id {game}\ncrc {crc:08X}\n' + ''.join(f'{k} {v}\n' for k, v in psp_settings(d).items())


PSP_README = """{name}

Put your own copy of the game's disc beside this, as an .iso file: disc {game}.
Any file name works. The player finds the disc by its ID and checks that it is the
same release Genexia recompiled. Saves are kept in your user folder, not here.
"""


def psp_dump_note(game, rom):
    """For a disc whose program is encrypted: the user's own decrypted dump goes beside it too."""
    sys.path.insert(0, os.path.join(REPO, 'tools'))
    import psp_game
    boot = psp_game.Disc(rom).read('PSP_GAME/SYSDIR/BOOT.BIN')
    if boot and boot[:4] == b'\x7fELF':
        return ''
    return (f"\nThis disc's program is encrypted. Put the decrypted copy you dumped from it beside\n"
            f"the disc as well: PPSSPP's {game}_EBOOT.BIN, under any name ending in .bin or .elf.\n")


def psp_settings(d):
    """The game's options, as the settings its player reads."""
    o = {}
    try:
        for line in open(os.path.join(d, 'options.txt')):
            k, _, v = line.partition(' ')
            o[k] = int(v)
    except (OSError, ValueError):
        pass
    env = {'PSP_SCALE': o.get('scale', 4), 'PSP_MSAA': o.get('msaa', 4),
           'PSP_SIZE': '960x544' if o.get('window', 1) == 0 else '1920x1088'}
    if o.get('window') == 2:
        env['PSP_FULLSCREEN'] = 1
    if o.get('fps'):
        env['PSP_FPS'] = o['fps']
    return env


TOOLCHAINS = os.path.expanduser('~/Library/Application Support/Genexia/toolchains')
PC = {
    'windows': {'zig': 'x86_64-windows-gnu', 'label': 'Windows',
                'ffmpeg': 'ffmpeg-win/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0',
                'sdl': 'sdl2-win/SDL2-2.32.10/x86_64-w64-mingw32',
                'libs': ['avcodec-63.dll', 'avutil-61.dll', 'swresample-7.dll']},
    'linux': {'zig': 'x86_64-linux-gnu.2.28', 'label': 'Linux',
              'ffmpeg': 'ffmpeg-linux/ffmpeg-n9.0-latest-linux64-lgpl-shared-9.0',
              'libs': ['libavcodec.so.63', 'libavutil.so.61', 'libswresample.so.7']},
}


def zig():
    z = shutil.which('zig') or next((p for p in ('/opt/homebrew/bin/zig', '/usr/local/bin/zig') if os.path.exists(p)), None)
    if not z:
        fail('Packaging for Windows and Linux needs zig: brew install zig')
    return z


def psp_objects(d, system, cc):
    """The game's recompiled code, compiled for another system. Kept, and redone only where the code changed."""
    code, objects = os.path.join(d, 'code'), os.path.join(d, 'obj-' + system)
    os.makedirs(objects, exist_ok=True)
    sources = sorted(n for n in os.listdir(code) if n.endswith('.c'))
    todo = [n for n in sources if not os.path.exists(os.path.join(objects, n[:-2] + '.o'))
            or os.path.getmtime(os.path.join(objects, n[:-2] + '.o')) < os.path.getmtime(os.path.join(code, n))]

    def one(name):
        out = os.path.join(objects, name[:-2] + '.o')
        r = subprocess.run(cc + ['-O1', '-c', os.path.join(code, name), '-o', out + '.new'], capture_output=True, text=True)
        if not r.returncode:
            os.replace(out + '.new', out)
        return r

    finished = 0
    say(event='progress', label='Compiling', done=0, total=len(todo))
    with cf.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for r in pool.map(one, todo):
            if r.returncode:
                fail('The compiler stopped: ' + r.stderr.strip()[:300])
            finished += 1
            say(event='progress', label='Compiling', done=finished, total=len(todo))
    return [os.path.join(objects, n[:-2] + '.o') for n in sources]


def windows_icon(png_path, ico, work):
    """An .ico holding PNG pictures, which Windows reads since Vista."""
    pictures = []
    for size in (256, 48, 32, 16):
        f = os.path.join(work, f'icon{size}.png')
        must(['sips', '-z', str(size), str(size), png_path, '-s', 'format', 'png', '--out', f])
        pictures.append((size, open(f, 'rb').read()))
    with open(ico, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, len(pictures)))
        at = 6 + 16 * len(pictures)
        for size, data in pictures:
            f.write(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), at))
            at += len(data)
        for _, data in pictures:
            f.write(data)


def package_psp_pc(info, game, name, out, d, system):
    """A folder for Windows or Linux on x86-64: the OpenGL player with the game's recompiled
    code, game.txt and FFmpeg. The user's disc goes beside it. Linux uses the system's SDL2."""
    t = PC[system]
    z = zig()
    ffmpeg = os.path.join(TOOLCHAINS, t['ffmpeg'])
    if not os.path.isdir(ffmpeg) or (system == 'windows' and not os.path.isdir(os.path.join(TOOLCHAINS, t['sdl']))):
        fail(f'Packaging for {t["label"]} needs its SDL2 and FFmpeg builds in {TOOLCHAINS}.')
    src, build = os.path.join(REPO, 'src/backends/psp'), os.path.join(REPO, 'build')
    subprocess.run([sys.executable, os.path.join(REPO, 'tools/psp_nids.py'), os.path.join(build, 'psp_nids.h')], check=True, capture_output=True)
    cc = [z, 'cc', '-target', t['zig'], '-w', '-std=gnu11', '-I' + os.path.join(REPO, 'src/chips'), '-I' + build, '-I' + src]
    say(event='packaging', id='compile')
    objects = psp_objects(d, system, cc)

    folder = os.path.join(out, f'{name} ({t["label"]})')
    work = tempfile.mkdtemp(prefix='genexia-package')
    try:
        new = os.path.join(work, 'game')
        os.makedirs(new)
        say(event='packaging', id='runtime')
        cc += ['-O2', '-DPSP_WINDOW', '-DPSP_GL', '-I' + os.path.join(ffmpeg, 'include')]
        player = [os.path.join(src, n) for n in ('psp_runtime.c', 'psp_vfpu.c', 'psp_ge.c', 'psp_gl.c')]
        if system == 'windows':
            sdl = os.path.join(TOOLCHAINS, t['sdl'])
            cc += ['-I' + os.path.join(sdl, 'include/SDL2')]
            player.append(os.path.join(src, 'psp_os_win.c'))
            shot = os.path.join(d, 'cartridge.png')
            shot = shot if os.path.exists(shot) else os.path.join(d, 'shot.png')
            if os.path.exists(shot):
                say(event='packaging', id='icon')
                windows_icon(shot, os.path.join(work, 'game.ico'), work)
                with open(os.path.join(work, 'game.rc'), 'w') as f:
                    f.write('1 ICON "game.ico"\n')
                player.append(os.path.join(work, 'game.rc'))
            exe = os.path.join(new, name + '.exe')
            libs = ['-L' + os.path.join(sdl, 'lib'), '-L' + os.path.join(ffmpeg, 'lib'), '-lmingw32', '-lSDL2main', '-lSDL2',
                    '-lavcodec', '-lavutil', '-lgdi32', '-lwinmm', '-limm32', '-lole32', '-loleaut32', '-lversion',
                    '-lsetupapi', '-luuid', '-lcfgmgr32', '-Wl,--subsystem,windows', '-s']
            lib_from, lib_to = os.path.join(ffmpeg, 'bin'), new
        else:
            sdl_headers = subprocess.run(['sdl2-config', '--cflags'], capture_output=True, text=True).stdout.split()
            cc += [f for f in sdl_headers if f.startswith('-I')]
            stub = os.path.join(work, 'sdl')        # the system's SDL2, by its stable library name
            os.makedirs(stub)
            names = set()
            for n in player:
                r = subprocess.run(cc + ['-c', n, '-o', os.path.join(stub, os.path.basename(n) + '.o')], capture_output=True, text=True)
                if r.returncode:
                    fail('The player would not compile: ' + r.stderr.strip()[:300])
                names |= set(re.findall(r'\b(SDL_\w+)\b', subprocess.run(['nm', '-u', os.path.join(stub, os.path.basename(n) + '.o')],
                                                                         capture_output=True, text=True).stdout))
            with open(os.path.join(stub, 'stub.c'), 'w') as f:
                f.write(''.join(f'void {n}(void) {{}}\n' for n in sorted(names)))
            must([z, 'cc', '-target', t['zig'], '-shared', '-Wl,-soname,libSDL2-2.0.so.0', os.path.join(stub, 'stub.c'),
                  '-o', os.path.join(stub, 'libSDL2-2.0.so.0')])
            player = [os.path.join(stub, os.path.basename(n) + '.o') for n in player]
            exe = os.path.join(new, name)
            libs = ['-L' + stub, '-l:libSDL2-2.0.so.0', '-L' + os.path.join(ffmpeg, 'lib'), '-Wl,--no-as-needed',
                    '-lswresample', '-lavcodec', '-lavutil', '-lm', "-Wl,-rpath,$ORIGIN/lib", '-s']
            lib_from, lib_to = os.path.join(ffmpeg, 'lib'), os.path.join(new, 'lib')
        r = subprocess.run(cc + player + objects + libs + ['-o', exe], capture_output=True, text=True, cwd=work)
        if r.returncode:
            fail('Linking stopped: ' + r.stderr.strip()[:300])

        say(event='packaging', id='assemble')
        os.makedirs(lib_to, exist_ok=True)
        for n in t['libs']:
            shutil.copy(os.path.join(lib_from, n), os.path.join(lib_to, n))
        shutil.copy(os.path.join(ffmpeg, 'LICENSE.txt'), os.path.join(lib_to, 'FFmpeg LICENSE.txt'))
        with open(os.path.join(new, 'game.txt'), 'w') as f:
            f.write(psp_game_txt(game, d))
        with open(os.path.join(new, 'Put your disc here.txt'), 'w', newline='\r\n' if system == 'windows' else '\n') as f:
            f.write(PSP_README.format(name=name, game=game) + psp_dump_note(game, info['rom']))
        os.makedirs(out, exist_ok=True)
        if os.path.exists(folder):
            shutil.rmtree(folder)
        shutil.move(new, folder)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(folder) for f in fs)
    say(event='packaged', app=folder, name=name, bytes=size)


def package_psp_mac(info, game, name, out, d):
    """A Mac app holding the PSP player and the game's recompiled code. The user's disc goes beside it."""
    if not os.path.exists(os.path.join(d, 'play_metal')):
        fail('This game has not been compiled yet.')
    sys.path.insert(0, os.path.join(REPO, 'tools'))
    import psp_game
    say(event='packaging', id='runtime')
    psp_game.link(d)                    # the player, current with the runtime
    app = os.path.join(out, name + '.app')
    work = tempfile.mkdtemp(prefix='genexia-package')
    try:
        say(event='packaging', id='icon')
        new = os.path.join(work, name + '.app')
        res, macos = os.path.join(new, 'Contents/Resources'), os.path.join(new, 'Contents/MacOS')
        os.makedirs(res)
        os.makedirs(macos)
        shot = os.path.join(d, 'cartridge.png')     # the UMD the Recompiler draws, else the disc's picture
        shot = shot if os.path.exists(shot) else os.path.join(d, 'shot.png')
        if os.path.exists(shot):
            make_icon(None, os.path.join(res, 'game.icns'), work, shot)

        say(event='packaging', id='assemble')
        exe = os.path.join(macos, 'play_metal')
        shutil.copy(os.path.join(d, 'play_metal'), exe)
        with open(os.path.join(res, 'game.txt'), 'w') as f:
            f.write(psp_game_txt(game, d))
        bundle_dylibs(exe, os.path.join(new, 'Contents/Frameworks'))
        with open(os.path.join(new, 'Contents/Info.plist'), 'wb') as f:
            plist = {
                'CFBundleExecutable': 'play_metal',
                'CFBundleIdentifier': 'local.genexia.game.' + game,
                'CFBundleName': name,
                'CFBundleDisplayName': name,
                'CFBundlePackageType': 'APPL',
                'CFBundleShortVersionString': '1.0',
                'LSMinimumSystemVersion': '11.0',
                'NSHighResolutionCapable': True,
            }
            if os.path.exists(shot):
                plist['CFBundleIconFile'] = 'game'
            plistlib.dump(plist, f)
        must(['codesign', '--force', '-s', '-', '--deep', new])
        os.makedirs(out, exist_ok=True)
        if os.path.exists(app):
            shutil.rmtree(app)
        shutil.move(new, app)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(app) for f in fs)
    say(event='packaged', app=app, name=name, bytes=size)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    game, out, target = a[0], OUT, 'arm'
    for k, v in zip(a[1::2], a[2::2]):
        if k == '--out':
            out = v
        elif k == '--target' and v in ('arm', 'intel', 'ppc', 'psp', 'windows', 'linux'):
            target = v
        else:
            sys.exit('bad option ' + k)
    intel = target == 'intel'
    d = os.path.join(GAMES, game)
    try:
        with open(os.path.join(d, 'info.json')) as f:
            info = json.load(f)
    except OSError:
        fail('This game has not been built yet.')
    if info['console'] == 'PSP':
        if target not in ('arm', 'windows', 'linux'):
            fail('PSP games package for Apple Silicon Macs, Windows and Linux so far.')
        if not os.path.exists(info['rom']):
            fail('The disc has moved: ' + info['rom'])
        if target != 'arm':
            return package_psp_pc(info, game, app_name(info), out, d, target)
        return package_psp_mac(info, game, app_name(info), out, d)
    lib = os.path.join(d, 'game.dylib')
    if not os.path.exists(lib):
        fail('This game has no native library yet. Build it first.')
    if not os.path.exists(info['rom']):
        fail('The ROM has moved: ' + info['rom'])

    name = app_name(info)
    if target == 'ppc':
        return package_ppc(info, game, name, out)
    if target == 'psp':
        return package_psp(info, game, name, out, d)
    say(event='packaging', id='runtime')
    must(['make', '-j8', 'genexia-runtime'])
    if intel:
        intel_runtime()
        lib_for_app = intel_library(d, info['console'])
    else:
        lib_for_app = lib

    title = name + (' (Intel)' if intel else '')
    app = os.path.join(out, title + '.app')
    work = tempfile.mkdtemp(prefix='genexia-package')
    try:
        say(event='packaging', id='icon')
        new = os.path.join(work, title + '.app')
        res = os.path.join(new, 'Contents/Resources')
        os.makedirs(res)
        os.makedirs(os.path.join(new, 'Contents/MacOS'))
        cartridge = os.path.join(d, 'cartridge.png')       # drawn by the Recompiler
        cartridge = cartridge if os.path.exists(cartridge) else None
        shot = None if cartridge else screenshot(info['rom'], lib, work)
        if cartridge or shot:
            make_icon(shot, os.path.join(res, 'game.icns'), work, cartridge)

        say(event='packaging', id='assemble')
        shutil.copy(RUNTIME_X86 if intel else RUNTIME, os.path.join(new, 'Contents/MacOS/genexia'))
        shutil.copy(info['rom'], os.path.join(res, 'game.rom'))
        shutil.copy(lib_for_app, os.path.join(res, 'game.dylib'))
        options = os.path.join(d, 'options.txt')
        if os.path.exists(options):
            shutil.copy(options, os.path.join(res, 'options.txt'))
        else:
            open(os.path.join(res, 'options.txt'), 'w').close()
        bundle_sdl(new, intel)
        with open(os.path.join(new, 'Contents/Info.plist'), 'wb') as f:
            plist = {
                'CFBundleExecutable': 'genexia',
                'CFBundleIdentifier': 'local.genexia.game.' + game + ('.intel' if intel else ''),
                'CFBundleName': name,
                'CFBundleDisplayName': name,
                'CFBundlePackageType': 'APPL',
                'CFBundleShortVersionString': '1.0',
                'CFBundleVersion': str(info.get('built', 1)),
                'LSMinimumSystemVersion': '10.15' if intel else '11.0',
                'NSHighResolutionCapable': True,
            }
            if cartridge or shot:
                plist['CFBundleIconFile'] = 'game'
            plistlib.dump(plist, f)
        must(['codesign', '--force', '-s', '-', '--deep', new])

        os.makedirs(out, exist_ok=True)
        if os.path.exists(app):
            shutil.rmtree(app)
        shutil.move(new, app)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(app) for f in fs)
    say(event='packaged', app=app, name=name, bytes=size)


if __name__ == '__main__':
    main()

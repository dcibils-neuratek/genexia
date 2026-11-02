#!/usr/bin/env python3
"""Package a built game as a double-clickable Mac app.

    tools/package_game.py GAME_ID [--target arm|intel|ppc] [--out DIR]

The app holds the emulators with no game built in, the game's ROM, its native
library, its options and SDL, so it runs with neither Genexia nor this project
around. It contains the ROM: it is for your own use, and it lands outside the
repository so it can't be committed.

arm and intel make a Mac app. ppc makes an HFS disk image for Mac OS 9 holding
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


def png(path, size, rgba):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    rows = b''.join(b'\0' + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
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


def make_icon(shot, target, d):
    iconset = os.path.join(d, 'game.iconset')
    os.makedirs(iconset)
    for size, names in ((16, ['16x16']), (32, ['16x16@2x', '32x32']), (64, ['32x32@2x']),
                        (128, ['128x128']), (256, ['128x128@2x', '256x256']),
                        (512, ['256x256@2x', '512x512'])):
        image = icon_image(shot, size)
        for n in names:
            png(os.path.join(iconset, f'icon_{n}.png'), size, image)
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


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    game, out, target = a[0], OUT, 'arm'
    for k, v in zip(a[1::2], a[2::2]):
        if k == '--out':
            out = v
        elif k == '--target' and v in ('arm', 'intel', 'ppc'):
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
    lib = os.path.join(d, 'game.dylib')
    if not os.path.exists(lib):
        fail('This game has no native library yet. Build it first.')
    if not os.path.exists(info['rom']):
        fail('The ROM has moved: ' + info['rom'])

    name = app_name(info)
    if target == 'ppc':
        return package_ppc(info, game, name, out)
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
        shot = screenshot(info['rom'], lib, work)
        if shot:
            make_icon(shot, os.path.join(res, 'game.icns'), work)

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
            if shot:
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

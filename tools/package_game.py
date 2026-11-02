#!/usr/bin/env python3
"""Package a built game as a double-clickable Mac app.

    tools/package_game.py GAME_ID [--out DIR]

The app holds the emulators with no game built in, the game's ROM, its native
library, its options and SDL, so it runs with neither Genexia nor this project
around. It contains the ROM: it is for your own use, and it lands outside the
repository so it can't be committed. Prints one line of JSON per event, like
genexia_recompile.py.
"""
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


def bundle_sdl(app):
    frameworks = os.path.join(app, 'Contents/Frameworks')
    os.makedirs(frameworks)
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
    game, out = a[0], OUT
    if a[1:3] and a[1] == '--out':
        out = a[2]
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

    say(event='packaging', step='Building the emulators')
    must(['make', '-j8', 'genexia-runtime'])

    name = app_name(info)
    app = os.path.join(out, name + '.app')
    work = tempfile.mkdtemp(prefix='genexia-package')
    try:
        say(event='packaging', step='Making the icon')
        new = os.path.join(work, name + '.app')
        res = os.path.join(new, 'Contents/Resources')
        os.makedirs(res)
        os.makedirs(os.path.join(new, 'Contents/MacOS'))
        shot = screenshot(info['rom'], lib, work)
        if shot:
            make_icon(shot, os.path.join(res, 'game.icns'), work)

        say(event='packaging', step='Putting the game together')
        shutil.copy(RUNTIME, os.path.join(new, 'Contents/MacOS/genexia'))
        shutil.copy(info['rom'], os.path.join(res, 'game.rom'))
        shutil.copy(lib, os.path.join(res, 'game.dylib'))
        options = os.path.join(d, 'options.txt')
        if os.path.exists(options):
            shutil.copy(options, os.path.join(res, 'options.txt'))
        else:
            open(os.path.join(res, 'options.txt'), 'w').close()
        bundle_sdl(new)
        with open(os.path.join(new, 'Contents/Info.plist'), 'wb') as f:
            plist = {
                'CFBundleExecutable': 'genexia',
                'CFBundleIdentifier': 'local.genexia.game.' + game,
                'CFBundleName': name,
                'CFBundleDisplayName': name,
                'CFBundlePackageType': 'APPL',
                'CFBundleShortVersionString': '1.0',
                'CFBundleVersion': str(info.get('built', 1)),
                'LSMinimumSystemVersion': '11.0',
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

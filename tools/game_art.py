#!/usr/bin/env python3
"""Pictures for a game's cartridge: game_art.py <game id>.

shot.png is a moment of the game's own attract mode. art.png is its box art from
the libretro-thumbnails collection, found by the ROM's file name; art.none records
that there is none. Both stay in the game's library folder, never in this repo.
"""
import json
import os
import re
import sys
import tempfile
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from package_game import GAMES, app_name, png, read_ppm, screenshot

COLLECTIONS = {
    'SNES': 'Nintendo_-_Super_Nintendo_Entertainment_System',
    'Nintendo 64': 'Nintendo_-_Nintendo_64',
    'Genesis': 'Sega_-_Mega_Drive_-_Genesis',
}
REGIONS = ('USA', 'USA, Europe', 'World', 'Europe', 'Japan', 'Japan, USA')


def save_shot(info, d):
    with tempfile.TemporaryDirectory(prefix='genexia-art') as work:
        shot = screenshot(info['rom'], os.path.join(d, 'game.dylib'), work)
        if not shot:
            return
        w, h, px = read_ppm(shot)
        rgba = bytearray(b'\xff' * (w * h * 4))
        rgba[0::4], rgba[1::4], rgba[2::4] = px[0::3], px[1::3], px[2::3]
        png(os.path.join(d, 'shot.png'), w, bytes(rgba), h)


def fetch_art(info, d):
    collection = COLLECTIONS.get(info['console'])
    name = re.sub(r'[&*/:`<>?\\|"]', '_', app_name(info))
    names = dict.fromkeys((name, re.sub(r' \d+$', '', name)))     # "Game 2.sfc" is a second copy of Game
    for name, region in ((n, r) for n in names for r in REGIONS if collection):
        file = urllib.parse.quote(f'{name} ({region}).png')
        url = f'https://raw.githubusercontent.com/libretro-thumbnails/{collection}/master/Named_Boxarts/{file}'
        try:
            with urllib.request.urlopen(url, timeout=15) as r:
                data = r.read()
        except urllib.error.HTTPError:
            continue
        except OSError:
            return                      # offline: try again another day
        with open(os.path.join(d, 'art.png'), 'wb') as f:
            f.write(data)
        return
    open(os.path.join(d, 'art.none'), 'w').close()


def main():
    d = os.path.join(GAMES, sys.argv[1])
    with open(os.path.join(d, 'info.json')) as f:
        info = json.load(f)
    if os.path.exists(info['rom']) and os.path.exists(os.path.join(d, 'game.dylib')) \
            and not os.path.exists(os.path.join(d, 'shot.png')):
        save_shot(info, d)
    if not os.path.exists(os.path.join(d, 'art.png')) and not os.path.exists(os.path.join(d, 'art.none')):
        fetch_art(info, d)


if __name__ == '__main__':
    main()

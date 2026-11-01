#!/usr/bin/env python3
"""montage.py out.ppm in1.ppm in2.ppm ... : tile PPM screenshots 4 per row (half size)."""
import sys
def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]
ims = [load(p) for p in sys.argv[2:]]
cols = 4; tw, th = max(w for w, _, _ in ims) // 2, 120
rows = (len(ims) + cols - 1) // cols
W, H = cols * tw, rows * th
out = bytearray(W * H * 3)
for i, (w, h, px) in enumerate(ims):
    ox, oy = (i % cols) * tw, (i // cols) * th
    for y in range(min(th, h // 2)):
        for x in range(min(tw, w // 2)):
            s = ((y * 2) * w + x * 2) * 3
            d = ((oy + y) * W + ox + x) * 3
            out[d:d + 3] = px[s:s + 3]
open(sys.argv[1], 'wb').write(b'P6\n%d %d\n255\n' % (W, H) + out)

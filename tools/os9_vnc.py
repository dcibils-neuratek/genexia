#!/usr/bin/env python3
"""A small VNC client for looking at and driving a Mac on the network.

    os9_vnc.py HOST shot FILE.png
    os9_vnc.py HOST click X Y [double]
    os9_vnc.py HOST move X Y
    os9_vnc.py HOST type TEXT
    os9_vnc.py HOST key NAME [NAME...]     return esc tab up down left right f1-f12 cmd+q

Only for servers with no password.
"""
import socket
import struct
import sys
import time
import zlib

KEYS = {'return': 0xFF0D, 'esc': 0xFF1B, 'tab': 0xFF09, 'space': 0x20, 'backspace': 0xFF08, 'left': 0xFF51,
        'up': 0xFF52, 'right': 0xFF53, 'down': 0xFF54, 'cmd': 0xFFE7, 'shift': 0xFFE1, 'ctrl': 0xFFE3, 'option': 0xFFE9}
KEYS.update({'f%d' % n: 0xFFBD + n for n in range(1, 13)})


class Vnc:
    def __init__(self, host, port=5900):
        self.s = socket.create_connection((host, port), timeout=30)
        assert self.need(12).startswith(b'RFB')
        self.s.sendall(b'RFB 003.003\n')
        sec = struct.unpack('>I', self.need(4))[0]
        assert sec == 1, 'server wants security type %d' % sec
        self.s.sendall(b'\x01')
        self.w, self.h = struct.unpack('>HH', self.need(4))
        self.need(16)
        self.name = self.need(struct.unpack('>I', self.need(4))[0]).decode('mac_roman')
        self.s.sendall(struct.pack('>B3xBBBBHHHBBB3x', 0, 32, 24, 1, 1, 255, 255, 255, 16, 8, 0))
        self.s.sendall(struct.pack('>BxHi', 2, 1, 0))

    def need(self, n):
        b = bytearray()
        while len(b) < n:
            c = self.s.recv(n - len(b))
            assert c, 'connection closed'
            b += c
        return bytes(b)

    def update(self, fb):
        """Reads one framebuffer update into fb; returns the rectangles it held."""
        while True:
            t = self.need(1)[0]
            if t == 2:
                continue
            if t == 1:
                self.need(5)
                self.need(struct.unpack('>H', self.need(2))[0] * 6)
                continue
            if t == 3:
                self.need(struct.unpack('>I', self.need(7)[3:])[0])
                continue
            assert t == 0, t
            self.need(1)
            rects = []
            for _ in range(struct.unpack('>H', self.need(2))[0]):
                x, y, w, h, enc = struct.unpack('>HHHHi', self.need(12))
                assert enc == 0, enc
                data = self.need(w * h * 4)
                for r in range(h):
                    at = ((y + r) * self.w + x) * 4
                    fb[at:at + w * 4] = data[r * w * 4:(r + 1) * w * 4]
                rects.append((x, y, w, h))
            return rects

    def frame(self):
        """The server answers in tiles, a few at a time: ask until they cover the screen."""
        fb = bytearray(self.w * self.h * 4)
        seen = set()
        for _ in range(400):
            self.s.sendall(struct.pack('>BBHHHH', 3, 0, 0, 0, self.w, self.h))
            seen.update(self.update(fb))
            if sum(r[2] * r[3] for r in seen) >= self.w * self.h:
                break
        return bytes(fb)

    def png(self, path):
        fb, w, h = self.frame(), self.w, self.h
        rows = bytearray()
        for y in range(h):
            rows.append(0)
            line = fb[y * w * 4:(y + 1) * w * 4]    # x R G B
            rows += bytes(c for i in range(0, len(line), 4) for c in line[i + 1:i + 4])

        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
        open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                               chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))

    def pointer(self, x, y, down=0):
        self.s.sendall(struct.pack('>BBHH', 5, down, x, y))

    def click(self, x, y, times=1):
        self.pointer(x, y)
        time.sleep(0.15)
        for _ in range(times):
            self.pointer(x, y, 1)
            time.sleep(0.08)
            self.pointer(x, y, 0)
            time.sleep(0.08)

    def keysym(self, sym, down):
        self.s.sendall(struct.pack('>BBxxI', 4, down, sym))

    def press(self, name):
        syms = [KEYS.get(p, ord(p[0])) for p in name.split('+')]
        for k in syms:
            self.keysym(k, 1)
            time.sleep(0.03)
        for k in reversed(syms):
            self.keysym(k, 0)
            time.sleep(0.03)


def main():
    host, what, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    v = Vnc(host)
    if what == 'shot':
        v.png(args[0])
        print('%s: %d x %d, %s' % (args[0], v.w, v.h, v.name))
    elif what == 'click':
        v.click(int(args[0]), int(args[1]), 2 if args[2:] else 1)
    elif what == 'move':
        v.pointer(int(args[0]), int(args[1]))
    elif what == 'type':
        for c in args[0]:
            v.press(c)
    elif what == 'key':
        for k in args:
            v.press(k)
    time.sleep(0.2)


if __name__ == '__main__':
    main()

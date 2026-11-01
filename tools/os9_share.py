#!/usr/bin/env python3
"""
os9_share.py - talk to a Mac OS 9 machine's File Sharing from a modern Mac.

macOS can no longer mount AFP 2.2, so this is a minimal client that logs in as
a guest and uses the first shared volume.

    os9_share.py HOST push MACBINARY NAME   upload an application
    os9_share.py HOST put FILE NAME         upload a plain file
    os9_share.py HOST get NAME              print a file
    os9_share.py HOST ls                    list the shared folder
"""
import socket, struct, sys

ROOT = 2


class Afp:
    def __init__(self, host):
        self.sock = socket.create_connection((host, 548), 6)
        self.sock.settimeout(30)
        self.req = 0
        assert self.dsi(4, b'\x01\x04\x00\x00\x80\x00')[0] == 0
        err, _ = self.afp(bytes([18]) + pstr(b'AFP2.2') + pstr(b'No User Authent'))
        if err:
            sys.exit('guest login refused (%d): is guest access on?' % err)
        err, b = self.afp(bytes([16, 0]))
        name = b[7:7 + b[6]]
        p = bytes([24, 0]) + struct.pack('>H', 0x20) + pstr(name)
        if len(p) & 1:
            p += b'\0'
        err, b = self.afp(p)
        assert err == 0, err
        self.vol = struct.unpack('>H', b[2:4])[0]

    def dsi(self, cmd, payload=b'', offset=0):
        self.req = (self.req + 1) & 0xFFFF
        self.sock.sendall(struct.pack('>BBHIII', 0, cmd, self.req, offset, len(payload), 0) + payload)
        while True:
            b = b''
            while len(b) < 16:
                b += self.sock.recv(65536)
            flags, _, req, err, size, _ = struct.unpack('>BBHiII', b[:16])
            b = b[16:]
            while len(b) < size:
                b += self.sock.recv(65536)
            if flags == 1 and req == self.req:
                return err, b

    def afp(self, payload):
        return self.dsi(2, payload)

    def spec(self, name):
        return struct.pack('>HI', self.vol, ROOT), b'\x02' + pstr(name.encode('mac_roman'))

    def create(self, name):
        v, p = self.spec(name)
        return self.afp(bytes([7, 0]) + v + p)[0]

    def delete(self, name):
        v, p = self.spec(name)
        return self.afp(bytes([8, 0]) + v + p)[0]

    def write_fork(self, name, flag, data):
        v, p = self.spec(name)
        err, b = self.afp(bytes([26, flag]) + v + struct.pack('>HH', 0, 3) + p)
        assert err == 0, ('open', name, err)
        fork = struct.unpack('>H', b[2:4])[0]
        for o in range(0, len(data), 4096):
            c = data[o:o + 4096]
            err, _ = self.dsi(6, bytes([33, 0]) + struct.pack('>Hii', fork, o, len(c)) + c, 12)
            assert err == 0, ('write', o, err)
        self.afp(bytes([4, 0]) + struct.pack('>H', fork))

    def read(self, name):
        v, p = self.spec(name)
        err, b = self.afp(bytes([26, 0]) + v + struct.pack('>HH', 0, 1) + p)
        assert err == 0, ('open', name, err)
        fork = struct.unpack('>H', b[2:4])[0]
        out = b''
        while True:
            err, d = self.afp(bytes([27, 0]) + struct.pack('>HiiBB', fork, len(out), 4096, 0, 0))
            out += d
            if err or not d:
                break
        self.afp(bytes([4, 0]) + struct.pack('>H', fork))
        return out

    def set_type(self, name, ftype, creator):
        v, p = self.spec(name)
        q = bytes([30, 0]) + v + struct.pack('>H', 0x20) + p
        if len(q) & 1:
            q += b'\0'
        err, _ = self.afp(q + ftype + creator + b'\0' * 24)
        assert err == 0, err

    def names(self):
        out, start = [], 1
        while True:
            q = bytes([9, 0]) + struct.pack('>HIHHHHH', self.vol, ROOT, 0x40, 0x40, 64, start, 4096) + b'\x02\x00'
            err, b = self.afp(q)
            if err or len(b) < 6:
                return out
            count = struct.unpack('>H', b[4:6])[0]
            o = 6
            for _ in range(count):
                size = b[o]
                entry = b[o + 2:o + size]
                off = struct.unpack('>H', entry[:2])[0]
                n = entry[off]
                out.append(('/' if b[o + 1] & 0x80 else '') + entry[off + 1:off + 1 + n].decode('mac_roman'))
                o += size
            start += count

    def close(self):
        self.afp(bytes([20, 0]))
        self.dsi(1)


def pstr(x):
    return bytes([len(x)]) + x


def main():
    host, what = sys.argv[1], sys.argv[2]
    a = Afp(host)
    if what == 'push':
        m = open(sys.argv[3], 'rb').read()
        name = sys.argv[4]
        dl, rl = struct.unpack('>II', m[83:91])
        data = m[128:128 + dl]
        rsrc_at = 128 + ((dl + 127) & ~127)
        a.delete(name)
        assert a.create(name) == 0
        a.write_fork(name, 0, data)
        a.write_fork(name, 0x80, m[rsrc_at:rsrc_at + rl])
        a.set_type(name, m[65:69], m[69:73])
        assert a.read(name) == data, 'read back differs'
        print('%s: %d bytes, verified' % (name, len(data)))
    elif what == 'put':
        data = open(sys.argv[3], 'rb').read()
        a.delete(sys.argv[4])
        assert a.create(sys.argv[4]) == 0
        a.write_fork(sys.argv[4], 0, data)
        assert a.read(sys.argv[4]) == data, 'read back differs'
        print('%s: %d bytes, verified' % (sys.argv[4], len(data)))
    elif what == 'get':
        sys.stdout.write(a.read(sys.argv[3]).replace(b'\r', b'\n').decode('mac_roman'))
    elif what == 'ls':
        print('\n'.join(a.names()))
    a.close()


if __name__ == '__main__':
    main()

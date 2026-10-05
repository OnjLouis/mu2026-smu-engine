# license:BSD-3-Clause
import struct, subprocess, os, sys
from paths import WORK as OUT, ROMS, RENDER
TYPES = [('HALL1', 1, 0), ('HALL2', 1, 1), ('HALLM', 1, 6), ('HALLL', 1, 7),
         ('ROOM1', 2, 0), ('ROOM2', 2, 1), ('ROOM3', 2, 2), ('ROOMS', 2, 5), ('ROOMM', 2, 6), ('ROOML', 2, 7),
         ('STAGE1', 3, 0), ('STAGE2', 3, 1), ('PLATE', 4, 0), ('GMPLATE', 4, 7),
         ('WHITEROOM', 0x10, 0), ('TUNNEL', 0x11, 0), ('CANYON', 0x12, 0), ('BASEMENT', 0x13, 0)]


def vlq(n):
    out = bytearray([n & 0x7f]); n >>= 7
    while n:
        out.insert(0, (n & 0x7f) | 0x80); n >>= 7
    return bytes(out)


def sysex(d): return bytes([0xf0]) + vlq(len(d)) + bytes(d)
def xg(a): return sysex([0x43, 0x10, 0x4c] + a + [0xf7])


def build(path, msb, lsb, extra=()):
    ev = [(0, b'\xff\x51\x03' + struct.pack('>I', 500000)[1:]), (0, xg([0x00, 0x00, 0x7e, 0x00]))]
    ev.append((480, xg([0x02, 0x01, 0x00, msb, lsb])))
    for a in extra:
        ev.append((0, xg(a)))
    ev.append((0, b'\xb0\x5b\x7f'))
    ev.append((0, b'\xb0\x5d\x00'))
    ev.append((480, b'\x90\x3c\x64'))
    ev.append((240, b'\x80\x3c\x40'))
    ev.append((2880, b'\xff\x2f\x00'))
    trk = b''.join(vlq(d) + b for d, b in ev)
    open(path, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, 480) + b'MTrk' + struct.pack('>I', len(trk)) + trk)


if __name__ == '__main__':
    only = sys.argv[1:] or [t[0] for t in TYPES]
    for name, msb, lsb in TYPES:
        if name not in only:
            continue
        mid = os.path.join(OUT, 'rv_%s.mid' % name)
        build(mid, msb, lsb)
        subprocess.run([RENDER, ROMS, mid, os.path.join(OUT, 'rv_%s.wav' % name), '6',
                        '--dump-meg', os.path.join(OUT, 'rv_%s' % name)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        print(name, flush=True)

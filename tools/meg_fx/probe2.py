# license:BSD-3-Clause
"""コーラス・バリエーションの全種類を鳴らして MEG を書き出し、区画ごとの命令の形で分ける。
使い方: probe2.py cho|var|ins [render]"""
import os, sys, struct, subprocess, hashlib
from probe import vlq, xg, OUT, ROMS, RENDER
from group import load

kind = sys.argv[1]


class _tee:
    # 画面に出すものを WORK/<kind>_shapes.txt にも残す（mkmanifest.py・vartest.py が読む）
    def __init__(self, a, b):
        self.a, self.b = a, b

    def write(self, s):
        self.a.write(s)
        self.b.write(s)

    def flush(self):
        self.a.flush()
        self.b.flush()


sys.stdout = _tee(sys.stdout, open(os.path.join(OUT, '%s_shapes.txt' % kind), 'w', encoding='utf-8'))
do_render = len(sys.argv) > 2
types = []
for line in open(os.path.join(OUT, 'types.txt')):
    t, msb, lsb, name = line.split(' ', 3)
    if t == ('cho' if kind == 'cho' else 'var') and not (msb == '00' and lsb == '00'):
        types.append((int(msb, 16), int(lsb, 16), name.strip()))

ADDR = {'cho': [0x02, 0x01, 0x20], 'var': [0x02, 0x01, 0x40], 'ins': [0x03, 0x00, 0x00]}[kind]
os.makedirs(os.path.join(OUT, kind), exist_ok=True)


def build(path, msb, lsb):
    ev = [(0, b'\xff\x51\x03' + struct.pack('>I', 500000)[1:]), (0, xg([0x00, 0x00, 0x7e, 0x00]))]
    ev.append((480, xg(ADDR + [msb, lsb])))
    if kind == 'var':
        ev.append((0, xg([0x02, 0x01, 0x5a, 0x01])))   # バリエーションの接続を SYSTEM に（既定はインサーションで、どのパートにも掛かっていない）
    if kind == 'ins':
        ev.append((0, xg([0x03, 0x00, 0x0c, 0x00])))   # 挿入 1 をパート 1 に
    ev.append((0, b'\xb0\x5b\x00'))
    ev.append((0, b'\xb0\x5d\x7f' if kind == 'cho' else b'\xb0\x5d\x00'))
    ev.append((0, b'\xb0\x5e\x7f' if kind == 'var' else b'\xb0\x5e\x00'))
    ev.append((480, b'\x90\x3c\x64'))
    ev.append((240, b'\x80\x3c\x40'))
    ev.append((1440, b'\xff\x2f\x00'))
    trk = b''.join(vlq(d) + b for d, b in ev)
    open(path, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, 480) + b'MTrk' + struct.pack('>I', len(trk)) + trk)


def regions(mp):
    """区画ごとの pc の範囲"""
    res = {}
    for pc in range(0x180):
        key = (pc // 12) << 11
        for i in range(8):
            if i == 7 or mp[i + 1] <= mp[i] or (mp[i + 1] & 0xf800) > key:
                res.setdefault(i, []).append(pc)
                break
    return res


groups = {}
for msb, lsb, name in types:
    tag = '%02x%02x' % (msb, lsb)
    base = os.path.join(OUT, kind, tag)
    if do_render:
        build(base + '.mid', msb, lsb)
        subprocess.run([RENDER, ROMS, base + '.mid', base + '.wav', '4',
                        '--dump-meg', base], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    prg, const, off, mp = load(base + '.m')
    reg = regions(mp)
    k = {'cho': 1, 'var': 3, 'ins': 2}[kind]
    pcs = reg.get(k, [])
    words = tuple(prg[pc] for pc in pcs)
    h = hashlib.md5(repr(words).encode()).hexdigest()[:8]
    groups.setdefault(h, []).append(name)
    print('%s %-12s region%d pc %03x-%03x shape %s' % (tag, name, k, pcs[0] if pcs else 0, pcs[-1] if pcs else 0, h), flush=True)
print()
for h, names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    print(h, len(names), names)
print(len(groups), 'shapes')
sys.stdout.flush()

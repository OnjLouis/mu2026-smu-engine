# license:BSD-3-Clause
import os, hashlib
from probe import TYPES, OUT


def load(path):
    prg, const, off, mp = {}, {}, {}, {}
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == 'prg':
            prg[int(p[1], 16)] = int(p[2], 16)
            const[int(p[1], 16)] = int(p[3], 16)
        elif p[0] == 'off':
            off[int(p[1], 16)] = int(p[2], 16)
        elif p[0] == 'map':
            mp[int(p[1], 16)] = int(p[2], 16)
    return prg, const, off, mp


def region_end(mp):
    # 区画 0 の終わり（resolve_address と同じ選び方）
    for pc in range(0x180):
        key = (pc // 12) << 11
        if mp[1] > mp[0] and (mp[1] & 0xf800) <= key:
            return pc
    return 0x180


def main():
    groups = {}
    for name, msb, lsb in TYPES:
        prg, const, off, mp = load(os.path.join(OUT, 'rv_%s.m' % name))
        end = region_end(mp)
        words = tuple(prg[pc] for pc in range(end))
        h = hashlib.md5(repr(words).encode()).hexdigest()[:8]
        groups.setdefault(h, []).append(name)
        print('%-10s region0 %3d ops  map0 %04x map1 %04x  shape %s' % (name, end, mp[0], mp[1], h))
    print()
    for h, names in groups.items():
        print(h, names)


if __name__ == '__main__':
    main()

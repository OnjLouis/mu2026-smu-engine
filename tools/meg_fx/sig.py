# license:BSD-3-Clause
import os, sys, glob
from group import load

from paths import WORK as OUT


def fnv(prg, lo, hi):
    h = 0xcbf29ce484222325
    for pc in range(lo, hi):
        w = prg.get(pc, 0)
        for i in range(8):
            h ^= (w >> (8 * i)) & 0xff
            h = (h * 0x100000001b3) & 0xffffffffffffffff
    return h


if __name__ == "__main__":
  for f in sys.argv[1:] or sorted(glob.glob(os.path.join(OUT, '*.m'))):
    prg, const, off, mp = load(f)
    print('%-40s %016x  map0 %04x' % (os.path.basename(f), fnv(prg, 0, 0x98), mp[0]))

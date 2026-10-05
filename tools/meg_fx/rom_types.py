# license:BSD-3-Clause
"""ROM（mu2000.zip のプログラム ROM）から、エフェクトの種類の表を WORK/types.txt に書き出す。
xg/fx_types.h の set_fx_type_rom と同じ番地（MU2000 EX、firmware v2.01）。"""
import os
import zipfile
from paths import ROMS, WORK

z = zipfile.ZipFile(os.path.join(ROMS, 'mu2000.zip'))
h = z.read('mu2000-v2.01-h.bin')
l = z.read('mu2000-v2.01-l.bin')
b = bytearray()
for i in range(0, len(h), 2):          # ROM_LOAD32_WORD_SWAP
    b += bytes([h[i + 1], h[i], l[i + 1], l[i]])
NAMES = 0x2aeb6f
out = []
for tab, codes, n, first in (('rev', 0x2aea33, 19, 0), ('cho', 0x2aea59, 21, 19), ('var', 0x2aea83, 118, 40)):
    for i in range(n):
        name = b[NAMES + 10 * (first + i):NAMES + 10 * (first + i) + 10].decode('latin1').rstrip()
        out.append('%s %02x %02x %s' % (tab, b[codes + 2 * i], b[codes + 2 * i + 1], name))
open(os.path.join(WORK, 'types.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
print(len(out), 'types')

# license:BSD-3-Clause
"""manifest_extra.py を作る（バリエーションとインサーション 1 の、対応した形）"""
import os, re
from paths import WORK

CLS = {'0100': 'reverb', '0500': 'delay', '0900': 'er', '4102': 'chorus3', '4500': 'rotary', '4501': 'dt_rotary',
       '4800': 'phaser', '4900': 'dist', '4908': 'stereo_dist', '4c00': 'eq', '4e00': 'autowah', '5d00': 'talkmod',
       '5f00': 'dt_delay', '6100': 'wah_dt_delay', '6200': 'vdist', '6300': 'dual_rotary', '7300': 'isolator', '4701': 'autopan2', '4b01': 'ampsim2', '7100': 'ringmod', '7400': 'lowreso', '7500': 'turntable', '5e00': 'lofi', '6800': 'vflanger', '6900': 'multicomp', '6d00': 'dynaflt', '6e00': 'dynaflang', '6f00': 'dynaphase', '7000': 'dynaring', '7200': 'slice'}


def groups(path):
    tag_shape, members = {}, {}
    for l in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'([0-9a-f]{4}) (.{12}) region\d pc [0-9a-f]+-[0-9a-f]+ shape ([0-9a-f]{8})', l)
        if m:
            tag_shape[m.group(1)] = m.group(3)
            members.setdefault(m.group(3), []).append(m.group(2).strip())
    return tag_shape, members


lines = ['MANIFEST += [']
for kind, lo, hi, label in (('var', 0x120, 0x180, 'バリエーション'), ('ins', 0xc0, 0x120, 'インサーション 1')):
    tag_shape, members = groups(os.path.join(WORK, '%s_shapes.txt' % kind))
    for tag, name in CLS.items():
        lines.append("    ('%s/%s.m', 0x%x, 0x%x, 'meg_fx_%s_%s', '%s: %s')," % (
            kind, tag, lo, hi, kind, name, label, ', '.join(members[tag_shape[tag]])))
lines.append(']')
open(os.path.join(WORK, 'manifest_extra.py'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
print(len(lines) - 2, 'entries')

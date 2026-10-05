# license:BSD-3-Clause
"""バリエーションの種類ごとに、実機エミュ（var/XXXX.wav）と軽量モード（バリエーションの口だけ）を比べる。
使い方: vartest.py [形のハッシュ ...]（無ければ一覧の形すべて）"""
import os, re, subprocess, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', newline='\n')
HERE = os.path.dirname(os.path.abspath(__file__))
from paths import WORK, ROMS, RENDER
kind = os.environ.get('KIND', 'var')
slot = {'var': '4', 'ins': '8', 'cho': '2'}[kind]
shapes = {}
for l in open(os.path.join(WORK, '%s_shapes.txt' % kind), encoding='utf-8', errors='replace'):
    m = re.match(r'([0-9a-f]{4}) (.{12}) region\d pc [0-9a-f]+-[0-9a-f]+ shape ([0-9a-f]{8})', l)
    if m:
        shapes.setdefault(m.group(3), []).append((m.group(1), m.group(2).strip()))
want = sys.argv[1:] or list(shapes)
env = dict(os.environ, SMU2000_NATIVE_SLOTS=slot)
for h in want:
    for tag, name in shapes[h]:
        base = os.path.join(WORK, kind, tag)
        subprocess.run([RENDER, ROMS, base + '.mid', base + '_n.wav', '4', '--native-fx'],
                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        r = subprocess.run([sys.executable, os.path.join(HERE, 'bands.py'), base + '.wav', base + '_n.wav', '8.5'], capture_output=True, text=True)
        print('%s %s %-12s %s' % (h, tag, name, r.stdout.strip()[:75]), flush=True)

# license:BSD-3-Clause
"""形の一覧（MANIFEST）から、src/dsp/meg_fx_*.h と meg_fx_list.h を作り直す。
使い方: build_all.py"""
import os, subprocess, sys, io
from group import load
from sig import fnv

HERE = os.path.dirname(os.path.abspath(__file__))
from paths import WORK, DSP as _DSP
DSP = _DSP + os.sep

# (元の書き出し, lo, hi, クラス名, 説明)
MANIFEST = [
    ('cho/4100.m', 0x98, 0xc0, 'meg_fx_chorus', 'コーラス（CHORUS 1/2/4・GM CHORUS 1-4・FB CHORUS・CELESTE 1-4・FLANGER 1-3・GM FLANGER・SYMPHONIC）'),
    ('cho/4102.m', 0x98, 0xc0, 'meg_fx_chorus3', 'コーラス（CHORUS 3）'),
    ('cho/4800.m', 0x98, 0xc0, 'meg_fx_phaser_cho', 'コーラスの口の PHASER 1'),
    ('cho/5700.m', 0x98, 0xc0, 'meg_fx_ens_detune_cho', 'コーラスの口の ENS DETUNE'),
]
if os.path.exists(os.path.join(WORK, 'manifest_extra.py')):
    exec(open(os.path.join(WORK, 'manifest_extra.py'), encoding='utf-8').read())

entries = []
for dump, lo, hi, cls, desc in MANIFEST:
    from sym import decode as _dec
    _prg = load(os.path.join(WORK, dump))[0]
    branchy = any(_dec(_prg.get(pc, 0))['jump'] or ((_prg.get(pc, 0) >> 0x20) & 1) or _dec(_prg.get(pc, 0))['m1t'] == 2 for pc in range(lo, hi))
    tool = 'emit_dyn.py' if branchy else 'emit.py'
    out = subprocess.run([sys.executable, os.path.join(HERE, tool), os.path.join(WORK, dump), '%x' % lo, '%x' % hi, cls, desc],
                         capture_output=True, check=True).stdout
    open(DSP + cls + '.h', 'wb').write(out)
    warn = [l for l in out.decode('utf-8').split('\n') if l.startswith('// 注意')]
    prg = load(os.path.join(WORK, dump))[0]
    h = fnv(prg, lo, hi)
    entries.append((h, hi - lo, cls, desc))
    print('%-24s %016x %3d %s' % (cls, h, hi - lo, ' '.join(warn)))

rev_prg = load(os.path.join(WORK, 'rv_HALL1.m'))[0]
lines = ['// license:BSD-3-Clause',
         '// S-MU2000: MEG と同じ作りで鳴らせるエフェクトの形の一覧（meg_fx.h が引く）。',
         '// **tools/meg_fx が作ったもの。** 命令語の FNV-1a は MU2000 EX（firmware v2.01）で調べたもの。',
         '',
         '#ifndef S_MU2000_DSP_MEG_FX_LIST_H',
         '#define S_MU2000_DSP_MEG_FX_LIST_H',
         '']
for _, _, cls, _ in entries:
    lines.append('#include "%s.h"' % cls)
lines += ['', 'namespace smu2000::dsp {', '', 'inline const meg_fx_entry MEG_FX_LIST[] = {',
          '\t{ 0x%016xull, 0x%02x, &make_meg_fx<meg_fx_reverb> },   // リバーブ 18 種類' % (fnv(rev_prg, 0, 0x98), 0x98)]
for h, n, cls, desc in entries:
    lines.append('\t{ 0x%016xull, 0x%02x, &make_meg_fx<%s> },   // %s' % (h, n, cls, desc))
lines += ['};', '', '} // namespace smu2000::dsp', '', '#endif // S_MU2000_DSP_MEG_FX_LIST_H', '']
open(DSP + 'meg_fx_list.h', 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
print(len(entries) + 1, 'entries')

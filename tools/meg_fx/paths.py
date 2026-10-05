# license:BSD-3-Clause
"""tools/meg_fx の置き場所の決まり。

WORK: MEG の書き出し（--dump-meg）と試しの wav を置く所。firmware から取ったものなのでリポジトリには入れない
      （既定は build/meg_fx。MEG_FX_WORK で変えられる）
ROMS: ROM の置き場所（既定は ../MU2000/roms。MEG_FX_ROMS で変えられる）
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.environ.get('MEG_FX_WORK', os.path.join(ROOT, 'build', 'meg_fx'))
ROMS = os.environ.get('MEG_FX_ROMS', os.path.join(os.path.dirname(ROOT), 'MU2000', 'roms'))
RENDER = os.path.join(ROOT, 'build', 'render' + ('.exe' if sys.platform == 'win32' else ''))
DSP = os.path.join(ROOT, 'src', 'dsp')
os.makedirs(WORK, exist_ok=True)

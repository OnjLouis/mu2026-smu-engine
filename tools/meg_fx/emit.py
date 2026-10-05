# license:BSD-3-Clause
"""gen.py の結果を、src/dsp/ に置く C++ のクラスにする。
使い方: emit.py dump.m lo hi class_name "説明" > src/dsp/xxx.h"""
import sys, re, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', newline='\n')
from gen import gen

path, lo, hi, cls, desc = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4], sys.argv[5]
code, state, inputs, outputs, ver, lfos, warn, tail = gen(path, lo, hi, cls)
N = hi - lo
body = ''.join(code).replace('kt[', 'k[')
# 入口・出口を配列に
for i, name in enumerate(inputs):
    body = re.sub(r'\b%s\b' % name, 'in[%d]' % i, body)
# LFO は相対の番地から（区画をずらして置いても同じ LFO を指すように）
body = re.sub(r'lfo\[(\d+)\]', lambda m: 'lfo[%s]' % m.group(1), body)
guard = 'S_MU2000_DSP_%s_H' % cls.upper()
st = sorted(state)
out_lines = []
for i, o in enumerate(outputs):
    out_lines.append('\t\tout[%d] = %s;   // %s' % (i, ver[o], o))
print('''// license:BSD-3-Clause
// S-MU2000: %s
//
// **tools/meg_fx/emit.py が作ったもの。手で直さない。** firmware がこのエフェクトに置く MEG のプログラムの形を、
// float の C++ に書き起こした（doc/native-dsp.md の「MEG と同じ作りのエフェクト」）。係数と番地は firmware が MEG に
// 書いた値を configure() で読む。値の目盛りはレジスタの 24bit を 1.0（p も同じ）。
// 入口: %s / 出口: %s
''' % (desc, ' '.join(n[3:] for n in inputs), ' '.join(outputs)))
for w in warn:
    print('// 注意: ' + w)
print('''
#ifndef %s
#define %s

#include "meg_fx_common.h"

namespace smu2000::dsp {

class %s : public meg_fx_base<%d>
{
public:
	static constexpr int N_IN = %d, N_OUT = %d;
	static constexpr uint32_t LFO_USED = 0x%x;   // 使う LFO（ビット = 番号）
	static constexpr uint8_t IN_REGS[N_IN] = { %s };    // 入口のレジスタ（m の番号）
	static constexpr uint8_t OUT_REGS[N_OUT] = { %s };  // 出口のレジスタ

	void reset() { reset_ram(); %s }

	// in: 入口（%s）。out: 出口（%s）。lfo: MEG の LFO 24 本の今の値
	void process(const float *in, float *out, const float *lfo)
	{
		m_n++;
		float *const ram = m_ram.data();
%s
%s
%s
	}

private:
%s
};

} // namespace smu2000::dsp

#endif // %s''' % (guard, guard, cls, N, len(inputs), len(outputs), sum(1 << l for l in lfos), ', '.join('0x' + n[4:] for n in inputs), ', '.join('0x' + o[1:] for o in outputs),
                   ' '.join('%s = 0;' % x for x in st),
                   ' '.join(n[3:] for n in inputs), ' '.join(outputs),
                   body.rstrip(), '\n'.join(out_lines), '\n'.join(tail),
                   '\n'.join(('\tint32_t %s = 0;' if x in ('s_idx', 's_idx2') else '\tfloat %s = 0.0f;') % x for x in st), guard))

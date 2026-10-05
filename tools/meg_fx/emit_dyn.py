# license:BSD-3-Clause
"""gen_dyn.py の結果を C++ のクラスにする（分岐のある区画）。
使い方: emit_dyn.py dump.m lo hi class_name "説明" > src/dsp/xxx.h"""
import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', newline='\n')
from gen_dyn import gen_dyn
from group import load
from sym import decode

path, lo, hi, cls, desc = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4], sys.argv[5]
head, code, tail, state, inputs, outputs, lfos, warn = gen_dyn(path, lo, hi, cls)
prg = load(path)[0]
written = {decode(prg[pc])['dm'] for pc in range(lo, hi)}
outputs = [o for o in outputs if int(o[1:], 16) in written]
N = hi - lo
guard = 'S_MU2000_DSP_%s_H' % cls.upper()
print('''// license:BSD-3-Clause
// S-MU2000: %s
//
// **tools/meg_fx/emit_dyn.py が作ったもの。手で直さない。** firmware がこのエフェクトに置く MEG のプログラムの形を、
// float の C++ に書き起こした（doc/native-dsp.md の「MEG と同じ作りのエフェクト」）。このプログラムは分岐（条件つきで
// 先の命令を飛ばす）を持つので、レジスタを変数として持ち、3 命令遅れの書き込みを「書くかどうか」の印つきで回す。
// 係数と番地は firmware が MEG に書いた値を configure() で読む。値の目盛りはレジスタの 24bit を 1.0（p も同じ）。
// 入口: %s / 出口: %s''' % (desc, ' '.join('m' + n[1:] for n in inputs), ' '.join('m' + o[1:] for o in outputs)))
for w in warn:
    print('// 注意: ' + w)
decls = []
for name, t in sorted(state.items()):
    decls.append('\t%s %s = %s;' % ({'f': 'float', 'i': 'int32_t', 'b': 'bool'}[t], name, {'f': '0.0f', 'i': '0', 'b': 'false'}[t]))
resets = ' '.join('%s = %s;' % (name, {'f': '0.0f', 'i': '0', 'b': 'false'}[t]) for name, t in sorted(state.items()))
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

	// in: 入口。out: 出口。lfo: MEG の LFO 24 本の今の値
	void process(const float *in, float *out, const float *lfo)
	{
		m_n++;
		float *const ram = m_ram.data();
%s
%s
%s
%s
	}

private:
%s
};

} // namespace smu2000::dsp

#endif // %s''' % (guard, guard, cls, N, len(inputs), len(outputs), sum(1 << l for l in lfos),
                   ', '.join('0x' + n[1:] for n in inputs), ', '.join('0x' + o[1:] for o in outputs), resets,
                   '\n'.join(head), ''.join(code).rstrip(), '\n'.join(tail),
                   '\n'.join('\t\tout[%d] = %s;' % (i, o) for i, o in enumerate(outputs)),
                   '\n'.join(decls), guard))

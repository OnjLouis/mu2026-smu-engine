# license:BSD-3-Clause
"""MEG のプログラムの 1 区画（pc の範囲）を、float の C++ の process() に変換する。

使い方: gen.py dump.m lo hi ClassName > out.h
値の目盛り: レジスタの 24bit を 1.0。p も同じ目盛り（p の 2^38 が 1.0）。
"""
import sys
from group import load
from sym import decode, region_info


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def gen(path, lo, hi, cls):
    prg, const, off, mp = load(path)
    dec = {pc: decode(prg.get(pc, 0)) for pc in range(0x180)}
    warn = []
    code = []
    state = {}          # 名前 -> 初期値（前のサンプルから持ち越すもの）
    inputs = []         # 母線の入口（m20-2f）
    lfos = set()
    ver = {}            # レジスタ -> 今の値の C++ 式（変数名）
    n = [0]

    def tmp(prefix):
        n[0] += 1
        return '%s%d' % (prefix, n[0])

    # 範囲の外で最後に書いた命令（同じサンプルで範囲より前、または前のサンプルで範囲より後）
    def outside_writer(reg):
        kind, num = reg[0], int(reg[1:], 16)
        best = None
        for pc in list(range(lo - 1, -1, -1)) + list(range(0x17f, hi - 1, -1)):
            d = dec[pc]
            if kind == 'm' and d['dm'] == num:
                return pc
            if kind == 'r' and d['dr'] == num:
                return pc
        return best

    def read(reg):
        if reg in ver:
            return ver[reg]
        kind, num = reg[0], int(reg[1:], 16)
        # 範囲の中でまだ書いていない: 前のサンプルの値か、外から来る値
        if kind == 'm' and 0x20 <= num < 0x30:
            # 母線（ミキサが MEG の前に毎サンプル書く）。同じサンプルで範囲より前に別の区画が書き換えていれば注意
            for pc in range(0, lo):
                if dec[pc]['dm'] == num:
                    warn.append('入口 %s を %03x が書き換えている' % (reg, pc))
            name = 'in_' + reg
            if name not in inputs:
                inputs.append(name)
            ver[reg] = name
            return name
        wrote_inside = any((kind == 'm' and dec[pc]['dm'] == num) or (kind == 'r' and dec[pc]['dr'] == num)
                           for pc in range(lo, hi))
        if wrote_inside:
            name = 's_' + reg
            state[name] = reg
            ver[reg] = name
            return name
        w = outside_writer(reg)
        if kind == 'm' and 0x20 <= num < 0x30 and (w is None or w >= hi):
            name = 'in_' + reg
            if name not in inputs:
                inputs.append(name)
            ver[reg] = name
            return name
        if w is not None and dec[w]['dm'] == num and kind == 'm' and dec[w]['dm_src'] <= 3:
            lfos.add(w >> 4)
            return 'lfo[%d]' % (w >> 4)
        warn.append('外から来る値 %s（書く命令 %s）' % (reg, ('%03x' % w) if w is not None else 'なし'))
        name = 'x_' + reg
        state[name] = reg
        ver[reg] = name
        return name

    pend = {}           # 入る pc -> [(レジスタ, 値)]

    def later(pc, reg, val):
        pend.setdefault(pc, []).append((reg, val))

    p = 's_p'
    state['s_p'] = 'p'
    tv_hist = {}        # pc -> その命令の t の値（式）
    t_cur = {}
    uses_p_at_start = True
    flag = None
    for pc in range(lo, hi):
        d = dec[pc]
        for (reg, val) in pend.pop(pc, []):
            ver[reg] = val
        if d['jump']:
            warn.append('%03x 分岐' % pc)
            continue
        c = s16(const.get(pc, 0))
        stmts = []
        if d['alu']:
            if d['m1t'] in (1, 2):
                tn = 't%d' % d['t']
                tval = t_cur.get(tn)
                if tval is None:
                    tval = 's_' + tn
                    state[tval] = tn
                    t_cur[tn] = tval
                if d['m1t'] == 2:
                    warn.append('%03x m1 を印で選ぶ' % pc)
                m1 = tval
                if d['m1_expand']:
                    m1 = 'expand_t(%s)' % m1
            else:
                m1 = 'kx[%d]' % (pc - lo) if d['m1_expand'] else 'k[%d]' % (pc - lo)
            m2 = read('m%02x' % d['sm']) if d['m2_from_m'] else read('r%02x' % d['sr'])
            mm = {0: None, 1: m1, 2: '%s * %s' % (m1, m2), 3: m2}[d['mmode']]
            if d['asel'] in (0, 3) and p == 's_p':
                if pc == lo or uses_p_at_start:
                    warn.append('%03x 範囲の前の p を使う' % pc)
            aa = {0: p, 1: read('r%02x' % d['sr']), 2: read('m%02x' % d['sm']), 3: '(%s * (1.0f / 32768.0f))' % p, 4: None}[d['asel']]
            if d['rop'] == 3:
                ex = 'and38(%s, %s)' % (mm or '0.0f', aa or '0.0f')
            else:
                if d['rop'] == 2 and aa is not None:
                    aa = 'std::fabs(%s)' % aa
                parts = []
                if mm:
                    parts.append(mm)
                if aa:
                    parts.append(('- ' if d['rop'] == 1 else '+ ') + aa if parts else ('-' + aa if d['rop'] == 1 else aa))
                ex = ' '.join(parts) if parts else '0.0f'
                if not mm and aa and d['rop'] == 1:
                    ex = '-(%s)' % aa
            if d['shift']:
                ex = '(%s) * %d.0f' % (ex, 1 << d['shift'])
            ex = {0: ex, 1: 'sat(%s)' % ex, 2: 'satpos(%s)' % ex, 3: 'satabs(%s)' % ex}[d['clamp']]
            np_ = tmp('p')
            stmts.append('const float %s = %s;' % (np_, ex))
            p = np_
            if bits_latch(prg.get(pc, 0)):
                warn.append('%03x 印を覚える' % pc)
        uses_p_at_start = False
        if d['dm']:
            src = d['dm_src']
            if src <= 3:
                lfos.add(pc >> 4)
                v = 'lfo[%d]' % (pc >> 4)
            elif src == 4:
                v = ver.get('mr') or read_special('mr', state, ver)
            elif src == 5:
                v = 'noise()'
            elif src == 6:
                v = 'w24(%s)' % p
            else:
                v = read('m%02x' % d['sm'])
            nv = tmp('m')
            stmts.append('const float %s = %s;   // m%02x' % (nv, v, d['dm']))
            later(pc + 3, 'm%02x' % d['dm'], nv)
        if d['dr']:
            v = read('r%02x' % d['sr']) if d['dr_from_r'] else 'w24(%s)' % p
            nv = tmp('r')
            stmts.append('const float %s = %s;   // r%02x' % (nv, v, d['dr']))
            later(pc + 3, 'r%02x' % d['dr'], nv)
        if d['memw']:
            nv = tmp('w')
            stmts.append('const float %s = %s;' % (nv, p))
            later(pc + 2, 'mw', nv)
        if d['index']:
            nv = tmp('i')
            stmts.append('const int32_t %s = idx_of(%s);' % (nv, p))
            later(pc + 3, 'idx', nv)
        if d['index2']:
            nv = tmp('j')
            stmts.append('const int32_t %s = idx_of(%s);' % (nv, p))
            later(pc + 3, 'idx2', nv)
        # t の値（2 命令あとの t 書き込みが使う）
        tv_hist[pc] = ('tv_index(%s)' if (d['index'] or d['index2']) else 'tv_plain(%s)') % p
        if d['t_write']:
            tn = 't%d' % d['t']
            if d['t_from_p']:
                src = tv_hist.get(pc - 2)
                if src is None:
                    warn.append('%03x t が範囲の前の値を使う' % pc)
                    src = '0.0f'
                nv = tmp('t')
                stmts.append('const float %s = %s;   // %s' % (nv, src, tn))
            else:
                nv = tmp('t')
                stmts.append('const float %s = kt[%d];   // %s（定数）' % (nv, pc - lo, tn))
            t_cur[tn] = nv
        if d['memop']:
            idx = ''
            if d['use_idx']:
                idx += ' + ' + (ver.get('idx') or read_special('idx', state, ver))
            if d['use_idx2']:
                idx += ' + ' + (ver.get('idx2') or read_special('idx2', state, ver))
            plus1 = ' + 1' if d['memop'] == 3 else ''
            add = (idx + plus1).strip()
            if add.startswith('+ '):
                add = add[2:]
            ad = ('at(%d, %s)' % (pc - lo, add)) if add else ('at(%d)' % (pc - lo))
            if d['memop'] == 1:
                v = ver.get('mw') or read_special('mw', state, ver)
                stmts.append('ram[%s] = %s;' % (ad, v))
            else:
                nv = tmp('q')
                if d['table']:
                    stmts.append('const float %s = %s;' % (nv, ad.replace('at(', 'tab(', 1)))
                else:
                    stmts.append('const float %s = ram[%s];' % (nv, ad))
                later(pc + 2, 'mr', nv)
        code.append('\t\t// %03x\n' % pc + ''.join('\t\t%s\n' % s for s in stmts))
    # 残りを入れる
    for pcx in sorted(pend):
        for (reg, val) in pend[pcx]:
            ver[reg] = val
    # 持ち越し: 状態の値を最後の版で更新
    tail = []
    for name, reg in state.items():
        if reg == 'p':
            tail.append('\t\ts_p = %s;' % p)
        elif reg in t_cur and t_cur[reg] != name:
            tail.append('\t\t%s = %s;' % (name, t_cur[reg]))
        elif reg in ver and ver[reg] != name:
            tail.append('\t\t%s = %s;' % (name, ver[reg]))
    outputs = sorted(r for r in ver if r.startswith('m') and r not in ('mr', 'mw') and 0x20 <= int(r[1:], 16) < 0x34 and not ver[r].startswith('in_'))
    return code, state, inputs, outputs, ver, lfos, warn, tail


def bits_latch(op):
    return (op >> 0x20) & 1


def read_special(reg, state, ver):
    name = 's_' + reg
    state[name] = reg
    ver[reg] = name
    return name


if __name__ == '__main__':
    path, lo, hi, cls = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4]
    code, state, inputs, outputs, ver, lfos, warn, tail = gen(path, lo, hi, cls)
    for w in warn:
        print('// 注意: ' + w)
    print('// 入口: ' + ' '.join(inputs))
    print('// 出口: ' + ' '.join('%s=%s' % (o, ver[o]) for o in outputs))
    print('// LFO: ' + ' '.join('%d' % l for l in sorted(lfos)))
    print('// 状態: ' + ' '.join(sorted(state)))
    print(''.join(code))
    print('\n'.join(tail))

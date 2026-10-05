# license:BSD-3-Clause
"""分岐のある区画を float の C++ にする（レジスタを変数として持ち、遅れた書き込みを印つきで回す）。
gen.py の gen() と同じ形の結果を返す。"""
from group import load
from sym import decode


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def meg_cond_expr(cond):
    if not (cond >> 3) & 1:
        return 'true'
    c = 'fn' if (cond >> 2) & 1 else '!fn'
    if (cond >> 1) & 1:
        c = '(%s || fz)' % c
    return c


def gen_dyn(path, lo, hi, cls):
    prg, const, off, mp = load(path)
    dec = {pc: decode(prg.get(pc, 0)) for pc in range(0x180)}
    warn = []
    state = {}          # 名前 -> 種類（'f' float / 'i' int / 'b' bool）
    inputs, lfos = [], set()
    code = []

    written_m = {dec[pc]['dm'] for pc in range(lo, hi) if dec[pc]['dm'] and not dec[pc]['jump']}
    written_r = {dec[pc]['dr'] for pc in range(lo, hi) if dec[pc]['dr'] and not dec[pc]['jump']}

    def outside_writer(num):
        for pc in list(range(lo - 1, -1, -1)) + list(range(0x17f, hi - 1, -1)):
            if dec[pc]['dm'] == num:
                return pc
        return None

    def M(num):
        if num == 0:
            return '0.0f'
        if 0x20 <= num < 0x30 or num in written_m:
            name = 'M%02x' % num
            state[name] = 'f'
            if 0x20 <= num < 0x30 and name not in inputs:
                for pc in range(0, lo):
                    if dec[pc]['dm'] == num:
                        warn.append('入口 m%02x を %03x が書き換えている' % (num, pc))
                inputs.append(name)
            return name
        w = outside_writer(num)
        if w is not None and dec[w]['dm_src'] <= 3:
            lfos.add(w >> 4)
            return 'lfo[%d]' % (w >> 4)
        warn.append('外から来る値 m%02x' % num)
        name = 'M%02x' % num
        state[name] = 'f'
        return name

    def R(num):
        if num == 0:
            return '0.0f'
        name = 'R%02x' % num
        state[name] = 'f'
        if num not in written_r:
            warn.append('範囲の中で書かない r%02x を読む' % num)
        return name

    for n in ('p', 'fn', 'fz', 'rd', 'wr', 'ix', 'ix2'):
        pass
    state.update({'p': 'f', 'fn': 'b', 'fz': 'b', 'RRD': 'f', 'RWR': 'f', 'IX': 'i', 'IX2': 'i'})
    locals_ = []        # (型, 名前) 1 サンプルの中だけのもの
    pend = {}           # 入る pc -> [文]
    for pc in range(lo, hi):
        d = dec[pc]
        rel = pc - lo
        body = []
        # 頭: 遅れた書き込みを入れる（飛ばされる命令でも入る）
        pre = pend.pop(pc, [])
        body.append('\t\t// %03x' % pc)
        if any(dec[x]['jump'] for x in range(lo, hi)):
            body.append('\t\tif (skip && %d >= skip) skip = 0;' % pc)
        body += ['\t\t' + s for s in pre]
        tv = 'tv%d' % rel
        locals_.append(('float', tv))
        c = s16(const.get(pc, 0))
        if d['jump']:
            tgt = (pc & ~0xff) | ((prg[pc] >> 0x10) & 0xff)
            body.append('\t\tif (!skip && %s && %d > %d) skip = %d;' % (meg_cond_expr((prg[pc] >> 0x18) & 0xff), tgt, pc, tgt))
            if d['t_write']:
                tn = 'T%d' % d['t']
                state[tn] = 'f'
                src = ('tv%d' % (rel - 2)) if d['t_from_p'] else 'k[%d]' % rel
                if d['t_from_p'] and rel < 2:
                    warn.append('%03x t が範囲の前の値を使う' % pc)
                    src = '0.0f'
                body.append('\t\t%s = %s;' % (tn, src))
            body.append('\t\t%s = tv_plain(p);' % tv)
            code.append('\n'.join(body) + '\n')
            continue
        ex_lines = []
        if d['alu']:
            if d['m1t'] in (1, 2):
                tn = 'T%d' % d['t']
                state[tn] = 'f'
                m1 = tn if d['m1t'] == 1 else '(fn ? %s : %s)' % (tn, 'k[%d]' % rel)
                if d['m1_expand']:
                    m1 = 'expand_t(%s)' % m1
            else:
                m1 = 'kx[%d]' % rel if d['m1_expand'] else 'k[%d]' % rel
            m2 = M(d['sm']) if d['m2_from_m'] else R(d['sr'])
            mm = {0: '0.0f', 1: m1, 2: '%s * %s' % (m1, m2), 3: m2}[d['mmode']]
            aa = {0: 'p', 1: R(d['sr']), 2: M(d['sm']), 3: '(p * (1.0f / 32768.0f))', 4: '0.0f'}[d['asel']]
            ex = {0: '%s + %s' % (mm, aa), 1: '%s - %s' % (mm, aa), 2: '%s + std::fabs(%s)' % (mm, aa), 3: 'and38(%s, %s)' % (mm, aa)}[d['rop']]
            if d['shift']:
                ex = '(%s) * %d.0f' % (ex, 1 << d['shift'])
            ex = {0: ex, 1: 'sat(%s)' % ex, 2: 'satpos(%s)' % ex, 3: 'satabs(%s)' % ex}[d['clamp']]
            ex_lines.append('p = %s;' % ex)
            if (prg[pc] >> 0x20) & 1:
                ex_lines.append('fn = p < 0.0f; fz = p == 0.0f;')
        if d['dm']:
            src = d['dm_src']
            if src <= 3:
                lfos.add(pc >> 4)
                v = 'lfo[%d]' % (pc >> 4)
            elif src == 4:
                v = 'RRD'
            elif src == 5:
                v = 'noise()'
            elif src == 6:
                v = 'w24(p)'
            else:
                v = M(d['sm'])
            pv, pe = 'pm%d' % rel, 'em%d' % rel
            locals_ += [('float', pv), ('bool', pe)]
            ex_lines.append('%s = %s; %s = true;' % (pv, v, pe))
            pend.setdefault(pc + 3, []).append('if (%s) %s = %s;' % (pe, M(d['dm']), pv))
        if d['dr']:
            v = R(d['sr']) if d['dr_from_r'] else 'w24(p)'
            pv, pe = 'pr%d' % rel, 'er%d' % rel
            locals_ += [('float', pv), ('bool', pe)]
            ex_lines.append('%s = %s; %s = true;' % (pv, v, pe))
            pend.setdefault(pc + 3, []).append('if (%s) %s = %s;' % (pe, R(d['dr']), pv))
        if d['memw']:
            pv, pe = 'pw%d' % rel, 'ew%d' % rel
            locals_ += [('float', pv), ('bool', pe)]
            ex_lines.append('%s = p; %s = true;' % (pv, pe))
            pend.setdefault(pc + 2, []).append('if (%s) RWR = %s;' % (pe, pv))
        if d['index'] or d['index2']:
            pv, pe = 'pi%d' % rel, 'ei%d' % rel
            locals_ += [('int32_t', pv), ('bool', pe)]
            ex_lines.append('%s = idx_of(p); %s = true;' % (pv, pe))
            pend.setdefault(pc + 3, []).append('if (%s) %s = %s;' % (pe, 'IX' if d['index'] else 'IX2', pv))
        ex_lines.append('%s = %s;' % (tv, ('tv_index(p)' if (d['index'] or d['index2']) else 'tv_plain(p)')))
        if d['t_write']:
            tn = 'T%d' % d['t']
            state[tn] = 'f'
            if d['t_from_p']:
                if rel < 2:
                    warn.append('%03x t が範囲の前の値を使う' % pc)
                ex_lines.append('%s = %s;' % (tn, ('tv%d' % (rel - 2)) if rel >= 2 else '0.0f'))
            else:
                ex_lines.append('%s = k[%d];' % (tn, rel))
        if d['memop']:
            add = []
            if d['use_idx']:
                add.append('IX')
            if d['use_idx2']:
                add.append('IX2')
            if d['memop'] == 3:
                add.append('1')
            addx = (', ' + ' + '.join(add)) if add else ''
            if d['memop'] == 1:
                ex_lines.append('ram[at(%d%s)] = RWR;' % (rel, addx))
            else:
                pv, pe = 'pq%d' % rel, 'eq%d' % rel
                locals_ += [('float', pv), ('bool', pe)]
                if d['table']:
                    ex_lines.append('%s = tab(%d%s); %s = true;' % (pv, rel, addx, pe))
                else:
                    ex_lines.append('%s = ram[at(%d%s)]; %s = true;' % (pv, rel, addx, pe))
                pend.setdefault(pc + 2, []).append('if (%s) RRD = %s;' % (pe, pv))
        if any(dec[x]['jump'] for x in range(lo, hi)):
            body.append('\t\tif (!skip) {')
            body += ['\t\t\t' + s for s in ex_lines]
            body.append('\t\t} else')
            body.append('\t\t\t%s = tv_plain(p);' % tv)
        else:
            body += ['\t\t' + s for s in ex_lines]
        code.append('\n'.join(body) + '\n')
    tail = []
    for pcx in sorted(pend):
        tail += ['\t\t' + s for s in pend[pcx]]
    decl = []
    for t, n in locals_:
        decl.append('\t\t%s %s%s;' % (t, n, ' = false' if t == 'bool' else ''))
    head = decl + ['\t\tint skip = 0;']
    for i, name in enumerate(inputs):
        head.append('\t\t%s = in[%d];' % (name, i))
    outputs = sorted(n for n in state if n.startswith('M') and 0x20 <= int(n[1:], 16) < 0x34)
    return head, code, tail, state, inputs, outputs, lfos, warn

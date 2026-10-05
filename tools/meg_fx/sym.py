# license:BSD-3-Clause
"""MEG の 1 サンプルを記号のまま実行して、読める式の列にする（解析用。出力は firmware の中身なので WORK に置く）。

run_program（swp30.cpp）と同じ決まり:
  - m / r / idx / idx2 への書き込みは 3 命令あとに入る（その命令の頭で）
  - memw（RAM へ書く値）と mem_r の結果は 2 命令あとに入る
  - p はサンプルをまたいで持ち越す
  - 番地 = ((off + idx - サンプル番号) & mask) + base
出力は「命令ごとに、何を計算してどこへ入れたか」。レジスタの読みは、その時点で入っている版の名前
（Xn = このサンプルで n 番目に入った値、X' = 前のサンプルから持ち越した値）で書く。
"""
import sys
from group import load


def bits(v, pos, n=1):
    return (v >> pos) & ((1 << n) - 1)


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode(op):
    d = dict(sm=bits(op, 4, 6), sr=bits(op, 0xb, 7), dm=bits(op, 0x27, 6), dr=bits(op, 0x30, 7), t=bits(op, 0x38, 3),
             mmode=bits(op, 0x16, 2), m1t=bits(op, 0x14, 2), asel_raw=bits(op, 0x18, 2), rop=bits(op, 0x1a, 2),
             shift=bits(op, 0x1c, 2), clamp=bits(op, 0x1e, 2), dm_src=bits(op, 0x2d, 3), memop=bits(op, 0x24, 2),
             m1_expand=bits(op, 0x13), m2_from_m=bits(op, 0x12), dr_from_r=bits(op, 0x37), no_noise=bits(op, 0xa),
             index=bits(op, 0x3e) and not bits(op, 0x3d), index2=bits(op, 0x3e) and bits(op, 0x3d),
             memw=bits(op, 0x3d) and not bits(op, 0x3e), use_idx2=bits(op, 0x22), t_write=bits(op, 0x3b),
             t_from_p=bits(op, 0x3c), use_idx=bits(op, 0x21), table=bits(op, 0x23), jump=bits(op, 0x3f))
    d['alu'] = d['mmode'] != 0 or d['shift'] or d['clamp'] or d['rop']
    a = d['asel_raw']
    d['asel'] = {0: 0, 1: (1 if d['sr'] else 3), 2: (2 if d['sm'] else 3), 3: 4}[a]
    d['shift'] = 4 if d['shift'] == 3 else d['shift']
    return d


def region_info(mp, pc):
    key = (pc // 12) << 11
    for i in range(8):
        if i == 7 or mp[i + 1] <= mp[i] or (mp[i + 1] & 0xf800) > key:
            return i, (1 << (10 + bits(mp[i], 8, 3))) - 1, bits(mp[i], 0, 8) << 10
    return 7, 0, 0


def run(path, lo=0, hi=0x180):
    prg, const, off, mp = load(path)
    ver = {}          # 名前 -> 今の版の名前
    cnt = {}

    def cur(x):
        return ver.get(x, x + "'")

    def new(x):
        cnt[x] = cnt.get(x, 0) + 1
        return '%s_%d' % (x, cnt[x])

    pend3 = {}   # 入る命令 -> [(レジスタ, 値の名前)]
    pend2 = {}
    lines = []
    p = "p'"
    tv = {}
    for pc in range(lo, hi):
        d = decode(prg.get(pc, 0))
        for (x, v) in pend3.pop(pc, []):
            ver[x] = v
        for (x, v) in pend2.pop(pc, []):
            ver[x] = v
        c = s16(const.get(pc, 0))
        out = []
        if d['jump']:
            lines.append('%03x  JUMP' % pc)
            continue
        if d['alu']:
            m1 = ('%.6f' % (c / 32768.0)) if d['m1t'] not in (1, 2) else ('t%d' % d['t'])
            if d['m1_expand']:
                m1 = 'expand(%s)' % m1
            m2 = cur('m%02x' % d['sm']) if d['m2_from_m'] else cur('r%02x' % d['sr'])
            mm = {0: '0', 1: '(%s<<8)' % m1, 2: '%s*%s' % (m1, m2), 3: m2}[d['mmode']]
            aa = {0: p, 1: cur('r%02x' % d['sr']), 2: cur('m%02x' % d['sm']), 3: '(%s/32768)' % p, 4: '0'}[d['asel']]
            ex = {0: '%s + %s' % (mm, aa), 1: '%s - %s' % (mm, aa), 2: '%s + |%s|' % (mm, aa), 3: '%s & %s' % (mm, aa)}[d['rop']]
            if d['shift']:
                ex = '(%s)*%d' % (ex, 1 << d['shift'])
            if d['clamp']:
                ex = ['', 'sat', 'satpos', 'satabs'][d['clamp']] + '(' + ex + ')'
            p = new('p')
            out.append('%s = %s' % (p, ex))
        if d['dm']:
            src = d['dm_src']
            if src <= 3:
                v = 'lfo%02x' % (pc >> 4)
            elif src == 4:
                v = cur('mr')
            elif src == 5:
                v = 'rand'
            elif src == 6:
                v = p
            else:
                v = cur('m%02x' % d['sm'])
            x = 'm%02x' % d['dm']
            nv = new(x)
            out.append('%s := %s' % (nv, v))
            pend3.setdefault(pc + 3, []).append((x, nv))
        if d['dr']:
            v = cur('r%02x' % d['sr']) if d['dr_from_r'] else p
            x = 'r%02x' % d['dr']
            nv = new(x)
            out.append('%s := %s' % (nv, v))
            pend3.setdefault(pc + 3, []).append((x, nv))
        if d['memw']:
            nv = new('mw')
            out.append('%s := %s' % (nv, p))
            pend2.setdefault(pc + 2, []).append(('mw', nv))
        if d['index']:
            nv = new('idx')
            out.append('%s := %s/256' % (nv, p))
            pend3.setdefault(pc + 3, []).append(('idx', nv))
        if d['t_write']:
            out.append('t%d = %s' % (d['t'], ('tv' if d['t_from_p'] else 'const')))
        if d['memop']:
            reg, mask, base = region_info(mp, pc)
            o = off.get(pc // 3, 0)
            ad = '%04x' % o + ('+idx' if d['use_idx'] else '') + ('+idx2' if d['use_idx2'] else '')
            if d['table']:
                ad = 'ABS ' + ad
            if d['memop'] == 1:
                out.append('RAM[%s] = %s' % (ad, cur('mw')))
            else:
                nv = new('mr')
                out.append('%s := RAM[%s%s]' % (nv, ad, '+1' if d['memop'] == 3 else ''))
                pend2.setdefault(pc + 2, []).append(('mr', nv))
        lines.append('%03x  %s' % (pc, ' ; '.join(out)))
    return lines


if __name__ == '__main__':
    lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x180
    for l in run(sys.argv[1], lo, hi):
        print(l)

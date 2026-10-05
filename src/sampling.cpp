// license:BSD-3-Clause
//
// サンプリングの表をパネルを通さずに読み書きする（表の形は src/sampling.h、doc/sampling-ram.md）
#include "mu2000.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace sp = smu2000::sampling;

namespace {

u32 rd32(const std::vector<u8> &m, u32 off)
{
	return u32(m[off]) << 24 | u32(m[off + 1]) << 16 | u32(m[off + 2]) << 8 | m[off + 3];
}

void wr32(std::vector<u8> &m, u32 off, u32 v)
{
	m[off] = u8(v >> 24);
	m[off + 1] = u8(v >> 16);
	m[off + 2] = u8(v >> 8);
	m[off + 3] = u8(v);
}

constexpr u32 DRAM = 0x1000000;
constexpr u32 WORD_FLAG = 0x01000000;   // 語の位置に付いている上の印

u32 sample_rec(int n) { return sp::TAB_SAMPLE + 36 * u32(n - 1) - DRAM; }
u32 play_rec(int n)   { return sp::TAB_PLAY + 16 * u32(n - 1) - DRAM; }
u32 voice_rec(int slot) { return sp::TAB_VOICE + sp::VOICE_SIZE * u32(slot) - DRAM; }

// 表の名前（空白か 0 で終わる）を std::string に
std::string text(const std::vector<u8> &m, u32 off, int len)
{
	std::string s(reinterpret_cast<const char *>(&m[off]), size_t(len));
	const size_t z = s.find('\0');
	if (z != std::string::npos)
		s.resize(z);
	while (!s.empty() && s.back() == ' ')
		s.pop_back();
	return s;
}

// LCD に出せる文字だけにして len に詰める
void put_text(std::vector<u8> &m, u32 off, int len, const std::string &s, u8 fill)
{
	for (int i = 0; i < len; i++) {
		u8 c = i < int(s.size()) ? u8(s[size_t(i)]) : fill;
		if (i < int(s.size()) && (c < 0x20 || c > 0x7e))
			c = '_';
		m[off + u32(i)] = c;
	}
}

// 鳴り始め・ループの頭・鳴り終わりを、長さ frames のサンプルで書ける形にそろえる。
// 鳴り終わりは鳴り始めより 8 以上後、ループの頭は偶数で鳴り始めと鳴り終わりの 8 手前のあいだ。
// 鳴り終わりは、サンプルの終わりの TAIL_PAD 手前まで（終わりの 4 サンプルは余白。firmware が録ったサンプルも
// そうなっていて、記録の +24 の 04 がその数。音源は折り返す所の次のサンプルも読むので、余白が要る）
void fit_points(u32 frames, u32 &from, u32 &to, u32 &loop_from)
{
	const u32 last = frames > 3 * sp::TAIL_PAD ? frames - sp::TAIL_PAD : frames;
	if (!to || to > last)
		to = last;
	to = std::max(to, std::min(frames, 8u));
	from = std::min(from, to >= 8 ? to - 8 : 0);
	loop_from = std::clamp(loop_from & ~1u, (from + 1) & ~1u, to >= 8 ? (to - 8) & ~1u : 0);
	from = std::min(from, loop_from);
}

// 鳴らす表の 1 項目を書く。start・end は語、ほかはサンプル（頭から）。
// ループの頭の語を +12 に、鳴り始めはそこから +4 の下だけ手前、+8 はループの頭から鳴り終わりまで。
// 音源は「ループの頭 + (+8)」ちょうどで折り返す（サインを 4214 サンプルで回すと、+8 が 4210 なら位相が 4210 おきに跳び、
// 4214 なら跳ばない。2026-10-05）。前は鳴り終わりを +8 より 4 後ろと読んでいて、ループが 4 サンプル短かった
void put_play(std::vector<u8> &m, u32 p, u32 start, u32 end, bool loop, u32 from, u32 to, u32 loop_from)
{
	fit_points((end - start) * 2, from, to, loop_from);
	static const u8 HEAD[4] = { 0x00, 0x3c, 0x00, 0xff };
	std::memcpy(&m[p], HEAD, 4);
	wr32(m, p + 4, (loop ? 0u : 0x40000000u) | (loop_from - from));
	wr32(m, p + 8, to - loop_from);
	wr32(m, p + 12, (start + loop_from / 2) | WORD_FLAG);
}

} // namespace

std::vector<sp::sample> mu2000::sampling_list() const
{
	std::vector<sp::sample> out;
	for (int n = 1; n <= sp::MAX_SAMPLES; n++) {
		const u32 o = sample_rec(n);
		if (!(m_dram[o + 2] & 0x40))
			continue;
		sp::sample s;
		s.number = n;
		s.rate = rd32(m_dram, o + 12);
		s.start = rd32(m_dram, o + 16) & 0xffffff;
		s.end = rd32(m_dram, o + 20) & 0xffffff;
		s.name = text(m_dram, o + 28, 8);
		const u32 p = play_rec(n);
		s.loop = !(m_dram[p + 4] & 0x40);
		const u32 head = rd32(m_dram, p + 12) & 0xffffff, back = rd32(m_dram, p + 4) & 0xffffff;
		const u32 len = rd32(m_dram, p + 8);
		s.loop_from = head > s.start && head < s.end ? (head - s.start) * 2 : 0;
		s.play_from = back <= s.loop_from ? s.loop_from - back : 0;
		s.play_to = len <= s.frames() ? s.loop_from + len : s.frames();
		if (s.play_to > s.frames())
			s.play_to = s.frames();
		out.push_back(s);
	}
	return out;
}

int mu2000::sampling_peak(const sp::sample &s) const
{
	int peak = 0;
	for (u32 i = s.start * 2; i < s.end * 2 && (i + 1) * 2 <= m_sampram.size(); i++) {
		const s16 v = s16(m_sampram[i * 2] | m_sampram[i * 2 + 1] << 8);
		peak = std::max(peak, v < 0 ? -int(v) : int(v));
	}
	return peak;
}

int mu2000::sampling_gain(int number, double gain)
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		int peak = 0;
		for (u32 i = s.start * 2; i < s.end * 2 && (i + 1) * 2 <= m_sampram.size(); i++) {
			const s16 v = s16(m_sampram[i * 2] | m_sampram[i * 2 + 1] << 8);
			const long w = std::clamp(std::lround(double(v) * gain), -32768L, 32767L);
			m_sampram[i * 2] = u8(w);
			m_sampram[i * 2 + 1] = u8(u16(w) >> 8);
			peak = std::max(peak, int(w < 0 ? -w : w));
		}
		return peak;
	}
	return -1;
}

bool mu2000::sampling_trim(int number, u32 from, u32 to, std::string &err)
{
	const std::vector<sp::sample> list = sampling_list();
	const sp::sample *s = nullptr;
	for (const sp::sample &x : list)
		if (x.number == number)
			s = &x;
	if (!s) {
		err = "no such sample";
		return false;
	}
	to = std::min(to, s->frames());
	if (from >= to || to - from < 8) {
		err = "the trimmed sample would be too short";
		return false;
	}
	const u32 start = s->start, old_end = s->end;
	// 鳴り終わりの後ろに余白（TAIL_PAD）を残す。鳴り終わり・ループの終わりは余白の手前までなので、
	// 残さないと、合わせたループの終わりが 4 サンプル前へずれる
	const u32 frames = std::min(to + sp::TAIL_PAD, s->frames()) - from;
	const u32 new_end = start + (frames + 1) / 2;
	// 残す所を頭へ（前へ写すので重なっても前から順に写せばよい）。奇数なら最後の半語は 0
	u8 *ram = m_sampram.data();
	std::memmove(ram + size_t(start) * 4, ram + (size_t(start) * 2 + from) * 2, size_t(frames) * 2);
	if (frames & 1) {
		ram[(size_t(start) * 2 + frames) * 2] = 0;
		ram[(size_t(start) * 2 + frames) * 2 + 1] = 0;
	}
	// 鳴り始め・ループの頭・鳴り終わりは残した所の中での位置へ（切り落とした所にあれば端へ）
	auto set_range = [&](const sp::sample &x, u32 st, u32 en, u32 shift, u32 limit) {
		const u32 o = sample_rec(x.number);
		auto in = [&](u32 v) { return std::min(v > shift ? v - shift : 0, limit); };
		put_play(m_dram, play_rec(x.number), st, en, x.loop, in(x.play_from), in(x.play_to), in(x.loop_from));
		wr32(m_dram, o + 16, st | WORD_FLAG);
		wr32(m_dram, o + 20, en | WORD_FLAG);
	};
	set_range(*s, start, new_end, from, to - from);       // 鳴り終わりは切った所まで（その後ろは余白）
	// 後ろにあるものを前へ詰める（番地の若い順に）
	const u32 gap = old_end - new_end;
	if (gap) {
		std::vector<sp::sample> later;
		for (const sp::sample &x : list)
			if (x.number != number && x.start >= old_end)
				later.push_back(x);
		std::sort(later.begin(), later.end(), [](const sp::sample &a, const sp::sample &b) { return a.start < b.start; });
		for (const sp::sample &x : later) {
			std::memmove(ram + size_t(x.start - gap) * 4, ram + size_t(x.start) * 4, size_t(x.end - x.start) * 4);
			set_range(x, x.start - gap, x.end - gap, 0, x.frames());
		}
		const u32 next = rd32(m_dram, sp::NEXT_FREE - DRAM) & 0xffffff;
		if (next >= gap) {
			std::memset(ram + size_t(next - gap) * 4, 0, size_t(gap) * 4);
			wr32(m_dram, sp::NEXT_FREE - DRAM, (next - gap) | WORD_FLAG);
		}
	}
	return true;
}

bool mu2000::sampling_loop(int number, bool on, u32 loop_from)
{
	for (const sp::sample &s : sampling_list())
		if (s.number == number)
			return sampling_points(number, s.play_from, s.play_to, on, loop_from);
	return false;
}

bool mu2000::sampling_points(int number, u32 from, u32 to, bool on, u32 loop_from)
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		put_play(m_dram, play_rec(number), s.start, s.end, on, from, to, loop_from);
		// EDIT → SAMPLE の Loop と同じく記録の印にも 0x02（パネルの表示が合う）
		const u32 o = sample_rec(number);
		m_dram[o + 2] = u8(on ? m_dram[o + 2] | 0x02 : m_dram[o + 2] & ~0x02);
		return true;
	}
	return false;
}

namespace {

// サンプリング RAM のサンプル s の頭から i 番目
s16 pcm_at(const std::vector<u8> &ram, const sp::sample &s, u32 i)
{
	const size_t o = (size_t(s.start) * 2 + i) * 2;
	return o + 1 < ram.size() ? s16(ram[o] | ram[o + 1] << 8) : 0;
}

void pcm_put(std::vector<u8> &ram, const sp::sample &s, u32 i, long v)
{
	const size_t o = (size_t(s.start) * 2 + i) * 2;
	if (o + 1 >= ram.size())
		return;
	const s16 w = s16(std::clamp(v, -32768L, 32767L));
	ram[o] = u8(w);
	ram[o + 1] = u8(u16(w) >> 8);
}

} // namespace

bool mu2000::sampling_snap(int number, u32 at, bool even, u32 range, u32 &out) const
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		const u32 n = s.frames();
		// i の直前が負で i が 0 以上（下から上へ横切る）所のうち、at にいちばん近いもの
		auto rising = [&](u32 i) { return i > 0 && i < n && pcm_at(m_sampram, s, i - 1) < 0 && pcm_at(m_sampram, s, i) >= 0; };
		for (u32 d = 0; d <= range; d++) {
			for (int sgn : { -1, 1 }) {
				if (d == 0 && sgn > 0)
					continue;
				const long i = long(at) + sgn * long(d);
				if (i <= 0 || i >= long(n) || (even && (i & 1)))
					continue;
				// 偶数に限るときは、その 1 つ前で横切っていてもよい（ループの頭は語の境にしか置けない）
				if (rising(u32(i)) || (even && rising(u32(i) + 1))) {
					out = u32(i);
					return true;
				}
			}
		}
		return false;
	}
	return false;
}

bool mu2000::sampling_match_end(int number, u32 loop_from, u32 near, u32 range, u32 &out) const
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		const u32 n = s.frames();
		// E のまわり [E - W, E + W) が L のまわり [L - W, L + W) と同じ形なら、E から L へ戻ってもつながる。
		// 差の 2 乗の和を両方の大きさで割ったものがいちばん小さい E（E の後ろが無ければ前の半分だけで比べる）
		const long W = 256;
		const long lo = std::max<long>(long(loop_from) + 64, long(near) - long(range));
		const long hi = std::min<long>(long(n), long(near) + long(range));
		double best = 1e30;
		long best_e = -1;
		for (long e = lo; e <= hi; e++) {
			double diff = 0, energy = 0;
			for (long k = -W; k < W; k++) {
				const long a = long(loop_from) + k, b = e + k;
				if (a < 0 || b < 0 || b >= long(n) || a >= e)
					continue;
				const double x = pcm_at(m_sampram, s, u32(a)), y = pcm_at(m_sampram, s, u32(b));
				diff += (x - y) * (x - y);
				energy += x * x + y * y;
			}
			if (energy <= 0)
				continue;
			const double score = diff / energy;
			if (score < best) {
				best = score;
				best_e = e;
			}
		}
		if (best_e < 0)
			return false;
		out = u32(best_e);
		return true;
	}
	return false;
}

bool mu2000::sampling_pcm(int number, std::vector<s16> &out) const
{
	out.clear();
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		out.resize(s.frames());
		for (u32 i = 0; i < s.frames(); i++)
			out[i] = pcm_at(m_sampram, s, i);
		return true;
	}
	return false;
}

namespace smu2000::sampling {

bool find_loop(const std::vector<s16> &pcm, u32 from, u32 to, u32 min_len, u32 &loop_from, u32 &loop_to)
{
	// 候補は下から上へ 0 を横切る所。L と E のまわり ±W の形を比べ（差の 2 乗の和を両方の大きさで割る）、
	// いちばん似ている組を選ぶ。まず 4 つおきに粗く比べ、よかった組の E を 1 サンプルずつ詰める。
	// 小さすぎる所（区間の中でいちばん大きい所の 1/20 未満の大きさ）は候補にしない（無音どうしは似て見える）
	const long W = 256;
	to = std::min<u32>(to, u32(pcm.size()));
	if (to <= from || to - from < min_len + 2 * W)
		return false;
	auto at = [&](long i) { return i >= 0 && i < long(pcm.size()) ? double(pcm[size_t(i)]) : 0.0; };
	auto energy = [&](long c) {
		double e = 0;
		for (long k = -W; k < W; k += 4)
			e += at(c + k) * at(c + k);
		return e;
	};
	std::vector<u32> cand;
	for (u32 i = std::max<u32>(from, W) + 1; i + W < to; i++)
		if (pcm[i - 1] < 0 && pcm[i] >= 0)
			cand.push_back(i);
	if (cand.size() < 2)
		return false;
	// 多すぎれば等間隔に間引く（組の数が候補の 2 乗になる）
	const size_t MAX = 320;
	if (cand.size() > MAX) {
		std::vector<u32> thin;
		for (size_t j = 0; j < MAX; j++)
			thin.push_back(cand[j * cand.size() / MAX]);
		cand.swap(thin);
	}
	std::vector<double> en(cand.size());
	double emax = 0;
	for (size_t j = 0; j < cand.size(); j++)
		emax = std::max(emax, en[j] = energy(cand[j]));
	auto score = [&](long l, long e, long step) {
		double diff = 0, sum = 0;
		for (long k = -W; k < W; k += step) {
			const double x = at(l + k), y = at(e + k);
			diff += (x - y) * (x - y);
			sum += x * x + y * y;
		}
		return sum > 0 ? diff / sum : 1e30;
	};
	double best = 1e30;
	long bl = -1, be = -1;
	for (size_t a = 0; a < cand.size(); a++) {
		if (en[a] < emax * 0.05)
			continue;
		for (size_t b = a + 1; b < cand.size(); b++) {
			if (cand[b] - cand[a] < min_len || en[b] < emax * 0.05)
				continue;
			const double s = score(cand[a], cand[b], 4);
			if (s < best) {
				best = s;
				bl = cand[a];
				be = cand[b];
			}
		}
	}
	if (bl < 0)
		return false;
	// ループの頭は偶数（語の境）。E をそれに合わせて 1 サンプルずつ詰める
	bl &= ~1L;
	double fine = 1e30;
	long fe = be;
	for (long e = std::max<long>(bl + long(min_len), be - 24); e <= std::min<long>(long(to), be + 24); e++)
		if (const double s = score(bl, e, 1); s < fine) {
			fine = s;
			fe = e;
		}
	loop_from = u32(bl);
	loop_to = u32(fe);
	return true;
}

std::vector<std::vector<u8>> voice_sysex(int slot, const u8 *rec, int device)
{
	std::vector<std::vector<u8>> out;
	if (slot < 0 || slot >= MAX_VOICES || !rec)
		return out;
	const u8 base = u8(0x40 + 0x10 * (slot / 128));
	const u8 pgm = u8(slot % 128);
	auto msg = [&](u8 ah, u8 al, std::initializer_list<u8> data) {
		std::vector<u8> m = { 0xf0, 0x43, u8(0x10 | (device & 0x0f)), 0x68, ah, pgm, al };
		for (u8 d : data)
			m.push_back(d & 0x7f);
		m.push_back(0xf7);
		out.push_back(std::move(m));
	};
	// 要素 1-4: 波形（2 バイト）と [4]-[83]。波形の通を受けると firmware は要素 1 の [0] を 01 にする
	for (int e = 0; e < 4; e++) {
		const u8 *el = rec + 12 + 84 * e;
		const u8 ah = u8(base + 1 + e);
		msg(ah, 0x00, { el[2], el[3] });
		for (int i = 4; i < 84; i++)
			msg(ah, u8(i - 2), { el[i] });
	}
	// 頭: 使う要素の印、+1、名前。**要素の後に送る**: 要素 2 の波形の通を受けると、firmware は
	// 頭の +0（使う要素の印）を 5b に書き換える（ほかの要素では起きない）
	msg(base, 0x01, { rec[0] });
	msg(base, 0x02, { rec[1] });
	for (int i = 0; i < 8; i++)
		msg(base, u8(0x03 + i), { rec[2 + i] });
	return out;
}

namespace {

// 機種 0x68 の一括ダンプ 1 通。検査の和は、数・番地・データと足して下 7bit が 0 になる値
std::vector<u8> bulk68(int device, u8 ah, u8 am, u8 al, const std::vector<u8> &data)
{
	std::vector<u8> m = { 0xf0, 0x43, u8(device & 0x0f), 0x68, u8(data.size() >> 7 & 0x7f), u8(data.size() & 0x7f), ah, am, al };
	int sum = 0;
	for (u8 d : data)
		m.push_back(d & 0x7f);
	for (size_t i = 4; i < m.size(); i++)
		sum += m[i];
	m.push_back(u8(-sum & 0x7f));
	m.push_back(0xf7);
	return m;
}

// 32bit を 7bit × 5 に（上から）
void put5(std::vector<u8> &o, u32 v)
{
	for (int sh = 28; sh >= 0; sh -= 7)
		o.push_back(u8(v >> sh & 0x7f));
}

// 波形 64 バイト → 74 バイト。7 バイトずつ下 7bit を並べ、その後ろに 7 つの上の 1bit（先のバイトが bit 6）。
// これを 9 組（63 バイト）、最後の 1 バイトは下 7bit・上 1bit の 2 バイト
std::vector<u8> pack64(const u8 *p)
{
	std::vector<u8> o;
	o.reserve(74);
	for (int g = 0; g < 9; g++) {
		u8 top = 0;
		for (int i = 0; i < 7; i++) {
			o.push_back(p[g * 7 + i] & 0x7f);
			top |= u8((p[g * 7 + i] >> 7) << (6 - i));
		}
		o.push_back(top);
	}
	o.push_back(p[63] & 0x7f);
	o.push_back(p[63] >> 7);
	return o;
}

} // namespace

std::vector<std::vector<u8>> memory_sysex(const std::vector<u8> &dram, const std::vector<u8> &pcm, bool voices, int device)
{
	std::vector<std::vector<u8>> out;
	const u32 next = std::min(rd32(dram, NEXT_FREE - DRAM) & 0xffffff, RAM_WORDS);
	if (size_t(next) * 4 > pcm.size())
		return out;
	out.push_back({ 0xf0, 0x43, u8(0x10 | (device & 0x0f)), 0x68, 0x00, 0x00, 0x7f, 0x00, 0xf7 });
	// 波形。サンプリング RAM は 16bit の下のバイトが先なので、入れ替えて送る
	// 書く位置は 64 塊ごとに入れ直す。実機は長く送ると 1000 通に 1 通ほど取りこぼし（2026-10-04 に 17,950 通で 17 通）、
	// 位置は受けた通の数で進むので、入れ直さないと取りこぼした所から後ろが全部 1 塊ずつ前へずれる
	for (size_t at = 0; at < size_t(next) * 4; at += 64) {
		if (at % (64 * PCM_RESYNC_BLOCKS) == 0) {
			const u32 b = u32(at / 64);
			out.push_back(bulk68(device, 0x00, 0x00, 0x00, { u8(b >> 21 & 0x7f), u8(b >> 14 & 0x7f), u8(b >> 7 & 0x7f), u8(b & 0x7f) }));
		}
		u8 blk[64] = {};
		for (size_t i = 0; i < 64 && at + i < size_t(next) * 4; i++)
			blk[i] = pcm[(at + i) ^ 1];
		out.push_back(bulk68(device, 0x00, 0x01, 0x00, pack64(blk)));
	}
	std::vector<u8> d;
	put5(d, next);
	out.push_back(bulk68(device, 0x00, 0x00, 0x10, d));
	// サンプルの記録・名前・鳴らすための表
	for (int n = 1; n <= MAX_SAMPLES; n++) {
		const u32 r = sample_rec(n), s = play_rec(n);
		if (!(dram[r + 2] & 0x40))
			continue;
		const u8 ah = u8(0x10 | (n - 1) >> 7), am = u8((n - 1) & 0x7f);
		// ステレオの相手は +4 に記録の番地で入っている（無ければ 0）。送るのは番号で、無しは 0x3fff
		const u32 pair_at = rd32(dram, r + 4);
		const u32 pair = pair_at >= TAB_SAMPLE ? (pair_at - TAB_SAMPLE) / 36 & 0x3fff : 0x3fff;
		d = { dram[r + 2], dram[r + 3], 0x00, u8(pair >> 7), u8(pair & 0x7f) };
		put5(d, rd32(dram, r + 12));
		put5(d, rd32(dram, r + 16) & 0xffffff);
		put5(d, rd32(dram, r + 20) & 0xffffff);
		d.push_back(dram[r + 25]);
		d.push_back(dram[r + 24]);
		out.push_back(bulk68(device, ah, am, 0x00, d));
		out.push_back(bulk68(device, ah, am, 0x70, std::vector<u8>(dram.begin() + r + 28, dram.begin() + r + 36)));
		// 鳴らすための表: +0、+3、+1、+2（符号と大きさ）、ループの頭の語、+9〜+11、+5〜+7、+8、+4
		const int fine = s8(dram[s + 2]);
		d = { dram[s], dram[s + 3], dram[s + 1], u8((dram[s + 3] & 0x80 ? 0x40 : 0) | (fine < 0 ? 0x01 : 0)),
		      u8(fine < 0 ? -fine : fine) };
		put5(d, rd32(dram, s + 12) & 0xffffff);
		put5(d, rd32(dram, s + 8) & 0xffffff);
		put5(d, rd32(dram, s + 4) & 0xffffff);
		for (u8 b : { dram[s + 8], dram[s + 4] }) {
			d.push_back(b >> 7);
			d.push_back(b & 0x7f);
		}
		out.push_back(bulk68(device, ah, am, 0x20, d));
	}
	// サンプルを鳴らす音色（要素のどれかの波形がサンプル）
	for (int slot = 0; voices && slot < MAX_VOICES; slot++) {
		const u8 *rec = &dram[voice_rec(slot)];
		bool uses = false;
		for (int e = 0; e < VOICE_ELEMENTS; e++)
			uses = uses || ((rec[0] >> e & 1) && (rec[12 + 84 * e + 2] & 0x40));
		if (!uses)
			continue;
		for (auto &m : voice_sysex(slot, rec, device))
			out.push_back(std::move(m));
	}
	return out;
}

} // namespace smu2000::sampling

bool mu2000::sampling_crossfade(int number, u32 loop_from, u32 to, u32 len, bool power)
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		const u32 n = s.frames();
		to = std::min(to ? to : n, n);
		// E の手前 len を、L の手前 len と少しずつ混ぜる。終わりで L の手前とそろうので、E から L へなめらかにつながる
		len = std::min({ len, loop_from, to > loop_from ? to - loop_from : 0u });
		if (len < 2)
			return false;
		const double PI = 3.14159265358979323846;
		// 似た波形どうしは足して 1 の曲線（音量が揃う）。揺れのある音は 2 乗して足して 1 の曲線（途中で痩せない）
		for (u32 k = 0; k < len; k++) {
			const double t = double(k + 1) / double(len);
			const double wa = power ? std::cos(PI * 0.5 * t) : 0.5 + 0.5 * std::cos(PI * t);
			const double wb = power ? std::sin(PI * 0.5 * t) : 1.0 - wa;
			const double a = pcm_at(m_sampram, s, to - len + k), b = pcm_at(m_sampram, s, loop_from - len + k);
			pcm_put(m_sampram, s, to - len + k, std::lround(a * wa + b * wb));
		}
		// 鳴らすときに E の少し先まで読むことがあるので、E の後ろ（鳴らさない所）を L の後ろと同じに
		for (u32 k = 0; k < 4 && to + k < n; k++)
			pcm_put(m_sampram, s, to + k, pcm_at(m_sampram, s, loop_from + k));
		return true;
	}
	return false;
}

bool mu2000::sampling_bounds(int number, double ratio, u32 &from, u32 &to) const
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		auto at = [&](u32 i) {
			const u32 o = (s.start * 2 + i) * 2;
			const s16 v = s16(m_sampram[o] | m_sampram[o + 1] << 8);
			return v < 0 ? -int(v) : int(v);
		};
		const u32 n = s.frames();
		if ((s.start * 2 + n) * 2 > m_sampram.size())
			return false;
		int peak = 0;
		for (u32 i = 0; i < n; i++)
			peak = std::max(peak, at(i));
		if (!peak)
			return false;
		const int thr = std::max(1, int(std::lround(peak * ratio)));
		u32 a = 0, b = n;
		while (a < n && at(a) < thr)
			a++;
		while (b > a && at(b - 1) < thr)
			b--;
		if (b - a < 8)
			b = std::min(n, a + 8);
		from = a;
		to = b;
		return true;
	}
	return false;
}

bool mu2000::sampling_overview(int number, int buckets, std::vector<s16> &lo, std::vector<s16> &hi, u32 &frames,
                               u32 from, u32 to) const
{
	lo.clear();
	hi.clear();
	frames = 0;
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		frames = s.frames();
		if (!frames || buckets <= 0)
			return false;
		if (to <= from || to > frames) {
			if (to <= from)
				from = 0;
			to = frames;
		}
		const u32 span = to - from;
		if (u32(buckets) > span)
			buckets = int(span);
		lo.assign(size_t(buckets), 32767);
		hi.assign(size_t(buckets), -32768);
		for (u32 i = from; i < to; i++) {
			const u32 at = (s.start * 2 + i) * 2;
			if (at + 1 >= m_sampram.size())
				break;
			const s16 v = s16(m_sampram[at] | m_sampram[at + 1] << 8);
			const size_t b = size_t(u64(i - from) * u64(buckets) / span);
			lo[b] = std::min(lo[b], v);
			hi[b] = std::max(hi[b], v);
		}
		return true;
	}
	return false;
}

u32 mu2000::sampling_free_frames() const
{
	const u32 next = rd32(m_dram, sp::NEXT_FREE - DRAM) & 0xffffff;
	return next >= sp::RAM_WORDS ? 0 : (sp::RAM_WORDS - next) * 2;
}

int mu2000::sampling_add(const s16 *pcm, size_t frames, const std::string &name, std::string &err)
{
	if (frames < 8) {
		err = "the sample is too short";
		return 0;
	}
	if (frames > sampling_free_frames()) {
		err = "not enough sampling memory left";
		return 0;
	}
	// firmware は番号の若い空きから使う（REC の Sp= に出る番号）
	int n = 0;
	for (int i = 1; i <= sp::MAX_SAMPLES && !n; i++)
		if (!(m_dram[sample_rec(i) + 2] & 0x40))
			n = i;
	if (!n) {
		err = "all 512 sample slots are in use";
		return 0;
	}
	const u32 start = rd32(m_dram, sp::NEXT_FREE - DRAM) & 0xffffff;
	const u32 words = u32((frames + 1) / 2);
	const u32 end = start + words;
	// 波形。1 語に 2 つ、下の 16bit が先（swp30.cpp の sample_step と同じ）
	for (size_t i = 0; i < size_t(words) * 2; i++) {
		const u16 v = u16(i < frames ? pcm[i] : 0);
		const u32 at = (start * 2 + u32(i)) * 2;
		m_sampram[at] = u8(v);
		m_sampram[at + 1] = u8(v >> 8);
	}
	// 鳴らすための表
	put_play(m_dram, play_rec(n), start, end, false, 0, 0, 0);
	// サンプルの記録
	const u32 o = sample_rec(n);
	m_dram[o] = u8((n - 1) >> 8);
	m_dram[o + 1] = u8(n - 1);
	m_dram[o + 2] = 0x40;
	m_dram[o + 3] = 0;
	wr32(m_dram, o + 4, 0);
	wr32(m_dram, o + 8, 0xffffffff);
	wr32(m_dram, o + 12, sp::SAMPLE_RATE);
	wr32(m_dram, o + 16, start | WORD_FLAG);
	wr32(m_dram, o + 20, end | WORD_FLAG);
	wr32(m_dram, o + 24, 0x04000000);
	char def[16];
	std::snprintf(def, sizeof(def), "take%03d", n);
	put_text(m_dram, o + 28, 8, name.empty() ? std::string(def) : name, ' ');
	wr32(m_dram, sp::NEXT_FREE - DRAM, end | WORD_FLAG);
	return n;
}

// 機種 0x68 の SysEx（memory_sysex の列や、実機から読み出した一括ダンプ）を読み込む。波形・サンプルの記録・名前・
// 鳴らすための表・次に録る語は、firmware が受けたときと同じ結果を表とサンプリング RAM に直に書く（MIDI の速さで
// 待たない）。ほかの通（音色など）は rest に集めて返すので、MIDI の入口に入れて firmware に任せること。「全部を消す」の通は処理せず、
// あったかどうかを wipes で返す（先にそれだけ MIDI で送り、firmware が消し終えてから呼ぶこと）
int mu2000::sampling_load_sysex(const std::vector<u8> &bytes, bool &wipes, std::vector<u8> &rest)
{
	int direct = 0;
	wipes = false;
	rest.clear();
	u32 at = 0;                                    // 波形を書く位置（64 バイトの塊の番号）
	auto v7 = [](const u8 *d, int n) {
		u32 v = 0;
		for (int i = 0; i < n; i++)
			v = v << 7 | (d[i] & 0x7f);
		return v;
	};
	for (size_t i = 0; i < bytes.size(); i++) {
		if (bytes[i] != 0xf0)
			continue;
		size_t end = i + 1;
		while (end < bytes.size() && bytes[end] != 0xf7 && bytes[end] != 0xf0)
			end++;
		if (end >= bytes.size() || bytes[end] != 0xf7) {
			i = end - 1;
			continue;
		}
		const u8 *m = &bytes[i];
		const size_t len = end - i + 1;
		i = end;
		if (len < 9 || m[1] != 0x43 || m[3] != 0x68)
			continue;
		if ((m[2] & 0xf0) == 0x10 && m[4] == 0x00 && m[5] == 0x00 && m[6] == 0x7f) {
			wipes = true;
			continue;
		}
		bool done = false;
		if ((m[2] & 0xf0) == 0x00 && len >= 11) {
			const u32 count = u32(m[4]) << 7 | m[5];
			const u8 ah = m[6], am = m[7], al = m[8];
			const u8 *d = m + 9;
			int sum = 0;
			for (size_t k = 4; k + 1 < len; k++)
				sum += m[k];
			if (count + 11 == len && !(sum & 0x7f)) {
				if (ah == 0x00 && am == 0x00 && al == 0x00 && count == 4) {
					at = v7(d, 4);
					done = true;
				} else if (ah == 0x00 && am == 0x01 && count == 74) {
					// 7 バイトの下 7bit と、その上の 1bit を集めた 1 バイトが 9 組、最後の 1 バイトは 2 バイトで
					u8 blk[64];
					for (int g = 0; g < 9; g++)
						for (int k = 0; k < 7; k++)
							blk[g * 7 + k] = u8(d[g * 8 + k] | ((d[g * 8 + 7] >> (6 - k)) & 1) << 7);
					blk[63] = u8(d[72] | (d[73] & 1) << 7);
					if (size_t(at) * 64 + 64 <= m_sampram.size())
						for (u32 k = 0; k < 64; k++)
							m_sampram[size_t(at) * 64 + (k ^ 1)] = blk[k];      // 送られてくるのは上のバイトが先
					at++;
					done = true;
				} else if (ah == 0x00 && am == 0x00 && al == 0x10 && count == 5) {
					wr32(m_dram, sp::NEXT_FREE - DRAM, (v7(d, 5) & 0xffffff) | WORD_FLAG);
					done = true;
				} else if ((ah & 0xf0) == 0x10 && (u32(ah & 0x03) << 7 | am) < u32(sp::MAX_SAMPLES)) {
					const int n = int(u32(ah & 0x03) << 7 | am) + 1;
					const u32 r = sample_rec(n), p = play_rec(n);
					if (al == 0x00 && count == 22) {
						const u32 pair = v7(d + 3, 2);
						m_dram[r] = u8((n - 1) >> 8);
						m_dram[r + 1] = u8(n - 1);
						m_dram[r + 2] = d[0];
						m_dram[r + 3] = d[1];
						wr32(m_dram, r + 4, pair < u32(sp::MAX_SAMPLES) ? sp::TAB_SAMPLE + 36 * pair : 0);
						wr32(m_dram, r + 8, 0xffffffff);
						wr32(m_dram, r + 12, v7(d + 5, 5));
						wr32(m_dram, r + 16, (v7(d + 10, 5) & 0xffffff) | WORD_FLAG);
						wr32(m_dram, r + 20, (v7(d + 15, 5) & 0xffffff) | WORD_FLAG);
						wr32(m_dram, r + 24, u32(d[21]) << 24 | u32(d[20]) << 16);
						done = true;
					} else if (al == 0x70 && count == 8) {
						std::memcpy(&m_dram[r + 28], d, 8);
						done = true;
					} else if (al == 0x20 && count == 24) {
						m_dram[p] = d[0];
						m_dram[p + 1] = d[2];
						m_dram[p + 2] = u8(d[3] & 1 ? -int(d[4]) : int(d[4]));
						m_dram[p + 3] = d[1];                  // firmware も下 7bit だけで書く
						wr32(m_dram, p + 4, u32(u8(d[22] << 7 | d[23])) << 24 | (v7(d + 15, 5) & 0xffffff));
						wr32(m_dram, p + 8, u32(u8(d[20] << 7 | d[21])) << 24 | (v7(d + 10, 5) & 0xffffff));
						wr32(m_dram, p + 12, (v7(d + 5, 5) & 0xffffff) | WORD_FLAG);
						done = true;
					}
				}
			}
		}
		if (done)
			direct++;
		else
			rest.insert(rest.end(), m, m + len);
	}
	return direct;
}

bool mu2000::sampling_voice(int slot, sp::voice &out) const
{
	if (slot < 0 || slot >= sp::MAX_VOICES)
		return false;
	const u32 o = voice_rec(slot);
	out.name = text(m_dram, o + 2, 8);
	const u8 mask = m_dram[o];
	for (int e = 0; e < sp::VOICE_ELEMENTS; e++) {
		sp::element &x = out.el[size_t(e)];
		const u32 b = o + 12 + 84 * u32(e);
		x.on = (mask >> e) & 1;
		// 要素の波形の欄（[2]・[3]）は内蔵の音色の要素と同じ。0x4000 が立っていればサンプル、
		// 立っていなければ内蔵の波形の組（7bit が 2 つ）、3f 7f は無し
		const u16 sv = u16(m_dram[b + 2] << 8 | m_dram[b + 3]);
		x.assigned = (sv & 0x4000) != 0;
		x.sample = x.assigned ? int(sv & 0x1ff) + 1 : 0;
		const int set = (m_dram[b + 2] << 7) | (m_dram[b + 3] & 0x7f);
		x.rom_wave = !x.assigned && set < sp::ROM_WAVE_SETS ? set : -1;
		x.key_lo = m_dram[b + 4] & 0x7f;
		x.key_hi = m_dram[b + 5] & 0x7f;
		x.vel_lo = m_dram[b + 6] & 0x7f;
		x.vel_hi = m_dram[b + 7] & 0x7f;
		x.coarse = int(m_dram[b + 17]) - 0x40;
		x.fine = int(m_dram[b + 18]) - 0x40;
		x.level = m_dram[b + 59];
		x.pan = m_dram[b + 69];
		x.attack = m_dram[b + 73] & 0x3f;
		x.decay1 = m_dram[b + 74] & 0x3f;
		x.decay2 = m_dram[b + 75] & 0x3f;
		x.release = m_dram[b + 76] & 0x3f;
		x.level1 = m_dram[b + 77] & 0x7f;
		x.level2 = m_dram[b + 78] & 0x7f;
		x.cutoff = m_dram[b + 37] & 0x7f;
		x.resonance = m_dram[b + 35] & 0x7f;
		x.hpf = m_dram[b + 82] & 0x7f;
		x.vel_curve = m_dram[b + 68] & 0x7f;
		x.lfo_wave = std::min(int(m_dram[b + 9]), 2);
		x.lfo_phase_init = m_dram[b + 10] != 0;
		x.lfo_speed = m_dram[b + 11] & 0x3f;
		x.lfo_delay = m_dram[b + 12] & 0x7f;
		x.lfo_pitch = m_dram[b + 14] & 0x7f;
		x.lfo_filter = m_dram[b + 15] & 0x7f;
		x.lfo_amp = m_dram[b + 16] & 0x7f;
		x.peg_depth = m_dram[b + 21] & 0x7f;
		for (int i = 0; i < 4; i++) {
			x.peg_rate[i] = m_dram[b + 26 + u32(i)] & 0x3f;
			x.feg_rate[i] = m_dram[b + 50 + u32(i)] & 0x3f;
		}
		for (int i = 0; i < 5; i++) {
			x.peg_level[i] = int(m_dram[b + 30 + u32(i)] & 0x7f) - 0x40;
			x.feg_level[i] = int(m_dram[b + 54 + u32(i)] & 0x7f) - 0x40;
		}
	}
	return true;
}

bool mu2000::sampling_set_voice(int slot, const sp::voice &v, std::string &err)
{
	if (slot < 0 || slot >= sp::MAX_VOICES) {
		err = "no such voice";
		return false;
	}
	for (const sp::element &x : v.el)
		if (x.on && x.assigned && (x.sample < 1 || x.sample > sp::MAX_SAMPLES || !(m_dram[sample_rec(x.sample) + 2] & 0x40))) {
			err = "no such sample";
			return false;
		}
	const u32 o = voice_rec(slot);
	// 名前は 8 文字で、余りは空白（0 で埋めると LCD が CGRAM の 0 番の字を出す）。+10・+11 は別の欄
	if (!v.name.empty())
		put_text(m_dram, o + 2, 8, v.name, ' ');
	// 使う要素の印（ビットごと）。使わない要素の中身は触らない
	u8 mask = 0;
	for (int e = 0; e < sp::VOICE_ELEMENTS; e++)
		if (v.el[size_t(e)].on)
			mask |= u8(1 << e);
	m_dram[o] = mask;
	for (int e = 0; e < sp::VOICE_ELEMENTS; e++) {
		const sp::element &x = v.el[size_t(e)];
		if (!x.on)
			continue;
		const u32 b = o + 12 + 84 * u32(e);
		if (x.assigned) {
			m_dram[b] = 0x01;
			m_dram[b + 1] = 0x7f;
			const u16 sv = u16(0x4000 | (x.sample - 1));
			m_dram[b + 2] = u8(sv >> 8);
			m_dram[b + 3] = u8(sv);
		} else if (x.rom_wave >= 0 && x.rom_wave < sp::ROM_WAVE_SETS) {
			// 内蔵の波形の組。[0] は firmware がサンプルのときだけ 01 にする欄で、鳴るかどうかは変えない
			m_dram[b] = 0x00;
			m_dram[b + 1] = 0x7f;
			m_dram[b + 2] = u8(x.rom_wave >> 7);
			m_dram[b + 3] = u8(x.rom_wave & 0x7f);
		} else {
			m_dram[b] = 0x00;
			m_dram[b + 1] = 0x7f;
			m_dram[b + 2] = 0x3f;
			m_dram[b + 3] = 0x7f;
		}
		const int klo = std::clamp(x.key_lo, 0, 127), khi = std::clamp(x.key_hi, 0, 127);
		const int vlo = std::clamp(x.vel_lo, 1, 127), vhi = std::clamp(x.vel_hi, 1, 127);
		m_dram[b + 4] = u8(std::min(klo, khi));
		m_dram[b + 5] = u8(std::max(klo, khi));
		m_dram[b + 6] = u8(std::min(vlo, vhi));
		m_dram[b + 7] = u8(std::max(vlo, vhi));
		m_dram[b + 17] = u8(0x40 + std::clamp(x.coarse, -24, 24));
		m_dram[b + 18] = u8(0x40 + std::clamp(x.fine, -64, 63));
		m_dram[b + 59] = u8(std::clamp(x.level, 0, 127));
		m_dram[b + 69] = u8(std::clamp(x.pan, 0, 15));
		m_dram[b + 73] = u8(std::clamp(x.attack, 0, 63));
		m_dram[b + 74] = u8(std::clamp(x.decay1, 0, 63));
		m_dram[b + 75] = u8(std::clamp(x.decay2, 0, 63));
		m_dram[b + 76] = u8(std::clamp(x.release, 0, 63));
		m_dram[b + 77] = u8(std::clamp(x.level1, 0, 127));
		m_dram[b + 78] = u8(std::clamp(x.level2, 0, 127));
		m_dram[b + 37] = u8(std::clamp(x.cutoff, 0, 127));
		m_dram[b + 35] = u8(std::clamp(x.resonance, 0, 127));
		m_dram[b + 82] = u8(std::clamp(x.hpf, 0, 127));
		m_dram[b + 68] = u8(std::clamp(x.vel_curve, 0, 10));
		m_dram[b + 9] = u8(std::clamp(x.lfo_wave, 0, 2));
		m_dram[b + 10] = x.lfo_phase_init ? 1 : 0;
		m_dram[b + 11] = u8(std::clamp(x.lfo_speed, 0, 63));
		m_dram[b + 12] = u8(std::clamp(x.lfo_delay, 0, 127));
		m_dram[b + 14] = u8(std::clamp(x.lfo_pitch, 0, 127));
		m_dram[b + 15] = u8(std::clamp(x.lfo_filter, 0, 127));
		m_dram[b + 16] = u8(std::clamp(x.lfo_amp, 0, 127));
		m_dram[b + 21] = u8(std::clamp(x.peg_depth, 0, 127));
		for (int i = 0; i < 4; i++) {
			m_dram[b + 26 + u32(i)] = u8(std::clamp(x.peg_rate[i], 0, 63));
			m_dram[b + 50 + u32(i)] = u8(std::clamp(x.feg_rate[i], 0, 63));
		}
		for (int i = 0; i < 5; i++) {
			m_dram[b + 30 + u32(i)] = u8(0x40 + std::clamp(x.peg_level[i], -64, 63));
			m_dram[b + 54 + u32(i)] = u8(0x40 + std::clamp(x.feg_level[i], -64, 63));
		}
	}
	return true;
}

bool mu2000::preview_start(int number, u32 from, u32 to, u32 loop_at)
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		to = std::min(to ? to : s.frames(), s.frames());
		if (from >= to)
			return false;
		m_prev_ext.clear();
		m_prev_number = number;
		m_prev_base = s.start * 2;
		m_prev_pos = from;
		m_prev_end = to;
		m_prev_loop = loop_at < to ? loop_at : ~0u;
		m_prev_on = true;
		return true;
	}
	return false;
}

// keep_pos なら、いま外の波形を鳴らしている位置から続ける（鳴らしたまま波形を差し替える用。長さが変わって
// はみ出すなら頭から）
void mu2000::preview_pcm(std::vector<s16> pcm, u32 loop_at, bool keep_pos)
{
	const u32 pos = keep_pos && m_prev_on && m_prev_number == -1 ? m_prev_pos : 0;
	m_prev_on = false;
	if (pcm.empty())
		return;
	m_prev_ext = std::move(pcm);
	m_prev_number = -1;
	m_prev_base = 0;
	m_prev_end = u32(m_prev_ext.size());
	m_prev_pos = pos < m_prev_end ? pos : 0;
	m_prev_loop = loop_at < m_prev_end ? loop_at : ~0u;
	m_prev_on = true;
}

void mu2000::rec_start(sp::source src, int trigger, u32 max_frames)
{
	m_rec_src = src;
	m_rec_trigger = std::clamp(trigger, 0, 32767);
	m_rec_max = std::min(max_frames, sampling_free_frames());
	m_rec_buf.clear();
	m_rec_buf.reserve(m_rec_max);
	m_rec_state = m_rec_trigger > 0 ? 1 : 2;
}

std::vector<s16> mu2000::rec_take()
{
	m_rec_state = 0;
	std::vector<s16> out;
	out.swap(m_rec_buf);
	return out;
}

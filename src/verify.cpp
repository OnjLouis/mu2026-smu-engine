// license:BSD-3-Clause
//
// 移植が成立しているかの最小確認
#include "mame/sound/swp30.h"
#include "compat/a64asm.h"
#include "ui/bend_thinner.h"
#include <cstdio>
#include <vector>

// 再生のピッチベンドの間引き（ui/bend_thinner.h）。1ms おきに 1000 個のベンドの途中に音符を挟み、
// 送った数、音符の直前に送ったベンドがその時点の最新の値か、最後の値が届くかを見る
static void check_bend_thinner()
{
	ui::bend_thinner th;
	struct sent { int to; u8 b[3]; };
	std::vector<sent> out;
	auto send = [&out](int to, const u8 *d, size_t n) {
		sent s{ to, { 0, 0, 0 } };
		for (size_t i = 0; i < n && i < 3; i++)
			s.b[i] = d[i];
		out.push_back(s);
	};
	int bends_sent = 0;
	bool note_ok = false;
	u8 latest = 0;
	for (int i = 0; i < 1000; i++) {
		const double t = i * 0.001;
		const u8 msb = u8(i & 0x7f);
		const u8 bend[3] = { 0xe0, 0x00, msb };
		th.event(0, bend, 3, t, send);
		latest = msb;
		th.tick(t, false, send);
		if (i == 500) {
			const u8 on[3] = { 0x90, 60, 100 };
			th.event(0, on, 3, t + 0.0001, send);
			// 音符の直前に出たものが、今の最新のベンド
			note_ok = out.size() >= 2 && out[out.size() - 2].b[0] == 0xe0 && out[out.size() - 2].b[2] == latest &&
			          out.back().b[0] == 0x90;
		}
	}
	th.tick(1.0, true, send);
	for (const sent &s : out)
		if (s.b[0] == 0xe0)
			bends_sent++;
	const bool last_ok = !out.empty() && out.back().b[0] == 0xe0 && out.back().b[2] == latest;
	std::printf("ピッチベンドの間引き: 1000 個 → %d 個、音符の前の高さ %s、最後の値 %s\n",
	            bends_sent, note_ok ? "合" : "NG", last_ok ? "合" : "NG");
}

int main()
{
	std::vector<u8>  wave(64 * 1024 * 1024, 0);   // 波形 ROM 相当のダミー
	std::vector<u16> sintab(0x8000, 0);

	swp30_device swp;
	swp.set_wave_rom(wave.data(), wave.size());
	swp.set_sintab(sintab.data(), sintab.size());
	swp.reset();

	// レジスタの読み書きが素通しできるか（ピッチ = slot 0x11）
	swp.write16(0 * 0x40 + 0x11, 0x1234);
	u16 back = swp.read16(0 * 0x40 + 0x11);
	std::printf("ピッチレジスタ 書き 0x1234 -> 読み 0x%04x  %s\n",
	            back, back == 0x1234 ? "一致" : "不一致");

	// 1 サンプル回してみる（無音のはず）
	s32 l = 0, r = 0;
	for (int i = 0; i < 100; i++) swp.run_sample(l, r);
	std::printf("100 サンプル実行  最後の出力 L=%d R=%d\n", l, r);

	// 乱数が MAME と同じ数列か
	// printf の引数は評価順が未規定。1 個ずつ取り出さないと順序が入れ替わる
	std::printf("乱数 1〜3 個目:");
	for (int i = 0; i < 3; i++) std::printf(" %08x", swp.machine().rand());
	std::printf("\n");

	// MEG の JIT が機械語で書いたリバーブ RAM の詰め方・戻し方と係数の広げ方（m1_expand）が、C++ の関数と全部の入力で同じか
	std::printf("MEG の JIT のリバーブ RAM の詰め方と戻し方・係数の広げ方: 食い違い %llu\n",
	            (unsigned long long)swp30_device::meg_jit_selftest());

	// aarch64 emitter selftest: every primitive the arm64 JIT uses, executed
	// on this CPU and compared against plain C++ (a64asm.cpp; 0 on x86-64 builds)
	std::printf("a64 emitter selftest: %llu mismatch(es)\n", (unsigned long long)a64::selftest());

	check_bend_thinner();

	// Roland の液晶のデータ（F0 41 10 45 12）だけを抜き、GS リセット（F0 41 10 42 12）と XG（F0 43）は通す
	{
		ui::bend_thinner th;
		int passed = 0;
		auto send = [&passed](int, const u8 *, size_t) { passed++; };
		const u8 disp[] = { 0xf0, 0x41, 0x10, 0x45, 0x12, 0x10, 0x00, 0x00, 0x48, 0x49, 0x0f, 0xf7 };
		const u8 gs[]   = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
		const u8 xg[]   = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 };
		th.event(0, disp, sizeof(disp), 0.0, send);
		th.event(0, gs, sizeof(gs), 0.0, send);
		th.event(0, xg, sizeof(xg), 0.0, send);
		std::printf("Roland の液晶のデータを抜く: 抜いた %zu・通した %d（GS リセットと XG）\n", th.dropped(), passed);
	}
	return 0;
}

// license:BSD-3-Clause
//
// MIDI ファイルの再生で、MU2000 が追いつけない重いデータを軽くする（issue #82）。
// プレーヤーの選べる設定（既定は切り）。実機は同じファイルで遅れるので、実機の鳴り方からは外れる。
//
// * 詰まったピッチベンド。MU2000 の firmware は 1 秒に 1,500 個ほどしか処理できず、それより濃い流れは
//   溜まって遅れ、後でまとめて鳴る。同じ口・チャンネルのベンドが GAP より詰まって来たら最後の値だけを
//   持ち、GAP おきに送る。ほかのチャンネルの声（音符など）の前には持っている値を先に出すので、
//   音の頭の高さは変わらない。
// * Roland の液晶のデータ（F0 41 dd 45 12 …、SC-55〜SC-8850 の画面の文字と絵）。MU2000 は使わないのに、
//   SC の画面に動画を出す曲では 1 秒に 1 万バイトを超え、USB の受け口（実機で 1 秒 1 万バイト）を塞ぐ。送らない。

#ifndef S_MU2000_UI_BEND_THINNER_H
#define S_MU2000_UI_BEND_THINNER_H

#pragma once

#include "compat/mamecompat.h"

#include <algorithm>
#include <cstddef>

namespace ui {

class bend_thinner
{
public:
	static constexpr double GAP = 0.004;   // 1 チャンネルあたり 1 秒に 250 個まで

	// 届いたメッセージ（t 秒）。送るものは send(to, bytes, n) で出す
	template <typename F>
	void event(int to, const u8 *d, size_t n, double t, F &&send)
	{
		const u8 st = n ? d[0] : 0;
		if (is_roland_display(d, n)) {
			m_dropped++;
			return;
		}
		if (to < 0 || to >= PORTS || st < 0x80 || st >= 0xf0) {
			send(to, d, n);
			return;
		}
		held &h = m_held[to][st & 15];
		if ((st & 0xf0) == 0xe0 && n == 3) {
			if (t - h.last >= GAP) {
				send(to, d, n);
				h.last = t;
				h.have = false;
			} else {
				std::copy(d, d + 3, h.b);
				h.have = true;
			}
			return;
		}
		flush_one(to, st & 15, t, send);
		send(to, d, n);
	}

	// 持っている値のうち GAP たったものを送る（all なら全部）
	template <typename F>
	void tick(double now, bool all, F &&send)
	{
		for (int to = 0; to < PORTS; to++)
			for (int ch = 0; ch < 16; ch++)
				if (m_held[to][ch].have && (all || now - m_held[to][ch].last >= GAP))
					flush_one(to, ch, now, send);
	}

	// Roland の液晶のデータ（F0 41 <装置> 45 12 …）か
	static bool is_roland_display(const u8 *d, size_t n)
	{
		return n >= 5 && d[0] == 0xf0 && d[1] == 0x41 && d[3] == 0x45 && d[4] == 0x12;
	}
	// 送らずに捨てた液晶のデータの数（確かめる用）
	size_t dropped() const { return m_dropped; }

private:
	static constexpr int PORTS = 4;
	size_t m_dropped = 0;
	struct held { double last = -1.0; bool have = false; u8 b[3] = {}; };
	held m_held[PORTS][16];

	template <typename F>
	void flush_one(int to, int ch, double now, F &&send)
	{
		held &h = m_held[to][ch];
		if (!h.have)
			return;
		send(to, h.b, size_t(3));
		h.have = false;
		h.last = now;
	}
};

} // namespace ui

#endif // S_MU2000_UI_BEND_THINNER_H

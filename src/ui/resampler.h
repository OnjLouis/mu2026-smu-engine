// license:BSD-3-Clause
//
// 44100Hz で作った音を、デバイスが言う標本化周波数へ。
//
// MU2000 は 44100Hz より他では動かない（SWP30 の 1 サンプルが 768 サイクル）。
// 一方で今どきの Windows のデバイスは 48000Hz を言ってくることが多い。
// 変換を Windows に任せる（AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM）と、
// **既定の品質の変換器**を通され、溜めもその分増える。自分で変換する。
//
// 窓関数付き sinc の畳み込み。64 本で阻止域 -74dB くらい。
//
// 使い方は「先に要る入力の数を聞いて、それだけ入れてから出す」:
//
//   const int k = rs.input_needed(n);
//   作る(staging, k);  rs.push(staging, k);  rs.pull(out, n);
//
// **遅れは足さない。** 音源は「必要な先の音」をその場で作れるので、
// 畳み込みに要る先のサンプルも頼んだ時点で作れる。

#ifndef S_MU2000_UI_RESAMPLER_H
#define S_MU2000_UI_RESAMPLER_H

#pragma once

#include "compat/mamecompat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ui {

class resampler
{
public:
	// in から out へ。同じなら direct() が真になり、畳み込みを丸ごと省く
	void configure(double in_rate, double out_rate)
	{
		if (in_rate <= 0.0)  in_rate = 44100.0;
		if (out_rate <= 0.0) out_rate = in_rate;
		m_direct = std::fabs(in_rate - out_rate) < 1e-6;
		m_step   = in_rate / out_rate;
		// 上へ変換するときは入力のナイキストまで通す。
		// 下へ変換するときは出力のナイキストで切らないと折り返す
		m_cutoff = std::min(1.0, out_rate / in_rate) * 0.955;
		if (m_tab.empty())
			build_table();
		reset();
	}

	bool direct() const { return m_direct; }

	// 中身を捨てる（再生位置が飛んだ、止まった等）
	void reset()
	{
		std::memset(m_ring_l, 0, sizeof(m_ring_l));
		std::memset(m_ring_r, 0, sizeof(m_ring_r));
		m_written = 0;
		m_pos     = 0.0;
	}

	// out フレーム出すのに、あと何フレーム入力が要るか
	int input_needed(int out_frames) const
	{
		if (m_direct)
			return out_frames;
		if (out_frames <= 0)
			return 0;
		// 最後の 1 フレームの畳み込みに要る、いちばん先のサンプル
		const double last = m_pos + m_step * double(out_frames - 1);
		const s64 need = s64(std::floor(last)) + HALF + 1;
		return int(std::max<s64>(0, need - m_written));
	}

	// 今までに入れた分で、あと何フレーム出せるか（録音のように入力が先に来る向きで使う）
	int output_available() const
	{
		if (m_direct)
			return int(std::max<s64>(0, m_written - s64(m_pos)));
		const double room = double(m_written - HALF - 1) - m_pos;
		if (room < 0.0)
			return 0;
		return int(std::floor(room / m_step)) + 1;
	}

	// 16bit 2ch インタリーブで入れる
	void push(const s16 *in, int frames)
	{
		const float k = 1.0f / 32768.0f;
		for (int i = 0; i < frames; i++) {
			m_ring_l[m_written & RMASK] = float(in[i * 2 + 0]) * k;
			m_ring_r[m_written & RMASK] = float(in[i * 2 + 1]) * k;
			m_written++;
		}
	}

	void push_float(const float *in, int frames)
	{
		for (int i = 0; i < frames; i++) {
			m_ring_l[m_written & RMASK] = in[i * 2 + 0];
			m_ring_r[m_written & RMASK] = in[i * 2 + 1];
			m_written++;
		}
	}

	// float 2ch インタリーブで出す
	void pull(float *out, int frames)
	{
		for (int i = 0; i < frames; i++) {
			float l = 0.0f, r = 0.0f;
			one(l, r, true);
			out[i * 2 + 0] = l;
			out[i * 2 + 1] = r;
		}
	}

	void pull_unclamped(float *out, int frames)
	{
		for (int i = 0; i < frames; i++) {
			float l = 0.0f, r = 0.0f;
			one(l, r, false);
			out[i * 2 + 0] = l;
			out[i * 2 + 1] = r;
		}
	}

	// 入力に積んだ数（入れ過ぎの確認用）
	s64 written() const { return m_written; }

private:
	static constexpr int TAPS = 64;
	static constexpr int HALF = TAPS / 2;
	static constexpr int STEPS = 256;             // 1 サンプル間隔あたりの表の刻み
	// 1 回に頼まれる量（30ms ぶん = 1440 フレーム）より十分大きく取る。
	// 足りないと、畳み込みにまだ要るサンプルを上書きしてしまう
	static constexpr int RING = 4096, RMASK = RING - 1;

	void build_table()
	{
		m_tab.resize(size_t(HALF) * STEPS + 2);
		for (size_t k = 0; k < m_tab.size(); k++) {
			const double d = double(k) / STEPS;            // 中心からの距離
			const double x = 3.14159265358979323846 * d;
			const double sinc = (k == 0) ? 1.0 : std::sin(x) / x;
			// ブラックマン窓
			const double t = (d + HALF) / double(TAPS);
			const double w = 0.42 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * t)
			                      + 0.08 * std::cos(4.0 * 3.14159265358979323846 * t);
			m_tab[k] = float(sinc * w);
		}
	}

	void one(float &l, float &r, bool clamp_output)
	{
		const s64 centre = s64(std::floor(m_pos));
		if (m_direct) {
			const s64 idx = centre;
			l = m_ring_l[idx & RMASK];
			r = m_ring_r[idx & RMASK];
			m_pos += 1.0;
			return;
		}
		double al = 0.0, ar = 0.0, sum = 0.0;
		for (int k = -HALF + 1; k <= HALF; k++) {
			const s64 idx = centre + k;
			if (idx < 0 || idx >= m_written)
				continue;
			const double d  = std::fabs((m_pos - double(idx)) * m_cutoff);
			const double fx = d * STEPS;
			const size_t j  = size_t(fx);
			if (j + 1 >= m_tab.size())
				continue;
			const double t = fx - double(j);
			const double h = m_tab[j] + (m_tab[j + 1] - m_tab[j]) * t;
			al  += h * m_ring_l[idx & RMASK];
			ar  += h * m_ring_r[idx & RMASK];
			sum += h;
		}
		if (sum > 1e-9) {
			al /= sum;
			ar /= sum;
		}
		l = clamp_output ? float(std::clamp(al, -1.0, 1.0)) : float(al);
		r = clamp_output ? float(std::clamp(ar, -1.0, 1.0)) : float(ar);
		m_pos += m_step;
	}

	std::vector<float> m_tab;
	float   m_ring_l[RING] = {};
	float   m_ring_r[RING] = {};
	s64     m_written = 0;
	double  m_pos = 0.0;
	double  m_step = 1.0;
	double  m_cutoff = 1.0;
	bool    m_direct = true;
};

} // namespace ui

#endif // S_MU2000_UI_RESAMPLER_H

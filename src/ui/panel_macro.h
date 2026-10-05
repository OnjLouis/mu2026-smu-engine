// license:BSD-3-Clause
//
// 前面のボタンを決まった順に押す（サンプリングの窓の「カード」の「この M2A を読み込む」など）。
// 実機で人が押すのと同じ道を通るので、firmware の LOAD がそのまま動く。
//
// 音を作る糸で driver::sampling_tick から回す。待ちは音源の時間（mu2000::samples_run）で数えるので、
// 実時間より速く回る書き出しやプラグインでも押す間隔は変わらない。液晶の文字（DDRAM の 2 行）を見て、
// 出るまで待つ・消えるまで待つ・目当ての名前が出るまで select を押す、ができる。

#ifndef S_MU2000_UI_PANEL_MACRO_H
#define S_MU2000_UI_PANEL_MACRO_H

#pragma once

#include "mu2000.h"

#include <string>
#include <vector>

namespace ui {

class panel_macro
{
public:
	enum class kind { press, until, until_gone, find, if_shown };
	struct step {
		kind k = kind::press;
		mu2000::button b = mu2000::button::count;   // press・find で押すボタン
		std::string text;                            // until・until_gone・find で探す文字
		int ms = 0;                                  // until の待ちの上限（ミリ秒）
		int max = 0;                                 // find で押す回数の上限
	};

	static step press(mu2000::button b) { step s; s.b = b; return s; }
	static step until(const std::string &text, int ms) { step s; s.k = kind::until; s.text = text; s.ms = ms; return s; }
	static step until_gone(const std::string &text, int ms) { step s; s.k = kind::until_gone; s.text = text; s.ms = ms; return s; }
	static step find(const std::string &text, mu2000::button b, int max)
	{ step s; s.k = kind::find; s.text = text; s.b = b; s.max = max; return s; }
	// ms のうちに text が出たら b を押す。出なければそのまま次へ（確かめの問いがあるときだけ答える）
	static step if_shown(const std::string &text, mu2000::button b, int ms)
	{ step s; s.k = kind::if_shown; s.text = text; s.b = b; s.ms = ms; return s; }

	// SAMPLING → LOAD → ALL+SEQ で name（カードの一番上にある 8.3 の名前）を選んで読み込む押し方。
	// どの画面にいても EXIT で演奏の画面へ戻ってから入り、読み終えたら演奏の画面へ戻る
	static std::vector<step> load_m2a(const std::string &name)
	{
		using B = mu2000::button;
		std::vector<step> s;
		for (int i = 0; i < 4; i++)
			s.push_back(press(B::exit));
		s.push_back(press(B::sampling_mode));
		s.push_back(until("LOAD", 3000));
		for (int i = 0; i < 5; i++)
			s.push_back(press(B::select_left));   // 品書きは前の位置を覚えているので、左端（EDIT）へ
		s.push_back(press(B::select_right));      // LOAD
		s.push_back(press(B::enter));
		s.push_back(until("<LOAD>", 3000));
		s.push_back(find("ALL+SEQ", B::select_left, 12));
		s.push_back(press(B::enter));
		s.push_back(until(":/", 5000));
		s.push_back(find(name, B::select_right, 128));
		s.push_back(press(B::enter));
		// サンプルがもうあると「Overwrite ALL?」と聞かれる。置き換わるのは窓の側で確かめてあるので、ENTER で答える
		s.push_back(if_shown("Overwrite", B::enter, 1500));
		s.push_back(until_gone("LOADING", 180000));
		s.push_back(until("<LOAD>", 5000));       // 読み終わると LOAD の画面に戻る
		for (int i = 0; i < 3; i++)
			s.push_back(press(B::exit));
		return s;
	}

	bool active() const { return m_at < m_steps.size(); }
	size_t at() const { return m_at; }   // いま何段目か（確かめる用）

	void start(std::vector<step> steps, std::string done)
	{
		m_steps = std::move(steps);
		m_done = std::move(done);
		m_at = 0;
		m_phase = 0;
		m_count = 0;
		m_since = ~u64(0);
	}

	// 1 ブロックごとに呼ぶ。終わったら true と、知らせる一言（失敗ならその理由）
	bool tick(mu2000 &mu, std::string &message)
	{
		if (!active())
			return false;
		const u64 now = mu.samples_run();
		if (m_since == ~u64(0))
			m_since = now;
		const u64 ms = (now - m_since) * 1000 / 44100;
		const step &s = m_steps[m_at];
		switch (s.k) {
		case kind::press:
			if (!pressing(mu, s.b, ms))
				return false;
			next(now);
			break;
		case kind::until:
			if (lcd(mu).find(s.text) != std::string::npos)
				next(now);
			else if (int(ms) > s.ms)
				return fail("\"" + s.text + "\" did not appear on the LCD", mu, message);
			break;
		case kind::until_gone:
			// 出るまで少し待ってから、消えるのを待つ（出ないまま終わることもある）
			if (ms > 300 && lcd(mu).find(s.text) == std::string::npos)
				next(now);
			else if (int(ms) > s.ms)
				return fail("\"" + s.text + "\" stayed on the LCD", mu, message);
			break;
		case kind::find:
			if (m_phase == 0 && lcd(mu).find(s.text) != std::string::npos) {
				next(now);
				break;
			}
			if (m_count >= s.max)
				return fail("\"" + s.text + "\" was not found", mu, message);
			if (pressing(mu, s.b, ms)) {
				m_count++;
				m_since = now;
			}
			break;
		case kind::if_shown:
			if (m_count == 0) {
				if (lcd(mu).find(s.text) != std::string::npos) {
					m_count = 1;          // 出た。少し待ってから押す
					m_since = now;
				} else if (int(ms) > s.ms) {
					next(now);
				}
				break;
			}
			// 問いが出た直後に押しても firmware は受け取らない（出てから 20ms で押すと無視された）
			if (m_count == 1) {
				if (ms >= 400) {
					m_count = 2;
					m_since = now;
				}
				break;
			}
			if (pressing(mu, s.b, ms))
				next(now);
			break;
		}
		if (!active()) {
			message = m_done;
			return true;
		}
		return false;
	}

	void cancel(mu2000 &mu)
	{
		if (active() && m_phase == 1 && m_steps[m_at].b != mu2000::button::count)
			mu.set_button(m_steps[m_at].b, false);
		m_steps.clear();
		m_at = 0;
	}

	// 液晶の 2 行（印字できない字は '.'）
	static std::string lcd(mu2000 &mu)
	{
		const u8 *dd = mu.lcd().ddram();
		std::string s;
		for (int row = 0; row < 2; row++) {
			for (int c = 0; c < 24; c++) {
				const u8 ch = dd[row * 0x40 + c];
				s += (ch >= 32 && ch < 127) ? char(ch) : '.';
			}
			if (!row)
				s += '|';
		}
		return s;
	}

private:
	// 押して 80ms で離し、250ms 待つ。終わったら true
	bool pressing(mu2000 &mu, mu2000::button b, u64 ms)
	{
		if (m_phase == 0) {
			mu.set_button(b, true);
			m_phase = 1;
			return false;
		}
		if (m_phase == 1 && ms >= 80) {
			mu.set_button(b, false);
			m_phase = 2;
			return false;
		}
		if (m_phase == 2 && ms >= 330) {
			m_phase = 0;
			return true;
		}
		return false;
	}

	void next(u64 now)
	{
		m_at++;
		m_phase = 0;
		m_count = 0;
		m_since = now;
	}

	bool fail(const std::string &why, mu2000 &mu, std::string &message)
	{
		message = why + " [" + lcd(mu) + "]";
		m_steps.clear();
		m_at = 0;
		return true;
	}

	std::vector<step> m_steps;
	std::string m_done;
	size_t m_at = 0;
	int m_phase = 0, m_count = 0;
	u64 m_since = ~u64(0);
};

} // namespace ui

#endif // S_MU2000_UI_PANEL_MACRO_H

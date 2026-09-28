// license:BSD-3-Clause
//
// MIDI ファイルを実時間で音源へ流す。画面の「MIDI ファイルを再生」用。
//
// **ここだけは時計を持つ**。譜面を送る側は時計を持つのが当たり前で、
// 実機に MIDI ケーブルで繋いだ外の並べ機と同じ立場になる。音源のほうは
// 今までどおり、音声デバイスに頼まれた分だけ進む（doc/design.md）。
//
// 送り先は ui::bridge の輪。エディタのつまみと同じ道なので、
// VST3 でもそのまま動く。

#ifndef S_MU2000_UI_PLAYER_H
#define S_MU2000_UI_PLAYER_H

#pragma once

#include "bridge.h"
#include "smf.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace ui {

class player
{
public:
	~player() { stop(); }

	// 開いて流し始める。だめなら false（理由は err）
	bool start(const std::string &path, bridge &br, std::string &err);
	// 止めて、鳴りっぱなしを消す
	void stop();

	bool playing() const { return m_playing.load(std::memory_order_acquire); }
	// 音源が USB の口（A-D の 4 口）で受けるか。偽なら DIN の A・B だけ（--host-midi）
	void set_usb_ports(bool on) { m_usb.store(on, std::memory_order_relaxed); }
	// DIN の口だけのとき、3 口目以降（口 3・4）を A・B に重ねて鳴らすか（偽なら鳴らさない）。流している途中でも変えられる
	void set_fold_extra_ports(bool on) { m_fold.store(on, std::memory_order_relaxed); }
	bool fold_extra_ports() const { return m_fold.load(std::memory_order_relaxed); }
	// 開いたファイルが使っている口の数（1〜）
	int ports_used() const { return m_ports_used; }
	std::string name() const { return m_name; }
	double position() const { return m_pos.load(std::memory_order_relaxed); }
	double length() const   { return m_len; }

private:
	void run(bridge &br);

	std::vector<smf::event> m_events;
	std::thread       m_thread;
	std::atomic<bool> m_quit{false};
	std::atomic<bool> m_playing{false};
	std::atomic<double> m_pos{0};
	std::atomic<bool> m_fold{true};
	std::atomic<bool> m_usb{true};
	int         m_ports_used = 1;
	double      m_len = 0;
	std::string m_name;
};

} // namespace ui

#endif // S_MU2000_UI_PLAYER_H

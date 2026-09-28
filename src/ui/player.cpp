// license:BSD-3-Clause

#include "player.h"

#include <algorithm>
#include <chrono>

// timeBeginPeriod() only exists on Windows, as the way to ask the scheduler for
// a 1 ms timer resolution. macOS already sleeps finely enough, so the call is
// simply not made there
#if defined(_WIN32)
#include <windows.h>
#endif

namespace ui {

namespace {

// 鳴りっぱなしを消す。口 A と口 B の 16 チャンネルぶん
void all_off(bridge &br)
{
	for (int ch = 0; ch < 16; ch++) {
		const u8 msg[3] = { u8(0xb0 | ch), 0x7b, 0x00 };   // オールノートオフ
		const u8 sus[3] = { u8(0xb0 | ch), 0x40, 0x00 };   // ダンパも離す
		br.send(msg, 3);
		br.send(sus, 3);
		br.send_b(msg, 3);
		br.send_b(sus, 3);
	}
}

} // namespace


bool player::start(const std::string &path, bridge &br, std::string &err)
{
	stop();

	std::vector<smf::event> evs;
	if (!smf::load(path, evs, err))
		return false;
	if (evs.empty()) {
		err = "中身が空";
		return false;
	}

	m_events = std::move(evs);
	m_ports_used = 1;
	for (const smf::event &e : m_events)
		m_ports_used = std::max(m_ports_used, int(e.port) + 1);
	m_len = m_events.back().time;
	const size_t slash = path.find_last_of("/\\");
	m_name = (slash == std::string::npos) ? path : path.substr(slash + 1);

	m_quit.store(false);
	m_pos.store(0);
	m_playing.store(true, std::memory_order_release);
	m_thread = std::thread([this, &br] { run(br); });
	return true;
}

void player::stop()
{
	m_quit.store(true, std::memory_order_release);
	if (m_thread.joinable())
		m_thread.join();
	m_playing.store(false, std::memory_order_release);
	m_pos.store(0);
}

void player::run(bridge &br)
{
	// 1 ミリ秒で起きられるようにしておく。既定の 15.6 ミリ秒だと
	// 音符の頭がばらつく
#if defined(_WIN32)
	timeBeginPeriod(1);
#endif

	// std::chrono::steady_clock is QueryPerformanceCounter underneath on
	// Windows, so this reads the same clock the Windows code used to
	const auto t0 = std::chrono::steady_clock::now();

	size_t at = 0;
	while (!m_quit.load(std::memory_order_acquire) && at < m_events.size()) {
		const double sec = std::chrono::duration<double>(
		    std::chrono::steady_clock::now() - t0).count();
		m_pos.store(sec, std::memory_order_relaxed);

		// 来ている分をまとめて送る。トラックの出し先（SMF のポート指定かトラック名）が 0-3 なら口 A-D。
		// USB の口（gui の既定）なら 4 口ともそのまま。DIN の口だけ（--host-midi）なら、口 3・4 は
		// 選んだ扱いに従う（A・B に重ねるか、鳴らさない）
		const bool fold = m_fold.load(std::memory_order_relaxed);
		const bool usb = m_usb.load(std::memory_order_relaxed);
		while (at < m_events.size() && m_events[at].time <= sec) {
			const smf::event &e = m_events[at];
			const int to = smf::mu_port(e.port, fold, usb);
			if (to == 0)      br.send(e.bytes.data(), e.bytes.size());
			else if (to > 0)  br.send_port(to, e.bytes.data(), e.bytes.size());
			at++;
		}
		if (at >= m_events.size())
			break;

		// 次まで待つ。長く待ちすぎないように刻む
		const double wait = m_events[at].time - sec;
		std::this_thread::sleep_for(std::chrono::milliseconds(wait > 0.010 ? 5 : 1));
	}

	all_off(br);
#if defined(_WIN32)
	timeEndPeriod(1);
#endif
	m_playing.store(false, std::memory_order_release);
}

} // namespace ui

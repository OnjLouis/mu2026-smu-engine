// license:BSD-3-Clause
//
// CPU load meter shared by the standalone audio device and plugin engine.
// The averaging window is measured in audio time, not callback count.

#ifndef S_MU2000_UI_CPU_METER_H
#define S_MU2000_UI_CPU_METER_H

#pragma once

#include <atomic>

namespace ui {

class cpu_meter
{
public:
	// Short enough to react quickly, long enough to hide callback fragmentation
	// (0.1 s after trying a few values; issue #80).
	static constexpr double WINDOW_SECONDS = 0.1;

	cpu_meter() { reset(); }

	void reset()
	{
		m_busy_seconds = 0.0;
		m_audio_seconds = 0.0;
		m_value.store(0.0, std::memory_order_relaxed);
	}

	// Returns true when a complete measurement window has been published.
	// Only the audio thread calls add(); the UI thread only calls value().
	bool add(double busy_seconds, double audio_seconds)
	{
		if (!(busy_seconds >= 0.0) || !(audio_seconds > 0.0))
			return false;
		m_busy_seconds += busy_seconds;
		m_audio_seconds += audio_seconds;
		if (m_audio_seconds < WINDOW_SECONDS)
			return false;
		m_value.store(100.0 * m_busy_seconds / m_audio_seconds,
		              std::memory_order_relaxed);
		m_busy_seconds = 0.0;
		m_audio_seconds = 0.0;
		return true;
	}

	double value() const
	{
		return m_value.load(std::memory_order_relaxed);
	}

private:
	double m_busy_seconds = 0.0;
	double m_audio_seconds = 0.0;
	std::atomic<double> m_value{0.0};
};

} // namespace ui

#endif // S_MU2000_UI_CPU_METER_H


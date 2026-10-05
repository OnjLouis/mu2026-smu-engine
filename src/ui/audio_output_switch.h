// license:BSD-3-Clause
// Reopen a standalone output without leaving a failed selection silent.
#pragma once

#include "audio_out.h"

namespace ui {

struct audio_output_switch_result {
	bool selected = false;
	bool restored = false;
	std::string error;
};

inline audio_output_switch_result switch_audio_output(
	audio_out &out, int latency_ms, const audio_out::fill_fn &fill,
	bool exclusive, const std::string &wanted, const std::string &previous)
{
	audio_output_switch_result r;
	out.stop(); // joins the old callback before a new one can touch the machine
	if (out.start(latency_ms, fill, r.error, exclusive, wanted, false, true)) {
		r.selected = true;
		return r;
	}
	out.stop(); // also dispose a partially opened device
	std::string restore_error;
	r.restored = out.start(latency_ms, fill, restore_error, exclusive, previous, false, true);
	if (!r.restored) {
		out.stop();
		r.error += "\n" + restore_error;
	}
	return r;
}

} // namespace ui

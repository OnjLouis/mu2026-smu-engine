// license:BSD-3-Clause
// Build with the platform's audio_out.cpp; --devices also exercises real
// playback devices with silence. Menu tests require neither ROMs nor hardware.
#include "ui/audio_output_switch.h"
#include "ui/menu.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int main(int argc, char **argv)
{
	ui::init_lang("en");
	ui::menu_state s;
	s.audio_ready = true;
	s.audio_outs = { "Speakers", "USB headphones" };
	s.audio_name = "USB headphones";
	const auto phones = ui::menu_phones(s);
	assert(phones.front().title == "Audio output device");
	assert(!phones.front().items[0].checked);
	assert(phones.front().items[3].checked);
	assert(phones.front().items[3].id == ui::ID_AUDIO_BASE + 1);
	assert(phones.back().items[2].checked); // digital output is preserved
	s.analog = true;
	assert(ui::menu_phones(s).back().items[3].checked);
	// A newly enumerated snapshot reflects unplug/replug and changed ordering.
	s.audio_outs = { "USB headphones", "HDMI monitor" };
	const auto updated = ui::menu_audio_output(s);
	assert(updated.items[2].checked);
	assert(updated.items[3].label == "HDMI monitor");
	assert(phones.front().items[2].label == "Speakers"); // old snapshot stays stable
	s.audio_name.clear();
	assert(ui::menu_audio_output(s).items[0].checked);
	s.audio_ready = false;
	for (const auto &item : ui::menu_audio_output(s).items)
		assert(!item.enabled || item.separator);
	s.audio_outs.clear();
	assert(ui::menu_audio_output(s).items.back().label == "(No playback devices)");
	std::cout << "Audio output menus: PASS\n";
	if (argc < 2 || std::strcmp(argv[1], "--devices"))
		return 0;

	ui::audio_out out;
	const ui::audio_out::fill_fn silence = [](s16 *o, u32 n) { std::memset(o, 0, size_t(n) * 4); };
	std::string error;
	assert(out.start(20, silence, error));
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	assert(out.produced() > 0);
	// Selecting a missing endpoint must fail and restore the previous stream.
	const auto failed = ui::switch_audio_output(out, 20, silence, false,
		"S-MU2000 test missing endpoint 907278", "");
	assert(!failed.selected && failed.restored && !failed.error.empty());
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	assert(out.produced() > 0);
	for (const auto &name : ui::audio_out::list()) {
		const auto selected = ui::switch_audio_output(out, 20, silence, false, name, "");
		assert(selected.selected);
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		assert(out.produced() > 0);
		std::cout << "Opened: " << name << '\n';
	}
	assert(ui::switch_audio_output(out, 20, silence, false, "", "").selected);
	const auto unavailable = ui::switch_audio_output(out, 20, silence, false,
		"S-MU2000 test missing endpoint 907278", "S-MU2000 test missing endpoint 907279");
	assert(!unavailable.selected && !unavailable.restored && !unavailable.error.empty());
	assert(ui::switch_audio_output(out, 20, silence, false, "", "").selected);
	out.stop();
	std::cout << "Audio output switching and rollback: PASS\n";
}

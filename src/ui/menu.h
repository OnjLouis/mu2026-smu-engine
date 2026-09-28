// license:BSD-3-Clause
//
// The standalone windows' popup menus, shared by the Windows (gui.cpp) and
// macOS (gui_mac.cpp) front ends.
//
// Windows is the reference: the items, their order and their wording live
// here, so a menu added on one side cannot be missed on the other (the
// lightweight-mode toggle was). Each front end only renders menu_groups
// into its own toolkit (HMENU on Windows, NSMenu through window_mac.mm on
// macOS) and acts on the chosen id, whose numbers are shared too.
//
// Plain C++ only: gui.cpp is windows.h-heavy and window_mac.h must stay
// AppKit/gdi-free, so this header includes only <string> and <vector>.
// Header-only (inline) so no build system changes are needed anywhere.
//
// Alongside the menus this is home to the small shared pump vocabulary:
// mouse_out and the F-key codes every platform's event loop translates into.

#ifndef S_MU2000_UI_MENU_H
#define S_MU2000_UI_MENU_H

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "ui/texts.h"

namespace ui {

// One line in a popup menu
struct menu_item {
	std::string label;
	int         id = 0;
	bool        checked = false;
	bool        enabled = true;
	bool        separator = false;
	// A shortcut hint shown after the label: after a tab on Windows, in
	// parentheses on macOS. Only the overview/editor items use it.
	std::string shortcut;
};

// A titled group of items. A group whose title is empty is drawn at the top
// level rather than as a submenu, which is what the card slot's second half
// and the editor/filter rows want
struct menu_group {
	std::string            title;
	std::vector<menu_item> items;
};

// What one mouse press on the main window did. The pump decides what still
// to do: press a panel control down (panel_pressed, feeds drag/up and the
// repaint), open a PC window (bar_window, a kind id, -1 when none) or show
// the popup (show_menu; the items come from menu_groups_for()). Lives here
// because every platform's pump reads the same struct: the Mac's bool
// mouse_down once left an opened window looking like a popup request
struct mouse_out {
	bool panel_pressed = false;
	bool opened_window = false;
	bool show_menu = false;
};

// Panel keys that are not characters: the F-keys, in a shared code space
// above ASCII that every platform translates into (virtual keys on Windows,
// SDL keycodes on Linux, Carbon key codes on macOS). button_for_char()
// ignores anything outside ASCII, so these cannot alias a panel button --
// which is what lets the one shared handler act on them
enum : int {
	KEY_F2 = 0x100,   // the PC editor window
	KEY_F3 = 0x101,   // the overview window
	KEY_F4 = 0x102,   // the native-engine toggle
	KEY_F5 = 0x103,   // layout reload
};

// Menu command numbers for picking a port. MIDI IN has four ports, laid out
// 500 apart (A 900, B 1400, C 1900, D 2400).
//
// Each port menu occupies ID_BASE .. ID_BASE+255, and the front ends take
// anything in such a range for that port. **Ranges must not overlap**: A/D
// INPUT and the card items once sat inside ID_OUTMU_BASE's 256, so picking
// an input device (or a card item) went to MIDI OUT instead and the tick
// never moved to what was picked
enum : int {
	ID_IN_NONE = 900, ID_IN_BASE = 901, ID_IN_STRIDE = 500,
	ID_OUT_NONE = 3000, ID_OUT_BASE = 3001,
	ID_OUTB_NONE = 3500, ID_OUTB_BASE = 3501,
	ID_OUTMU_NONE = 4000, ID_OUTMU_BASE = 4001,
	ID_AIN_NONE = 4500, ID_AIN_BASE = 4501,
	ID_CARD_NEW16 = 5000, ID_CARD_NEW32, ID_CARD_NEW64, ID_CARD_NEW128,
	ID_CARD_OPEN = 5010, ID_CARD_EJECT = 5011,
	ID_PLAY_FILE = 5100, ID_STOP_FILE = 5101, ID_PORTS34_FOLD = 5102, ID_PORTS34_DROP = 5103,
	ID_FACTORY = 5200,
	ID_NATIVE_FX = 5215,     // lightweight mode (C++ effects)
	ID_NATIVE_ENGINE = 5216, // firmware を走らせない口（聞き比べ用）
	ID_PC_EDITOR = 5201,
	ID_OVERVIEW = 5202,
	ID_OUTPUT_DIGITAL = 5300, ID_OUTPUT_ANALOG = 5301,
};

// Checked at compile time, because the failure is silent: a menu id that lands
// in a port menu's range (ID_BASE .. ID_BASE+255) is taken for that port, so
// the chosen item never arrives and the tick never moves
static_assert([] {
	const int bases[] = { ID_IN_BASE, ID_IN_BASE + ID_IN_STRIDE, ID_IN_BASE + 2 * ID_IN_STRIDE,
	                      ID_IN_BASE + 3 * ID_IN_STRIDE,
	                      ID_OUT_BASE, ID_OUTB_BASE, ID_OUTMU_BASE, ID_AIN_BASE };
	const int singles[] = { ID_IN_NONE, ID_IN_NONE + ID_IN_STRIDE, ID_IN_NONE + 2 * ID_IN_STRIDE,
	                        ID_IN_NONE + 3 * ID_IN_STRIDE,
	                        ID_OUT_NONE, ID_OUTB_NONE, ID_OUTMU_NONE,
	                        ID_AIN_NONE, ID_CARD_NEW16, ID_CARD_NEW32, ID_CARD_NEW64,
	                        ID_CARD_NEW128, ID_CARD_OPEN, ID_CARD_EJECT,
	                        ID_PLAY_FILE, ID_STOP_FILE, ID_FACTORY, ID_NATIVE_FX,
	                        ID_NATIVE_ENGINE,
	                        ID_PORTS34_FOLD, ID_PORTS34_DROP, ID_PC_EDITOR, ID_OVERVIEW,
	                        ID_OUTPUT_DIGITAL, ID_OUTPUT_ANALOG };
	for (int base : bases) {
		for (int id : singles)
			if (id >= base && id < base + 256)
				return false;
		for (int other : bases)
			if (other != base && other >= base && other < base + 256)
				return false;
	}
	return true;
}(), "a menu id falls inside another menu's ID_BASE..ID_BASE+255 range");

// The names shown in the menu and in the startup report, A B C D.
// (The gui.ini keys stay per front end with the settings code.)
// Localized: the table in ui/texts.h (menu_in_a and friends).
inline const char *in_label(int p)
{
	switch (p) {
	case 0: return UI_TEXT(menu_in_a, "MIDI IN A (parts 1-16)");
	case 1: return UI_TEXT(menu_in_b, "MIDI IN B (parts 17-32)");
	case 2: return UI_TEXT(menu_in_c, "MIDI IN C (parts 33-48)");
	default: return UI_TEXT(menu_in_d, "MIDI IN D (parts 49-64)");
	}
}

// Everything a menu shows. MIDI IN is four ports in A-D order; device
// choices are indices into the lists (-1 is unused). The recording device
// is matched by name (empty is unused), the card by path (empty is none).
struct menu_state {
	std::vector<std::string> midi_ins;
	std::vector<std::string> midi_outs;
	std::vector<std::string> audio_ins;
	int in_dev[4] = { -1, -1, -1, -1 };
	int out_dev = -1, out_dev_b = -1, out_dev_mu = -1;
	std::string ain_name;
	std::string card_path;
	bool playing = false;
	std::string play_name;
	bool fold34 = true;      // ports 3+4 of a MIDI file fold onto A and B
	bool ready = false;      // the firmware is up (factory reset is offered)
	bool native_fx = false;  // lightweight C++ effects are on
	bool native_engine = false;  // skip-firmware ports, toggled live for listening tests
	bool analog = false;     // the PHONES output (digital otherwise)
};

// The plug-in card/panel menus (VST3/AU/CLAP share the VST3 view on both
// platforms). Same sharing as the standalone menus above: the numbers match
// what the Windows plug-in already used, so its choice dispatch is unchanged.
enum : int {
	ID_PLUG_CARD_NEW16 = 100, ID_PLUG_CARD_NEW32, ID_PLUG_CARD_NEW64, ID_PLUG_CARD_NEW128,
	ID_PLUG_CARD_OPEN = 110, ID_PLUG_CARD_EJECT = 111,
	ID_PLUG_LIST = 120, ID_PLUG_EDITOR = 121,
};

struct plug_menu_state {
	std::string card_path;   // the SmartMedia image in the slot (empty is none)
	bool card_ready = false; // the engine is up enough to swap images
};

namespace menu_detail {

inline menu_item text(const char *label, int id, bool checked, bool enabled,
                      const char *shortcut = "")
{
	menu_item m;
	m.label = label;
	m.id = id;
	m.checked = checked;
	m.enabled = enabled;
	m.shortcut = shortcut;
	return m;
}

inline menu_item separator()
{
	menu_item m;
	m.separator = true;
	return m;
}

// The file name in the slot, so it is clear which one is going out.
// Both / and \ count, because the two platforms write them differently
inline std::string basename(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace menu_detail

// One port picker: "unused", then the devices, or "(no devices)". now is the
// open device index (-1 is unused)
inline menu_group menu_port_group(const char *title, const std::vector<std::string> &names,
                                  int now, int id_none, int id_base)
{
	using namespace menu_detail;
	menu_group g;
	g.title = title;
	g.items.push_back(text(UI_TEXT(menu_unused, "Unused"), id_none, now < 0, true));
	g.items.push_back(separator());
	if (names.empty()) {
		g.items.push_back(text(UI_TEXT(menu_no_devices, "(No devices)"), 0, false, false));
		return g;
	}
	for (size_t i = 0; i < names.size(); i++)
		g.items.push_back(text(names[i].c_str(), id_base + int(i), int(i) == now, true));
	return g;
}

// The recording device the machine samples as its A/D INPUT, matched by name
inline menu_group menu_ain_group(const std::vector<std::string> &names, const std::string &ain_name)
{
	using namespace menu_detail;
	menu_group g;
	g.title = UI_TEXT(menu_ain_title, "A/D INPUT (sound to sample)");
	g.items.push_back(text(UI_TEXT(menu_unused, "Unused"), ID_AIN_NONE, ain_name.empty(), true));
	g.items.push_back(separator());
	if (names.empty()) {
		g.items.push_back(text(UI_TEXT(menu_no_ain, "(No recording devices)"), 0, false, false));
		return g;
	}
	for (size_t i = 0; i < names.size(); i++)
		g.items.push_back(text(names[i].c_str(), ID_AIN_BASE + int(i), names[i] == ain_name, true));
	return g;
}

// The right-click menu: the four MIDI IN ports, MIDI OUT, the two THRUs,
// A/D INPUT, the PC editor windows, the lightweight-mode toggle, and the
// factory reset. Same items in the same order on both platforms
inline std::vector<menu_group> menu_ports(const menu_state &s)
{
	using namespace menu_detail;
	std::vector<menu_group> groups;
	for (int p = 0; p < 4; p++)
		groups.push_back(menu_port_group(in_label(p), s.midi_ins, s.in_dev[p],
		                                 ID_IN_NONE + p * ID_IN_STRIDE,
		                                 ID_IN_BASE + p * ID_IN_STRIDE));
	groups.push_back(menu_port_group(UI_TEXT(menu_out_mu, "MIDI OUT (what the MU2000 sends)"), s.midi_outs,
	                                 s.out_dev_mu, ID_OUTMU_NONE, ID_OUTMU_BASE));
	groups.push_back(menu_port_group(UI_TEXT(menu_thru_a, "MIDI THRU A (sends out what A receives)"), s.midi_outs,
	                                 s.out_dev, ID_OUT_NONE, ID_OUT_BASE));
	groups.push_back(menu_port_group(UI_TEXT(menu_thru_b, "MIDI THRU B (sends out what B receives)"), s.midi_outs,
	                                 s.out_dev_b, ID_OUTB_NONE, ID_OUTB_BASE));
	groups.push_back(menu_ain_group(s.audio_ins, s.ain_name));

	menu_group ed;
	ed.items.push_back(text(UI_TEXT(menu_open_list, "Open the list"), ID_OVERVIEW, false, true, "F3"));
	ed.items.push_back(text(UI_TEXT(menu_open_editor, "Open the editor"), ID_PC_EDITOR, false, true, "F2"));
	ed.items.push_back(text(UI_TEXT(menu_native_fx, "Play effects in C++ (light; differs from hardware)"),
	                        ID_NATIVE_FX, s.native_fx, true));
	ed.items.push_back(text(UI_TEXT(menu_native_engine, "Play without the firmware (fast; still differs)"),
	                        ID_NATIVE_ENGINE, s.native_engine, true, "F4"));
	groups.push_back(ed);

	// Throwing the settings away reboots the machine, so it is only offered
	// once the firmware is actually up
	menu_group g;
	g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(menu_factory, "Factory reset..."), ID_FACTORY, false, s.ready));
	groups.push_back(g);
	return groups;
}

// The card slot: a fresh SmartMedia image, then the image in the slot and
// the MIDI file player
inline std::vector<menu_group> menu_card(const menu_state &s)
{
	using namespace menu_detail;
	std::vector<menu_group> groups;

	menu_group fresh;
	fresh.title = UI_TEXT(menu_card_new, "Make a new SmartMedia image");
	fresh.items.push_back(text("16MB", ID_CARD_NEW16, false, true));
	fresh.items.push_back(text("32MB", ID_CARD_NEW32, false, true));
	fresh.items.push_back(text("64MB", ID_CARD_NEW64, false, true));
	fresh.items.push_back(text("128MB", ID_CARD_NEW128, false, true));
	groups.push_back(fresh);

	menu_group g;
	g.items.push_back(text(UI_TEXT(menu_card_open, "Insert a SmartMedia image..."), ID_CARD_OPEN, false, true));
	char eject[512];
	if (!s.card_path.empty())
		std::snprintf(eject, sizeof(eject), UI_TEXT(menu_card_eject_fmt, "Eject the SmartMedia (%s)"),
		              basename(s.card_path).c_str());
	else
		std::snprintf(eject, sizeof(eject), "%s", UI_TEXT(menu_card_eject, "Eject the SmartMedia"));
	g.items.push_back(text(eject, ID_CARD_EJECT, false, !s.card_path.empty()));
	g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(menu_play_file, "Play a MIDI file..."), ID_PLAY_FILE, false, true));
	char stop[512];
	if (s.playing)
		std::snprintf(stop, sizeof(stop), UI_TEXT(menu_stop_fmt, "Stop (%s)"), s.play_name.c_str());
	else
		std::snprintf(stop, sizeof(stop), "%s", UI_TEXT(menu_stop, "Stop"));
	g.items.push_back(text(stop, ID_STOP_FILE, false, s.playing));
	// What to do with a MIDI file that uses ports 3 and 4
	g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(menu_fold34, "Fold ports 3+4 onto A and B (DIN ports only)"), ID_PORTS34_FOLD, s.fold34, true));
	g.items.push_back(text(UI_TEXT(menu_drop34, "Drop ports 3+4 (DIN ports only)"), ID_PORTS34_DROP, !s.fold34, true));
	groups.push_back(g);
	return groups;
}

// The PHONES jack: digital (as S/PDIF, DPCM DC included) or analogue
// (DC removed, src/analog_out.h)
inline std::vector<menu_group> menu_phones(bool analog)
{
	using namespace menu_detail;
	menu_group g;
	g.items.push_back(text(UI_TEXT(menu_out_title, "Sound output"), 0, false, false));
	g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(menu_out_digital, "Digital (S/PDIF; keeps DPCM DC)"),
	                       ID_OUTPUT_DIGITAL, !analog, true));
	g.items.push_back(text(UI_TEXT(menu_out_analog, "Analog (LINE OUT/PHONES; cuts DC)"),
	                       ID_OUTPUT_ANALOG, analog, true));
	return { g };
}

// The A/D INPUT jack on its own: the recording-device picker under its heading
inline std::vector<menu_group> menu_ain_only(const std::vector<std::string> &names,
                                             const std::string &ain_name)
{
	using namespace menu_detail;
	menu_group head = menu_ain_group(names, ain_name);
	menu_group g;
	g.items.push_back(text(head.title.c_str(), 0, false, false));
	g.items.push_back(separator());
	for (const menu_item &item : head.items)
		g.items.push_back(item);
	return { g };
}

// The plug-in card slot: a fresh SmartMedia image, the image in the slot,
// then the PC editor windows
inline std::vector<menu_group> menu_plug_card(const plug_menu_state &s)
{
	using namespace menu_detail;
	std::vector<menu_group> groups;

	menu_group fresh;
	fresh.title = UI_TEXT(menu_card_new, "Make a new SmartMedia image");
	fresh.items.push_back(text("16MB", ID_PLUG_CARD_NEW16, false, s.card_ready));
	fresh.items.push_back(text("32MB", ID_PLUG_CARD_NEW32, false, s.card_ready));
	fresh.items.push_back(text("64MB", ID_PLUG_CARD_NEW64, false, s.card_ready));
	fresh.items.push_back(text("128MB", ID_PLUG_CARD_NEW128, false, s.card_ready));
	groups.push_back(fresh);

	menu_group g;
	g.items.push_back(text(UI_TEXT(menu_card_open, "Insert a SmartMedia image..."), ID_PLUG_CARD_OPEN, false, s.card_ready));
	char eject[512];
	if (!s.card_path.empty())
		std::snprintf(eject, sizeof(eject), UI_TEXT(menu_card_eject_fmt, "Eject the SmartMedia (%s)"),
		              basename(s.card_path).c_str());
	else
		std::snprintf(eject, sizeof(eject), "%s", UI_TEXT(menu_card_eject, "Eject the SmartMedia"));
	g.items.push_back(text(eject, ID_PLUG_CARD_EJECT, false, !s.card_path.empty()));
	g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(menu_open_list, "Open the list"), ID_PLUG_LIST, false, true));
	g.items.push_back(text(UI_TEXT(menu_open_editor, "Open the editor"), ID_PLUG_EDITOR, false, true));
	groups.push_back(g);
	return groups;
}

// A plug-in right click that missed the card slot: the PC editor windows
inline std::vector<menu_group> menu_plug_panel()
{
	using namespace menu_detail;
	menu_group g;
	g.items.push_back(text(UI_TEXT(menu_open_list, "Open the list"), ID_PLUG_LIST, false, true));
	g.items.push_back(text(UI_TEXT(menu_open_editor, "Open the editor"), ID_PLUG_EDITOR, false, true));
	return { g };
}

} // namespace ui

#endif // S_MU2000_UI_MENU_H

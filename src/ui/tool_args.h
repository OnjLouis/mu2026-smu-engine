// license:BSD-3-Clause
//
// The command line both graphical front ends take.
//
// gui.exe and the Mac GUI accepted the same flags from two copies of one
// loop that drifted (--list grew an A/D list only on Mac). One struct and
// one parser keep them identical. Only latency stays per side: its default
// is tuned per audio backend (20 ms WASAPI, 30 ms CoreAudio), so each main
// presets args.latency before parsing.

#ifndef S_MU2000_UI_TOOL_ARGS_H
#define S_MU2000_UI_TOOL_ARGS_H

#pragma once

#include "compat/cli_text.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ui/audio_in.h"
#include "ui/audio_out.h"
#include "ui/layout.h"
#include "ui/midi_in.h"
#include "ui/midi_out.h"
#include "ui/options.h"
#include "ui/texts.h"

namespace ui {

// The command line, identical on both front ends (flag order included).
// The text lives in the texts table (help_usage), so --lang reaches it.
inline void print_usage()
{
	std::fprintf(stderr, "%s", UI_TEXT(help_usage, "Usage: gui <rom directory> [--midi N] [--midi-b N] [--midi-c N] [--midi-d N]"
                                    " [--midiout N] [--midiout-b N] [--midiout-mu N]"
                                    " [--latency ms] [--exclusive] [--layout panel.txt] [--play song.mid] [--lcd] [--fast-midi] [--host-midi]\n"
                                    "        [--factory]   forget remembered settings and boot factory-fresh\n"
                                    "        [--editor]    also open the PC editor (F2 or right-click in the window)\n"
                                    "        [--list-window] also open the list window (F3 or right-click)\n"
                                    "        [--fx-window] also open the insertion setup window (double-click an insertion cell in the list)\n"
                                    "        [--shapes-window] also open the part voice window (double-click a VIB picture etc. in the list)\n"
                                    "        [--master-window] also open the master window (double-click the master row)\n"
                                    "        [--sampling-window] also open the sampling window\n"
                                    "        [--lang ja|en] language (else lang= in editor.ini, else the locale: Japanese iff it says ja)\n"
                                    "        [--help]      show this help\n"
                                    "        gui --dump-layout panel.txt   write out the current layout\n"
                                    "        gui --list\n"
                                    "        gui [<rom directory> --boot] --shot image.png [--size 1000x400]\n"));
}

struct tool_args {
	std::string dir, shot_path, dump_layout, play_path, layout_path;
	std::string shot_mid;
	double shot_secs = 0.0;
	int in_dev[4] = { -2, -2, -2, -2 };   // -2 unset (remembered), -1 unused
	int mout_dev = -2, moutb_dev = -2, moutmu_dev = -2;
	bool usb_host = true;                  // HOST SELECT = USB (ports C/D work)
	int latency = 30;                      // preset per backend before parsing
	int win_w = 1000, win_h = 400;
	bool size_given = false;
	bool grid = false;
	bool boot_for_shot = false;
	bool nomidi = false;                   // open and remember no MIDI port
	std::string lang;                      // --lang ja|en (empty: editor.ini, then locale)
};

// Parses argv into args (plus the shared engine/output/window options).
// Returns -1 to continue, or the process exit code (--list and
// --dump-layout quit after printing).
inline int parse_tool_args(int argc, char **argv, tool_args &a,
                           engine_options &eng_opts, output_options &out_opts,
                           window_options &win_opts)
{
	// --lang anywhere on the line already counts (so --help/--list print
	// in the asked language); the mains resolve it again from a.lang
	// after parsing, which is the same value.
	for (int i = 1; i + 1 < argc; i++)
		if (!std::strcmp(argv[i], "--lang")) {
			pin_flag_lang(argv[i + 1]);
			break;
		}
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--help")) {
			print_usage();
			return 0;
		}
		else if (!std::strcmp(argv[i], "--list")) {
			const auto ins = midi_in::list();
			std::printf(CLI_T("MIDI inputs (--midi number; also selectable in the window):\n", "MIDI 入力（--midi 番号 / 画面からも選べる）:\n"));
			for (size_t k = 0; k < ins.size(); k++)
				std::printf("  %zu: %s\n", k, ins[k].c_str());
			if (ins.empty())
				std::printf(CLI_T("  (none)\n", "  （なし）\n"));
			const auto outs = midi_out::list();
			std::printf(CLI_T("MIDI outputs (--midiout number; passes on what comes in):\n", "MIDI 出力（--midiout 番号 / 受けたものをそのまま外へ）:\n"));
			for (size_t k = 0; k < outs.size(); k++)
				std::printf("  %zu: %s\n", k, outs[k].c_str());
			if (outs.empty())
				std::printf(CLI_T("  (none)\n", "  （なし）\n"));
			const auto aouts = audio_out::list();
			std::printf(CLI_T("Audio outputs (give --audio part of a name):\n", "音声の出口（--audio に名前の一部）:\n"));
			for (size_t k = 0; k < aouts.size(); k++)
				std::printf("  %zu: %s\n", k, aouts[k].c_str());
			const auto ains = audio_in::list();
			std::printf(CLI_T("A/D INPUT (recording devices; choose one in the window):\n", "A/D INPUT（録音デバイス。画面から選ぶ）:\n"));
			for (size_t k = 0; k < ains.size(); k++)
				std::printf("  %zu: %s\n", k, ains[k].c_str());
			if (ains.empty())
				std::printf(CLI_T("  (none)\n", "  （なし）\n"));
			return 0;
		}
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) a.in_dev[0] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-b") && i + 1 < argc) a.in_dev[1] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-c") && i + 1 < argc) a.in_dev[2] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-d") && i + 1 < argc) a.in_dev[3] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout") && i + 1 < argc) a.mout_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-b") && i + 1 < argc) a.moutb_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-mu") && i + 1 < argc) a.moutmu_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--usb")) a.usb_host = true;
		else if (!std::strcmp(argv[i], "--host-midi")) a.usb_host = false;
		// Opens nothing and remembers nothing: for tests, which must leave
		// the real settings file the way they found it
		else if (!std::strcmp(argv[i], "--nomidi")) {
			for (int &d : a.in_dev) d = -1;
			a.mout_dev = a.moutb_dev = a.moutmu_dev = -1;
			a.nomidi = true;
		}
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) a.latency = std::atoi(argv[++i]);
		else if (consume_engine_option(argv[i], eng_opts)) {}
		else if (consume_output_option(argv, argc, i, out_opts)) {}
		else if (consume_window_option(argv[i], win_opts)) {}
		else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) a.shot_path = argv[++i];
		else if (!std::strcmp(argv[i], "--boot")) a.boot_for_shot = true;
		else if (!std::strcmp(argv[i], "--grid")) a.grid = true;
		else if (!std::strcmp(argv[i], "--layout") && i + 1 < argc) a.layout_path = argv[++i];
		else if (!std::strcmp(argv[i], "--play") && i + 1 < argc) a.play_path = argv[++i];
		else if (!std::strcmp(argv[i], "--lang") && i + 1 < argc) a.lang = argv[++i];
		else if (!std::strcmp(argv[i], "--dump-layout") && i + 1 < argc) a.dump_layout = argv[++i];
		else if (!std::strcmp(argv[i], "--mid") && i + 2 < argc) {
			a.shot_mid = argv[++i];
			a.shot_secs = std::atof(argv[++i]);
			a.boot_for_shot = true;
		}
		else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) {
			if (std::sscanf(argv[++i], "%dx%d", &a.win_w, &a.win_h) != 2) { a.win_w = 1000; a.win_h = 400; }
			a.size_given = true;
		}
		else if (a.dir.empty()) a.dir = argv[i];
	}
	if (win_opts.lcd_only && !a.size_given) {
		a.win_w = 878;
		a.win_h = 270;
	}

	// Without --layout, look through the usual places in order
	if (a.layout_path.empty())
		a.layout_path = layout::find_default();

	if (!a.dump_layout.empty()) {
		layout l;
		std::string lerr;
		if (!a.layout_path.empty())
			l.load(a.layout_path, lerr);
		if (!l.save(a.dump_layout)) {
			std::fprintf(stderr, CLI_T("Cannot write %s\n", "%s に書けない\n"), a.dump_layout.c_str());
			return 1;
		}
		std::printf(CLI_T("Wrote the current layout: %s\n", "いまの配置を書き出した: %s\n"), a.dump_layout.c_str());
		std::printf(CLI_T("After editing it, pass it with --layout or press F5 in the window\n", "直したら --layout で渡すか、窓で F5 を押す\n"));
		return 0;
	}
	return -1;
}

} // namespace ui

#endif // S_MU2000_UI_TOOL_ARGS_H

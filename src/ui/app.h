// license:BSD-3-Clause
//
// The GUI application both graphical front ends are.
//
// gui.exe and the Mac GUI keep the same state (bridge, panel, player,
// button bar, remembered ports) and paint the same picture; only the event
// pump, the window system and the dialogs differ. That shared half lives
// here so a feature added on one side cannot be missed on the other. Each
// front end keeps its window class (WndProc / window_mac.mm) and forwards to
// these from thin per-platform shells.
//
// Slice 1: state + panel paint. Input dispatch, menu actions and lifecycle
// follow in later slices.

#ifndef S_MU2000_UI_APP_H
#define S_MU2000_UI_APP_H

#pragma once

#include "compat/cli_text.h"
#include <atomic>
#include <functional>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "ui/audio_in.h"
#include "ui/audio_out.h"
#include "ui/audio_output_switch.h"
#include "ui/bridge.h"
#include "ui/engine.h"
#include "ui/fx_editor.h"
#include "ui/keymap.h"
#include "ui/master_editor.h"
#include "ui/sampling_editor.h"
#include "ui/menu.h"
#include "ui/options.h"
#include "ui/overview.h"
#include "ui/panel.h"
#include "ui/part_shapes.h"
#include "ui/pc_editor.h"
#include "ui/pc_host.h"
// Linux's pc_window lives in its own header (SDL3 shell): same class name
// and shape, but it must not be declared twice in one program
#ifdef __linux__
#include "ui/pc_window_linux.h"
#else
#include "ui/pc_window.h"
#endif
#include "ui/player.h"
#include "ui/tool_args.h"
#include "ui/settings.h"
#include "ui/shot.h"
#include "ui/snapshot.h"
#include "ui/status.h"
#include "ui/toolbar.h"

#include "nvram.h"
#include "smartmedia.h"
#include "smf.h"
#include "voicecache.h"

#include <mutex>

namespace ui {

struct engine;
class midi_in;
class midi_out;
class audio_in;

class app
{
public:
	app(bridge &b, midi_in *mi, midi_out &tha, midi_out &thb, midi_out &muo)
	    : br(b), midi(mi), thru_a(tha), thru_b(thb), mu_out(muo) {}

	// ---- shared state (both windows keep the same)

	bridge   &br;
	midi_in  *midi;                  // MIDI IN A-D (mu2000::MIDI_PORTS of them)
	midi_out &thru_a, &thru_b, &mu_out; // THRU A, THRU B, the machine's own OUT

	// The qualified type: a bare `panel panel;` member is an error under
	// GCC's -Wchanges-meaning (the native Linux build compiles this file)
	ui::panel panel;
	player play;
	toolbar bar;                     // the window button bar (not on --lcd)

	// The five PC windows both sides show (same contents, own host window)
	pc_window list{ std::make_unique<overview>() };
	pc_window pc{ std::make_unique<pc_editor>() };
	pc_window fx{ std::make_unique<fx_editor>() };
	pc_window shapes{ std::make_unique<part_shapes>() };
	pc_window master{ std::make_unique<master_editor>() };
	pc_window sampling{ std::make_unique<sampling_editor>() };

	struct engine *eng = nullptr;    // set once the ROMs are loaded
	std::atomic<int> *state = nullptr; // the engine's, so menus can grey out
	bool lcd_only = false;           // --lcd: the LCD on its own
	std::string layout_path;

	// The remembered ports, by name (empty = default/unused). *_keep is the
	// name to fall back on when a port is not there (yet). Four entries,
	// like ui::SET_IN_KEYS (both front ends run 4 MIDI ports)
	std::string in_name[4];
	std::string in_keep[4];
	std::string out_name, out_name_b, out_name_mu;
	// 音色の窓の送り先（Ctrl＋右クリックで送るもの）。空ならパネルの設定（A → THRU A、B → THRU B）。
	// THRU A・B と同じ機器を選んだときはそちらの出力を使い、ほかの機器なら edit_out を開く
	std::string edit_out_name, edit_out_keep;
	midi_out    edit_out;
	std::string out_keep, out_keep_b, out_keep_mu;
	std::string audio_name;          // the audio device, by name
	std::vector<std::string> audio_menu_devices;
	std::atomic<bool> audio_ready{false};
	int audio_latency = 20;
	bool audio_exclusive = false;
	bool audio_failed = false; // firmware is booted, but its output failed
	std::string ain_name;            // the recording device, by name
	std::string ain_keep;
	std::string card_path;           // the SmartMedia in the slot, by path
	bool keep_settings = false;      // --nomidi: leave the remembered alone

	audio_out *out = nullptr;        // set once the audio device is open
	audio_in  *ain = nullptr;        // set once the recording device is picked

	// Device indices being opened (-1 unused). Names above outlive them:
	// unplugging USB shifts numbers, so reconnects look the names up again
	int in_dev[4] = { -1, -1, -1, -1 };
	int out_dev = -1, out_dev_b = -1, out_dev_mu = -1;
	int ain_dev = -1;
	u64 reported_drops = 0;          // MIDI drops the UI thread last reported

	std::thread reboot;              // the factory-reset reboot, while it runs

	void join_reboot()
	{
		if (reboot.joinable())
			reboot.join();
	}

	// ---- shared paint (the whole panel picture, status line included)

	// The timer half of a frame: the panel tick and the PC windows. Each
	// window runs it at 30 Hz, then paints through its draw list
	// (paint_main below)
	void frame_work()
	{
		run_deferred();
		poll();
		serve_ain_requests();
		pc_frame_all(list, pc, fx, shapes, master, sampling, panel.xg(), panel.ram(), br,
		             [this](pc_window &w) { open_pc_window(w); });
	}

	// A full frame: timer work, status middle, panel paint through the
	// window's draw list. The middle fragment and the PC window opening
	// stay virtual (backend stats, host windows)
	void paint_main(ImDrawList *dl, const im::fonts &f, int w)
	{
		frame_work();
		char middle[64] = {};
		if (out && out->produced())
			format_middle(middle, sizeof(middle));
		paint_into(dl, w, middle, f);
	}

	// The wait/drop fragment for the status line (WASAPI: 待ち + 遅れ,
	// CoreAudio: 遅れ). Called only when the device is up
	virtual void format_middle(char *dst, std::size_t n) = 0;
	// Opens one PC window (host windows differ)
	virtual void open_pc_window(pc_window &w) = 0;

	void paint_into(ImDrawList *dl, int w, const char *middle, const im::fonts &f)
	{
		snapshot s;
		br.read(s);
		const u64 pressed = br.buttons();
		char status[320] = {};
		if (out && out->produced()) {
			format_status_line(status, sizeof(status),
			                   s.voices_master + s.voices_slave,
			                   out->cpu_percent(), out->worst_ms(),
			                   middle,
			                   in_name[0].empty() ? UI_TEXT(status_none, "none") : in_name[0].c_str(),
			                   out_name.empty() ? UI_TEXT(status_none, "none") : out_name.c_str());
		}
		else
			std::snprintf(status, sizeof(status), "%s", UI_TEXT(status_booting, "Starting..."));
		panel.set_volume(br.gain());
		panel.paint(dl, s, pressed, status);
		// The bar paints after the panel (the panel fills everything). It keeps
		// the window's fixed 16 px set: it does not scale with the panel, so the
		// panel's own sizes would only make it jump around while resizing
		bar.paint(dl, w, f.label, f.bar_px);
	}

	// ---- shared input decisions (both windows act the same way)
	// What a mouse press means. bar_window is a BAR_* id to open; menu asks
	// for the context menu at the point (each side picks which one); neither
	// set means press the panel. The strip order, the jack spots and the LCD
	// guard are the same on both, so this is decided once.
	struct mouse_hit {
		bool handled = false;
		bool menu = false;
		int bar_window = -1;
	};

	mouse_hit hit_test(int x, int y, bool right) const
	{
		mouse_hit h;
		if (lcd_only)
			return h;
		// A secondary click opens the port picker wherever it lands
		if (right) {
			h.handled = true;
			h.menu = true;
			return h;
		}
		// The strip first: it is not the panel, so nothing reaches the machine
		const int id = bar.hit(x, y);
		if (id >= 0) {
			h.handled = true;
			h.bar_window = id;
			return h;
		}
		if (y < toolbar::HEIGHT) {
			h.handled = true;            // the strip's gaps
			return h;
		}
		// The jacks and the card slot are pressed, not clicked: they open a
		// menu instead of moving a panel control
		if (panel.on_midi_jack(x, y) || panel.on_ad_input(x, y) ||
		    panel.on_card_slot(x, y) || panel.on_phones(x, y)) {
			h.handled = true;
			h.menu = true;
			return h;
		}
		return h;
	}

	// A character key (both sides extract these from their key codes).
	// True when eaten: the meaning of a letter lives in ui/keymap.h.
	bool handle_panel_key(int ch, bool down)
	{
		mu2000::button b = mu2000::button::count;
		if (!button_for_char(ch, b))
			return false;
		br.press(b, down);
		return true;
	}

	// The one key handler: every pump translates its key codes into the
	// shared space (menu.h -- KEY_F2..F5 and the panel characters) and
	// calls this. Keyups of the F-keys mean nothing; the panel characters
	// latch their button while held
	virtual void key(int code, bool down)
	{
		if (lcd_only && down)
			return;
		switch (code) {
		case ui::KEY_F2:
			if (down)
				open_window_by_kind(BAR_EDITOR);
			return;
		case ui::KEY_F3:
			if (down)
				open_window_by_kind(BAR_LIST);
			return;
		case ui::KEY_F4:
			if (down)
				toggle_engine();
			return;
		case ui::KEY_F5:
			if (down)
				reload_layout();
			return;
		default:
			handle_panel_key(code, down);
			return;
		}
	}

	// The F4 native-engine toggle both sides offer (key and menu)
	void toggle_engine()
	{
		if (eng)
			eng->want_native_engine.store(eng->native_engine.load() ? 0 : 1);
	}

	// The window lost focus: let go of everything the user was holding
	virtual void focus_lost()
	{
		pressed = false;
		br.release_all();
	}

	// The main window changed size
	virtual void resized(int w, int h)
	{
		panel.resize(w, h);
	}

	// A file was dropped on the window: playing it is what a drop means on
	// every platform
	virtual void file_dropped(const std::string &path)
	{
		play_song(path);
	}

	// Panel layout from a file (F5 reads it back). Same file both sides
	void apply_layout(const std::string &path, bool quiet)
	{
		panel.lay() = layout();
		std::string err;
		if (!path.empty() && panel.lay().load(path, err)) {
			if (!quiet)
				std::printf(CLI_T("Layout: %s\n", "配置: %s\n"), path.c_str());
		} else if (!path.empty() && !quiet) {
			std::printf(CLI_T("Layout: cannot open %s. Using the built-in layout\n", "配置: %s を開けない。組み込みの配置を使う\n"), path.c_str());
		}
		if (!err.empty())
			std::fprintf(stderr, "%s", err.c_str());
		std::fflush(stdout);
		panel.resize(panel.width(), panel.height());
	}

	void reload_layout() { apply_layout(layout_path, false); }

	// Which popup the point asks for. The
	// card slot, PHONES and A/D INPUT have their own; everywhere else gets
	// the port picker
	virtual std::vector<menu_group> context_menu(int x, int y)
	{
		if (panel.on_card_slot(x, y))
			return menu_card(menu_snapshot());
		if (panel.on_phones(x, y))
			return menu_phones(menu_snapshot());
		if (panel.on_ad_input(x, y))
			return menu_ain_only(audio_in::list(), ain_name);
		return menu_ports(menu_snapshot());
	}

	// ---- the event verbs, in pump vocabulary. Every pump (wnd_proc, the
	// SDL loop, window_mac.mm) calls these under the same names, so a
	// meaning lives once: the lcd_only guards and the outcome struct are
	// here, and the platforms only translate their events into these calls
	virtual ui::mouse_out mouse_down(int x, int y, bool right)
	{
		mouse_out o;
		if (lcd_only)
			return o;
		const mouse_hit h = hit_test(x, y, right);
		if (h.bar_window >= 0) {
			bar.set_down(h.bar_window);
			open_window_by_kind(h.bar_window);
			o.opened_window = true;
			pressed = true;
			return o;
		}
		if (h.handled) {
			o.show_menu = h.menu;
			return o;
		}
		pressed = true;
		o.panel_pressed = true;
		panel.press(x, y, br);
		return o;
	}

	virtual bool mouse_drag(int x, int y)
	{
		if (lcd_only || !pressed)
			return false;
		return panel.drag(x, y, br);
	}

	virtual void mouse_up()
	{
		if (!pressed)
			return;
		pressed = false;
		bar.set_down(-1);
		panel.release(br);
	}

	virtual bool wheel(int x, int y, int steps)
	{
		if (lcd_only || !steps)
			return false;
		return panel.wheel_at(x, y, steps, br);
	}

	// A pointing-hand cursor where something opens
	virtual bool hand_cursor(int x, int y)
	{
		if (lcd_only)
			return false;
		return panel.on_midi_jack(x, y) || panel.on_ad_input(x, y) ||
		       panel.on_card_slot(x, y) || panel.on_phones(x, y);
	}

	// ---- per-platform acts (thin shells implement these)

	// Open a PC window by BAR_* id (F2/F3, the strip, the menus)
	void open_window_by_kind(int kind)
	{
		open_pc_window(*window_for_kind(kind, list, pc, fx, shapes, master, sampling));
	}

	// ---- remembered settings (gui.ini)

	// Where the file lives differs per platform (registry side vs Library)
	virtual std::string settings_path() const = 0;

	void save_settings()
	{
		if (keep_settings)                   // --nomidi: keep the ports
			return;
		const std::string path = settings_path();
		if (path.empty())
			return;
		remembered r;
		for (int p = 0; p < 4; p++)
			r.in[p] = in_name[p].empty() ? in_keep[p] : in_name[p];
		r.out       = out_name.empty()    ? out_keep    : out_name;
		r.out_b     = out_name_b.empty()  ? out_keep_b  : out_name_b;
		r.out_mu    = out_name_mu.empty() ? out_keep_mu : out_name_mu;
		r.audio_out = audio_name;
		r.audio_in  = ain_name.empty() ? ain_keep : ain_name;
		r.card      = card_path;
		r.volume    = br.gain();
		r.fold34    = play.fold_extra_ports();
		r.thin_bends = play.thin_bends();
		r.analog    = eng && eng->analog.load();
		r.edit_out  = edit_out_name.empty() ? edit_out_keep : edit_out_name;
		write_settings_file(path, collect_settings(r));
	}

	static remembered load_remembered(const std::string &path)
	{
		remembered r;
		if (path.empty())
			return r;
		settings_map kv;
		if (!read_settings_file(path, kv))
			return r;
		apply_settings(kv, r);
		return r;
	}

	// ---- ports (the menus pick these)

	// Open what the menu picked, falling back to "unused". keep is true
	// only while starting up: the asked-for name is then kept even if the
	// port is not there yet. Failures print to stderr always and reach
	// menu_error only for menu picks (never boot-time).
	bool choose_in(int port, int dev, bool keep = false)
	{
		if (port < 0 || port >= 4)
			return false;
		if (!keep)
			in_keep[port].clear();
		std::string err;
		if (!midi[port].open(dev, err)) {
			std::fprintf(stderr, "%s: %s\n", in_label(port), err.c_str());
			if (!keep)
				menu_error(err);
			midi[port].open(-1, err);
			dev = -1;
		}
		in_dev[port]  = midi[port].is_open() ? dev : -1;
		in_name[port] = midi[port].device_name();
		save_settings();
		return dev >= 0;
	}

	bool choose_out(int dev, bool keep = false)
	{
		return open_out(thru_a, out_dev, out_name, out_keep,
		                CLI_T("MIDI output", "MIDI 出力"), dev, keep);
	}

	bool choose_out_b(int dev, bool keep = false)
	{
		return open_out(thru_b, out_dev_b, out_name_b, out_keep_b,
		                CLI_T("MIDI output B", "MIDI 出力 B"), dev, keep);
	}

	// 音色の窓の送り先。dev が負ならパネルの設定
	void choose_edit_out(int dev, bool keep = false)
	{
		if (!keep)
			edit_out_keep.clear();
		edit_out.close();
		edit_out_name.clear();
		if (dev >= 0) {
			const std::vector<std::string> names = midi_out::list();
			if (dev < int(names.size())) {
				const std::string &name = names[size_t(dev)];
				std::string err;
				// THRU A・B に使っている機器は開き直さない（同じ機器を 2 度は開けない）
				if (name == out_name || name == out_name_b || edit_out.open(dev, err))
					edit_out_name = name;
				else
					std::fprintf(stderr, CLI_T("Send to %s: %s\n", "送り先 %s: %s\n"), name.c_str(), err.c_str());
			}
		}
		save_settings();
	}
	// 口（0-3）→ 送る行き先（bridge::send_out の dest）
	int edit_dest(int port) const
	{
		if (edit_out_name.empty())
			return port == 1 ? 1 : 0;            // パネルの設定。C・D は外へ出す端子が無いので A へ
		if (edit_out_name == out_name)
			return 0;
		if (edit_out_name == out_name_b)
			return 1;
		return 2;
	}
	// 音色の窓に、送り先の品書きと送る道を渡す
	void wire_send_out()
	{
		xgui::out_hooks h;
		h.devices = [] { return midi_out::list(); };
		h.chosen = [this] { return edit_out_name; };
		h.panel_desc = [this] {
			return "A: " + (out_name.empty() ? std::string("-") : out_name) +
			       " / B: " + (out_name_b.empty() ? std::string("-") : out_name_b);
		};
		h.choose = [this](int dev) { choose_edit_out(dev); };
		h.dest = [this](int port) { return edit_dest(port); };
		xgui::set_out_hooks(std::move(h));
	}

	bool choose_out_mu(int dev, bool keep = false)
	{
		return open_out(mu_out, out_dev_mu, out_name_mu, out_keep_mu,
		                CLI_T("MIDI output (the unit's OUT)", "MIDI 出力（本体の OUT）"), dev, keep);
	}

	// サンプリングの窓の録音デバイスの欄（bridge::set_ain_devices / request_ain）。
	// 一覧は窓が欄を開いたときと、選び直した後に作り直す（デバイスを数えるのは重いので毎コマはしない）。
	//
	// **パネルを描いている最中（frame_work の中）にデバイスを数えたり開いたりしない。** Windows の UI の糸は
	// COM の STA なので、WASAPI を初めて使うときにたまっている窓のメッセージを処理することがあり、そこで
	// パネルの WM_PAINT が入れ子で来ると、終わっていないコマの上で ImGui::NewFrame が呼ばれて assert で落ちた
	// （作り直した exe の初回の起動で、起動が遅いときに出た）。一覧は別の糸で数え、選び直しは描画の外で行う
	// （Windows は窓のメッセージで、ほかは次のコマの頭で。defer_outside_paint）
	bool m_ain_listed = false;
	std::atomic<bool> m_ain_listing{false};
	std::thread m_ain_lister;
	void list_ain_async()
	{
		if (m_ain_listing.exchange(true))
			return;
		if (m_ain_lister.joinable())
			m_ain_lister.join();
		const std::string current = ain_name;
		m_ain_lister = std::thread([this, current] {
			br.set_ain_devices(audio_in::list(), current);
			m_ain_listing.store(false);
		});
	}
	void serve_ain_requests()
	{
		if (br.take_ain_list_request() || !m_ain_listed) {
			m_ain_listed = true;
			list_ain_async();
		}
		const int want = br.take_ain_request();
		if (want >= -1)
			defer_outside_paint([this, want] {
				choose_ain(want);
				list_ain_async();
			});
		// サンプリングの窓の「カード」: 頼まれたカードを差し、差しているカードの場所を知らせる
		std::string card;
		if (br.take_card_request(card))
			defer_outside_paint([this, card] {
				if (insert_card(card))
					save_settings();
			});
		br.set_card_path(card_path);
	}
	// 描画の外で行う仕事。Windows は窓のメッセージで（app_win.h）、ほかは次のコマの頭で
	std::vector<std::function<void()>> m_deferred;
	virtual void defer_outside_paint(std::function<void()> f) { m_deferred.push_back(std::move(f)); }
	void run_deferred()
	{
		std::vector<std::function<void()>> todo;
		todo.swap(m_deferred);
		for (auto &f : todo)
			f();
	}

	bool choose_ain(int dev, bool keep = false)
	{
		if (!keep)
			ain_keep.clear();
		if (!ain)
			return false;
		ain->stop();
		if (dev < 0) {
			ain_name.clear();
		} else {
			const auto names = audio_in::list();
			if (dev < int(names.size())) {
				std::string err;
				if (!ain->start(names[size_t(dev)], err)) {
					std::fprintf(stderr, "A/D INPUT: %s\n", err.c_str());
					if (!keep)
						menu_error(err);
				} else {
					std::printf("A/D INPUT: %s（%s）\n",
					            ain->device_name().c_str(),
					            ain->format_line().c_str());
					std::fflush(stdout);
				}
				ain_name = names[size_t(dev)];
			}
		}
		save_settings();
		return true;
	}

	// ---- SmartMedia (the card slot)

	// Written-back blocks go to the file; only snapshotting stops the
	// audio thread. Called from the timers, and on eject/close/save
	void flush_card()
	{
		if (!eng || card_path.empty())
			return;
		std::vector<smu2000::smartmedia::block> blocks;
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card().take_dirty_blocks(blocks);
		}
		if (blocks.empty())
			return;
		std::string err;
		if (!smu2000::smartmedia::write_blocks(card_path, blocks, err))
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
	}

	void card_tick()
	{
		const u64 now = smu2000::perf_ticks() * 1000 / smu2000::perf_freq();
		if (now - last_flush < 2000)
			return;
		last_flush = now;
		flush_card();
	}

	// A MIDI loop (THRU fed back into an IN) overflows the guards. Said out
	// loud once a second, from the window's timer rather than the paint
	void report_drops()
	{
		if (!eng)
			return;
		const u64 now = smu2000::perf_ticks() * 1000 / smu2000::perf_freq();
		if (now - last_drop_report < 1000)
			return;
		last_drop_report = now;
		const u64 drops = eng->guard_a.dropped() + eng->guard_b.dropped() +
		                  eng->mu.midi_dropped();
		if (drops == reported_drops)
			return;
		reported_drops = drops;
		std::fprintf(stderr,
		             CLI_T("Dropped MIDI, too much of it: THRU A %llu / THRU B %llu / received %llu bytes"
" (check for a MIDI loop)\n", "MIDI が多すぎるので捨てた: THRU A %llu / THRU B %llu / 受信 %llu バイト"
		             "（MIDI の輪ができていないか確かめる）\n"),
		             (unsigned long long)eng->guard_a.dropped(),
		             (unsigned long long)eng->guard_b.dropped(),
		             (unsigned long long)eng->mu.midi_dropped());
	}

	// The window's timer work, both sides: feed the panel, publish CPU and
	// engine state for the PC windows, flush the card file, report drops.
	// Opening/drawing the PC windows stays per side (different window types)
	void poll()
	{
		panel.tick(br);
		if (out && out->produced())
			br.set_cpu(float(out->cpu_recent()));   // 直近の重さ（平均は終わりの集計に）
		br.set_engine(eng ? eng->native_engine.load() : -1);
		card_tick();
		report_drops();
	}

	void eject_card()
	{
		if (!eng)
			return;
		flush_card();
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card().eject();
		}
		if (!card_path.empty())
			std::printf(CLI_T("SmartMedia removed: %s\n", "SmartMedia を抜いた: %s\n"), card_path.c_str());
		std::fflush(stdout);
		card_path.clear();
		save_settings();
	}

	// Load it first, so a file that cannot be read does not take the slot
	// away from the card that is already in it. quiet is for boot, where a
	// missing file must stay silent
	bool insert_card(const std::string &path, bool quiet = false)
	{
		if (!eng)
			return false;
		smu2000::smartmedia card;
		std::string err;
		if (!card.load(path, err)) {
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
			if (!quiet)
				menu_error(err);
			return false;
		}
		eject_card();
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card() = std::move(card);
			eng->mu.card_swapped();   // firmware に抜けたのを見せる（前のカードの FAT を忘れさせる）
		}
		card_path = path;
		std::printf(CLI_T("SmartMedia inserted: %s (%uMB)\n", "SmartMedia を差した: %s（%uMB）\n"),
		            path.c_str(), eng->mu.card().megabytes());
		std::fflush(stdout);
		save_settings();
		return true;
	}

	// A new card, already formatted the way the machine's UTIL -> CARD ->
	// Format leaves it (smartmedia::format), so it can be saved to at once
	void new_card(u32 megabytes)
	{
		const std::string path = ask_card_save_path();
		if (path.empty())
			return;
		smu2000::smartmedia card;
		if (!card.create(megabytes) || !card.format()) {
			std::fprintf(stderr, "%s\n", UI_TEXT(dlg_card_create_fail, "Cannot create the SmartMedia image"));
			return;
		}
		std::string err;
		if (!card.save(path, err)) {
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
			menu_error(err);
			return;
		}
		if (insert_card(path))
			menu_note(UI_TEXT(dlg_fresh_card, "Inserted a new SmartMedia image.\n"
			                                  "It is already formatted (as UTIL → CARD → Format leaves it), so it can be saved to right away."));
	}

	void do_card_open()
	{
		const std::string path = ask_card_open_path();
		if (!path.empty() && insert_card(path))
			save_settings();
	}

	// ---- MIDI file playback

	// Plays the file picked from a menu or dropped on a window. Restarts it
	// when it is already playing
	bool play_song(const std::string &path)
	{
		std::string err;
		if (!play.start(path, br, err)) {
			std::fprintf(stderr, CLI_T("Cannot open: %s\n", "開けない: %s\n"), err.c_str());
			char m[512];
			std::snprintf(m, sizeof(m), UI_TEXT(dlg_cannot_fmt, "Cannot open: %s"), err.c_str());
			menu_error(m);
			return false;
		}
		std::printf(CLI_T("Playing: %s (%.1f s)\n", "再生: %s（%.1f 秒）\n"), path.c_str(), play.length());
		// The machine has two ports, so a four-port file is either folded
		// onto them or has its extra parts dropped
		if (play.ports_used() > 2)
			std::printf(CLI_T("  This song uses %d ports. C and D are not supported, so ports 3 and up are %s\n", "  この曲は %d 口ぶん。C・D は未対応なので、口 3 以降は%s\n"),
			            play.ports_used(),
			            play.fold_extra_ports() ? CLI_T("played on top of A and B", " A・B に重ねて鳴らす") : CLI_T("not played", "鳴らさない"));
		std::fflush(stdout);
		return true;
	}

	void do_midi_file()
	{
		const std::string path = ask_midi_file_path();
		if (!path.empty())
			play_song(path);
	}

	// ---- the rest of the menu

	// Same backend contract on Windows, macOS and Linux. Device names come
	// from the menu snapshot, never from a new enumeration after the click.
	void choose_audio(int dev)
	{
		if (!audio_ready.load() || !out || !eng || !state || state->load() == 0)
			return;
		if (dev >= 0 && size_t(dev) >= audio_menu_devices.size())
			return;
		const std::string wanted = dev < 0 ? std::string() : audio_menu_devices[size_t(dev)];
		join_reboot();
		if (state->load() != 1 && !audio_failed)
			return;
		// Pause emulation while opening outputs; their initial callbacks may
		// run before start() returns. Resume the existing machine afterward.
		eng->state.store(0);
		while (eng->in_fill.load())
			smu2000::sleep_ms(1);
#if defined(__APPLE__)
		// The slave must leave the old AudioUnit's workgroup before that
		// unit is disposed. Keep the threading policy, not just its current
		// admission state (the instance limit may have parked the slave).
		const bool threaded = eng->mu.threading_requested();
		eng->mu.set_threaded(false);
		eng->mu.set_realtime_workgroup(nullptr);
#endif
		const auto r = switch_audio_output(*out, audio_latency,
			[this](s16 *o, u32 n) { eng->fill(o, n); },
			audio_exclusive, wanted, audio_name);
#if defined(__APPLE__)
		if (r.selected || r.restored)
			eng->mu.set_realtime_workgroup(out->realtime_workgroup());
		eng->mu.set_threaded(threaded);
#endif
		audio_failed = !r.selected && !r.restored;
		if (!audio_failed)
			eng->state.store(1); // also recover a failed output at startup
		if (r.selected) {
			audio_name = wanted;
			save_settings();
			say_audio_opened(audio_exclusive);
		} else {
			if (!r.restored) {
				eng->message = r.error;
				eng->state.store(2);
				eng->publish();
			}
			char error[2048];
			std::snprintf(error, sizeof(error), UI_TEXT(audio_switch_failed_fmt,
			              "Cannot switch audio output:\n%s"), r.error.c_str());
			menu_error(error);
		}
	}

	// Throwing the settings away means rebooting the machine, which takes
	// tens of seconds, so it runs on its own thread (joined first: two
	// boots at once would both be writing the machine)
	void do_factory_reset()
	{
		if (!eng || !state || state->load() != 1)
			return;
		if (!confirm_factory_reset())
			return;
		play.stop();
		join_reboot();
		reboot = std::thread([this] { eng->factory_reset(); });
	}

	void set_fold34(bool on)
	{
		play.set_fold_extra_ports(on);
		save_settings();
	}

	void toggle_thin_bends()
	{
		play.set_thin_bends(!play.thin_bends());
		save_settings();
	}

	void set_analog(bool on)
	{
		if (!eng)
			return;
		// Digital matches S/PDIF (some DPCM samples keep their DC, as on
		// the hardware); analog cuts DC like LINE OUT and PHONES do
		eng->analog.store(on);
		std::printf(CLI_T("Sound output: %s\n", "音の出口: %s\n"), on ? CLI_T("analogue (DC removed)", "アナログ（直流を切る）") : CLI_T("digital", "デジタル"));
		std::fflush(stdout);
		save_settings();
	}

	void toggle_fx()
	{
		if (eng)
			eng->want_native_fx.store(eng->native_fx.load() ? 0 : 2);
	}

	// What the shared menu builders (ui/menu.h) show, from this window's state
	menu_state menu_snapshot()
	{
		menu_state s;
		s.midi_ins = midi_in::list();
		s.midi_outs = midi_out::list();
		s.audio_ins = audio_in::list();
		s.audio_outs = audio_out::list();
		audio_menu_devices = s.audio_outs;
		s.audio_ready = audio_ready.load() && state && (state->load() == 1 || audio_failed);
		if (s.audio_ready)
			s.audio_name = audio_name;
		for (int p = 0; p < 4; p++)
			s.in_dev[p] = in_dev[p];
		s.out_dev = out_dev;
		s.out_dev_b = out_dev_b;
		s.out_dev_mu = out_dev_mu;
		s.ain_name = ain_name;
		s.card_path = card_path;
		s.playing = play.playing();
		s.play_name = play.name();
		s.fold34 = play.fold_extra_ports();
		s.thin_bends = play.thin_bends();
		s.ready = eng && state && state->load() == 1;
		s.native_fx = eng && eng->native_fx.load();
		s.native_engine = eng && eng->native_engine.load();
		s.analog = eng && eng->analog.load();
		return s;
	}

	// The shared dispatch for the 26 menu IDs both front ends render
	// (ui/menu.h). Only the dialogs and the error display are per-platform
	// (the hooks below); everything else is the same calls in the same order
	void menu_chosen(int id)
	{
		for (int p = 0; p < 4; p++) {
			const int none = ID_IN_NONE + p * ID_IN_STRIDE, base = ID_IN_BASE + p * ID_IN_STRIDE;
			if (id == none)                    { choose_in(p, -1); return; }
			if (id >= base && id < base + 256) { choose_in(p, id - base); return; }
		}
		if (id == ID_OUT_NONE)                                        choose_out(-1);
		else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 256)         choose_out(id - ID_OUT_BASE);
		else if (id == ID_OUTMU_NONE)                                 choose_out_mu(-1);
		else if (id >= ID_OUTMU_BASE && id < ID_OUTMU_BASE + 256)     choose_out_mu(id - ID_OUTMU_BASE);
		else if (id == ID_OUTB_NONE)                                  choose_out_b(-1);
		else if (id >= ID_OUTB_BASE && id < ID_OUTB_BASE + 256)       choose_out_b(id - ID_OUTB_BASE);
		else if (id == ID_AIN_NONE)                                   choose_ain(-1);
		else if (id >= ID_AIN_BASE && id < ID_AIN_BASE + 256)         choose_ain(id - ID_AIN_BASE);
		else if (id == ID_AUDIO_DEFAULT)                              choose_audio(-1);
		else if (id >= ID_AUDIO_BASE && id < ID_AUDIO_BASE + 256)     choose_audio(id - ID_AUDIO_BASE);
		else if (id == ID_CARD_OPEN)                                  do_card_open();
		else if (id == ID_CARD_EJECT)                                 { eject_card(); save_settings(); }
		else if (id >= ID_CARD_NEW16 && id <= ID_CARD_NEW128)         new_card(16u << (id - ID_CARD_NEW16));
		else if (id == ID_PLAY_FILE)                                  do_midi_file();
		else if (id == ID_STOP_FILE)                                  play.stop();
		else if (id == ID_PORTS34_FOLD)                               set_fold34(true);
		else if (id == ID_PORTS34_DROP)                               set_fold34(false);
		else if (id == ID_THIN_BENDS)                                 toggle_thin_bends();
		else if (id == ID_NATIVE_FX)                                  toggle_fx();
		else if (id == ID_NATIVE_ENGINE)                              toggle_engine();
		else if (id == ID_FACTORY)                                    do_factory_reset();
		else if (id == ID_PC_EDITOR)                                  open_window_by_kind(BAR_EDITOR);
		else if (id == ID_OVERVIEW)                                   open_window_by_kind(BAR_LIST);
		else if (id == ID_OUTPUT_DIGITAL || id == ID_OUTPUT_ANALOG)   set_analog(id == ID_OUTPUT_ANALOG);
	}

	// ---- bring-up and shutdown (both mains call these in order)

	// Wires the engine to the ports main() owns and the parsed engine flags
	void wire_engine(engine &eng, engine_options &o)
	{
		if (std::getenv("SMU2000_VOICECACHE"))
			o.voicecache = 1;
		apply_engine_options(eng.mu, o);
		eng.native_fx.store(o.native_fx);
		for (int p = 1; p < mu2000::MIDI_PORTS; p++)
			eng.midi_p[p] = &midi[p];
		eng.mout_b = &thru_b;
		eng.mout_edit = &edit_out;
		eng.mout_mu = &mu_out;
		eng.mout = &thru_a;
	}

	// Switches the booted machine to the native engine on request (the
	// stored flag is what the overview badge reads)
	void apply_native_engine(engine &eng, const engine_options &o)
	{
		if (!o.native_engine)
			return;
		eng.mu.set_native_engine(o.native_engine);
		eng.native_engine.store(o.native_engine);
		if (o.voicecache)
			smu2000::voicecache::load(eng.mu, smu2000::voicecache::key(eng.mu));
	}

	// Loads the ROMs and wires the USB host flag. False exits with code 1
	bool load_machine(engine &eng, const tool_args &a)
	{
		if (!eng.load(a.dir)) {
			std::fprintf(stderr, "%s\n", eng.message.c_str());
			return false;
		}
		// The overview reads voice names and instrument icons from the ROM
		xgui::set_voice_rom(eng.mu.program_rom());
		// USB ports by default, as when the hardware sits on PC USB (parts
		// C/D only pass when HOST SELECT is USB; on USB, A/B go quiet on DIN
		// just like the hardware). --host-midi leaves DIN A/B only.
		// Decided before booting: reset() learns here whether to queue the
		// "a host is here" notice
		eng.mu.set_usb_host(a.usb_host);
		play.set_usb_ports(a.usb_host);        // ファイルの口 3・4 を C・D へ送るか
		std::printf(a.usb_host ? CLI_T("MIDI goes to the USB ports (A-D, 64 parts)\n", "MIDI は USB の口（A-D の 64 パート）\n")
		                     : CLI_T("--host-midi: the DIN ports A and B only (parts 1-32)\n", "--host-midi: DIN の口 A・B だけ（パート 1-32）\n"));
		return true;
	}

	// Boots the firmware to snap --shot/--mid pictures (settles the display,
	// plays the MIDI for the meters). 1 fails, 0 snaps, -1 carries on
	int run_boot_shot(engine &eng, bridge &br, const tool_args &a,
	                  const window_options &w)
	{
		if (a.shot_path.empty())
			return -1;
		if (!eng.boot()) {
			std::fprintf(stderr, "%s\n", eng.message.c_str());
			return 1;
		}
		eng.state.store(1);
		// Right after boot the display is still settling
		{
			s32 l, r;
			for (size_t i = 0; i < size_t(2.0 * AUDIO_RATE); i++)
				eng.mu.run_sample(l, r);
		}
		// With MIDI, play it first so the level meters have something to show
		if (!a.shot_mid.empty()) {
			std::vector<smf::event> evs;
			std::string err;
			if (!smf::load(a.shot_mid, evs, err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
			} else {
				std::printf(CLI_T("Feeding %zu MIDI events, %.1f s of them\n", "MIDI %zu 件を %.1f 秒ぶん流す\n"), evs.size(), a.shot_secs);
				size_t at = 0;
				s32 l, r;
				for (size_t i = 0; i < size_t(a.shot_secs * AUDIO_RATE); i++) {
					const double now = double(i) / AUDIO_RATE;
					while (at < evs.size() && evs[at].time <= now) {
						for (u8 b : evs[at].bytes)
							eng.mu.midi_in(b);
						at++;
					}
					eng.mu.run_sample(l, r);
				}
			}
		}
		eng.publish();
		return write_shot(a.shot_path, a.win_w, a.win_h, br, a.grid,
		                  w.lcd_only, a.layout_path);
	}

	// Window-side setup both mains do once the machine object exists:
	// LCD/bar/panel sizing and layout, remembered volume and output,
	// clean or remembered boot
	void setup_for_window(const tool_args &a, const window_options &w, bool factory)
	{
		lcd_only = w.lcd_only;
		wire_send_out();
		panel.set_lcd_only(lcd_only);
		if (!lcd_only) {
			bar.set_items(window_bar_items());
			panel.set_top_inset(toolbar::HEIGHT);
		}
		panel.resize(a.win_w, a.win_h);
		layout_path = a.layout_path;
		apply_layout(a.layout_path, false);
		panel.resize(a.win_w, a.win_h);
		{
			// The VOLUME knob starts where it was left
			remembered r = load_remembered(settings_path());
			br.set_gain(r.volume);
			if (eng)
				eng->analog.store(r.analog);
			if (r.analog)
				std::printf(CLI_T("Sound output: analogue (DC removed)\n", "音の出口: アナログ（直流を切る）\n"));
			play.set_fold_extra_ports(r.fold34);
			play.set_thin_bends(r.thin_bends);
		}
		// Only the window boots from remembered settings: --shot must give
		// the same picture every time
		if (eng)
			eng->use_nvram = !factory;
		if (factory)
			std::printf(CLI_T("Starting from factory defaults (the remembered settings are overwritten on exit)\n", "工場出荷状態で起動する（覚えていた設定は終わるときに上書きされる）\n"));
	}

	// A found port prints by name; a missing one keeps showing its
	// remembered name until it is picked again
	static void show_port(const char *label, const std::string &now,
	                      const std::string &keep)
	{
		if (!now.empty())
			std::printf("%s: %s\n", label, now.c_str());
		else if (!keep.empty())
			std::printf(CLI_T("%s: none (\"%s\" was not found or could not be opened; it stays remembered)\n", "%s: なし（「%s」が見つからないか開けない。覚えたままにしてある）\n"),
			            label, keep.c_str());
		else
			std::printf(CLI_T("%s: none\n", "%s: なし\n"), label);
	}

	// Opens the remembered MIDI ports by name (--midi and friends win).
	// Reports what is and is not there
	void open_remembered_ports(tool_args &a, const output_options &o)	{
		remembered want = load_remembered(settings_path());
		ain_name = want.audio_in;
		ain_keep = want.audio_in;
		if (!want.card.empty())
			insert_card(want.card, true);
		// --audio wins; otherwise the port that was opened last time
		audio_name = o.audio_dev ? std::string(o.audio_dev) : want.audio_out;
		// --audio-in wins; otherwise the recording device from last time
		// (empty is off). Opened by start_ad once the firmware is up
		if (o.audio_in_dev)
			ain_name = o.audio_in_dev;
		for (int p = 0; p < 4; p++)
			if (a.in_dev[p] == -2)
				a.in_dev[p] = find_device(midi_in::list(), want.in[p]);
		if (a.mout_dev == -2)   a.mout_dev   = find_device(midi_out::list(), want.out);
		if (a.moutb_dev == -2)  a.moutb_dev  = find_device(midi_out::list(), want.out_b);
		if (a.moutmu_dev == -2) a.moutmu_dev = find_device(midi_out::list(), want.out_mu);
		// A port that is not there yet keeps its name in the settings
		for (int p = 0; p < 4; p++)
			in_keep[p] = want.in[p];
		out_keep    = want.out;
		out_keep_b  = want.out_b;
		out_keep_mu = want.out_mu;
		for (int p = 0; p < 4; p++)
			choose_in(p, a.in_dev[p], true);
		choose_out(a.mout_dev, true);
		choose_out_b(a.moutb_dev, true);
		choose_out_mu(a.moutmu_dev, true);
		// 音色の窓の送り先。無い機器なら名前だけ覚えておく
		edit_out_keep = want.edit_out;
		if (!want.edit_out.empty())
			choose_edit_out(find_device(midi_out::list(), want.edit_out), true);
		// A port that would not open keeps showing its remembered name
		// until it is picked again
		for (int p = 0; p < 4; p++)
			show_port(in_label(p), in_name[p], in_keep[p]);
		show_port("MIDI OUT", out_name_mu, out_keep_mu);
		show_port("MIDI THRU A", out_name, out_keep);
		show_port("MIDI THRU B", out_name_b, out_keep_b);
		std::fflush(stdout);
	}

	// Opens the --editor/--list-window and friends alongside the panel
	void open_startup_windows(const window_options &w)
	{
		if (w.open_editor && !w.lcd_only)
			open_window_by_kind(BAR_EDITOR);
		if (w.open_fx && !w.lcd_only)
			open_window_by_kind(BAR_FX);
		if (w.open_list && !w.lcd_only)
			open_window_by_kind(BAR_LIST);
		if (w.open_shapes && !w.lcd_only)
			open_window_by_kind(BAR_SHAPES);
		if (w.open_master && !w.lcd_only)
			open_window_by_kind(BAR_MASTER);
		if (w.open_sampling && !w.lcd_only)
			open_window_by_kind(BAR_SAMPLING);
	}

	// Starts the audio device. False parks the engine on the failure and
	// asks main to return (the device line each side prints stays per side)
	bool start_audio(int latency_ms, bool exclusive)
	{
		if (!out || !eng)
			return false;
		std::string err;
		if (!out->start(latency_ms, [this](s16 *o, u32 n) { eng->fill(o, n); },
		                err, exclusive, audio_name)) {
			std::fprintf(stderr, CLI_T("Audio: %s\n", "音声: %s\n"), err.c_str());
			eng->message = CLI_T("cannot open the audio device", "音声デバイスを開けない");
			eng->state.store(2);
			eng->publish();
			return false;
		}
		// Remember the port that was actually opened, by name. After the
		// MIDI ports above, or the settings written here would carry an
		// empty MIDI name and the next start would come up with no ports
		// Empty stays empty, so System default is still the default next time.
		// Normalize CLI substring matches to a full menu label (ALSA labels
		// include a description after the PCM name returned by device_name()).
		if (!audio_name.empty()) {
			const std::string opened = out->device_name();
			audio_name = opened;
			for (const auto &name : audio_out::list())
				if (name == opened || name.substr(0, name.find("  (")) == opened) {
					audio_name = name;
					break;
				}
		}
#if defined(__APPLE__)
		// The parallel slave thread joins the output unit's audio workgroup
		// from here (Apple's parallel real-time threads pattern; the join
		// itself is in compat/realtime.h). Null keeps today's behavior.
		// Only macOS has a group to hand over, so only it asks. This was
		// gui_mac.cpp's own line before the three front ends shared a base
		eng->mu.set_realtime_workgroup(out->realtime_workgroup());
#endif
		return true;
	}

	// Opens the remembered recording device, by name. A device that is not
	// there now keeps its name in the settings, like a MIDI port
	void start_ad()
	{
		if (ain_name.empty() || !ain)
			return;
		const auto names = audio_in::list();
		const int dev = find_device(names, ain_name);
		std::string aerr;
		if (dev >= 0 && ain->start(names[size_t(dev)], aerr))
			std::printf("A/D INPUT: %s（%s）\n", ain->device_name().c_str(),
			            ain->format_line().c_str());
		else
			std::printf(CLI_T("A/D INPUT: none (%s)\n", "A/D INPUT: なし（%s）\n"),
			            dev < 0 ? CLI_T("device not found", "デバイスが見つからない") : aerr.c_str());
		std::fflush(stdout);
		save_settings();
	}

	// Everything after the pump returns, both sides in this order: tell the
	// editor windows, drain the audio thread, all-notes-off the THRU ports,
	// keep the card and the settings, snapshot for the next boot, close up.
	// The boot thread join stays in main (it owns the thread)
	void shutdown()
	{
		pc_shutdown_all(list, pc, fx, shapes, master, sampling, br);
		if (m_ain_lister.joinable())
			m_ain_lister.join();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		// Stop a MIDI file first: its all-notes-off travels out through the
		// audio thread, so stopping the sound first would leave the far-end
		// gear ringing. Then give the thread a breath to flush it through
		if (play.playing()) {
			play.stop();
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
		}
		if (out)
			out->stop();
		if (ain)
			ain->stop();
		// Leaving the THRU ports open with notes still held would leave them
		// stuck on whatever is listening, so all sound off and all notes off
		// go out first
		for (midi_out *thru : { &thru_a, &thru_b }) {
			if (!thru->is_open())
				continue;
			for (int ch = 0; ch < 16; ch++) {
				for (u8 v : { u8(0xb0 | ch), u8(120), u8(0), u8(0xb0 | ch), u8(123), u8(0) })
					thru->send(v);
			}
		}
		join_reboot();
		flush_card();      // the sound has stopped; keep what was on the card
		save_settings();   // the ports, the A/D input and the VOLUME knob
		// The sound has stopped by now. Keep the machine's settings only if
		// it came up
		if (eng) {
			eng->settle_for_save();
			if (eng->state.load() == 1 && !smu2000::nvram::save(eng->mu))
				std::fprintf(stderr, CLI_T("Could not save the settings: %s\n", "設定を残せなかった: %s\n"),
				             smu2000::nvram::path(eng->mu).c_str());
			// Snapshot for the settings just saved, or the next boot after
			// touching them is the slow one. The window is gone, so the
			// second it takes holds nobody up; old snapshots are pruned here
			if (eng->state.load() == 1) {
				if (smu2000::bootcache::refresh(eng->mu))
					std::printf(CLI_T("Made the boot copy for the next start\n", "次の起動ぶんの写しを作った\n"));
				smu2000::bootcache::prune();
			}
		}
		play.stop();
		for (int p = 0; p < 4; p++)
			midi[p].close();
		thru_a.close();
		thru_b.close();
		mu_out.close();
		edit_out.close();
	}

	// How the run ended up sounding, when it sounded at all. drops is what
	// the backend counted (WASAPI late(), CoreAudio starved())
	void print_exit_stats(u64 drops)
	{
		if (out && out->produced()) {
			std::printf(CLI_T("CPU %.1f%%, worst single block %.2f ms, late %llu times\n", "CPU %.1f%%、1 回の最悪 %.2f ms、間に合わなかった %llu 回\n"),
			            out->cpu_percent(), out->worst_ms(),
			            (unsigned long long)drops);
			print_audio_details();
		}
	}

	// Backend details for the exit line (device format on WASAPI,
	// hog mode on CoreAudio). Nothing shared to say: each side says its own
	virtual void print_audio_details() = 0;

	// ---- the program itself (both mains end here)

	// Everything after the machine is loaded: put the window up, boot on a
	// thread, pump events, close down. The mains keep the argument parsing
	// and the object construction; what still differs is only the shell
	// below (window creation, the event pump, the audio say-lines)
	int run(tool_args &a, const engine_options &eo, const output_options &oo,
	        const window_options &wo)
	{
		keep_settings = a.nomidi;
		audio_latency = a.latency;
		audio_exclusive = oo.exclusive;
		setup_for_window(a, wo, oo.factory);

		// The remembered ports open on this thread, while the machine boots
		// beside it: the audio device is the only thing that needs the
		// firmware. --midi and friends win over what was remembered
		open_remembered_ports(a, oo);

		// Give the panel something to read before the boot thread says
		// anything, so the window comes up showing the boot message rather
		// than a blank LCD
		eng->publish();

		if (!open_main_window("S-MU2000", a.win_w, a.win_h)) {
			std::fprintf(stderr, "%s\n", UI_TEXT(dlg_window_fail, "Cannot open the window"));
			return 1;
		}

		make_audio();

		// Boot on a separate thread, and start the audio once it is done
		std::thread boot_thread([&] {
			if (!eng->boot()) {
				eng->state.store(2);
				eng->publish();
				return;
			}
			// After boot, as always: starting needs the firmware
			apply_native_engine(*eng, eo);
			eng->state.store(1);
			eng->publish();

			if (!start_audio(a.latency, oo.exclusive)) {
				audio_failed = true;
				audio_ready.store(true); // PHONES can recover by picking another output
				return;
			}
			say_audio_opened(oo.exclusive);
			// A/D INPUT: open the recording device that was picked last time
			start_ad();
			// With --play, start streaming as soon as it begins to sound
			if (!a.play_path.empty())
				play_song(a.play_path);
			say_audio_running();
			std::fflush(stdout);
			audio_ready.store(true); // all startup writes finish before UI switching
		});

		// --editor and friends, alongside the panel
		open_startup_windows(wo);

		pump_window("S-MU2000", a.win_w, a.win_h);

		if (boot_thread.joinable())
			boot_thread.join();
		shutdown();
		print_exit_stats(audio_drops());
		return 0;
	}

	// ---- the window-system shell (thin shells implement these)

	// Creates the main window and shows it. False when it could not be made
	virtual bool open_main_window(const char *title, int w, int h) = 0;
	// Pumps events until the window closes. Blocks. The Mac's make and pump
	// are one call (run_window), so the arguments ride along unused there
	virtual void pump_window(const char *title, int w, int h) = 0;
	// Creates the audio devices and points out/ain at them. Opening the
	// device itself waits for the firmware (start_audio, on the boot thread)
	virtual void make_audio() = 0;
	// Said once the device is up. exclusive is whether hog mode was asked for
	virtual void say_audio_opened(bool exclusive) = 0;
	// Said once it is actually sounding (latency, thread class)
	virtual void say_audio_running() = 0;
	// What the backend counted (WASAPI late(), CoreAudio starved())
	virtual u64 audio_drops() = 0;

	// ---- per-platform acts (thin shells implement these)

	// Something in a menu failed. Windows remembers it for the end of the
	// command; macOS tells the user straight away
	virtual void menu_error(const std::string &text) = 0;
	// Something worth saying that is not a failure (fresh card needs Format)
	virtual void menu_note(const std::string &text) = 0;
	// File dialogs ("" means cancelled)
	virtual std::string ask_card_open_path() = 0;
	virtual std::string ask_card_save_path() = 0;
	virtual std::string ask_midi_file_path() = 0;
	// Factory reset confirmation (false keeps everything)
	virtual bool confirm_factory_reset() = 0;

protected:
	// One MIDI OUT opener for the three (A/B/MU): same calls, different
	// slots and labels. Failures print always and reach menu_error for
	// menu picks (never boot-time, which passes keep)
	bool open_out(midi_out &port, int &dev_slot, std::string &name_slot,
	              std::string &keep_slot, const char *label, int dev, bool keep)
	{
		if (!keep)
			keep_slot.clear();
		std::string err;
		if (!port.open(dev, err)) {
			std::fprintf(stderr, "%s: %s\n", label, err.c_str());
			if (!keep)
				menu_error(err);
			port.open(-1, err);
			dev = -1;
		}
		dev_slot  = port.is_open() ? dev : -1;
		name_slot = port.device_name();
		save_settings();
		return dev >= 0;
	}

	u64 last_flush = 0;                // card file last written back
	u64 last_drop_report = 0;          // MIDI drops last said out loud
	bool pressed = false;            // a panel press is in flight (drag/up)
};

// --shot without a ROM or without booting: draw the empty screen. Both
// mains do this before anything else is built
inline int empty_shot(bridge &b, const tool_args &a, const window_options &w)
{
	snapshot s;
	std::snprintf(s.message, sizeof(s.message), "S-MU2000");
	b.publish(s);
	return write_shot(a.shot_path, a.win_w, a.win_h, b, a.grid,
	                  w.lcd_only, a.layout_path);
}

} // namespace ui

#endif // S_MU2000_UI_APP_H

// license:BSD-3-Clause
//
// Linux side of the window that hosts an ImGui view (imgui_view) for gui.
// One SDL3 window + SDL_gpu swapchain per view, driven by the Dear ImGui SDL3
// backends (vendored in third_party/imgui/backends).
//
// ui::pc_window.cpp is the Windows side (Win32 + Direct3D 11) and
// ui/pc_window_mac.* the macOS one (AppKit + Metal); this is the same class
// with the same shape. The contents (ui::pc_editor / ui::overview /
// ui::fx_editor / ui::part_shapes / ui::master_editor) and ui::xgui are
// **the very same files** as elsewhere. Only the window differs.
//
// Closing only hides the window, so reopening comes back in the same state.
// **Each window owns an ImGui context**, so the editor and the overview can
// be open at the same time. SDL events are global, so the main loop offers
// every event to route_event(), which feeds the owning window's context.

#ifndef S_MU2000_UI_PC_WINDOW_LINUX_H
#define S_MU2000_UI_PC_WINDOW_LINUX_H

#pragma once

#include "xg_ui.h"
#include "ui/imgui_shell_sdl.h"   // imshell::sdl_state

#include <SDL3/SDL.h>

#include <memory>
#include <string>

struct ImGuiContext;

namespace ui {

class pc_window
{
public:
	explicit pc_window(std::unique_ptr<imgui_view> view) : m_view(std::move(view)) {}
	~pc_window();

	// Show it. Creates the window, renderer and ImGui context on first call.
	// On failure err says why
	bool show(std::string &err);
	// Hide it without destroying anything, so showing it again comes back
	// in the same state
	void hide();
	// Tear the SDL resources down (before SDL_Quit). Showing again rebuilds
	void close();
	bool visible() const;
	// gui is ending. Tell the contents it closed (unmute the overview, etc.)
	void shutdown(bridge &br);

	// From gui's timer. Does nothing while the window is not visible
	void frame(xg::model &m, const xg_snapshot &ram, bridge &br);

	// Who to call when a file is dropped on the window (gui plays a MIDI
	// file). Decide before the window exists; without it drops are ignored
	static void set_drop_handler(void (*fn)(const std::string &path));

	// Offer one SDL event to every Linux pc window. True when consumed
	// (matched a window, including a file drop onto one)
	static bool route_event(const SDL_Event &ev);

private:
	bool create(std::string &err);
	void destroy();

	std::unique_ptr<imgui_view> m_view;
	SDL_Window   *m_win = nullptr;
	// The window's share of the ImGui/SDL_gpu bring-up, same shape the main
	// window's is (ui/imgui_shell_sdl.h). Each editor window has its own
	// ImGui context, and its own swapchain on the one shared device.
	imshell::sdl_state m_im{};
	ImGuiContext *m_imgui = nullptr;
	Uint32        m_id = 0;
	bool          m_was_visible = false;   // was visible in the previous frame (to catch hiding)
};

} // namespace ui

#endif // S_MU2000_UI_PC_WINDOW_LINUX_H

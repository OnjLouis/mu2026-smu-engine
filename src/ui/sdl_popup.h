// license:BSD-3-Clause
//
// Popup menus and native dialogs for the SDL3 front ends (gui and the
// plug-in editor). SDL has no menus, so popups draw over the live panel
// through Dear ImGui, modal until chosen or cancelled. Two menu
// levels at most (a category opens a second flat list), mirroring the
// NSMenu/HMENU structure on the other platforms.
//
// File panels are asynchronous in SDL3: the result arrives in a callback that
// may run on another thread, so a modal event pump waits for it. Esc abandons
// the wait (a late callback lands in shared state, harmlessly). Message boxes
// block and answer directly.

#ifndef S_MU2000_UI_SDL_POPUP_H
#define S_MU2000_UI_SDL_POPUP_H

#pragma once

#include <SDL3/SDL.h>

#include "ui/draw_imgui.h"
#include "ui/imgui_shell_sdl.h"     // imshell::sdl_state, the SDL_gpu window

#include "imgui.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace ui {
namespace sdl_popup {

struct item {
	std::string label;
	int  id = -1;          // final choice; SUB_* with submenu == true
	bool checked = false;
	bool enabled = true;
	bool separator = false;
	bool submenu = false;
	int  sub = -1;
};

// Modal menu over the live panel: returns the chosen id, a submenu marker
// via sub_chosen, or -1. behind() paints the live panel into the current
// frame's background list; this draws the dim + box + rows on top and
// presents. quit is set when the window closes underneath.
int run(SDL_Window *win, imshell::sdl_state &st, ImGuiContext *ctx,
        const im::fonts &fonts, int ww, int wh,
        std::function<void(ImDrawList *)> behind, std::atomic<bool> &quit,
        const std::vector<item> &items, int x, int y, int &sub_chosen);

struct file_filter {
	const char *name;      // e.g. "MIDI files"
	const char *pattern;   // e.g. "mid;midi"
};

std::string open_file(SDL_Window *win, std::atomic<bool> &quit,
                      const file_filter *filters, int nfilters,
                      const std::string &defloc);
std::string save_file(SDL_Window *win, std::atomic<bool> &quit,
                      const file_filter *filters, int nfilters,
                      const std::string &defloc);

int message_box(SDL_Window *win, const char *title, const char *text,
                unsigned flags,
                std::initializer_list<SDL_MessageBoxButtonData> buttons);
void alert(SDL_Window *win, const char *title, const std::string &text);
bool confirm(SDL_Window *win, const char *title, const std::string &text,
             const char *ok_label);

} // namespace sdl_popup
} // namespace ui

#endif // S_MU2000_UI_SDL_POPUP_H

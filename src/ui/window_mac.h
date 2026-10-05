// license:BSD-3-Clause
//
// The macOS window system's services, shared with the Windows (ui/window_win.h)
// and SDL (ui/window_sdl.h) twins.
//
// Nothing in this header mentions AppKit. window_mac.mm implements it. It is
// one of the project's Objective-C++ files, not the only one: shot_mac.mm,
// pc_window_mac.mm and the plug-in views are too.

#ifndef S_MU2000_UI_WINDOW_MAC_H
#define S_MU2000_UI_WINDOW_MAC_H

#pragma once

#include <string>

namespace ui {

class app;

// Function keys other than F2..F5 arrive as MAC_KEY_FUNCTION_BASE + the
// Carbon key code, because the app maps keys to panel buttons and has no
// business knowing about NSEvent (F2..F5 translate to the shared KEY_F2..F5
// of ui/menu.h right in the window)
constexpr int MAC_KEY_FUNCTION_BASE = 0x10000;

// Runs the open panel and returns the chosen path, or "" if it was cancelled.
// A file panel belongs to the window system, so the app asks for it by name
// rather than reaching for AppKit itself
std::string open_midi_file_panel();

// The same for a file of some other kind. ext is a filename extension (no dot)
// and is only a hint: the panel lets anything through, because the files the
// machine writes are not registered with the system
std::string open_file_panel(const char *title, const char *ext);

// Asks where to save a new file of some other kind. "" if it was cancelled
std::string save_file_panel(const char *title, const char *default_name, const char *ext);

// Asks a yes/no question and returns true only when the user accepts. Used
// before the settings are thrown away, so the buttons are ordered for the
// answer that changes nothing: Cancel is the default, and Return picks it
bool confirm_modal(const char *title, const char *message, const char *ok_label);

// Tells the user something they only have to acknowledge. confirm_modal's
// counterpart for a message with no choice in it (inserting a blank
// SmartMedia, for instance)
void alert_modal(const char *title, const char *message);

// Makes the window and pumps events until it closes. Blocks
void run_window(app &app, const char *title, int w, int h);

} // namespace ui

#endif // S_MU2000_UI_WINDOW_MAC_H

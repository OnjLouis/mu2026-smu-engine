// license:BSD-3-Clause
//
// The five PC windows every front end hosts (overview, editor, insertion
// settings, part voice, master), driven the same way everywhere.
//
// gui.exe, gui (macOS), and the VST3/CLAP/AU plug-ins on both platforms each
// repeated the same dozen lines: one frame() per window per tick, then the
// double-click follow-ups from the overview. Only opening a window differs
// per host (show() takes an owner handle on Windows, none on macOS, and each
// side reports failure its own way), so the caller passes that in and this
// stays platform-neutral: it never includes either pc_window.h, and the
// window type is duck-typed. Nothing else about the hosts moves here --
// menus, function keys, file drops, shutdown order and the card slot stay
// where they are, which is what keeps the standalone extras intact.

#ifndef S_MU2000_UI_PC_HOST_H
#define S_MU2000_UI_PC_HOST_H

#pragma once

#include "xg_ui.h"

#include "imgui.h"

namespace ui {

// One tick for all five windows. Invisible windows cost nothing (each
// frame() returns early while hidden). The overview asks for the insertion,
// part-voice or master window by double-click; open() shows it, however the
// host shows windows
//
// Each of these windows has an ImGui context of its own, and both frame() and
// open() leave that context current -- frame() calls SetCurrentContext, and
// open() creates it, which makes it current too. That is fine on its own, but
// this runs from *inside* the panel's frame: the panel is drawn by ImGui, and
// paint_main() calls us between the window's NewFrame() and Render(). Handing
// the panel's Render() to an editor's context strands the panel's own frame --
// nothing ever ends it -- and the next NewFrame() then asserted "Forgot to
// call Render() or EndFrame() at the end of the previous frame?", the moment
// any PC window was opened. So the panel's context goes back afterwards, and
// the editors keep theirs to themselves.
template <typename Window, typename Open>
inline void pc_frame_all(Window &list, Window &editor, Window &fx, Window &shapes, Window &master,
                         Window &sampling, xg::model &m, const xg_snapshot &ram, bridge &br, Open open)
{
	ImGuiContext *const panel_ctx = ImGui::GetCurrentContext();
	list.frame(m, ram, br);
	editor.frame(m, ram, br);
	fx.frame(m, ram, br);
	shapes.frame(m, ram, br);
	master.frame(m, ram, br);
	sampling.frame(m, ram, br);
	// A double-click on an insertion row in the overview asks for the
	// settings window; on a VIB/FILTER/EG/EQ cell for the part voice window;
	// on the MASTER name or MASTER EQ cell for the master window
	if (xgui::take_fx_request())
		open(fx);
	if (xgui::take_part_request())
		open(shapes);
	if (xgui::take_master_request())
		open(master);
	ImGui::SetCurrentContext(panel_ctx);
}

// What a front end owes its windows on the way out (the overview unmutes,
// the editor releases held buttons, and so on)
template <typename Window>
inline void pc_shutdown_all(Window &list, Window &editor, Window &fx, Window &shapes, Window &master,
                            Window &sampling, bridge &br)
{
	// Same reason as pc_frame_all: destroy() switches the context on its way
	// out, and the panel's frame is open around this on the way down too
	ImGuiContext *const panel_ctx = ImGui::GetCurrentContext();
	list.shutdown(br);
	editor.shutdown(br);
	fx.shutdown(br);
	shapes.shutdown(br);
	master.shutdown(br);
	sampling.shutdown(br);
	ImGui::SetCurrentContext(panel_ctx);
}

} // namespace ui

#endif // S_MU2000_UI_PC_HOST_H

// license:BSD-3-Clause
//
// The window hosting an ImGui view (imgui_view) for gui: Win32 + Direct3D 11
// on Windows (pc_window.cpp), AppKit + Metal on macOS (pc_window_mac.mm).
// The contents (ui::pc_editor / ui::overview / ui::fx_editor) are the very
// same files on both; only the window differs.
//
// The window draws in frame(), called from gui's timer, 30 frames a second.
// Closing only hides it, so reopening comes back in the same state. Each
// window owns an ImGui context, so the editor and the overview can be open
// at the same time.
//
// Platform differences, collected here instead of a second header:
//   * show() takes the registering module on Windows (the exe or, for the
//     plug-in, its own DLL); macOS has no such handle
//   * hide() exists only on macOS: the plug-in calls it when its own view
//     closes
//   * imgui_impl_osx is not used. It installs one event monitor for the whole
//     process and feeds whatever context is current, so with a context per
//     window the input would arrive in another window. The view hands the
//     events to io itself, the way the Windows side does in its wnd_proc

#ifndef S_MU2000_UI_PC_WINDOW_H
#define S_MU2000_UI_PC_WINDOW_H

#pragma once

#include "xg_ui.h"

#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
#endif
struct ImGuiContext;

namespace ui {

class pc_window
{
public:
	explicit pc_window(std::unique_ptr<imgui_view> view) : m_view(std::move(view)) {}
	~pc_window();

	// Show it. Creates the window and the device on the first call.
	// On failure err says why
#ifdef _WIN32
	bool show(HINSTANCE inst, std::string &err);
#else
	bool show(std::string &err);
	// Hide it without destroying anything, so showing it again comes back
	// in the same state. The plug-in calls this when its own view closes
	void hide();
#endif
	bool visible() const;
	// gui is ending. Tell the contents it closed (unmute the overview, etc.)
	void shutdown(bridge &br);

	// From gui's timer. Does nothing while the window is not visible
	void frame(xg::model &m, const xg_snapshot &ram, bridge &br);

	// Who to call when a file is dropped on the window (gui plays a MIDI
	// file, as UTF-8). Decide before the window exists; without it drops
	// are ignored
	static void set_drop_handler(void (*fn)(const std::string &path)) { s_drop = fn; }
	static inline void (*s_drop)(const std::string &) = nullptr;

private:
#ifdef _WIN32
	bool create(HINSTANCE inst, std::string &err);
	bool create_device(std::string &err);
	void make_target();
	void drop_target();
	void destroy();
	static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

	std::unique_ptr<imgui_view> m_view;
	HWND m_hwnd = nullptr;
	ID3D11Device           *m_dev = nullptr;
	ID3D11DeviceContext    *m_ctx = nullptr;
	IDXGISwapChain         *m_swap = nullptr;
	ID3D11RenderTargetView *m_rtv = nullptr;
	ImGuiContext           *m_imgui = nullptr;
	UINT m_resize_w = 0, m_resize_h = 0;     // WM_SIZE で受けて、次に描く前に直す
	bool m_was_visible = false;              // 前のコマで見えていたか（隠れた瞬間を知る）
	// ファイルの窓の頼み（xgui::file_ask）と書き出す中身。開くのは描画の外（WM_APP_FILE_DIALOG）。
	// frame は gui のパネルのコマの途中で呼ばれるので、そこで窓を開くと、窓が回している間に
	// パネルのタイマーが来てコマが入れ子になり、ImGui が止まる
	int m_file_ask = 0;
	std::vector<u8> m_file_bytes;
#else
	bool create(std::string &err);
	void destroy();

	std::unique_ptr<imgui_view> m_view;
	void *m_ns = nullptr;                    // the window/view/Metal plumbing, inside pc_window_mac.mm
	ImGuiContext *m_imgui = nullptr;
	bool m_was_visible = false;              // was visible in the previous frame (to catch hiding)
#endif
};

} // namespace ui

#endif // S_MU2000_UI_PC_WINDOW_H

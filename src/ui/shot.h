// license:BSD-3-Clause
//
// Write just the panel picture, with no window. Used to check the looks.
//
// Headless through the shared renderer plumbing: a WARP device on Windows,
// Metal on macOS, SDL_gpu on Linux. One copy keeps --shot the same picture
// on all three.
//
// SDL_gpu has no SDL_RenderReadPixels, and a swapchain texture cannot be
// read (it is write-only, and not ours to keep), so the Linux branch draws
// into an offscreen colour texture it owns and reads that back with
// SDL_DownloadFromGPUTexture -- see imshell::sdl_read_pixels.

#ifndef S_MU2000_UI_SHOT_H
#define S_MU2000_UI_SHOT_H

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "ui/bridge.h"
#include "ui/panel.h"
#include "ui/png.h"
#include "ui/snapshot.h"
#include "ui/toolbar.h"
#include "ui/imgui_shell.h"

#include "imgui.h"

#ifdef _WIN32
#include <d3d11.h>
#elif !defined(__APPLE__)
#include "ui/imgui_shell_sdl.h"
#include <SDL3/SDL.h>
#endif

namespace ui {

#ifdef __APPLE__
// The macOS picture is Metal, and it lives in shot_mac.mm. This header is also
// read by plain C++ translation units (gui_mac.cpp runs --shot, and app.h pulls
// this in), which cannot hold an Objective-C renderer at all; and macOS must
// not need SDL3, which is a Linux-only dependency it has no business having.
// The declaration takes plain types for the same reason.
int write_shot_metal(int w, int h, std::vector<u8> &rgba, bool grid, bool lcd_only,
                     const std::string &layout_path, bridge &br);
#endif

namespace shot_detail {

// The panel and the button bar, held by the caller rather than made here.
// The draw commands point at the panel's picture textures, so both have to
// still be alive when ImGui::Render() reads them -- a panel that goes out of
// scope before Render takes its textures with it, and Render is what uploads
// them. Building the panel inside shot_frame() and letting it die at the end of
// the function gets that wrong by one line, and it tends not to crash: the
// freed ImTextureData still holds a usable TexID. Not a thing to rely on.
struct rig {
	panel   p;
	toolbar bar;
};

// The empty machine's front page into the current frame's draw list.
inline void shot_frame(rig &r, ImDrawList *dl, const im::fonts &f, int w, int h,
                       bool grid, bool lcd_only, const std::string &layout_path,
                       bridge &br)
{
	panel &p = r.p;
	std::string lerr;
	if (!layout_path.empty() && !p.lay().load(layout_path, lerr))
		std::fprintf(stderr, "配置: %s を開けない\n", layout_path.c_str());
	if (!lerr.empty())
		std::fprintf(stderr, "%s", lerr.c_str());
	p.set_lcd_only(lcd_only);
	if (!lcd_only) {
		r.bar.set_items(window_bar_items());
		p.set_top_inset(toolbar::HEIGHT);
	}
	p.resize(w, h);
	p.set_grid(grid);
	snapshot s;
	br.read(s);
	p.set_volume(0.8);
	p.paint(dl, s, 0, "");
	if (!lcd_only)
		r.bar.paint(dl, w, f.label, f.bar_px);
}

} // namespace shot_detail

// Renders the panel (with the window button bar, unless lcd_only) to a PNG.
// Same picture the window shows.
inline int write_shot(const std::string &path, int w, int h, bridge &br,
                      bool grid, bool lcd_only, const std::string &layout_path)
{
	using namespace shot_detail;

	std::vector<u8> bgra(size_t(w) * size_t(h) * 4);
	bool drew = false;

#ifdef _WIN32
	// WARP: pixels with no window and no GPU.
	imshell::dx11_state st{};
	if (imshell::dx11_start(st, nullptr)) {
		// Outlives dx11_stop() below, not just Render(): the panel's textures
		// have to still be registered when the backend tears down, because
		// ImGui_ImplDX11_InvalidateDeviceObjects() walks
		// ImGui::GetPlatformIO().Textures -- a list built at end of frame that
		// still points at them -- and dereferences each entry. A rig scoped
		// inside the block below dies first, its ImTextureData are freed, and
		// that walk reads freed memory: the access violation this used to be.
		shot_detail::rig rig;

		ID3D11Texture2D *tex = nullptr, *stage = nullptr;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = UINT(w);
		td.Height = UINT(h);
		td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		D3D11_TEXTURE2D_DESC sd = td;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (SUCCEEDED(st.dev->CreateTexture2D(&td, nullptr, &tex)) &&
		    SUCCEEDED(st.dev->CreateTexture2D(&sd, nullptr, &stage)) &&
		    SUCCEEDED(st.dev->CreateRenderTargetView(tex, nullptr, &st.rtv))) {
			imshell::dx11_paint(st, w, h, [&](ImDrawList *dl) {
				shot_frame(rig, dl, st.fonts, w, h, grid, lcd_only, layout_path, br);
			});
			st.ctx->CopyResource(stage, tex);
			D3D11_MAPPED_SUBRESOURCE map{};
			if (SUCCEEDED(st.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &map))) {
				for (int y = 0; y < h; y++) {
					const u8 *src = static_cast<const u8 *>(map.pData) + size_t(y) * map.RowPitch;
					u8 *dst = bgra.data() + size_t(y) * size_t(w) * 4;
					for (int x = 0; x < w; x++) {   // BGRA -> RGBA
						dst[x * 4 + 0] = src[x * 4 + 2];
						dst[x * 4 + 1] = src[x * 4 + 1];
						dst[x * 4 + 2] = src[x * 4 + 0];
						dst[x * 4 + 3] = src[x * 4 + 3];
					}
				}
				st.ctx->Unmap(stage, 0);
				drew = true;
			}
		}
		if (stage) stage->Release();
		if (tex) tex->Release();
		imshell::dx11_stop(st);
	}
#elif defined(__APPLE__)
	drew = write_shot_metal(w, h, bgra, grid, lcd_only, layout_path, br) != 0;
#else
	// Linux, through SDL_gpu. The picture is drawn into an offscreen colour
	// texture and read back with SDL_DownloadFromGPUTexture -- see
	// imshell::sdl_read_pixels, which is the stand-in for the
	// SDL_RenderReadPixels this platform used to have.
	if (SDL_Init(SDL_INIT_VIDEO) && imshell::gpu_device()) {
		// A window is still needed: imgui_impl_sdl3 is a platform backend and
		// wants one to take input and display size from. Hidden, and never
		// shown -- everything after this is offscreen.
		SDL_Window *win = SDL_CreateWindow("shot", w, h, SDL_WINDOW_HIDDEN);
		imshell::sdl_state st{};
		std::vector<std::uint8_t> got;
		if (win && imshell::sdl_start(st, win)) {
			// outlives Render(), which happens inside sdl_read_pixels
			shot_detail::rig rig;
			imshell::sdl_begin(st);
			shot_frame(rig, ImGui::GetBackgroundDrawList(), st.fonts, w, h,
			           grid, lcd_only, layout_path, br);
			drew = imshell::sdl_read_pixels(st, w, h, got);
			// The readback is in the swapchain's own format. write_png() wants
			// BGRA, so a swapchain that hands back RGBA has to be swapped here
			// (the WARP path above has to as well: a DIB is not BGRA either).
			if (drew && st.color_format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) {
				for (size_t i = 0, n = got.size(); i < n; i += 4) {
					const std::uint8_t r = got[i + 0];
					got[i + 0] = got[i + 2];
					got[i + 2] = r;
				}
			}
			bgra = std::move(got);
			if (!drew)
				std::fprintf(stderr, "絵を GPU から読めない: %s\n", SDL_GetError());
			imshell::sdl_stop(st);
		} else {
			std::fprintf(stderr, "画面を作れない: %s\n", SDL_GetError());
		}
		if (win) SDL_DestroyWindow(win);
		imshell::gpu_device_release();
		SDL_Quit();
	} else {
		std::fprintf(stderr, "画面を作れない: %s\n", SDL_GetError());
	}
#endif

	bool ok = false;
	if (drew)
		ok = write_png(path, bgra.data(), w, h, w * 4);
	else
		std::fprintf(stderr, "画面を作れない\n");

	std::printf(ok ? "書き出した: %s（%d×%d）\n" : "書き出せない: %s\n",
	            path.c_str(), w, h);
	return ok ? 0 : 1;
}

} // namespace ui

#endif // S_MU2000_UI_SHOT_H

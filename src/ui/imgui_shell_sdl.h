// license:BSD-3-Clause
//
// The SDL family of the shared ImGui plumbing (ui/imgui_shell.h): main
// window, popup menu and headless shot on Linux, headless shot on macOS.
// Split out so macOS plug-ins do not gain an SDL dependency through the
// shared header.
//
// Linux draws through SDL_gpu (ui/imgui_shell_sdl.h used to go through
// SDL_Renderer). Dear ImGui upstream prefers the GPU API where it is
// available -- docs/BACKENDS.md lists SDL_Renderer3 as "Prefer using
// SDL_GPU!" -- and it is the only path that keeps the panel's per-frame
// LCD texture upload off the CPU blitter.
//
// What the swap costs: there is no SDL_RenderReadPixels, so --shot has no
// pixel path here and is compiled out (shot.h). Everything else -- the main
// window, the popup menu and the five PC editor windows -- goes through
// gpu_state below.

#ifndef S_MU2000_UI_IMGUI_SHELL_SDL_H
#define S_MU2000_UI_IMGUI_SHELL_SDL_H

#pragma once

#include "ui/imgui_shell.h"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <vector>


namespace ui {
namespace imshell {

// One window's worth of GPU state: the device is shared by every window in
// the process (see gpu_device()), each window claims it and gets its own
// swapchain.
struct sdl_state {
	ImGuiContext *ctx = nullptr;
	im::fonts fonts{};
	// The colour format the renderer backend's pipeline was built for. Vulkan
	// requires a pipeline's colour format to match the attachment it draws
	// into, so anything else this state renders to has to use this too --
	// sdl_read_pixels() renders to a texture it makes, and getting it wrong
	// gives a black frame rather than an error.
	SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
};

// The process-wide SDL_GPUDevice, created on the first call and shared by
// the main window and the PC editor windows. SDL_gpu wants one device per
// process and many claimed windows, not a device per window: sharing is
// also what keeps five editor windows from costing five devices.
//
// Returns null when no driver is usable, which is the case in CI (dummy
// video driver) and on machines with no Vulkan.
SDL_GPUDevice *gpu_device();

// Releases the shared device. Every window must be stopped first.
void gpu_device_release();

// One window: claim it to the device, then bring ImGui up on
// imgui_impl_sdl3 (platform) + imgui_impl_sdlgpu3 (renderer).
inline bool sdl_start(sdl_state &st, SDL_Window *win)
{
	st.ctx = new_context();
	st.fonts = panel_fonts();
	SDL_GPUDevice *dev = gpu_device();
	if (!dev || !SDL_ClaimWindowForGPUDevice(dev, win)) {
		ImGui::DestroyContext(st.ctx);
		st.ctx = nullptr;
		return false;
	}
	// SDR: the panel is authored for sRGB, and the LCD's colours are picked
	// as sRGB triples. HDR composition would shift every one of them.
	SDL_SetGPUSwapchainParameters(dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
	                              SDL_GPU_PRESENTMODE_VSYNC);
	if (!ImGui_ImplSDL3_InitForSDLGPU(win)) {
		SDL_ReleaseWindowFromGPUDevice(dev, win);
		ImGui::DestroyContext(st.ctx);
		st.ctx = nullptr;
		return false;
	}
	ImGui_ImplSDLGPU3_InitInfo info{};
	info.Device = dev;
	info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, win);
	if (info.ColorTargetFormat == SDL_GPU_TEXTUREFORMAT_INVALID
	    || !ImGui_ImplSDLGPU3_Init(&info)) {
		ImGui_ImplSDL3_Shutdown();
		SDL_ReleaseWindowFromGPUDevice(dev, win);
		ImGui::DestroyContext(st.ctx);
		st.ctx = nullptr;
		return false;
	}
	st.color_format = info.ColorTargetFormat;
	return true;
}

inline void sdl_stop(sdl_state &st)
{
	if (!st.ctx)
		return;
	ImGui::SetCurrentContext(st.ctx);
	im::drop_user_textures();      // the panel's textures, see imgui_shell.h
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext(st.ctx);
	st.ctx = nullptr;
	st.fonts = im::fonts{};
}

// Backend NewFrame for the caller's paint. Split from presenting so modal
// loops (the popup menu) can paint menu rows over the panel in one frame.
inline void sdl_begin(ImGuiContext *ctx)
{
	ImGui::SetCurrentContext(ctx);
	ImGui_ImplSDLGPU3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
}

inline void sdl_begin(sdl_state &st) { sdl_begin(st.ctx); }

// Draw the finished frame into one colour target, already inside a command
// buffer. sdl_present() and the --shot readback (sdl_read_pixels) both go
// through here, because the two differ only in what they aim at and what
// they do with the command buffer afterwards.
//
// The order matters and is not the SDL_Renderer shape: SDL_gpu needs the
// vertex and index buffers uploaded into a command buffer *before* the
// render pass that draws with them, which is what
// ImGui_ImplSDLGPU3_PrepareDrawData() does. The backend's own header calls
// that call mandatory.
inline void sdl_draw_into(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target,
                          SDL_FColor clear)
{
	SDL_GPUColorTargetInfo cti{};
	cti.texture = target;
	cti.clear_color = clear;
	cti.load_op = SDL_GPU_LOADOP_CLEAR;
	cti.store_op = SDL_GPU_STOREOP_STORE;

	ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);
	SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &cti, 1, nullptr);
	if (pass) {
		ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
		SDL_EndGPURenderPass(pass);
	}
}

// Render, then run one command buffer against this window's swapchain.
inline void sdl_present(sdl_state &st, SDL_Window *win)
{
	ImGui::Render();

	SDL_GPUDevice *dev = gpu_device();
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmd)
		return;

	// A null texture here is not an error: SDL_gpu hands one back when the
	// window is minimised, and says so. The command buffer must still be
	// submitted (the swapchain texture is presented by the submit), and
	// cancelling is not an option once a texture has been acquired.
	SDL_GPUTexture *target = nullptr;
	Uint32 tw = 0, th = 0;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &target, &tw, &th)
	    || !target) {
		SDL_SubmitGPUCommandBuffer(cmd);
		return;
	}

	sdl_draw_into(cmd, target, SDL_FColor{ 0.0f, 0.0f, 0.0f, 1.0f });
	SDL_SubmitGPUCommandBuffer(cmd);
}

// Render into an offscreen texture of w × h and hand the pixels back, so
// write_png() can have them. std::uint8_t rather than the project's u8: this
// header does not include compat/mamecompat.h, and the type is all it needs
// from it. Returns false if any step fails.
//
// This is the readback --shot needs and SDL_RenderReadPixels used to do.
// A swapchain texture cannot be read (it is write-only, and it is not ours
// to keep), so this draws into a texture we own with COLOR_TARGET, then
// copies that into a download transfer buffer with SDL_DownloadFromGPUTexture
// and maps it.
//
// **The texture must be st's own colour format.** The backend built its
// pipeline for that one format, and a Vulkan pipeline may only draw into an
// attachment of the same format -- a mismatch does not raise an error, it
// draws nothing, and the frame comes back black. Hence the format comes from
// sdl_state rather than being spelled out here.
//
// Bytes come back in the texture's format, so the caller may need to swap
// channels; write_png() wants BGRA, and swapchain formats are usually
// B8G8R8A8, so the usual path needs no swap (the WARP path in shot.h does
// have to swap, because a DIB is not).
inline bool sdl_read_pixels(sdl_state &st, int w, int h,
                            std::vector<std::uint8_t> &out)
{
	SDL_GPUDevice *dev = gpu_device();
	if (!dev || w <= 0 || h <= 0
	    || st.color_format == SDL_GPU_TEXTUREFORMAT_INVALID)
		return false;

	ImGui::Render();

	SDL_GPUTextureCreateInfo tci{};
	tci.type = SDL_GPU_TEXTURETYPE_2D;
	tci.format = st.color_format;
	tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	tci.width = Uint32(w);
	tci.height = Uint32(h);
	tci.layer_count_or_depth = 1;
	tci.num_levels = 1;
	tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
	SDL_GPUTexture *target = SDL_CreateGPUTexture(dev, &tci);
	if (!target)
		return false;

	SDL_GPUTransferBufferCreateInfo tbci{};
	tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	tbci.size = Uint32(size_t(w) * size_t(h) * 4);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
	if (!tb) {
		SDL_ReleaseGPUTexture(dev, target);
		return false;
	}

	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmd) {
		SDL_ReleaseGPUTransferBuffer(dev, tb);
		SDL_ReleaseGPUTexture(dev, target);
		return false;
	}

	// Black, like the swapchain path: the panel covers the window, and a
	// stray gap should read as background rather than as undefined.
	sdl_draw_into(cmd, target, SDL_FColor{ 0.0f, 0.0f, 0.0f, 1.0f });

	SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
	if (copy) {
		SDL_GPUTextureRegion src{};
		src.texture = target;
		src.w = Uint32(w);
		src.h = Uint32(h);
		src.d = 1;
		SDL_GPUTextureTransferInfo dst{};
		dst.transfer_buffer = tb;
		dst.pixels_per_row = Uint32(w);
		dst.rows_per_layer = Uint32(h);
		SDL_DownloadFromGPUTexture(copy, &src, &dst);
		SDL_EndGPUCopyPass(copy);
	}
	// **Wait for the copy before mapping.** Submitting is not finishing:
	// SDL_MapGPUTransferBuffer's own docs say nothing about waiting for the
	// GPU, so mapping straight after the submit races the download. Reading a
	// transfer buffer the GPU has not written yet hands back the zeros it was
	// created with, which is a black PNG and no error anywhere -- so the fence
	// is not optional here.
	SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	if (fence) {
		SDL_WaitForGPUFences(dev, /*wait_all=*/true, &fence, 1);
		SDL_ReleaseGPUFence(dev, fence);
	}

	// Map with cycle=true: the same transfer buffer is reused across shots
	// and the driver is free to be writing it until this returns.
	bool ok = false;
	if (void *mapped = SDL_MapGPUTransferBuffer(dev, tb, /*cycle=*/true)) {
		out.assign(size_t(w) * size_t(h) * 4, 0);
		std::memcpy(out.data(), mapped, out.size());
		SDL_UnmapGPUTransferBuffer(dev, tb);
		ok = true;
	}

	SDL_ReleaseGPUTransferBuffer(dev, tb);
	SDL_ReleaseGPUTexture(dev, target);
	return ok;
}

// --- the shared device -------------------------------------------------
//
// Out of line in window_sdl.cpp: the SDL_gpu headers pull in a lot, and
// only the window code needs the symbols.

} // namespace imshell
} // namespace ui

#endif // S_MU2000_UI_IMGUI_SHELL_SDL_H
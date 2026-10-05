// license:BSD-3-Clause
//
// The macOS --shot, in Metal, with no window and no SDL3.
//
// ui/shot.h picks a renderer per platform: WARP on Windows, the SDL3 software
// renderer on Linux, and this on macOS. SDL3 is a Linux-only dependency, and
// the mac front end has no business needing it -- asking it to is what broke
// the macOS CI build, whose runner has no SDL3 installed.
//
// The picture goes into an ordinary texture rather than a CAMetalLayer, because
// a layer's drawable is framebufferOnly and wants presenting, and a shot wants
// neither: it wants the bytes. So one render pass into a shared-memory texture,
// wait for it, and read it back. The ImGui half is the same frame the window
// runs (NewFrame, paint the background draw list, Render), and the panel comes
// from shot_detail::rig, which outlives Render() because the draw commands
// point at its textures.

// imgui_shell.h only forward-declares the AppKit and Metal classes (it is read
// by plain C++ too), so the real headers have to come first, as they do in
// window_mac.mm.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "ui/shot.h"

#include "ui/imgui_shell.h"

namespace ui {

int write_shot_metal(int w, int h, std::vector<u8> &rgba, bool grid, bool lcd_only,
                     const std::string &layout_path, bridge &br)
{
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	if (!dev) {
		std::fprintf(stderr, "Metal の装置が取れない\n");
		return 0;
	}
	id<MTLCommandQueue> queue = [dev newCommandQueue];

	MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
	td.textureType = MTLTextureType2D;
	// RGBA8Unorm. Metal's BGRA8Unorm would store the same R, G, B, A on Apple
	// platforms, so this is the name that tells the truth about the memory; the
	// byte order write_png wants is sorted out after the read.
	td.pixelFormat = MTLPixelFormatRGBA8Unorm;
	td.width = (NSUInteger)w;
	td.height = (NSUInteger)h;
	td.mipmapLevelCount = 1;
	td.sampleCount = 1;
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	// Shared, not managed: macOS has been unified memory since High Sierra, and
	// this is the mode whose contents we are allowed to read from the CPU.
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> target = [dev newTextureWithDescriptor:td];
	if (!target) {
		std::fprintf(stderr, "書込み先の面を作れない\n");
		return 0;
	}

	ImGuiContext *ctx = imshell::new_context();
	im::fonts fonts = imshell::panel_fonts();
	ImGui_ImplMetal_Init(dev);

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(float(w), float(h));
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = target;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.10, 0.10, 0.11, 1.0);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;

	// Declared before the frame and destroyed after it, on purpose: the panel's
	// picture textures are uploaded during Render, and dropping them first
	// would leave the draw commands pointing at freed memory.
	shot_detail::rig rig;

	ImGui_ImplMetal_NewFrame(pass);
	ImGui::NewFrame();
	shot_detail::shot_frame(rig, ImGui::GetBackgroundDrawList(), fonts, w, h,
	                        grid, lcd_only, layout_path, br);
	ImGui::Render();

	id<MTLCommandBuffer> buf = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [buf renderCommandEncoderWithDescriptor:pass];
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), buf, enc);
	[enc endEncoding];
	[buf commit];
	[buf waitUntilCompleted];   // the bytes are wanted now, not next frame

	// The read-back is in the byte order write_png wants, which is BGRA (hence
	// the parameter's name) -- the same order SDL's ARGB8888 hands over on a
	// little-endian machine. Metal will not do that for us: on Apple platforms
	// BGRA8Unorm and RGBA8Unorm are *both* stored R, G, B, A, the name being
	// the visual channel order rather than the byte order. So ask for RGBA (the
	// name at least then matches what comes out) and swap red and blue on the
	// way past, exactly as the WARP branch has to.
	[target getBytes:rgba.data()
	      bytesPerRow:size_t(w) * 4
	       fromRegion:MTLRegionMake2D(0, 0, w, h)
	      mipmapLevel:0];
	u8 *px = rgba.data();
	for (size_t i = 0, n = size_t(w) * size_t(h); i < n; i++, px += 4) {
		const u8 r = px[0];
		px[0] = px[2];
		px[2] = r;
	}

	im::drop_user_textures();
	ImGui_ImplMetal_Shutdown();
	ImGui::DestroyContext(ctx);
	return 1;
}

} // namespace ui

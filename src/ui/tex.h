// license:BSD-3-Clause
//
// One RGBA image in a Dear ImGui texture. The panel art needs this (ui/svg.h
// draws the photo panel background and the antialiased key-top symbols as
// textures); nothing else in the panel does -- every other shape goes
// straight into an ImDrawList.
//
// The ownership dance is ImGui 1.92's, not ours: an ImTextureData is created
// here, filled, and registered with the current context, and the renderer
// backend uploads it during the next Render(). So nothing in this header
// knows about DX11, Metal or SDL_gpu, which is what lets svg.cpp stay
// platform free.
//
// **α はかけていない値**（straight alpha）が要る。DX11 / Metal / SDL_gpu
// の三つとも SRC_ALPHA 合成で、パネルの絵は半透明の縁を持つので、α を
// かけたまま渡すと縁が濃く出る。
//
// One wrinkle worth knowing: an ImTextureData may only be unregistered while
// the ImGuiContext that owns it is still alive, but a panel outlives its
// window's context (gui.cpp keeps the app in a function-local static, so it
// is destroyed at exit, long after dx11_stop). The registry below remembers
// which context owns which texture; imshell::*_stop() calls
// drop_user_textures() before DestroyContext, and drop_user_textures() also
// empties each owner, so a tex that outlives its window is simply empty.

#ifndef S_MU2000_UI_TEX_H
#define S_MU2000_UI_TEX_H

#pragma once

// RegisterUserTexture / UnregisterUserTexture are still marked EXPERIMENTAL
// and live in the internal header (1.92 exposes no public way to register an
// app-owned texture). The rest of the project already includes it for ImFont
// work, so this is nothing new -- but keep it in this one header rather than
// spreading it further.
#include "imgui_internal.h"

#include <cstdint>
#include <vector>

namespace ui {
namespace im {

class tex;

// Every texture this process has made, with the context that owns it
struct user_tex {
	ImGuiContext *ctx;
	ImTextureData *data;
	tex           *owner;   // emptied when the context takes the texture away
};

inline std::vector<user_tex> &user_textures()
{
	static std::vector<user_tex> all;
	return all;
}

// A texture that has been replaced, but whose graphics object the backend has
// not finished with. ImGui's own font atlas retires textures exactly this way
// (ImFontAtlasTextureAdd sets WantDestroyNextFrame), and the two-frame wait is
// not optional: freeing an ImTextureData while a draw command still points at
// it is what took the Metal window down with "ImDrawCmd is referring to
// ImTextureData that wasn't uploaded to graphics system".
struct retired_tex {
	ImGuiContext *ctx;
	ImTextureData *data;
	ImU64         marked;   // the frame it was retired on
};

inline std::vector<retired_tex> &retired_textures()
{
	static std::vector<retired_tex> all;
	return all;
}

// Called by the window teardown (imgui_shell.h / imgui_shell_sdl.h) just
// before DestroyContext: everything still registered goes away with the
// context, and every owner is emptied so its destructor has nothing to do
// (defined below, after tex is complete)
inline void drop_user_textures();

// Once per frame, before anything is drawn. See tex::retire.
inline void drop_retired_textures();

class tex
{
public:
	tex() = default;
	tex(const tex &) = delete;
	tex &operator=(const tex &) = delete;
	~tex() { release(); }

	// w × h の 0xAARRGGBB（α はかけていない）を転送する。同じ大きさのものが
	// あるなら転送し直さない（毎フレーム作り直さない）
	bool upload(int w, int h, const std::vector<uint32_t> &argb)
	{
		if (w <= 0 || h <= 0 || argb.size() < size_t(w) * size_t(h))
			return false;
		if (m_data && m_w == w && m_h == h)
			return true;
		retire();
		ImGuiContext *ctx = ImGui::GetCurrentContext();
		if (!ctx)
			return false;                 // no context: the picture just does not show
		m_data = IM_NEW(ImTextureData)();
		m_data->Create(ImTextureFormat_RGBA32, w, h);
		m_data->UseColors = true;
		m_w = w;                       // convert() reads these
		m_h = h;
		convert(argb);
		ImGui::RegisterUserTexture(m_data);
		user_textures().push_back({ ctx, m_data, this });
		return true;
	}

	// Same size, new pixels: write over the ones already there and tell the
	// backend to send the whole thing up again. This is what ImFontAtlas does
	// when it repacks (ImTextureDataQueueUpload), and unlike upload() it does
	// not re-register the texture, so it is safe to call every frame. The panel
	// LCD needs exactly this: its size only moves on resize, but its contents
	// change with every frame of playback.
	bool refresh(const std::vector<uint32_t> &argb)
	{
		if (!m_data || m_w <= 0 || m_h <= 0 || argb.size() < size_t(m_w) * size_t(m_h))
			return false;
		convert(argb);
		ImTextureDataQueueUpload(m_data, 0, 0, m_w, m_h);
		return true;
	}

	// The context took the texture away with it (drop_user_textures)
	void forget() { m_data = nullptr; m_w = m_h = 0; }

	// Hand the texture back the way ImGui hands back its own: queue it for
	// destruction and let the backend drop the graphics object when it is
	// done, instead of unregistering and freeing here. ImTextureData says so
	// itself: WantDestroyNextFrame is "may still be used in the current
	// frame". Freeing mid-frame is
	// invisible until a draw command recorded a moment earlier is read at the
	// next Render(), and then it is a use-after-free: the Metal backend
	// asserted on it, reading a Width of 0 and a garbage status out of memory
	// the allocator had already handed to the next picture. It needs only one
	// picture to be rebuilt twice in a frame, and the panel does that on its
	// own: its coordinates are fractional, so the nine nav keys come out 32
	// and 33 pixels tall on different calls, and the texture between them is
	// rebuilt.
	//
	// The registry entry goes now (the owner has moved on), but the
	// ImTextureData stays alive and registered until drop_retired_textures()
	// sees the backend has finished with it.
	void retire()
	{
		ImTextureData *t = m_data;
		forget();
		if (!t)
			return;
		ImGuiContext *ctx = ImGui::GetCurrentContext();
		std::vector<user_tex> &all = user_textures();
		for (size_t i = 0; i < all.size(); i++) {
			if (all[i].data != t)
				continue;
			ctx = all[i].ctx;
			all.erase(all.begin() + long(i));
			break;
		}
		t->WantDestroyNextFrame = true;
		retired_textures().push_back({ ctx, t, ImU64(ImGui::GetFrameCount()) });
	}

	// Teardown and layout reload, where nothing is drawing and there is no
	// frame to be careful about. The re-upload path uses retire() above.
	void release()
	{
		ImTextureData *t = m_data;
		forget();
		if (!t)
			return;
		std::vector<user_tex> &all = user_textures();
		for (size_t i = 0; i < all.size(); i++) {
			if (all[i].data != t)
				continue;
			ImGuiContext *prev = ImGui::GetCurrentContext();
			ImGui::SetCurrentContext(all[i].ctx);
			t->SetStatus(ImTextureStatus_WantDestroy);
			ImGui::UnregisterUserTexture(t);
			ImGui::SetCurrentContext(prev);
			all.erase(all.begin() + long(i));
			break;
		}
		IM_DELETE(t);
	}

	bool valid() const { return m_data != nullptr; }

	int width() const { return m_w; }
	int height() const { return m_h; }

	// For ImGui::Image / ImDrawList::AddImage
	ImTextureRef ref() const { return m_data ? m_data->GetTexRef() : ImTextureRef(); }

private:
	// ImTextureFormat_RGBA32 is four bytes in the order r, g, b, a. Our
	// 0xAARRGGBB is a Windows COLORREF order, which has r and b the other way
	// around -- so this copies through a swap. Without it the panel art comes
	// out with its reds and blues exchanged (the beige face reads blue-grey).
	void convert(const std::vector<uint32_t> &argb)
	{
		unsigned char *out = static_cast<unsigned char *>(m_data->GetPixels());
		const size_t n = size_t(m_w) * size_t(m_h);
		for (size_t i = 0; i < n; i++) {
			const uint32_t v = argb[i];
			out[i * 4 + 0] = (unsigned char)(v >> 16);     // r
			out[i * 4 + 1] = (unsigned char)(v >> 8);      // g
			out[i * 4 + 2] = (unsigned char)v;             // b
			out[i * 4 + 3] = (unsigned char)(v >> 24);     // a
		}
	}

	ImTextureData *m_data = nullptr;
	int m_w = 0, m_h = 0;
};

// Hand back the pictures that were replaced. The backend clears TexID and
// BackendUserData once it has released the graphics object (Metal and DX11
// take an extra frame for in-flight rendering, SDL does not), and that is the
// signal that the ImTextureData belongs to us again -- not Status, which a
// texture that was never uploaded has not reached either. A picture that is
// somehow never reported is forced out after a few frames, where no draw
// command can still name it: the draw list is cleared every frame.
inline void drop_retired_textures()
{
	std::vector<retired_tex> &r = retired_textures();
	const ImU64 now = ImU64(ImGui::GetFrameCount());
	for (size_t i = 0; i < r.size();) {
		ImTextureData *t = r[i].data;
		const bool gone = t->TexID == ImTextureID_Invalid && t->BackendUserData == NULL;
		if (gone || now >= r[i].marked + 8) {
			ImGuiContext *prev = ImGui::GetCurrentContext();
			ImGui::SetCurrentContext(r[i].ctx);
			ImGui::UnregisterUserTexture(t);
			ImGui::SetCurrentContext(prev);
			IM_DELETE(t);
			r.erase(r.begin() + long(i));
		} else
			i++;
	}
}

inline void drop_user_textures()
{
	drop_retired_textures();
	std::vector<user_tex> &all = user_textures();
	for (const user_tex &u : all) {
		ImGuiContext *prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(u.ctx);
		u.data->SetStatus(ImTextureStatus_WantDestroy);
		ImGui::UnregisterUserTexture(u.data);
		ImGui::SetCurrentContext(prev);
		IM_DELETE(u.data);
		if (u.owner)
			u.owner->forget();
	}
	all.clear();
}

} // namespace im
} // namespace ui

#endif // S_MU2000_UI_TEX_H

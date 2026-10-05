// license:BSD-3-Clause
//
// The panel drawing surface on Dear ImGui: the windows paint through the
// same ImGui renderer the PC editor windows use on every platform
// (DX11 on Windows, Metal on macOS, SDL_gpu on Linux).
//
// Panel palette first (the same names and packing panel.txt uses), then the
// primitives one by one, painting into an ImDrawList. What this mapping
// taught:
//
// - COLORREF is 0x00BBGGRR while IM_COL32 wants RGBA: extract the bytes
//   with GetRValue/GetGValue/GetBValue (compat/gdi.h, cross-platform),
//   never bit-cast.
// - The three font slots (label/small/tiny) ride along with their px
//   sizes because ImGui rasterizes per size.
// - DT_WORDBREAK becomes a wrap width; DT_SINGLELINE + CENTER/RIGHT become
//   CalcTextSize + cursor math (the bools below).
// - Arcs and ellipses become sampled polylines (ImGui arcs are circular;
//   the panel dials use axis-aligned ellipses). Fills only do convex
//   outlines; the panel art does not rely on holes.

#ifndef S_MU2000_UI_DRAW_IMGUI_H
#define S_MU2000_UI_DRAW_IMGUI_H

#pragma once

#include "compat/gdi.h"   // COLORREF + GetRValue/GetGValue/GetBValue only
#include "ui/fonts.h"      // im::fonts -- ImGui only, no <windows.h> on Windows

#include "imgui.h"

#include <cmath>

namespace ui {
// ---- 色

inline constexpr COLORREF BODY      = RGB(28, 30, 34);
inline constexpr COLORREF BODY_TOP  = RGB(44, 47, 53);
inline constexpr COLORREF LCD_BACK  = RGB(150, 205, 45);
inline constexpr COLORREF LCD_GHOST = RGB(140, 194, 44);   // 消えている点。実物もうっすら見える
inline constexpr COLORREF LCD_DOT   = RGB(18, 22, 14);
inline constexpr COLORREF LED_ON    = RGB(178, 255, 51);
inline constexpr COLORREF BTN_FACE  = RGB(58, 62, 68);
inline constexpr COLORREF BTN_EDGE  = RGB(92, 97, 104);
inline constexpr COLORREF BTN_DOWN  = RGB(126, 170, 70);
inline constexpr COLORREF TEXT      = RGB(226, 229, 233);
inline constexpr COLORREF TEXT_DIM  = RGB(150, 155, 162);
inline constexpr COLORREF ACCENT    = RGB(126, 200, 90);

namespace im {



inline ImU32 col(COLORREF c, unsigned char a = 255)
{
	return IM_COL32(GetRValue(c), GetGValue(c), GetBValue(c), a);
}

// The panel's font slots are ui::im::fonts, in ui/fonts.h: six ImFont pointers
// and their pixel sizes, and nothing else. px rides along with each one
// because ImGui rasterizes per size, and the panel re-rasterizes its own set on
// every resize (panel.cpp) -- the LCD lettering is 4-9 px and unreadable when
// it is one 16 px font scaled down. The window's own pieces (the button strip,
// the popups) use the fixed 16 px set from imgui_shell.h instead. The struct
// lives apart so that vst3/view.h can name it without this header's
// compat/gdi.h, and with it <windows.h> on Windows.

// Cap height in px, taken from the font's own baked metrics ('A' top to 'A'
// bottom, unscaled, so it is scaled here). The panel centers its legends on
// the *visible* cap height, not on the line box: an ImGui text position is the
// top of the line, and the line carries the whole ascent and descent, so
// centering the line box pushes an all-caps label down. That is what
// DT_VCENTER did under GDI too, and why panel.cpp stops using it wherever a
// label is all caps.
inline float cap_height(ImFont *font, float px)
{
	if (!font)
		return px * 0.7f;                    // nothing measured: a sane guess
	ImFontBaked *baked = font->GetFontBaked(px);
	if (!baked || baked->Size <= 0.0f)
		return px * 0.7f;
	// The real cap height, off the H. Ascent covers kanji and the line gap,
	// which sits every legend too low -- SOLO fell clean below its key.
	if (const ImFontGlyph *h = baked->FindGlyphNoFallback('H'))
		if (h->Y1 > h->Y0)
			return (h->Y1 - h->Y0) * (px / baked->Size);
	return px * 0.7f;
}

// The line box: what GDI's DT_VCENTER centers on, and what AddText measures
// a text position against
inline float font_line_height(ImFont *font, float px)
{
	if (!font)
		return px;
	ImFontBaked *baked = font->GetFontBaked(px);
	if (!baked || baked->Size <= 0.0f)
		return px;
	return (baked->Ascent - baked->Descent) * (px / baked->Size);
}

// How far below the top of the line the cap actually starts
inline float cap_top_offset(ImFont *font, float px)
{
	if (!font)
		return 0.0f;
	ImFontBaked *baked = font->GetFontBaked(px);
	if (!baked || baked->Size <= 0.0f)
		return 0.0f;
	if (const ImFontGlyph *h = baked->FindGlyphNoFallback('H'))
		return h->Y0 * (px / baked->Size);
	return 0.0f;
}

inline ImVec2 pos_of(const RECT &r) { return ImVec2(float(r.left), float(r.top)); }
inline ImVec2 size_of(const RECT &r)
{
	return ImVec2(float(r.right - r.left), float(r.bottom - r.top));
}

inline void fill(ImDrawList *dl, ImVec2 pos, ImVec2 size, COLORREF c)
{
	dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), col(c));
}

inline void fill(ImDrawList *dl, const RECT &r, COLORREF c)
{
	fill(dl, pos_of(r), size_of(r), c);
}

// 角を落とした四角。ボタンはこれで描く
inline void round_box(ImDrawList *dl, const RECT &r, COLORREF face,
                      COLORREF edge, float radius)
{
	dl->AddRectFilled(pos_of(r), ImVec2(float(r.right), float(r.bottom)),
	                  col(face), radius);
	dl->AddRect(pos_of(r), ImVec2(float(r.right), float(r.bottom)),
	            col(edge), radius, 0, 1.0f);
}

inline void disc(ImDrawList *dl, ImVec2 center, float r, COLORREF face,
                 COLORREF edge, float pen = 2.0f)
{
	dl->AddCircleFilled(center, r, col(face));
	dl->AddCircle(center, r, col(edge), 0, pen);
}

inline void disc(ImDrawList *dl, int cx, int cy, int r, COLORREF face,
                 COLORREF edge, int pen = 2)
{
	disc(dl, ImVec2(float(cx), float(cy)), float(r), face, edge, float(pen));
}

inline void line(ImDrawList *dl, ImVec2 a, ImVec2 b, COLORREF c, float width)
{
	dl->AddLine(a, b, col(c), width);
}

inline void line(ImDrawList *dl, int x1, int y1, int x2, int y2, COLORREF c,
                 int width)
{
	line(dl, ImVec2(float(x1), float(y1)), ImVec2(float(x2), float(y2)), c,
	     float(width));
}

// 多角形の塗りと輪郭。凹んでいてもよい
inline void poly(ImDrawList *dl, const ImVec2 *pts, int n, COLORREF fill_c,
                 COLORREF edge_c, float pen = 1.0f)
{
	dl->AddConcavePolyFilled(const_cast<ImVec2 *>(pts), n, col(fill_c));
	dl->AddPolyline(const_cast<ImVec2 *>(pts), n, col(edge_c),
	                ImDrawFlags_Closed, pen);
}

// 字は UTF-8 のまま渡す (ImGui wants UTF-8)。font + px は label/small/tiny
// のどれか（未指定なら既定の字）。
inline void text_in(ImDrawList *dl, ImVec2 pos, ImVec2 size, const char *s,
                    COLORREF c, ImFont *font = nullptr, float px = 0.0f,
                    bool center_x = false, bool center_y = false,
                    bool wrap = false)
{
	if (!s || !s[0])
		return;
	const ImVec2 ts = font ? font->CalcTextSizeA(px, FLT_MAX, 0.0f, s)
	                       : ImGui::CalcTextSize(s);
	ImVec2 at = pos;
	if (center_x)
		at.x = pos.x + (size.x - ts.x) * 0.5f;
	if (center_y)
		at.y = pos.y + (size.y - ts.y) * 0.5f;
	if (wrap)
		dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
	if (font)
		dl->AddText(font, px, at, col(c), s, nullptr, wrap ? size.x : 0.0f);
	else
		dl->AddText(at, col(c), s);
	if (wrap)
		dl->PopClipRect();
}

// A DT_* combination, the vocabulary panel.txt and layout.txt use, so the
// drawing calls read the same as the GDI ones they replace. Only the bits
// that mean something survive: DT_CENTER / DT_VCENTER / DT_WORDBREAK.
inline void text_dt(ImDrawList *dl, const RECT &r, const char *s, COLORREF c,
                    ImFont *font, float px, UINT flags)
{
	text_in(dl, pos_of(r), size_of(r), s, c, font, px,
	        (flags & DT_CENTER) != 0, (flags & DT_VCENTER) != 0,
	        (flags & DT_WORDBREAK) != 0);
}

// All-caps legend centered in a box on its cap height: the cap block sits at
// the box's middle (horizontally centered on the box too when center_x).
//
// `line_center` switches the vertical rule to GDI's DT_VCENTER, which centers
// the line box instead of the cap. The two agree only when the line box is
// exactly cap + Y0, so a call site that replaced DT_VCENTER should pass this
// and keep the placement GDI had rather than the near-miss cap centering.
inline void text_cap(ImDrawList *dl, const RECT &r, const char *s, COLORREF c,
                     ImFont *font, float px, bool center_x, bool line_center = false)
{
	if (!s || !s[0])
		return;
	// AddText puts `at` at the top of the line, not of the cap
	const float cap = cap_height(font, px);
	const float cap_top = cap_top_offset(font, px);
	const float mid = (float(r.top) + float(r.bottom)) * 0.5f;
	const float y = line_center
	                    ? mid - font_line_height(font, px) * 0.5f
	                    : mid - cap * 0.5f - cap_top;
	const ImVec2 ts = font ? font->CalcTextSizeA(px, FLT_MAX, 0.0f, s)
	                       : ImGui::CalcTextSize(s);
	ImVec2 at((float(r.left) + float(r.right)) * 0.5f - ts.x * 0.5f, y);
	if (!center_x)
		at.x = float(r.left);
	if (font)
		dl->AddText(font, px, at, col(c), s);
	else
		dl->AddText(at, col(c), s);
}

} // namespace im
} // namespace ui

#endif // S_MU2000_UI_DRAW_IMGUI_H
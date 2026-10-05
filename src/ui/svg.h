// license:BSD-3-Clause
//
// ごく小さい SVG の絵描き。パネルに絵を重ねるためだけのもの。
//
// 読めるのは
//   <path d="…"> の M L H V C Z（大文字小文字とも）
//   transform の translate(…) と matrix(…)
//   style の fill / fill-opacity / stroke / stroke-width
//
// 弧（A）も二次ベジエ（Q S T）も、勾配も、文字も読まない。**要るのは
// これだけ**で、MAME の mu2000.lay に入っている絵（CC0）はこれで出せる。
//
// 曲線は読み込むときに折れ線にしておく。描くときは viewBox を渡された
// 四角に当てはめて、点を移すだけ。だから窓の大きさが変わっても綺麗に出る。
//
// 描く先は Dear ImGui の ImDrawList（ui/draw_imgui.h、ui/imgui_shell.h）。
// 線画は点としてそのまま出し、画像はミップマップ（縮小の段）を作ってから一枚の
// テクスチャにまとめて AddImage で置く（後者は ui/imgui_shell.h の im::tex）。

#ifndef S_MU2000_UI_SVG_H
#define S_MU2000_UI_SVG_H

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "compat/gdi.h"   // RECT only

// Dear ImGui list (full type from imgui.h in the .cpp, never here, so this
// header stays light for every TU that draws panel art).
struct ImDrawList;

namespace ui {

class svg_art
{
public:
	// 名前が .png で終われば画像として読む（下の「画像のとき」）
	bool load_file(const std::string &path);
	bool load_text(const std::string &text);
	// 画素から作る。1 画素 0xAARRGGBB、α はかけていない値（read_png と同じ）
	bool load_pixels(int w, int h, const std::vector<uint32_t> &argb);
	bool ok() const { return !m_shapes.empty() || !m_mips.empty(); }
	void clear() { release_gpu(); m_shapes.clear(); m_mips.clear(); m_cache = cache{}; }

	// viewBox（画像なら画像の大きさ）を dst に当てはめて描く。縦横比は保ったまま
	// 真ん中に置く。deg を渡すと、dst の真ん中を軸にその角度だけ回す（つまみ用）。
	// 塗りの中抜きは 1 区画ずつ巡る（even-odd の穴は再現しない。
	// パネルの絵は穴に頼っていない）。
	void draw(ImDrawList *dl, const RECT &dst, double deg = 0.0) const;

private:
	// ---- 画像のとき。読んだ絵を半分ずつ縮めた段（ミップ）を持っておき、
	// 描く大きさに近い段から補間して取る。α はかけ済み（0xAARRGGBB）。
	// 出来上がりは大きさと角度が同じあいだ取っておく（毎フレーム作らない）
	struct level {
		int w = 0, h = 0;
		std::vector<uint32_t> px;
	};
	// 1 枚にした絵。テクスチャへ入れるので α は戻してある（0xAARRGGBB）
	struct cache {
		int w = 0, h = 0;                // 付けた dst の大きさ。変わったら作り直す
		void *gpu = nullptr;             // im::tex (ui/tex.h), freed by release_gpu()
	};
	bool load_png(const std::string &path);
	// The picture, as one quad. deg turns it about the middle of dst: the
	// rotation is the quad's four corners, not a new bitmap, so a knob turns
	// without the texture being touched
	void draw_image(ImDrawList *dl, const RECT &dst, double deg) const;
	// Drops the texture. The context may already have taken it (see ui/tex.h);
	// the tex object empties itself in that case, so this is always safe
	void release_gpu() const;
	std::vector<level> m_mips;
	mutable cache m_cache;

	struct pt { double x, y; };
	struct shape {
		std::vector<std::vector<pt>> subs;   // 折れ線にした輪郭
		std::vector<bool> closed;
		COLORREF fill = 0, stroke = 0;
		bool     has_fill = false, has_stroke = false;
		double   stroke_w = 1;
	};

	std::vector<shape> m_shapes;
	double m_vb[4] = { 0, 0, 1, 1 };          // viewBox
};

} // namespace ui

#endif // S_MU2000_UI_SVG_H

// license:BSD-3-Clause
//
// **窓の最上段に出す、マウスで押せるボタンの帯**。
//
// 一覧やエディタはショートカットキー（F3・F2）でも開けるが、**F3 を自分の
// コマンドとして先に食う DAW がある**ので、キーだけだとプラグインでは
// 開けないことがある。だれでも押せる入口として帯を出しておく。
//
// 帯はパネルの絵の**上に足す**（`panel::set_top_inset`）。パネルの絵は
// 1000 × 385 の全面を使っていて空きが無いので、重ねると絵が隠れてしまう。
//
// 描くのも当たりを見るのも、描いた四角を覚える paint() とそれに当てる
// hit() の対にしているので、字の測り方が違ってずれることはない。

#ifndef S_MU2000_UI_TOOLBAR_H
#define S_MU2000_UI_TOOLBAR_H

#pragma once

#include <string>
#include <vector>

#include "compat/gdi.h"
#include "ui/draw_imgui.h"
#include "ui/texts.h"

namespace ui {

// 帯に並べる 1 つ。`id` は窓の側が決める（押されたらそれが返る）
struct tool_item {
	std::string label;
	int id = 0;
};

// Which window each button opens. The one vocabulary for gui.exe, the Mac
// GUI, --shot and the plug-ins: every painter builds the same strip from
// window_bar_items() and dispatches the hit id through these, so a button
// can never name one window in one program and another elsewhere.
enum bar_window {
	BAR_LIST = 0,   // 一覧
	BAR_EDITOR = 1, // エディタ
	BAR_FX = 2,     // インサーションの設定
	BAR_SHAPES = 3, // パートの音色
	BAR_MASTER = 4, // マスター
	BAR_SAMPLING = 5, // サンプリング
};

// The strip every window shows, in the same order with the same ids.
// Labels come from the texts table (bar_list and friends), so --lang
// reaches the strip too.
inline std::vector<tool_item> window_bar_items()
{
	return { { UI_TEXT(bar_list, "List"), BAR_LIST },
	         { UI_TEXT(bar_editor, "Editor"), BAR_EDITOR },
	         { UI_TEXT(bar_shapes, "Voices"), BAR_SHAPES },
	         { UI_TEXT(bar_fx, "Effects"), BAR_FX },
	         { UI_TEXT(bar_master, "Master"), BAR_MASTER },
	         { UI_TEXT(bar_sampling, "Sampling"), BAR_SAMPLING } };
}

class pc_window;   // ui/pc_window.h (one class, two hosts)

// Which PC window a toolbar id names. One place so no front end maps one
// button to a different window
inline pc_window *window_for_kind(int kind, pc_window &list, pc_window &editor,
                                  pc_window &fx, pc_window &shapes, pc_window &master,
                                  pc_window &sampling)
{
	switch (kind) {
	case BAR_EDITOR: return &editor;
	case BAR_FX:     return &fx;
	case BAR_SHAPES: return &shapes;
	case BAR_MASTER: return &master;
	case BAR_SAMPLING: return &sampling;
	default:         return &list;
	}
}

class toolbar
{
public:
	static constexpr int HEIGHT = 26;     // 帯の高さ（画素）

	void set_items(std::vector<tool_item> items) { m_items = std::move(items); }
	bool empty() const { return m_items.empty(); }

	// 帯の中で押された場所の `id`。帯の外や隙間なら -1。
	// 描いたときの四角（paint が覚える）に当てるので、字の測り方と
	// ずれない。まだ描いていない間だけ見当で出す
	int hit(int x, int y) const
	{
		if (m_items.empty() || y < 0 || y >= HEIGHT)
			return -1;
		if (m_rects.size() == m_items.size()) {
			for (size_t i = 0; i < m_items.size(); i++)
				if (x >= m_rects[i].left && x < m_rects[i].right)
					return m_items[i].id;
			return -1;
		}
		int left = PAD;
		for (const tool_item &it : m_items) {
			const int w = width_guess(it.label);
			if (x >= left && x < left + w)
				return it.id;
			left += w + GAP;
		}
		return -1;
	}

	// 押されている最中のものを覚えておくと、押した感じが出る
	void set_down(int id) { m_down = id; }
	int  down() const { return m_down; }

	// ボタンの帯を描く。字は実測で、hit() が当てる四角もここで覚える
	//
	// px は半角のときの大きさ。和文のときは label_px() が大きくする
	void paint(ImDrawList *dl, int w, ImFont *font, float px) const
	{
		if (m_items.empty())
			return;
		px = label_px(px);
		im::fill(dl, ImVec2(0, 0), ImVec2(float(w), float(HEIGHT)), BAR_BG);
		im::line(dl, ImVec2(0, float(HEIGHT - 1)), ImVec2(float(w), float(HEIGHT - 1)),
		         BAR_EDGE, 1.0f);

		float left = float(PAD);
		m_rects.clear();
		for (const tool_item &it : m_items) {
			const ImVec2 ts = font ? font->CalcTextSizeA(px, FLT_MAX, 0.0f, it.label.c_str())
			                        : ImVec2(0, 0);
			const float bw = ts.x + SIDE * 2.0f;
			const bool down = it.id == m_down;
			const ImVec2 pos(left, 3.0f), size(bw, float(HEIGHT) - 7.0f);
			im::fill(dl, pos, size, down ? BTN_DOWN : BTN_BG);
			im::line(dl, pos, ImVec2(pos.x + size.x, pos.y), BTN_EDGE, 1.0f);
			im::line(dl, ImVec2(pos.x, pos.y + size.y), ImVec2(pos.x + size.x, pos.y + size.y),
			         BTN_EDGE, 1.0f);
			im::line(dl, pos, ImVec2(pos.x, pos.y + size.y), BTN_EDGE, 1.0f);
			im::line(dl, ImVec2(pos.x + size.x, pos.y), ImVec2(pos.x + size.x, pos.y + size.y),
			         BTN_EDGE, 1.0f);
			im::text_in(dl, pos, size, it.label.c_str(), TEXT, font, px,
			            true, true, false);
			m_rects.push_back(RECT{ int(left), 3, int(left + bw), HEIGHT - 4 });
			left += bw + GAP;
		}
	}

private:
	static constexpr int PAD = 8;         // 帯の左の余白
	static constexpr int GAP = 6;         // ボタンの間
	static constexpr int SIDE = 11;       // 字の左右の余白
	static constexpr int CHAR_W = 13;     // 字 1 つぶんの見当（全角で測る）

	// On Windows the strip is sized per language; everywhere else, and on macOS
	// in both, it is GDI's 13 exactly.
	//
	// Japanese: kanji and kana carry their ink over the full em height (measured
	// ink/em ~ 1.0) while a Latin capital reaches only ~0.70 of it (Hiragino
	// 76.6%, Meiryo UI 73.6%, Segoe UI 70.0%), so GDI's 13 fills the 26 px band
	// with Japanese and looks lost in it with English. 16.5 is where Windows
	// Japanese matched the screenshots.
	//
	// English: a different problem. GDI drew the bar in Segoe UI; this draws it
	// in the CJK face the walk picked (Yu Gothic UI), because the port has one
	// face per weight and no fallback chain. Same em, different Latin designs,
	// so 13 lands smaller than the reference and needs 14.5. The honest fix is a
	// Latin fallback face rather than a second tuned number; that is a bigger
	// change than this strip, so it waits.
	//
	// Both are tuned against the GDI screenshots, not derived, and both are
	// applied as ratios so the sizes cannot drift apart.
	//
	// The test is the UI language (lang.h), not the string: every label in the
	// strip comes from one texts table, so a per-label byte scan would only
	// ever re-derive what get_lang() already says -- and wrongly, since an
	// English table could still carry a fullwidth form.
	static float label_px(float latin_px)
	{
#ifdef _WIN32
		// Both tuned against the GDI screenshots; see the note above. Japanese
		// wants 16.5 because kanji fill the em. English wants a little more than
		// GDI's 13 for a different reason: the bar's Latin is drawn by Yu Gothic
		// UI here, where GDI drew it in Segoe UI, and the two faces set Latin
		// differently at one em.
		return get_lang() == lang::ja ? latin_px * (16.5f / 13.0f)
		                              : latin_px * (14.5f / 13.0f);
#else
		return latin_px;
#endif
	}

	static constexpr COLORREF BAR_BG   = RGB(0x1c, 0x1c, 0x20);
	static constexpr COLORREF BAR_EDGE = RGB(0x38, 0x38, 0x40);
	static constexpr COLORREF BTN_BG   = RGB(0x2c, 0x2c, 0x33);
	static constexpr COLORREF BTN_DOWN = RGB(0x4a, 0x4a, 0x56);
	static constexpr COLORREF BTN_EDGE = RGB(0x50, 0x50, 0x5c);
	static constexpr COLORREF TEXT     = RGB(0xe0, 0xe0, 0xe6);

	// 初回（まだ描いていない間）の見当。字の幅は測らず、UTF-8 の字数
	// （半角は半分）から出す。paint() の実測に置き換わるまでのつなぎ
	static int width_guess(const std::string &s)
	{
		int n = 0;
		for (size_t i = 0; i < s.size();) {
			const unsigned char c = static_cast<unsigned char>(s[i]);
			if (c < 0x80) { n += 1; i += 1; }             // 半角
			else if (c < 0xe0) { n += 2; i += 2; }
			else if (c < 0xf0) { n += 2; i += 3; }        // 漢字・かな
			else { n += 2; i += 4; }
		}
		return SIDE * 2 + n * CHAR_W / 2;
	}

	std::vector<tool_item> m_items;
	mutable std::vector<RECT> m_rects;   // paint() が描いた四角。hit() が当てる
	int m_down = -1;
};

} // namespace ui

#endif // S_MU2000_UI_TOOLBAR_H

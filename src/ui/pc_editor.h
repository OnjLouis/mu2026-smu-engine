// license:BSD-3-Clause
//
// PC で触るためのエディタ（doc/pc-editor.md）。Dear ImGui で描く。
//
// 窓や描画装置には依存しない。ImGui の 1 コマの中で draw() を呼んでもらうだけ。
// 値は画面では覚えない。パラメータの層（xg::model）の写しを読み、書くときは
// パラメータチェンジを bridge に積む。パネルの面と同じ写しを使うので、どちらで
// 変えても同じ値が見える。

#ifndef S_MU2000_UI_PC_EDITOR_H
#define S_MU2000_UI_PC_EDITOR_H

#pragma once

#include "xg_ui.h"

namespace ui {

class pc_editor : public imgui_view
{
public:
	const wchar_t *title() const override
	{
		return get_lang() == lang::ja ? L"S-MU2000 エディタ" : L"S-MU2000 Editor";
	}
	int default_width() const override  { return 1280; }
	int default_height() const override { return 800; }

	// 窓いっぱいに描く。値は panel::tick が RAM の写しから層に入れている
	void draw(xg::model &m, const xg_snapshot &ram, bridge &br) override;

private:
	void part_list(xg::model &m, bridge &br);
	void mixer(xg::model &m, bridge &br);
	void part_page(xg::model &m, bridge &br);
	// ドラムセットアップ（DRUMS1-4）の面。行が鍵 13-91、列が 1 鍵ぶんの 23 個
	void drum_page(xg::model &m, const xg_snapshot &ram, bridge &br);
	// SysEx の読み解き。左に貼り付ける欄、右に 1 行ずつの意味（ui/sysex_decode.h）
	void decode_page(xg::model &m);

	// 1 つの値を触る部品。数を並べる形（普段）と、つまみの形（開いたとき）がある。
	// 選ぶ種類の値（MONO/POLY など）は品書きになる。width は数の形の幅
	void value(const xg::param &p, int part, xg::model &m, bridge &br, float width);
	bool knob(const xg::param &p, int part, int &v, bool known, float width);
	void write(const xg::param &p, int part, int v, xg::model &m, bridge &br);

	int  m_part = 0;
	const xg_snapshot *m_ram = nullptr;   // draw の間だけ（品書きが音色の引き方を知るため）
	bool m_knobs = false;       // つまみで出すか。普段は数だけ
	double m_scrolled_at = -1;  // 最後にホイールで表をスクロールした時刻（ImGui の時計）
	bool m_wheel_taken = false; // このコマでつまみがホイールを取ったか
	int  m_drum_set = 0;        // ドラムの面で見ている組（0-3 = DRUMS1-4）
	char m_sx_text[65536] = {}; // 読み解く MIDI（貼り付けた字）
};

} // namespace ui

#endif // S_MU2000_UI_PC_EDITOR_H

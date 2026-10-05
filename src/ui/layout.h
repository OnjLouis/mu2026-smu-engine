// license:BSD-3-Clause
//
// パネルの絵の配置。**作り直さずに文字ファイルで直せる**ようにしてある。
//
// 何も無ければ、ここに書いてある既定値がそのまま使われる（実機の写真から
// 採寸したもの）。panel.txt があればその値で上書きする。
// 書き方は doc/panel-editing.md。
//
// 座標はぜんぶ論理座標（1000 × 400）。窓の大きさに合わせて一律に伸び縮み
// するので、窓の大きさは気にしなくてよい。

#ifndef S_MU2000_UI_LAYOUT_H
#define S_MU2000_UI_LAYOUT_H

#pragma once

#include "svg.h"

#include <memory>
#include <string>
#include <vector>

// COLORREF, UINT and the DT_* alignment bits, on both platforms
#include "compat/gdi.h"

namespace ui {

// 飾り。ボタンでも LCD でもない、ただ描くだけのもの
struct deco {
	enum kind { text, disc, box, art };

	int      k = text;
	double   x = 0, y = 0, w = 0, h = 0;   // disc は x,y が中心、w が半径、h が線の太さ
	double   radius = 0;                   // box の角の丸み
	COLORREF a = RGB(0, 0, 0);             // text は字の色、disc と box は面の色
	COLORREF b = RGB(0, 0, 0);             // disc と box のふちの色
	int      font = 0;                     // 0 小さい / 1 大きい
	UINT     align = 0;                    // DT_ の組み合わせ
	std::string str;                       // art は SVG の道

	// art のときだけ。読んだ絵。deco を写すと共有される
	std::shared_ptr<svg_art> pic;
};

struct layout
{
	layout();                              // 既定値を入れる

	double body_h;                         // 本体の高さ。下は面を切り替える帯
	double lcd[4];                         // LCD の窓 x y 幅 高さ

	double cat_x[6], cat_y[3];             // 音色カテゴリ 18 個。6 列 3 行
	double cat_w, cat_h;

	double mode[6][2];                     // 丸ボタン 6 個の中心
	double mode_r, mode_led_r;
	double nav[9][4];                      // 四角いボタン 9 個
	double round_[2][4];                   // SELECT と AUDITION
	double dial[3];                        // 大きなダイヤル x y 半径
	double volume[3];                      // 音量つまみ x y 半径（中心）
	double adgain[3];                      // A/D INPUT のつまみ x y 半径（半径 0 なら無い）

	// つまみの絵。SVG を渡すと、組み込みの絵の代わりに**回して**描く。
	//   dial   893 268 58 "dial.svg"
	//   volume 141 154 30 "knob.svg"
	std::string dial_art_path, volume_art_path, adgain_art_path;
	std::shared_ptr<svg_art> dial_art, volume_art, adgain_art;

	// ボタンと表示灯の絵。**ようす（消えている／点いている／押している）
	// ごとに 1 枚**渡す。押しているぶんを省くと、点いているぶんで代える
	//   mode.art  "btn.svg" "btn-on.svg" "btn-down.svg"
	//   nav.art   "key.svg" "key-down.svg"
	//   cat.art   "cat.svg" "cat-down.svg"
	//   round.art "rnd.svg" "rnd-down.svg"
	//   plg.art   "plg.svg" "plg-on.svg"
	struct art_set {
		std::string path[3];
		std::shared_ptr<svg_art> pic[3];
		bool any() const { return pic[0] || pic[1] || pic[2]; }
		// on は点いている／押している、down は押している
		const svg_art *pick(bool on, bool down) const
		{
			if (down && pic[2]) return pic[2].get();
			if (down && !pic[2] && pic[1]) return pic[1].get();
			if (on && pic[1]) return pic[1].get();
			return pic[0].get();
		}
	};
	art_set mode_art, nav_art, cat_art, round_art, plg_art;
	// キーごとの絵（nav.face <名前> "絵" "押した絵"）。**キートップの印刷まで入った絵**で、
	// あればそれを貼って印刷はコードで書かない（字の描き方が描画の道具で変わらない）
	art_set nav_face[9];
	// ボタンごとの点灯の絵（mode.on <名前> "絵"）。実機は SAMPLING だけ赤く光る
	std::shared_ptr<svg_art> mode_on[6];
	std::string mode_on_path[6];

	double low_x[11], low_w[11];           // LCD 下段の並び（上の面の点の間隔が単位。端数も可）
	double columns_y;                      // 窓の下の札の高さ
	double modes_x;                        // 右の札（XG / GS / PERFORM）の左端。負なら描かない
	double plg[3];                         // MU / PLG-1..3 の表示灯 左端 間隔 y
	double plg_size[2];                    // 表示灯の絵の幅と高さ
	bool   labels_in_art;                  // 印刷された札は絵に入っている（コードで書かない）
	bool   lcd_frame;                      // LCD のまわりの枠をコードで描く

	// 押すと品書きが出るところ。絵を描き替えたときに合わせられるよう、
	// 当たりの四角だけ持っている
	double card[4];                        // カードの差し込み口。MIDI ファイル再生
	// 差し込み口の開いた所。カードが差さっているとき、ここにカードの縁を描く。
	// 幅が 0 なら card の札のすぐ下（組み込みの絵には開いた所が無い）
	double card_slot[4] = { 0, 0, 0, 0 };
	double adin[4];                        // A/D INPUT のジャック
	double phones[4];                      // PHONES のジャック。音の出口（デジタル / アナログ）を選ぶ

	std::vector<deco> decos;

	// panel.txt を読む。無ければ false（既定値のまま）。
	// 中身が変でも、読めた行だけ反映して err に理由を積む
	bool load(const std::string &path, std::string &err);
	// いまの値をそのまま書き出す。編集の出発点に使う
	bool save(const std::string &path) const;

	// 探す順に見て、最初に見つかったものを読む。読んだ道を返す
	static std::string find_default();
};

} // namespace ui

#endif // S_MU2000_UI_LAYOUT_H

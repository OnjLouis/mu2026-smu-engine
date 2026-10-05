// license:BSD-3-Clause
//
// 音源から画面へ渡すもの。音声スレッドが作り、GUI スレッドが読む。
// panel.h と bridge.h の両方が要るので、ここだけ分けてある。

#ifndef S_MU2000_UI_SNAPSHOT_H
#define S_MU2000_UI_SNAPSHOT_H

#pragma once

#include "compat/mamecompat.h"

namespace ui {

constexpr int LOGICAL_W = 1000;
constexpr int LOGICAL_H = 400;

// LCD。firmware は 2 行 40 桁で使う。実機の窓に出るのは 24 桁ぶんで、
// そのうち左 20 桁が文字の並ぶところ、残り 4 桁が絵記号のセグメント部
constexpr int LCD_ROWS = 2, LCD_COLS = 24;

// 実機の窓は、DDRAM の桁がそのまま横一列に並んでいるのではない。
//
//   0-16   上の面。メータ 9 桁（1 マス 2 本で 18 本）＋ 文字 8 桁
//   17-19  下の面の左。行 0 が「01」（2 桁）、行 1 が「A01」（3 桁）
//   20-23  下の面。楽器のかたち。両行で 1 枚の絵
//
// 押して確かめた（doc/gui.md）
constexpr int TOP_COLS = 17;

// 下の面に並ぶもの。左から
enum : int {
	LOW_PART = 0, LOW_BANK, LOW_ICON, LOW_VOL, LOW_EXP, LOW_PAN,
	LOW_REV, LOW_CHO, LOW_VAR, LOW_KEY, LOW_MODE, LOW_COUNT
};
constexpr int CELL_W = 5, CELL_H = 8;

struct snapshot {
	u8   dots[LCD_ROWS * LCD_COLS * CELL_H] = {};   // 各バイトの下位 5bit
	u16  leds = 0;
	bool lcd_on = false;
	u8   contrast = 2;           // UTIL > SYS の Contrast（1-8）
	bool ready = false;          // 起動が終わったか
	char message[96] = {};       // 起動中／ROM が無い等。空なら出さない
	// 鳴っている声の数（SWP30 のマスタとスレーブ、それぞれ 64 スロット）
	u8   voices_master = 0, voices_slave = 0;
	bool card = false;           // 前面の差し込み口に SmartMedia が差さっている（パネルに絵を出す）
};

// XG の値の写し。音声の糸が firmware のワーク RAM から 25ms ごとに写す（xg/ram.h）。
// 画面は MU2000 に問い合わせずにこれを読む
constexpr int XG_PARTS = 64;   // 口 A-D。C・D は実機では USB だけの口
constexpr int XG_PART_COPY = 0x100;     // xg::ram::PART_COPY
constexpr int XG_SYSTEM_SIZE = 7;
constexpr int XG_EFFECT_SIZE = 0x16b;   // xg::ram::EFFECT_SIZE
// ドラムセットアップ（XG の 3n rr pp）。xg::ram::DRUM_SETUP_* と同じ大きさ
constexpr int XG_DRUM_SETS = 4, XG_DRUM_KEYS = 79, XG_DRUM_PARAMS = 23, XG_DRUM_KEY0 = 13;

struct xg_snapshot {
	u64 serial = 0;                                  // 写すたびに増える
	u8  system[XG_SYSTEM_SIZE] = {};
	u8  voice_mode = 1, voice_set = 1;               // 音色の引き方（xg/voices.h の lookup）
	u8  effect[XG_EFFECT_SIZE] = {};                 // 02 01 00 からマスター EQ まで（RAM の並び）
	u8  parts[XG_PARTS][XG_PART_COPY] = {};          // **XG のパート番号の順**に並べ直してある
	// ドラムセットアップ 4 組 × 鍵 13-91 × 23 個（ワーク RAM の並び。ドラムの画面が読む）
	u8  drum[XG_DRUM_SETS][XG_DRUM_KEYS][XG_DRUM_PARAMS] = {};
	// パートのキットの番号（パートの塊の +0x110。xg::nv::PART_KIT）。ドラムの打の記録を引くのに使う
	u8  kit[XG_PARTS] = {};
	// 入ってきた MIDI から。口×チャンネル（口 * 16 + ch）ごと。パートとの対応は受信チャンネルで
	u64 notes[XG_PARTS][2] = {};                     // 押さえている鍵
	u8  velocity[XG_PARTS] = {};                     // 最後のノートオンの強さ
	// **ピッチベンド**。真ん中からの離れ（-8192〜+8191）で持つので、既定の 0 が真ん中。
	// 入ってきた MIDI から取る。**式だけの口では firmware にベンドを渡さない**ので、
	// ワーク RAM の PART_BEND は動かない（doc/native-engine.md の 6.227）
	s16 bend[XG_PARTS] = {};
	u32 note_ons[XG_PARTS] = {};                     // ノートオンの回数（画面がメーターを振る合図）
};

} // namespace ui

#endif // S_MU2000_UI_SNAPSHOT_H

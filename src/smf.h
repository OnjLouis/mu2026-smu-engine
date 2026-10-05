// license:BSD-3-Clause
//
// 標準 MIDI ファイル（SMF）を「秒 + バイト列」の並びに開く。
// render（ファイルを WAV に）と midisend（実時間で MIDI 出力へ）で共用する。

#ifndef S_MU2000_SMF_H
#define S_MU2000_SMF_H

#pragma once

#include "compat/mamecompat.h"

#include <string>
#include <vector>

namespace smf {

struct event {
	double time;              // 秒
	std::vector<u8> bytes;
	// どの MIDI の口へ出すか。SMF のメタイベント `FF 21 01 pp`
	// （ポート指定）か、`FF 09`（機器名）が「A」〜「D」「Port 1」〜「Port 4」のものを
	// トラックごとに見る。ヤマハのシーケンサー固有のポート指定（`FF 7F 04 43 00 01 pp`）も同じ扱い。
	// どれも無ければ**トラック名**（`FF 03`）が「PartA」「Part A」
	// 「A01」「A1」「A-1」〜「D16」の形なら、その口（issue #63。MU2000EX 向けの 64 パートの曲）。
	// 何も無ければ 0。MU2000 は口 0-3 が A-D（パート 1-16、17-32、33-48、49-64）
	u8 port = 0;
};

// ファイルの口をエミュの口（0-3 = A-D）に割り当てる。USB の口（usb。gui の既定）なら
// A-D の 4 口がそのまま届く。DIN の口だけ（--host-midi）のときは実機と同じく A・B しか無いので、
// 3 口目以降は fold が真なら A・B に交互に重ね（口 3 → A、口 4 → B）、偽なら鳴らさない（-1）。
// 5 口目以降は USB でも同じ扱い（fold なら 4 口に重ねる）
inline int mu_port(u8 port, bool fold, bool usb = false)
{
	const int n = usb ? 4 : 2;
	if (port < n)
		return port;
	return fold ? port % n : -1;
}

// トラック名から口を読む（issue #63）。「PartA」「Part A」「Part-A」「A01」「A1」「A-1」「A 16」、
// 2 桁の番号のあとに区切りと名前が続く「A01-FrHorn 2」（大文字小文字は問わない）。読めなければ -1。
// 「A」1 文字だけのものや「B3 Organ」は曲名・楽器名と紛れるので読まない
int port_from_track_name(const std::string &name);

// format 0/1 に対応。テンポ変化は追う。SMPTE 単位には未対応
bool load(const std::string &path, std::vector<event> &out, std::string &err);

// メモリ上の SMF を開く（wasm 用。load(path) と同じものを返す）。
// data/size はファイルの中身そのまま。
bool load_from_memory(const u8 *data, size_t size, std::vector<event> &out, std::string &err);

} // namespace smf

#endif // S_MU2000_SMF_H

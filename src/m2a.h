// license:BSD-3-Clause
//
// MU2000 の SAMPLING → SAVE が書く .M2A ファイル（ALL+SEQ）の中の波形を読む。
//
// 中身は RIFF の RMID で、DLS とほぼ同じ形（エミュの firmware に保存させて調べた）:
//   RIFF RMID
//     data            MIDI（MThd …）
//     LIST YCVF       Yamaha の音色の集まり。colh / LIST lins（音色）/ ptbl
//       LIST wvpl     波形の置き場
//         LIST wave   波形 1 つ: LIST YMHM（MU00v000 …）、fmt （PCM、44.1kHz、16bit、1ch）、
//                     wsmp（基準の鍵・微調・ループ）、data（PCM）、LIST INFO INAM（名前 8 文字）
//     LIST INFO INAM  ファイルの名前
// サンプリングの窓の「カード」が、本体に読み込まずに波形を見て試聴するのに使う。

#ifndef S_MU2000_M2A_H
#define S_MU2000_M2A_H

#pragma once

#include "compat/mamecompat.h"

#include <string>
#include <vector>

namespace smu2000::m2a {

struct wave {
	std::string name;
	u32 rate = 44100;
	u16 channels = 1, bits = 16;
	u32 frames = 0;
	int unity = 60;           // 基準の鍵
	bool loop = false;
	u32 loop_start = 0, loop_length = 0;   // サンプル
	size_t data_offset = 0, data_bytes = 0;   // ファイルの中の PCM の位置
};

// ファイルの中の波形を並べる。RIFF でなければ false（理由は err）
bool parse(const std::vector<u8> &file, std::vector<wave> &out, std::string &err);
// 波形の PCM を 16bit のモノラルにして返す（2ch は混ぜる、8bit は広げる）
std::vector<s16> pcm(const std::vector<u8> &file, const wave &w);

} // namespace smu2000::m2a

#endif // S_MU2000_M2A_H

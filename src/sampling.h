// license:BSD-3-Clause
//
// サンプリングの管理情報（firmware が CPU の DRAM に持つ表）の形と、録音の道具。
// パネルを通さず、firmware と同じ形で表を読み書きする（doc/sampling-ram.md）。
//
// 調べ方: パネルで録る・残す・音色に割り当てる前後で DRAM とワーク RAM を突き合わせ（src/smpre.cpp）、
// 波形と表だけを起動したての機械に写して、firmware が一覧に出し、試聴でき、音色として鳴ることを確かめた。
// ワーク RAM にあるサンプルの数や残りの容量の写しは、firmware が SAMPLING に入るたびに表から数え直す。
#ifndef S_MU2000_SAMPLING_H
#define S_MU2000_SAMPLING_H
#pragma once

#include "compat/mamecompat.h"

#include <array>
#include <string>
#include <vector>

namespace smu2000::sampling {

// ---- DRAM の番地（CPU から見た番地）
// 鳴らすための表。16 バイト × 512。サンプル n（1 から）は TAB_PLAY + 16 * (n - 1)。
//   +0 00 3c 00 ff（基準の鍵 60 ほか）
//   +4 上の 8bit = 0x40 ならループしない、0x00 ならループする。
//      下の 24bit = 鳴り始めがループの頭より何サンプル手前か
//   +8 u32 ループの頭から鳴り終わりまでのサンプル数。音源はここで折り返す（か、止まる）。firmware が録ったサンプルでは
//      全体の長さ − 4（終わりの 4 サンプルは余白 = TAIL_PAD）
//   +12 u32 ループの頭の語（サンプリング RAM の 32bit 語の位置）| 0x01000000
// 録った直後は +4 が 40 00 00 00、+12 が開始の語（全体を 1 度鳴らす）。
// EDIT → SAMPLE の Loop は +4 の上を 0x00 にし（記録の +2 に 0x02 を足す）、Start は +4 の下・+8・+12 を動かす
constexpr u32 TAB_PLAY = 0x103fa28;
// サンプルの記録。36 バイト × 512。サンプル n（1 から）は TAB_SAMPLE + 36 * (n - 1)。
//   +0 u16 番号（n − 1、ビッグエンディアン）  +2 印（0x40 = 使っている）  +3 0
//   +4 00 00 00 00  +8 ff ff ff ff  +12 u32 サンプリング周波数（44100）
//   +16 u32 開始の語 | 0x01000000  +20 u32 終わりの語 | 0x01000000
//   +24 04 00 00 00  +28 名前 8 文字（空白で埋める）
constexpr u32 TAB_SAMPLE = 0x106be04;
// 次に録る語 | 0x01000000（表のすぐ後ろ）
constexpr u32 NEXT_FREE = 0x1070604;
constexpr int MAX_SAMPLES = 512;
// サンプル音色。350 バイト × 256（Bank# 0 の PGM001-128、Bank# 1 の PGM129-256）。
//   +0 01 7f  +2 名前 8 文字（空白で埋める）  +10 00 00
//   +12 1 つ目の要素: 鳴らすなら 01、+13 7f、+14 u16 0x4000 | (サンプル n − 1)。割り当て無しは 00 7f 3f 7f
//   +0x1d 半音（0x40 = 0、±12 で 1 オクターブ）  +0x1e 微調（0x40 = 0、1 でおよそ 1 セント）
//   +0x47 Level（0-127）  +0x51 Pan（0 = L7、7 = C、14 = R7、15 = Scaling）
constexpr u32 TAB_VOICE = 0x1054e00;
constexpr u32 VOICE_SIZE = 350;
constexpr int MAX_VOICES = 256;
constexpr int ROM_WAVE_SETS = 503;   // 内蔵の波形の組の数（xg/native_voice.h の SET_TABLE）
constexpr u32 SAMPLE_RATE = 44100;
// サンプリング RAM は 4MB = 0x100000 語（1 語に 16bit のサンプル 2 つ、下の 16bit が先）
constexpr u32 RAM_WORDS = 0x100000;
// サンプルの終わりに置く余白（サンプル数）。鳴り終わり・ループの終わりは、サンプルの終わりよりこれだけ手前まで
constexpr u32 TAIL_PAD = 4;

struct sample
{
	int number = 0;          // 1 から
	std::string name;
	u32 start = 0, end = 0;  // サンプリング RAM の語の位置
	u32 rate = SAMPLE_RATE;
	int peak = -1;           // 波形の最大の絶対値（16bit）。-1 はまだ測っていない（sampling_peak）
	// 鳴らす所（波形は切らず、鳴らすための表だけで決める。firmware の EDIT → SAMPLE の Start・End・Loop と同じ欄）。
	// play_from から鳴り始め、play_to で鳴り終わる。loop なら押しているあいだ loop_from から play_to までをくり返す
	// （play_to のサンプルは鳴らさず loop_from へ戻る。play_to はサンプルの終わりの TAIL_PAD 手前まで）。
	// どれも頭からのサンプル数。loop_from は偶数で play_from 以上
	bool loop = false;
	u32 play_from = 0, play_to = 0, loop_from = 0;
	u32 frames() const { return (end - start) * 2; }
};

// サンプル音色の要素 1 つ（84 バイトのうち、窓で触る欄。doc/sampling-ram.md の要素の表）
struct element
{
	bool on = false;         // 鳴らすか（音色の頭の、使う要素の印のビット）
	bool assigned = false;   // サンプルを鳴らすか
	int sample = 0;          // 1 から（assigned のとき）
	// assigned でないとき鳴らす内蔵の波形の組（0-502。XG の音色が使うのと同じ番号）。-1 は鳴らさない
	int rom_wave = -1;
	int level = 127;         // 0-127
	int pan = 7;             // 0 = L7、7 = C、14 = R7、15 = Scaling
	int coarse = 0;          // 半音（-24〜+24）
	int fine = 0;            // セント（-64〜+63）
	// 音量のエンベロープ（AWM の形）。速さは 0-63 で大きいほど速く、0 は動かない。レベルは 0-127（1 でおよそ 0.77dB）。
	// 押すと attack で最大へ、decay1 で level1 へ、decay2 で level2 へ（押しているあいだはそこに留まる）、離すと release で 0 へ
	int attack = 63, decay1 = 0, decay2 = 0, release = 63;
	int level1 = 127, level2 = 127;
	// 鳴らす鍵と強さの範囲（両端を含む）
	int key_lo = 0, key_hi = 127, vel_lo = 1, vel_hi = 127;
	// フィルター。cutoff は切る高さ（0-127、127 で開ききる）、resonance は 0-127
	int cutoff = 127, resonance = 8;
	// 2 つ目のフィルター（HPF）。0 で切らない、127 でいちばん高い所まで切る
	int hpf = 0;
	// 強さ（ベロシティ）から音量への曲線の番号（0-10。0 が普通、1・2 は弱く弾いても大きめ、3 以降は差が大きい、
	// 9・10 は中くらいの強さで最大に届く）
	int vel_curve = 0;
	// LFO。形は 0 = ノコギリ、1 = 三角、2 = S&H。phase_init なら鍵を押すたびに同じ所から（外すとでたらめな所から）。
	// 速さ 0-63、遅れ 0-127（60 でおよそ 1 秒）、音程・フィルター・音量にかける深さ 0-127
	int lfo_wave = 1;
	bool lfo_phase_init = true;
	int lfo_speed = 31, lfo_delay = 0, lfo_pitch = 0, lfo_filter = 0, lfo_amp = 0;
	// 音程とフィルターの EG。速さ 4 つ（アタック・ディケイ 1・ディケイ 2・リリース、0-63）と、レベル 5 つ
	// （始め・アタックの行き先・ディケイ 1 の行き先・ディケイ 2 の行き先 = 押している間・離した後。-64〜+63、0 で動かない）。
	// peg_depth は音程 EG の大きさ（0-127。64 以上でレベル -64 が 1 オクターブ下）
	int peg_depth = 1;
	int peg_rate[4] = { 63, 63, 63, 63 }, peg_level[5] = { 0, 0, 0, 0, 0 };
	int feg_rate[4] = { 63, 63, 63, 63 }, feg_level[5] = { 0, 0, 0, 0, 0 };
};

constexpr int VOICE_ELEMENTS = 4;

struct voice
{
	std::string name;
	std::array<element, VOICE_ELEMENTS> el;   // el[0] が要素 1
	voice() { el[0].on = true; }
};

// 録音で入力のどれを録るか（firmware の InputSrc と同じ並び）
enum class source { ad1, ad2, both };

// ループ区間を探す。pcm の [from, to) の中で、長さ min_len 以上の組 (loop_from, loop_to) のうち、
// つなぎ目のまわりの形がいちばん似ているもの。loop_from は偶数。見つからなければ false（src/sampling.cpp）
bool find_loop(const std::vector<s16> &pcm, u32 from, u32 to, u32 min_len, u32 &loop_from, u32 &loop_to);

// サンプル音色を書く SysEx（機種 0x68 のパラメータチェンジ。doc/sampling-ram.md）。
//   F0 43 1n 68 <AH> <AM> <AL> <値> F7
//   AH = 0x40 + 0x10 × Bank# + 区画（0 = 頭、1-4 = 要素 1-4）、AM = PGM − 1
//   頭:   AL 01 = 使う要素の印（+0）、02 = +1、03-0A = 名前 8 文字
//   要素: AL 00 = 波形（2 バイト。要素の [2] [3]）、02-51 = 要素の [4]-[83]
// rec は音色の記録 350 バイト。slot はそれを書く先（0-255）。1 通ずつ返す
std::vector<std::vector<u8>> voice_sysex(int slot, const u8 *rec, int device = 0);

// サンプリングの中身まるごと（波形・サンプルの記録・鳴らすための表・サンプルを鳴らす音色）を、別の MU2000 に
// 写す SysEx（機種 0x68 の一括ダンプ。doc/sampling-ram.md）。受け取った側の前の中身は消える。
//   F0 43 0n 68 <数の上> <数の下> <AH> <AM> <AL> <データ…> <検査の和> F7
//   1 通目  パラメータチェンジ 00 00 7F = 00: サンプリングの中身を全部消す（この後 1 秒待つこと。INIT_WAIT_MS）
//   00 00 00  波形を書く位置（64 バイトの塊の番号、7bit × 4）
//   00 01 00  波形 64 バイト（ビッグエンディアンの 16bit）を 74 バイトに詰めたもの。書く位置は 1 通ごとに進む
//   00 00 10  次に録る語（7bit × 5）
//   10+(n>>7) n&7F 00  サンプル n（0 から）の記録 22 バイト、同 70 = 名前 8 文字、同 20 = 鳴らすための表 24 バイト
// 並びは shingo45endo さんの M2A to SMF Converter（MIT）で知り、firmware 自身のダンプと突き合わせて確かめた。
// dram は CPU の DRAM（0x1000000 から）、pcm はサンプリング RAM。voices ならサンプルを鳴らす音色も付ける（voice_sysex）
constexpr u32 INIT_WAIT_MS = 1000;
constexpr u32 PCM_RESYNC_BLOCKS = 64;   // 波形の何塊ごとに書く位置を入れ直すか
std::vector<std::vector<u8>> memory_sysex(const std::vector<u8> &dram, const std::vector<u8> &pcm, bool voices,
                                          int device = 0);

} // namespace smu2000::sampling

#endif

// license:BSD-3-Clause
//
// サンプリングの窓（doc/sampling.md の「窓から」）。パネルを通さず、firmware の表を直に読み書きする
// （src/sampling.h、doc/sampling-ram.md）。
//
//   入力     録音デバイス（gui）、録る入力（AD1 / AD2 / AD1+2）、引き金、レベルメーター
//   録音     名前・録音・止める・残りの時間。WAV ファイルから取り込むこともできる
//   サンプル firmware の表にあるサンプルの一覧。選ぶと波形を出し、音量を上げ下げ・ノーマライズ・トリムできる
//   割り当て サンプル音色（Bank# 0/1 × PGM 1-128）に、サンプル・名前・音量・パンを書く
//
// 窓は音源に触らない。仕事は bridge::post で音を作る糸へ渡す（driver::sampling_tick）

#ifndef S_MU2000_UI_SAMPLING_EDITOR_H
#define S_MU2000_UI_SAMPLING_EDITOR_H

#pragma once

#include "xg_ui.h"
#include "card_fs.h"
#include "m2a.h"
#include "wavegen.h"

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ui {

class sampling_editor : public imgui_view
{
public:
	~sampling_editor() override
	{
		if (m_find_thread.joinable())
			m_find_thread.join();
	}
	const wchar_t *title() const override
	{
		return get_lang() == lang::ja ? L"S-MU2000 サンプリング" : L"S-MU2000 Sampling";
	}
	int default_width() const override  { return 1280; }
	int default_height() const override { return 800; }
	void draw(xg::model &m, const xg_snapshot &ram, bridge &br) override;

private:
	void input_pane(bridge &br);
	void record_pane(bridge &br);
	void samples_pane(bridge &br);
	void pump_sysex(bridge &br);
	void wave_pane(bridge &br);
	void assign_pane(bridge &br);
	void card_pane(bridge &br);
	void import_wav(const std::vector<u8> &bytes, bridge &br);
	void load_sysex(const std::vector<u8> &bytes, bridge &br);
	void apply_sysex(bridge &br);
	// カード: 一覧を作り直す・選んだファイルを読む（差しているカードは音を作る糸で、画像はここで）
	void card_refresh(bridge &br);
	void card_open_file(bridge &br, int index);
	void card_select_wave(int index);
	void make_pane(bridge &br);

	bridge::sampling_view m_view;

	// ---- 波形を作る（wavegen.h）。作り方ごとの値と、そこから作った倍音・1 周期の形・サンプルにする波形。
	// 値を触ったら m_wm_stale を立て、描く前に作り直す
	int m_wm_mode = 0;                 // 作り方（make_pane の M_BASIC…。基本・倍音・手描き・ノイズ・ファミコン・FM・オルガン・ユニゾン・声・シンク・フォールド）
	int m_wm_shape = 1;                // smu2000::wavegen::shape
	float m_wm_pulse = 0.5f;           // 矩形の上側の割合
	int m_wm_fc = 1;                   // ファミコン: 0-3 = 矩形 12.5・25・50・75%、4 = 三角、5 = ノイズ、6 = 短いノイズ
	int m_wm_fm_c = 1, m_wm_fm_m = 1;  // FM: キャリアとモジュレーターの比
	float m_wm_fm_index = 2.5f, m_wm_fm_fb = 0.3f;   // FM: 変調の深さ（ラジアン）とフィードバック（0〜1）
	int m_wm_noise_color = 0;          // ノイズ: 0 = 白、1 = ピンク、2 = ブラウン
	int m_wm_organ[9] = { 8, 8, 8, 0, 0, 0, 0, 0, 0 };   // オルガン: ドローバー 9 本（0〜8）
	int m_wm_uni_voices = 5, m_wm_uni_step = 1;          // ユニゾン: 重ねる数と、隣とのずれ（8.6 セント刻み）
	float m_wm_vowel = 0.0f;           // 声: 0〜4（あ・い・う・え・お）
	float m_wm_sync = 2.5f;            // シンク: 従う側の速さの比
	float m_wm_fold_gain = 3.0f, m_wm_fold_bias = 0.0f;  // フォールド: 折り返す量と、かたより
	int m_wm_steps = 0, m_wm_bits = 0; // 手描き: 段数と bit 数の選び（0 = そのまま）
	u32 m_wm_seed = 0;                 // 倍音のランダムの種
	int m_wm_lofi_bits = 16, m_wm_lofi_hold = 1;         // ローファイ: bit 数と、同じ値を続けるサンプル数
	float m_wm_pwm_center = 0.5f, m_wm_pwm_depth = 0.35f; // PWM: 幅の中心と、ゆれる深さ
	int m_wm_pwm_sweeps = 1;                              // PWM: ループの中でゆれる回数
	float m_wm_pluck_sustain = 0.7f, m_wm_pluck_bright = 0.8f, m_wm_pluck_len = 1.5f;   // プラック
	int m_wm_drum = 0;                                    // ドラム: wavegen::drum
	int m_wm_pd = 0;                                      // 位相ひずみ: wavegen::pd_wave
	float m_wm_pd_amount = 0.7f, m_wm_pd_ratio = 4.0f;
	float m_wm_bell_ratio = 3.5f, m_wm_bell_index = 5.0f, m_wm_bell_decay = 1.2f;   // FM のベル
	bool m_wm_vowel_morph = false;                        // 声: 2 つ目の母音へ行って戻る
	float m_wm_vowel_to = 1.0f;
	float m_wm_drum_tune = 0.5f, m_wm_drum_decay = 0.4f, m_wm_drum_tone = 0.5f;
	float m_wm_bars[32] = { 1.0f };    // 倍音 1-32 の強さ
	float m_wm_draw[256] = {};         // 手描きの 1 周期（-1〜1）
	bool m_wm_draw_init = false;
	int m_wm_draw_last = -1;           // 描いている途中の、前の点
	int m_wm_max_h = 64;               // 足す倍音の上限
	int m_wm_level = 90;               // いちばん大きい所（%）
	char m_wm_name[9] = "wave";
	bool m_wm_assign = true;           // 登録したら、割り当ての欄の音色の要素 1 に入れる
	bool m_wm_stale = true;
	smu2000::wavegen::spectrum m_wm_spec;
	std::vector<float> m_wm_cycle;     // 見せる用の 1 周期
	std::vector<s16> m_wm_pcm;         // サンプルにする波形

	// ---- カード（SmartMedia の中身を、本体に読み込まずに見る。card_fs.h・m2a.h）
	// 中身は差しているカード（m_card_src 0）か、開いた画像ファイル（1）。差しているカードを読むのは
	// 音を作る糸の仕事なので、答えは card_job で届く
	struct card_job {
		std::atomic<bool> done{ false };
		bool ok = false;
		std::string err;
		std::vector<smu2000::cardfs::entry> files;   // 一覧
		std::vector<u8> bytes;                       // 読んだファイル
		int index = -1;                              // 読んだファイルの番号（-1 は一覧）
	};
	int m_card_src = 0;
	std::string m_card_file;                    // 開いた画像の場所（M2A から作ったカードは、差すまで空）
	std::string m_card_m2a;                     // 開いた M2A の場所（そこから m_card_img を作った）
	bool card_materialize(std::string &err);    // M2A から作ったカードをファイルにする（差す前に）
	std::vector<u8> m_card_img;                 // 開いた画像の中身
	char m_card_input[512] = {};                // 画像の場所（ファイルの窓が無い所で）
	std::vector<smu2000::cardfs::entry> m_card_files;
	std::string m_card_note;                    // 一覧や読み込みの結果
	bool m_card_stale = true;                   // 一覧を作り直す
	std::string m_card_seen;                    // 一覧を作ったときに差していたカード
	int m_card_sel = -1;                        // 選んだファイル
	std::shared_ptr<card_job> m_card_job;
	std::vector<u8> m_m2a;                      // 選んだ M2A の中身
	std::vector<smu2000::m2a::wave> m_m2a_waves;
	int m_m2a_sel = -1;                         // 選んだ波形
	std::vector<s16> m_m2a_pcm;                 // その波形（44.1kHz・モノラル）
	std::vector<s16> m_m2a_lo, m_m2a_hi;        // 見取り図
	std::string m_load_after_insert;            // 差してから読み込むファイルの名前
	bool m_load_confirm = false;                // 読み込む前の確かめ
	int m_source = 0;                  // smu2000::sampling::source
	int m_trigger_db = 0;              // 0 = 引き金なし、ほかは -60〜-6 dBFS
	char m_name[9] = {};               // 録るサンプルの名前（空なら firmware と同じ takeNNN）
	char m_path[512] = {};             // WAV の場所（ファイルの窓が無い所で）
	std::string m_note;                // 直前の結果
	u64 m_note_serial = 0;             // m_note を受け取った写しの番号

	// 一覧で選んだサンプル（0 = 無し）と、音量を変える量（dB）
	int m_selected = 0;
	float m_gain_db = 6.0f;
	// トリムで残す所 [m_start, m_end)（サンプルの位置）と、表示している範囲（拡大・縮小）。
	// m_trim_for のサンプル（長さ m_trim_frames）のもの。m_drag は 1 = 始点、2 = 終点、3 = 表示を動かす、
	// 4 = ループの頭。ループは選んだときに表から読み、変えたらすぐ書く
	u32 m_start = 0, m_end = 0;
	bool m_loop_on = false;
	u32 m_loop_at = 0;
	// つなぎ目の道具。m_snap はゼロクロスに吸い付ける、m_xfade_ms はクロスフェードの長さ
	bool m_snap = true;
	// 始点・終点・ループの頭の拡大（0 = S、1 = E、2 = L）。表示する幅（サンプル）と中心、つまんでいる枠
	double m_det_span[3] = { 400.0, 400.0, 400.0 };
	double m_det_center[3] = {};
	u32 m_det_last[3] = { ~0u, ~0u, ~0u };   // 前のコマの点の位置（動いたら中心を合わせる）
	int m_det_drag = -1;
	int m_xfade_ms = 150;
	bool m_xfade_power = true;
	// ループ区間を探す。stage 0 = 波形を待つ、1 = 写せた（糸を立てる）、2 = 答えが出た
	struct find_job
	{
		std::atomic<int> stage{ 0 };
		int number = 0;
		u32 from = 0, to = 0, min_len = 0;
		std::vector<s16> pcm;
		bool ok = false;
		u32 loop_from = 0, loop_to = 0;
	};
	std::shared_ptr<find_job> m_find;
	std::thread m_find_thread;
	int m_find_ms = 300;
	// 表へ書いた後の S・E・L（音源の側でそろえた値。吸い付けや E を合わせるで動いたものを窓へ戻す）
	struct points_result
	{
		std::atomic<bool> done{ false };
		int number = 0;
		u32 from = 0, to = 0, loop_from = 0;
	};
	std::shared_ptr<points_result> m_points;
	double m_view0 = 0.0, m_view1 = 0.0;
	int m_drag = 0;
	int m_trim_for = 0;
	u32 m_trim_frames = 0;
	// 前後の無音を除いて選ぶの答え（音源の側で調べる）。上 32bit が始点、下が終点。~0 はまだ、~1 は見つからない
	std::shared_ptr<std::atomic<u64>> m_auto;

	// 割り当て
	int m_bank = 0, m_pgm = 1;
	int m_loaded_slot = -1;            // 編集欄に読み込んだ音色
	bool m_dirty = false;              // 編集欄を触った
	int m_sample = 0;                  // 0 = 無し
	int m_rom_wave = -1;               // サンプルでなく内蔵の波形の組を鳴らすとき（0-502）
	std::vector<std::string> m_wave_labels;   // 組ごとの名前代わり（使っている XG の音色）
	char m_wave_find[32] = {};         // 組の絞り込み
	// 音色を SysEx にする仕事（音を作る糸で作る）と、外へ送っている列
	struct sx_job {
		std::atomic<bool> done{ false };
		bool to_file = false;
		bool wipes = false;      // 1 通目が受け取る側の中身を消す（その後 1 秒待つ）
		std::vector<std::vector<u8>> msgs;
	};
	std::shared_ptr<sx_job> m_sx_job;
	std::deque<std::vector<u8>> m_sx_queue;
	size_t m_sx_total = 0;
	double m_sx_next = 0;     // 次の 1 通を送ってよい時刻（ImGui::GetTime）。実機が受けきれる速さに抑える
	bool m_sx_wipe_wait = false;
	bool m_mem_confirm = false;
	// 読み込む SysEx。「全部を消す」が入っていれば、確かめてから先にそれだけ送り、m_syx_at に残りを直に書く
	std::shared_ptr<std::vector<u8>> m_syx;
	double m_syx_at = -1;
	bool m_syx_confirm = false;
	char m_voice_name[9] = {};
	int m_level = 127, m_pan = 7;
	int m_coarse = 0, m_fine = 0;
	// エンベロープ（sp::voice の attack・decay1・decay2・release・level1・level2）
	// 押して試聴する鍵と、鳴らしている鍵（-1 = なし）
	int m_audition_key = 60, m_held_key = -1;
	int m_attack = 63, m_decay1 = 0, m_decay2 = 0, m_release = 63, m_level1 = 127, m_level2 = 127;
	int m_key_lo = 0, m_key_hi = 127, m_vel_lo = 1, m_vel_hi = 127;
	bool m_el_on = true;
	// 要素 1-4。上の編集欄は m_cur_el のもので、要素を切り替えるときに m_els と出し入れする
	std::array<smu2000::sampling::element, smu2000::sampling::VOICE_ELEMENTS> m_els{};
	int m_cur_el = 0;
	void stash_el()
	{
		smu2000::sampling::element &x = m_els[size_t(m_cur_el)];
		x.on = m_el_on;
		x.assigned = m_sample != 0;
		x.sample = m_sample;
		x.rom_wave = m_sample ? -1 : m_rom_wave;
		x.level = m_level;
		x.pan = m_pan;
		x.coarse = m_coarse;
		x.fine = m_fine;
		x.attack = m_attack;
		x.decay1 = m_decay1;
		x.decay2 = m_decay2;
		x.release = m_release;
		x.level1 = m_level1;
		x.level2 = m_level2;
		x.key_lo = m_key_lo;
		x.key_hi = m_key_hi;
		x.vel_lo = m_vel_lo;
		x.vel_hi = m_vel_hi;
	}
	void load_el(int e)
	{
		m_cur_el = e;
		const smu2000::sampling::element &x = m_els[size_t(e)];
		m_el_on = x.on;
		m_sample = x.assigned ? x.sample : 0;
		m_rom_wave = x.assigned ? -1 : x.rom_wave;
		m_level = x.level;
		m_pan = x.pan;
		m_coarse = x.coarse;
		m_fine = x.fine;
		m_attack = x.attack;
		m_decay1 = x.decay1;
		m_decay2 = x.decay2;
		m_release = x.release;
		m_level1 = x.level1;
		m_level2 = x.level2;
		m_key_lo = x.key_lo;
		m_key_hi = x.key_hi;
		m_vel_lo = x.vel_lo;
		m_vel_hi = x.vel_hi;
	}
};

} // namespace ui

#endif // S_MU2000_UI_SAMPLING_EDITOR_H

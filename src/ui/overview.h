// license:BSD-3-Clause
//
// 一覧の窓（doc/pc-editor.md）。Domino のトラック一覧のように、32 パートを 1 行ずつ並べて、
// 曲を流しながら全体のバランスを見て整える。
//
// 1 行に: パートと音色、VEL メーター、VOL / EXP / PAN / P.BEND / MOD / HOLD の棒と数、
// VIB / FILTER / EG / EQ の絵（見るだけ。ダブルクリックでパートの音色の窓）、INS、VAR / CHO / REV の棒と数、鳴っている鍵盤。
// 値は RAM の写し（panel::tick が層に入れたもの）と、MIDI の見張り（押さえている鍵）から。

#ifndef S_MU2000_UI_OVERVIEW_H
#define S_MU2000_UI_OVERVIEW_H

#pragma once

#include "xg_ui.h"
#include "imgui.h"
#include "xg/fx_types.h"

namespace ui {

class overview : public imgui_view
{
public:
	overview()
	{
		for (int i = 0; i < XG_PARTS; i++)
			m_playing[i] = m_saved_rcv[i] = -1;
		for (int &n : m_pc_note)
			n = -1;
	}

	const wchar_t *title() const override
	{
		return get_lang() == lang::ja ? L"S-MU2000 一覧" : L"S-MU2000 List";
	}
	// 表示の大きさ（xgui::overview_zoom、既定 0.625）で描くので、窓もその分だけ小さく出す
	int default_width() const override  { return 1200; }
	int default_height() const override { return 700; }   // 64 パートぶん並ぶので高めに
	void draw(xg::model &m, const xg_snapshot &ram, bridge &br) override;
	// 閉じたら、鳴らしている鍵を離し、ミュートとソロを外す（受信チャンネルを戻す）
	void hidden(bridge &br) override;

	struct column;                   // 列の中身（overview.cpp）

	// 絵の 1 マス。パートの音色の窓（part_shapes）も同じものを大きく描く。
	// compact は一覧の中の小さなマスのとき。描くだけでマウスでは触れない（ダブルクリックで
	// パートの音色の窓を頼むのは呼ぶ側）。点をつまんで変えるのは compact でないときだけ
	//
	// EG: 音量の形を実際の時間で描き、Attack・Decay・Release のフェーダー（音色の窓。ピッチ EG と同じ時間の目盛り）
	static void eg_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// 一覧の小さなマスのEG（目安の形。eg_cell が compact のとき使う）
	static void eg_small(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// ピッチ EG: 音程の動きを実際の時間で描き、4 本のフェーダー（音色の窓。EG と同じ時間の目盛り）
	static void peg_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// 一覧の小さなマスのピッチ EG（目安の形。peg_cell が compact のとき使う）
	static void peg_small(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// EG とピッチ EG を 1 枚に（同じ時間の目盛り）。上の段に絵、下の段に 7 本のフェーダー（音色の窓の右の列）
	static void env_cell(int part, xg::model &m, bridge &br, float w, float h);
	// ---- ドラムの 1 打（音色の窓のドラムのタブ）。set は DRUMS の組（0-3）、key は鍵（13-91）。
	// 値は xgui::drum_value で読み、フェーダーを動かすと drum_write（3n rr pp）で書く。どれも上下 2 段の区画
	// フィルタと EQ: 打のフィルタと打ごとの EQ を合わせた実際の特性。下に Cutoff・Reso・VelCut・HPF と EQ の 4 本
	static void drum_filter_cell(int part, int set, int key, const xg_snapshot &ram, bridge &br, float w, float h);
	// EG: 音量の形を実際の時間で。下に立ち上がり・減衰 1・減衰 2
	static void drum_env_cell(int part, int set, int key, const xg_snapshot &ram, bridge &br, float w, float h);
	// 高さ・音量・パン・送り: 上に置き場所の絵と高さの字、下に 8 本
	static void drum_mix_cell(int part, int set, int key, const xg_snapshot &ram, bridge &br, float w, float h);
	// 音色の窓の縦 2 段つなぎの区画（フィルタと EQ、EG とピッチ EG）で、絵の段が占める割合
	static constexpr float MAISON_SPLIT = 0.5f;
	// パートの音のスペクトラムを a-b の四角に描く。横は実際の周波数（20 Hz-20 kHz の対数）、縦は出す線の
	// いちばん大きい所から 60 dB 下まで。src は bridge::read_scope の番号（0 が声の和、bridge::scope_src で
	// エフェクトの入口・出口）。ghost_src が 0 以上なら、それ（エフェクトの入口など）を灰色の線で同じ目盛りに重ねる。
	// key は下がるときの滑らかさの状態を区画ごとに分ける番号。label は左上に小さく出す字（nullptr で無し）。
	// backdrop ならほかの絵の背景に薄く描く（地の四角・周波数の目盛り・「鳴っていない」の字は出さない）
	static void spectrum_view(bridge &br, int part, int src, int ghost_src, int key, ImVec2 a, ImVec2 b, const char *label,
	                          bool backdrop = false);
	// フェーダーを n 本、今の位置から size の四角に横に並べる（音色の窓の下の段と同じ絵と操作）。
	// パートのパラメータは part の値、エフェクトのパラメータ（reverb.* など）は共通の値。group_after の後ろで組を分ける。
	// dim_mask のビットが立ったフェーダーは、動かせるが今は効かない値として色を落とす。
	// 戻り値はカーソルが載っているかつまんでいるフェーダー（無ければ -1）
	static int fader_strip(const char *id, const char *const *keys, const char *const *names, int n, int group_after, int part,
	                       xg::model &m, bridge &br, ImVec2 size, unsigned dim_mask = 0);
	// フィルタ: 実際の周波数特性とパートの音のスペクトラムを同じ目盛りで描き、Cutoff・Resonance・HPF のフェーダー（音色の窓）
	static void filter_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// 一覧の小さなマスのフィルタ（目安の形。filter_cell が compact のとき使う）
	static void filter_small(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// パートの EQ: 低音と高音の点をつまんで、横で周波数、縦でゲイン
	static void eq_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// ビブラート: 実際の揺れの波と、Rate・Depth・Delay のフェーダー（音色の窓）
	static void vib_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// 一覧の小さなマスのビブラート（波の山をつまむ前の絵。vib_cell が compact のとき使う）
	static void vib_small(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// モジュレーションのビブラート（ホイールの位置ごとの揺れの深さ。音色の窓）
	static void mod_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact);
	// **ゆれ（VIB・MW・BEND）**。ビブラートとモジュレーションを 1 枚の絵にした区画
	// （音色の窓）。上が絵、下がフェーダーで、ピッチベンドの縦フェーダーもここに居る
	static void wobble_cell(int part, xg::model &m, bridge &br, float w, float h);
	// そのパートのベンドの今の値（送ったばかりならその値。真ん中からの離れ）と、動かして送る
	static int  bend_now(int part, int slot);
	static void bend_send(int part, int slot, int value, bridge &br);
	// マスター EQ の 5 つの帯の特性。edit なら点をつまんで周波数とゲイン、ホイールで Q（マスターの窓）。
	// edit でなければ描くだけ（一覧のマスターの行）
	static void master_eq_plot(xg::model &m, bridge &br, float w, float h, bool edit);

	// パートの音色の窓の上のペイン: 掛かっているエフェクト（種類の名前まで）、VOL〜HOLD と VAR〜REV の棒
	// （一覧と同じく触れる）、このパートの鍵盤。窓を閉じたら strip_hidden で鳴らしている鍵を離す
	void part_strip(int part, xg::model &m, const xg_snapshot &ram, bridge &br);
	// part_strip の高さ（今の字の大きさで。枠の余白は入らない）
	static float part_strip_height();
	static constexpr float LABEL_SCALE = 0.75f;   // 帯の見出しと数の字の大きさ（本文に対して）
	static constexpr float METER_H = 0.9f;        // 帯の棒の高さ（字の大きさに対して）
	void strip_hidden(bridge &br)
	{
		release_keys(br);
		release_pc_keys(br);
	}

private:
	// 1 パートの鍵盤（押さえている鍵が光る。押すと鳴らす）。slot は受信の口 × 16 + ch（無ければ -1）。
	// marker（パートの音色の窓）なら、右クリックで試聴の鍵を決め（目印を描く）、左で鳴らす。
	// 一覧では左右どちらでも鳴らす。pc_low は PC のキーボードで弾ける範囲の下の端（-1 なら描かない）
	void keys_cell(int part, int slot, const xg_snapshot &ram, bridge &br, float w, float h,
	               bool marker = false, int pc_low = -1);
	// モジュレーションホイール（CC1）。カーソルを載せてホイールか、上下にドラッグで変える
	// PC のキーボードで弾く（A W S E D F T G Y H U J K O L P ; が C から、Z / X でオクターブ）
	void pc_keys(int part, int slot, bridge &br);
	void release_pc_keys(bridge &br);
	// 行を選ぶ。パートの音色の窓も同じパートに替える
	void select_part(int part);
	void release_keys(bridge &br);          // マウスで鳴らしている鍵を全部離す
	// 上の帯の右端の、同時発音数（マスタとスレーブの内訳）と CPU の負荷。数字の後ろに棒
	void meters(bridge &br);
	// ミュートとソロを音源に効かせる。消すパートは受信チャンネルを OFF にし（先に
	// そのチャンネルへオールサウンドオフ）、戻すパートは覚えておいたチャンネルに戻す。
	// 曲の XG リセットなどで受信チャンネルが書き換わったら、覚えを捨ててもう一度消す
	void apply_mutes(xg::model &m, bridge &br);
	// ミュートか、ほかのパートのソロで消えているか
	bool silenced(int part) const
	{
		bool any_solo = false;
		for (int p = 0; p < XG_PARTS; p++)
			any_solo |= m_solo[p];
		return m_mute[part] || (any_solo && !m_solo[part]);
	}
	// パートの欄の右端の M / S の印
	void mute_buttons(int part, float x, float y, float w, float h);
	void row(int part, xg::model &m, const xg_snapshot &ram, bridge &br, float h);
	// INS 列の 1 マス。掛かっているエフェクトの印（1-4、V）を横に並べる。names なら種類の名前も。
	// which で並べるものを絞る（パートの音色の窓はインサーションとバリエーションを別の場所に出す）。
	// 右クリックで掛ける・外す・種類、印のドラッグで別のパートへ、印のダブルクリックで設定の窓
	enum class fx_which { all, insertions, variation };
	void ins_cell(int part, xg::model &m, bridge &br, float h, bool names = false, fx_which which = fx_which::all);
	// パートの帯の 1 行目（VAR の棒の真上）に、バリエーションの種類と繋がり方（x0-x1 の幅に収める）
	void variation_label(int part, xg::model &m, bridge &br, float x0, float y, float x1);
	// マスター EQ の 1 マス。見るだけで、ダブルクリックでマスターの窓
	void master_eq_cell(xg::model &m, bridge &br, float h);
	// 上のマスターの表。マスターボリューム、移調、リバーブ・コーラス・バリエーションの種類と戻り、
	// インサーション 1-4 の種類と掛け先、全パートの鍵盤
	void master_pane(xg::model &m, const xg_snapshot &ram, bridge &br);
	void system_fx_cell(const char *title, const std::vector<xg::fx_type> &types, const char *type_key,
	                    const char *return_col, bool variation, xg::model &m, const xg_snapshot &ram,
	                    bridge &br, float h);
	void insertion_cell(int slot_index, xg::model &m, bridge &br, float h);
	// 棒 1 つ。XG のパラメータなら触れる。part が -1 ならマスターの行
	// value_out を渡すと数を描かずに返す（棒が高さいっぱいになる。パートの帯は数を見出しの行に出す）
	struct cell_text { std::string text; bool bright = true; bool hovered = false; };
	void cell(const column &c, int part, xg::model &m, const xg_snapshot &ram, bridge &br, float w, float h,
	          cell_text *value_out = nullptr);

	int    m_part = 0;
	float  m_level[XG_PARTS] = {};          // VEL メーターの今の高さ（0-1）
	u32    m_seen_ons[XG_PARTS] = {};       // 見張りのノートオンの回数を最後に見た値
	// 鳴らしている鍵（-1 は無し）と、そのときの受信の口×チャンネル。
	// 配列の初期化で -1 を並べるのは 64 個では長いので、開くときに埋める（reset_rows）
	int    m_playing[XG_PARTS];
	int    m_playing_slot[XG_PARTS] = {};
	// PC のキーボードで弾いている音（キーごと。-1 は鳴らしていない）と、その口×チャンネル
	static constexpr int PC_KEYS = 37;   // A 段 12 + Q 段 12 + 数字の段 13
	static constexpr int PC_MOD = 80;    // Shift を押している間のモジュレーション
	int    m_pc_note[PC_KEYS];          // 鳴らしている鍵（-1 = なし）。作るときに -1 で埋める
	int    m_pc_slot[PC_KEYS] = {};
	int    m_pc_mod_part = 0, m_pc_mod_slot = -1;   // Shift でモジュレーションを上げた先（-1 = 上げていない）
	int    m_pc_base = 60;                 // A の鍵（C3）
	// モジュレーションホイールで送った値と時刻（RAM の写しが追いつくまではこちらを出す）。
	// 帯のホイールとモジュレーションの絵（mod_cell）の両方から回すので共有する
	static inline int    m_mod_sent = -1;
	static inline int    m_mod_sent_part = -1;
	static inline double m_mod_sent_at = -10.0;
	// ピッチベンドも同じ（一覧の列と、音色の窓のつまみの両方から動かす）
	static inline int    m_bend_sent = 0;
	static inline int    m_bend_sent_part = -1;
	static inline double m_bend_sent_at = -10.0;
	// そのパートのホイールの今の値（送ったばかりならその値）と、回して送る
	static int  mod_now(int part, int ram_value);
	static void mod_send(int part, int slot, int value, bridge &br);
	// マウスホイールの回した量 → 動かす量（1 目で 1、big（Ctrl）で 10）
	static int  wheel_steps(float wheel, bool big);
	xg::model *m_model = nullptr;     // 閉じたときに受信チャンネルを戻すため（draw で覚える）
	bool   m_mute[XG_PARTS] = {}, m_solo[XG_PARTS] = {};
	int    m_saved_rcv[XG_PARTS];     // ミュートで OFF にする前の受信チャンネル（-1 は消していない）
	double m_scrolled_at = -1;        // ホイールで表をスクロールした時刻（エディタと同じ決まり）
	bool   m_wheel_taken = false;
};

} // namespace ui

#endif // S_MU2000_UI_OVERVIEW_H

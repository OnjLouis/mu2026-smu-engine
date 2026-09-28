// license:BSD-3-Clause

#include "part_shapes.h"

#include "overview.h"
#include "fx_editor.h"
#include "fx_help.h"
#include "fx_icons.h"
#include "eq_curve.h"
#include "ui/texts.h"

#include "imgui.h"
#include "xg/fx_params.h"
#include "xg/fx_types.h"
#include "xg/sysfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ui {

using namespace xgui;

namespace {

// パートは口 A-D の 64（C・D は実機では USB だけの口）
constexpr int PARTS = XG_PARTS;
constexpr float BAR_SCALE = 0.85f;   // 下の説明の帯の字の大きさ（本文に対して）
constexpr int FOLD_BIT = 16;         // 音色を選ぶ面を畳んでいるか（shapes_knobs のビットを借りて editor.ini に覚える）

// 「グラフ ○ つまみ」の切り替え。見出しの行の右端に描き、押されたら true。
// 見出しと並べて入らなければ字を外して切り替えだけにし、それでも入らなければ見出しを切る
bool title_toggle(const char *title, const char *id, bool knobs, bool &toggle_hovered)
{
	const float fs = ImGui::GetFontSize();
	const char *l = UI_TEXT(ps_graph, "Graph"), *r = UI_TEXT(ps_knobs, "Knobs");
	const float lw = ImGui::CalcTextSize(l).x, rw = ImGui::CalcTextSize(r).x;
	const float th = fs * 0.9f, tw = th * 1.8f, gap = fs * 0.3f;
	const float room = ImGui::GetContentRegionAvail().x;
	const float title_w = ImGui::CalcTextSize(title).x;
	const bool words = title_w + fs + lw + gap + tw + gap + rw <= room;
	const float total = words ? lw + gap + tw + gap + rw : tw;
	const ImVec2 start = ImGui::GetCursorScreenPos();
	const float line_h = ImGui::GetTextLineHeight();
	// 見出し（切り替えにかからないところまで）
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float title_room = std::max(0.0f, room - total - fs * 0.5f);
	dl->PushClipRect(start, ImVec2(start.x + title_room, start.y + line_h), true);
	dl->AddText(start, ImGui::GetColorU32(ImGuiCol_Text), title);
	dl->PopClipRect();
	ImGui::Dummy(ImVec2(title_room, line_h));
	if (ImGui::IsItemHovered() && title_w > title_room)
		hint("%s", title);
	ImGui::SameLine(0, 0);
	ImGui::SetCursorScreenPos(ImVec2(start.x + room - total, start.y));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(total, line_h));
	toggle_hovered = ImGui::IsItemHovered();
	if (toggle_hovered)
		hint("%s", knobs ? UI_TEXT(ps_hint_knobs, "Showing knobs (drag the value bars). Click to switch to the graph.")
		           : UI_TEXT(ps_hint_graph, "Showing the graph (drag the picture). Click to switch to knobs."));
	const ImU32 on = ImGui::GetColorU32(ImGuiCol_Text), off = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	float sx = p.x;
	if (words) {
		dl->AddText(p, knobs ? off : on, l);
		dl->AddText(ImVec2(p.x + lw + gap * 2 + tw, p.y), knobs ? on : off, r);
		sx += lw + gap;
	}
	const float ty = p.y + (line_h - th) * 0.5f;
	const ImVec2 a(sx, ty), b(sx + tw, ty + th);
	dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), th * 0.5f);
	const float kx = knobs ? b.x - th * 0.5f : a.x + th * 0.5f;
	dl->AddCircleFilled(ImVec2(kx, ty + th * 0.5f), th * 0.38f, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
	return pressed;
}

// 1 つの区画。見出しと、大きな絵か値の棒（右上の切り替えで選ぶ。index が負なら絵は無く棒だけ、
// PANEL_FIXED なら切り替えは無く、いつも draw で描く）
constexpr int PANEL_FIXED = -2;
template <typename Draw>
void panel(const char *id, const char *title, float w, float h, int part, xg::model &m, bridge &br,
           std::initializer_list<const char *> keys, int index, Draw draw, const char *about = nullptr)
{
	const float fs = ImGui::GetFontSize();
	// 見出しを枠の上端に寄せる（上下の余白を詰める）
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().WindowPadding.x, fs * 0.1f));
	// 切り替えの無い区画（エフェクト・つなぎ）は中身を区画に収めて描くので、スクロールバーを出さない
	const bool open = ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders,
	                                    index == PANEL_FIXED ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
	ImGui::PopStyleVar();
	if (!open) {
		ImGui::EndChild();
		return;
	}
	const bool knobs = index != PANEL_FIXED && (index < 0 || shapes_knobs(index));
	bool toggle_hovered = false;
	const float title_top = ImGui::GetCursorScreenPos().y;
	ImGui::PushFont(nullptr, fs * 0.8f);      // 見出しは小さめに
	if (index < 0)
		ImGui::TextUnformatted(title);
	else if (title_toggle(title, "##mode", knobs, toggle_hovered))
		set_shapes_knobs(index, !knobs);
	ImGui::PopFont();
	// 見出しの行の上か（Ctrl＋右クリックで区画ごと送るのは見出しだけ）
	const float title_bottom = ImGui::GetCursorScreenPos().y;
	const float my = ImGui::GetIO().MousePos.y;
	const bool over_title = !toggle_hovered && my >= title_top && my < title_bottom &&
	                        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	const std::string before = hint_text();
	if (!knobs) {
		// 絵だけ。区画の残りを全部使う
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		draw(part, m, br, avail.x, std::max(fs * 4.0f, avail.y));
	} else {
		ImGui::PushItemWidth(-fs * 6.0f);
		for (const char *k : keys)
			param_slider(k, part, m, br);
		ImGui::PopItemWidth();
	}
	// 見出しの上なら、Ctrl＋右クリックで区画のパラメータをまとめて送る
	if (keys.size() && over_title)
		out_hover_group(std::vector<const char *>(keys.begin(), keys.end()), part);
	// カーソルの下の部品が説明を出さなかったら、区画そのものの説明を
	if (about && !toggle_hovered && hint_text() == before &&
	    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
		hint("%s\n%s", title, about);
	ImGui::EndChild();
}

// ---- モジュレーションのマトリクス。操作子 6 つ（行）× 行き先 6 つ（列）の深さ
//
// 行き先の 3 つ（音程・切る高さ・音量）は「動かす」量で 64 が 0（±）、残りの 3 つは LFO の揺れの深さ（0-127）。
// マスの色は、± の量なら + が青・− が赤、深さなら橙で、濃さが量。既定から外れたマスは枠を明るく。
// マスを上下にドラッグ（2 ドットで 1）、マウスホイール（1 目で 1、Ctrl で 10）、ダブルクリックで既定に戻す
void mod_matrix(int part, xg::model &m, bridge &br, float w, float h)
{
	struct src { const char *key, *name, *about; };
	const src SRCS[6] = {
		{ "mw",   UI_TEXT(mx_src_mw_name, "Modulation"),   UI_TEXT(mx_src_mw_about, "Modulation wheel (CC1)") },
		{ "bend", UI_TEXT(mx_src_bend_name, "Pitch bend"), UI_TEXT(mx_src_bend_about, "Pitch bend (amount from center, either direction)") },
		{ "cat",  UI_TEXT(mx_src_cat_name, "Channel AT"),  UI_TEXT(mx_src_cat_about, "Channel aftertouch (key pressure, one per channel)") },
		{ "pat",  UI_TEXT(mx_src_pat_name, "Poly AT"),     UI_TEXT(mx_src_pat_about, "Polyphonic aftertouch (pressure per key)") },
		{ "ac1",  UI_TEXT(mx_src_ac1_name, "AC1"),         UI_TEXT(mx_src_ac1_about, "AC1 (control change set by AC1 CC No)") },
		{ "ac2",  UI_TEXT(mx_src_ac2_name, "AC2"),         UI_TEXT(mx_src_ac2_about, "AC2 (control change set by AC2 CC No)") },
	};
	struct dst { const char *key, *name, *sub; };
	const dst DSTS[6] = {
		{ "pitch",    UI_TEXT(mx_dst_pitch_name, "Pitch"),       UI_TEXT(mx_dst_pitch_sub, "Pitch") },
		{ "filter",   UI_TEXT(mx_dst_filter_name, "Filter"),     UI_TEXT(mx_dst_filter_sub, "Filter") },
		{ "amp",      UI_TEXT(mx_dst_amp_name, "Volume"),        UI_TEXT(mx_dst_amp_sub, "Amp") },
		{ "lfo_pmod", UI_TEXT(mx_dst_lfo_pmod_name, "LFO pitch"),  UI_TEXT(mx_dst_lfo_pmod_sub, "Vibrato") },
		{ "lfo_fmod", UI_TEXT(mx_dst_lfo_fmod_name, "LFO filter"), UI_TEXT(mx_dst_lfo_fmod_sub, "Wah") },
		{ "lfo_amod", UI_TEXT(mx_dst_lfo_amod_name, "LFO volume"), UI_TEXT(mx_dst_lfo_amod_sub, "Tremolo") },
	};
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 org = ImGui::GetCursorScreenPos();
	const float head_w = std::min(fs * 8.0f, w * 0.26f);
	const float head_h = ImGui::GetTextLineHeight() * 2.2f;
	const float gap = std::max(2.0f, fs * 0.15f);
	const float cw = (w - head_w) / 6.0f, ch = std::max(fs * 1.6f, (h - head_h) / 6.0f);
	ImGuiStorage *st = ImGui::GetStateStorage();

	// 列の見出し（2 行: 行き先と、揺れなら何になるか）。動かすのと揺らすのの間に線
	for (int c = 0; c < 6; c++) {
		const float x = org.x + head_w + cw * float(c);
		const ImVec2 a = ImGui::CalcTextSize(DSTS[c].name), b = ImGui::CalcTextSize(DSTS[c].sub);
		dl->AddText(ImVec2(x + (cw - a.x) * 0.5f, org.y), ImGui::GetColorU32(ImGuiCol_Text), DSTS[c].name);
		dl->AddText(ImVec2(x + (cw - b.x) * 0.5f, org.y + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled), DSTS[c].sub);
	}
	{
		const float x = org.x + head_w + cw * 3.0f - gap * 0.5f;
		dl->AddLine(ImVec2(x, org.y), ImVec2(x, org.y + head_h + ch * 6.0f), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
	}

	for (int r = 0; r < 6; r++) {
		const float y = org.y + head_h + ch * float(r);
		// 行の見出し。AC1・AC2 は CC の番号もここで（マウスホイールで変える）
		ImGui::SetCursorScreenPos(ImVec2(org.x, y));
		ImGui::PushID(r);
		ImGui::InvisibleButton("##row", ImVec2(head_w - gap, ch - gap));
		const bool row_hover = ImGui::IsItemHovered();
		dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(row_hover ? ImGuiCol_HeaderHovered : ImGuiCol_Header, 0.35f), 3.0f);
		std::string label = SRCS[r].name;
		if (r >= 4) {
			const std::string cck = std::string("part.") + SRCS[r].key + "_cc";
			const xg::param &cp = P(cck.c_str());
			int ccv = 0;
			if (m.get(cp, part, ccv)) {
				char b[32];
				std::snprintf(b, sizeof(b), "  CC%d", ccv);
				label += b;
				if (row_hover) {
					ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
					if (io.MouseWheel != 0.0f) {
						const int nv = std::clamp(ccv + (io.MouseWheel > 0 ? 1 : -1) * (io.KeyCtrl ? 10 : 1), cp.min, cp.max);
						if (nv != ccv)
							br.send(m.set(cp, part, nv));
					}
				}
			}
		}
		const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
		dl->AddText(ImVec2(org.x + fs * 0.3f, y + (ch - gap - ls.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
		if (row_hover) {
			// Ctrl＋右クリック: AC1・AC2 の見出しは CC の番号、ほかの見出しはその行の 6 マス
			if (r >= 4) {
				out_hover_param(P((std::string("part.") + SRCS[r].key + "_cc").c_str()), part);
			} else {
				std::vector<const char *> row_keys;
				for (int c = 0; c < 6; c++)
					row_keys.push_back(P((std::string("part.") + SRCS[r].key + "_" + DSTS[c].key).c_str()).key);
				out_hover_group(row_keys, part);
			}
			if (r >= 4)
				hint(UI_TEXT(mx_hint_cc_fmt, "%s\n%s. The 6 cells in this row are what this source moves. Wheel over a header changes the CC number"),
				     official_name((std::string("part.") + SRCS[r].key + "_cc").c_str()).c_str(), SRCS[r].about);
			else
				hint(UI_TEXT(mx_hint_row_fmt, "%s\nThe 6 cells in this row are what this source moves"), SRCS[r].about);
		}

		for (int c = 0; c < 6; c++) {
			const std::string key = std::string("part.") + SRCS[r].key + "_" + DSTS[c].key;
			const xg::param &p = P(key.c_str());
			int v = p.def;
			const bool known = m.get(p, part, v);
			const float x = org.x + head_w + cw * float(c);
			ImGui::SetCursorScreenPos(ImVec2(x, y));
			ImGui::PushID(c);
			ImGui::InvisibleButton("##cell", ImVec2(cw - gap, ch - gap));
			const ImGuiID id = ImGui::GetItemID();
			const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
			if (known) {
				int nv = v;
				if (ImGui::IsItemActivated()) {
					st->SetInt(id, v);
					st->SetFloat(id + 1, io.MousePos.y);
				}
				if (act && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					nv = st->GetInt(id, v) + int((st->GetFloat(id + 1, io.MousePos.y) - io.MousePos.y) / 2.0f);
				if (hov) {
					ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
					if (io.MouseWheel != 0.0f)
						nv = v + (io.MouseWheel > 0 ? 1 : -1) * (io.KeyCtrl ? 10 : 1);
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						nv = p.def;
				}
				nv = std::clamp(nv, p.min, p.max);
				if (nv != v) {
					drag_send(br, m.set(p, part, nv));
					v = nv;
				}
			}
			// 描く
			const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
			const bool bip = p.how == xg::view::center;
			float t = 0.0f;
			ImU32 fill;
			if (bip) {
				const float span = float(v >= p.center ? p.max - p.center : p.center - p.min);
				t = span > 0 ? float(v - p.center) / span : 0.0f;
				fill = t >= 0 ? IM_COL32(70, 140, 235, int(40 + 190 * t)) : IM_COL32(230, 80, 70, int(40 - 190 * t));
			} else {
				t = float(v - p.min) / float(std::max(1, p.max - p.min));
				fill = IM_COL32(235, 160, 50, int(30 + 200 * t));
			}
			dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
			if ((bip && v != p.center) || (!bip && v != p.min))
				dl->AddRectFilled(a, b, fill, 3.0f);
			const bool changed = known && v != p.def;
			dl->AddRect(a, b, changed ? IM_COL32(250, 230, 150, 255)
			                          : ImGui::GetColorU32(hov || act ? ImGuiCol_Border : ImGuiCol_BorderShadow), 3.0f,
			            0, changed ? 1.5f : 1.0f);
			const std::string text = known ? xg::format(p, v) : std::string("--");
			const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
			dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
			            ImGui::GetColorU32(known && ((bip && v != p.center) || (!bip && v != p.min)) ? ImGuiCol_Text : ImGuiCol_TextDisabled),
			            text.c_str());
			if (hov && known)
				out_hover_param(p, part);          // Ctrl＋右クリックでこのマスを送る
			if (hov || act) {
				const char *help = help_for(key.c_str());
				char to[64];
				if (c >= 3)
					std::snprintf(to, sizeof(to), UI_TEXT(mx_dst_sub_fmt, "%s (%s)"), DSTS[c].name, DSTS[c].sub);
				else
					std::snprintf(to, sizeof(to), "%s", DSTS[c].name);
				hint(UI_TEXT(mx_hint_cell_fmt, "%s  %s\n%s → %s. %s (drag up/down, wheel, double-click for default %s)"),
				     official_name(key.c_str()).c_str(), text.c_str(), SRCS[r].name, to, help ? help : "",
				     xg::format(p, p.def).c_str());
			}
			ImGui::PopID();
		}
		ImGui::PopID();
	}
	ImGui::SetCursorScreenPos(ImVec2(org.x, org.y + head_h + ch * 6.0f));
	ImGui::Dummy(ImVec2(w, 0));
}

// ---- エフェクトの区画（「形」のタブの EG の右。横に送って見る）
//
// slot は設定の窓（fx_editor）と同じ番号: 1-4 がインサーション、5 がリバーブ、6 がコーラス、7 がバリエーション

const char *fx_prefix(int slot)
{
	static const char *const P4[4] = { "insertion1", "insertion2", "insertion3", "insertion4" };
	return slot <= 4 ? P4[std::clamp(slot, 1, 4) - 1] : slot == 5 ? "reverb" : slot == 6 ? "chorus" : "variation";
}

// エフェクトのパラメータの値の字（fx_editor と同じ書き方）
std::string fx_value_text(const xg::fx_param &p, int v)
{
	char buf[24];
	switch (p.fmt) {
	case xg::fx_fmt::table:
		if (p.texts && v >= p.lo && v <= p.hi)
			return p.texts[v - p.lo];
		break;
	case xg::fx_fmt::tenths:
		std::snprintf(buf, sizeof(buf), "%.1f", v / 10.0);
		return buf;
	default:
		break;
	}
	std::snprintf(buf, sizeof(buf), "%d", v);
	return buf;
}

int get_value(xg::model &m, const std::string &key, int part = 0)
{
	const xg::param &p = P(key.c_str());
	int v = p.def;
	m.get(p, part, v);
	return v;
}

// パラメータの番地。インサーションは表のまま、システムエフェクトは xg/sysfx.h で読み替える（fx_editor::where と同じ）
bool fx_where(int slot, const xg::fx_param &fp, u32 &addr, int &size)
{
	if (slot <= 4) {
		addr = xg::pack(0x03, u8(slot - 1), fp.addr);
		size = fp.size;
		return true;
	}
	const xg::sysfx which = slot == 5 ? xg::sysfx::reverb : slot == 6 ? xg::sysfx::chorus : xg::sysfx::variation;
	const int lo = xg::sysfx_addr(which, fp, size);
	if (lo < 0)
		return false;
	addr = xg::pack(0x02, 0x01, u8(lo));
	return true;
}

// 音源のエフェクトの入口・出口の番号（mu2000::scope_fx）
int scope_fx_of(int slot)
{
	return slot <= 4 ? mu2000::SCOPE_INS1 + (slot - 1) : slot == 5 ? mu2000::SCOPE_REV : slot == 6 ? mu2000::SCOPE_CHO : mu2000::SCOPE_VAR;
}

// ワウ（AUTO WAH・TOUCH WAH・WAH+DT+DLY）の CutoffFreq（0-127）→ Hz。
// 番地の表が見つからないので、エミュで**鳴らして測った**（2026-09-23）: インサーションにワウを掛け、
// 雑音（Seashore）を鳴らして入口と出口のスペクトラムの比を取り、山の頂を放物線で読む（LFO と Sensitivity は 0、
// Resonance 12.0、Dry/Wet は全部 wet）。値 4 ごと。AUTO WAH（4E）と TOUCH WAH（52）は同じ値だった。
// 山の形は 2 次（1 オクターブ離れて Q=2 で -9.6 dB。理屈どおり）で、Resonance の表の字がそのまま Q
constexpr float WAH_HZ[33] = {
	  52.7f,   59.1f,   82.2f,   96.4f,  124.7f,  150.0f,  184.6f,  221.6f,
	 268.5f,  328.7f,  405.5f,  488.9f,  582.9f,  688.3f,  800.0f,  943.6f,
	1096.6f, 1279.5f, 1437.8f, 1677.9f, 1949.8f, 2234.8f, 2502.9f, 2820.7f,
	3209.8f, 3545.5f, 4014.7f, 4490.5f, 5027.8f, 5594.1f, 6204.3f, 6977.0f,
	7850.0f,   // 127 の先は 124 までの伸びから延ばした
};

inline float wah_hz(int cut)
{
	const float x = std::clamp(float(cut), 0.0f, 128.0f) / 4.0f;
	const int i = std::min(31, int(x));
	const float t = std::clamp(x - float(i), 0.0f, 1.0f);
	return WAH_HZ[i] * std::pow(WAH_HZ[i + 1] / WAH_HZ[i], t);   // 対数で間を取る
}

// ---- エフェクトの EQ・フィルタの特性（目安）を、スペクトラムと同じ周波数の目盛りで重ねる
//
// パラメータは LCD の名前で見分ける（設定の窓の EQ の絵 fx_editor::eq_graph と同じ読み方）:
//   低域の棚   EQ LowFreq / Low Freq と EQ LowGain / Low Gain
//   中域の山   EQ MidFreq / Mid Freq / EQ Freq と EQ MidGain / Mid Gain / EQ Gain、幅 EQ MidWidt / Mid Width / EQ Width
//   高域の棚   EQHighFreq / High Freq と EQHighGain / High Gain
//   LPF・HPF   LPF Cutoff・DryLPFFreq・HPF Cutoff（Thru は掛けない）。LPF Reso があれば共振の山つきの 2 次
//   クロスオーバー CrsoverFrq（縦の点線と周波数だけ）
//   ワウ       CutoffFreq と Resonance（山は 2 次、Q は Resonance の字。周波数は測った表 WAH_HZ）
// 周波数は表の字（"5.6k" など）から読む。形は eq_curve.h と同じく見た目の目安で、MEG の実際の式ではない
// （LPF・HPF は 6 dB/oct とみる）。ワウは AUTO WAH の LFO・TOUCH WAH の触れぶんで**動く**ので、
// 描くのは止まっているときの位置（つまみの値）。DYNA FLT（切る周波数が音で動き、止まった値が無い）と
// LO-FI の FltrType（音色の型）は描かない。focus は下の段でカーソルが載っている・つまんでいるパラメータ
// （その印を大きく）
void fx_response_overlay(int slot, const xg::fx_def &def, xg::model &m, ImVec2 a, ImVec2 b, const xg::fx_param *focus)
{
	auto find = [&](std::initializer_list<const char *> names) {
		for (int i = 0; i < def.count; i++)
			for (const char *nm : names)
				if (!std::strcmp(def.params[i].label, nm))
					return i;
		return -1;
	};
	auto value = [&](int i, int &v) {
		if (i < 0)
			return false;
		u32 addr = 0;
		int size = 0;
		if (!fx_where(slot, def.params[i], addr, size) || !m.get_raw(addr, size, v))
			return false;
		v = std::clamp(v, int(def.params[i].lo), int(def.params[i].hi));
		return true;
	};
	// 表の字から Hz（Thru・読めない字は false）
	auto hz_of = [&](int i, float &hz) {
		int v = 0;
		if (!value(i, v))
			return false;
		const std::string t = fx_value_text(def.params[i], v);
		char *end = nullptr;
		const double x = std::strtod(t.c_str(), &end);
		if (end == t.c_str() || x <= 0.0)
			return false;
		hz = float(x * (end && *end == 'k' ? 1000.0 : 1.0));
		return true;
	};
	auto number_of = [&](int i, float &x) {
		int v = 0;
		if (!value(i, v))
			return false;
		x = float(std::atof(fx_value_text(def.params[i], v).c_str()));
		return x > 0.0f;
	};

	struct band { eq::shape shape; float hz = 0, db = 0, q = 0.7f; int fi = -1, gi = -1, wi = -1; const char *name; };
	std::vector<band> bands;
	auto add_band = [&](eq::shape sh, std::initializer_list<const char *> fn, std::initializer_list<const char *> gn,
	                    std::initializer_list<const char *> wn, const char *name) {
		band bd;
		bd.shape = sh;
		bd.name = name;
		bd.fi = find(fn);
		bd.gi = find(gn);
		bd.wi = wn.size() ? find(wn) : -1;
		int g = 64;
		if (bd.fi < 0 || bd.gi < 0 || !hz_of(bd.fi, bd.hz) || !value(bd.gi, g))
			return;
		bd.db = float(g - 64);
		float q = 0;
		if (bd.wi >= 0 && number_of(bd.wi, q))
			bd.q = q;
		bands.push_back(bd);
	};
	add_band(eq::shape::low_shelf, { "EQ LowFreq", "Low Freq" }, { "EQ LowGain", "Low Gain" }, {}, "Lo");
	add_band(eq::shape::peak, { "EQ MidFreq", "Mid Freq", "EQ Freq" }, { "EQ MidGain", "Mid Gain", "EQ Gain" },
	         { "EQ MidWidt", "Mid Width", "EQ Width" }, "Mid");
	add_band(eq::shape::high_shelf, { "EQHighFreq", "High Freq" }, { "EQHighGain", "High Gain" }, {}, "Hi");
	const int lpf_i = find({ "LPF Cutoff", "DryLPFFreq" }), hpf_i = find({ "HPF Cutoff" });
	const int reso_i = find({ "LPF Reso" }), xo_i = find({ "CrsoverFrq" });
	float lpf = 0, hpf = 0, xo = 0, reso = 0;
	const bool has_lpf = lpf_i >= 0 && hz_of(lpf_i, lpf);
	const bool has_hpf = hpf_i >= 0 && hz_of(hpf_i, hpf);
	const bool has_xo = xo_i >= 0 && hz_of(xo_i, xo);
	const bool has_reso = reso_i >= 0 && number_of(reso_i, reso);
	// ワウ（切る周波数は測った表、Q は Resonance の字）。AUTO WAH は LFO で**上へ**振れるので、その先まで帯で出す
	// （振れる先の値 = 値 + (127 - 値) x LFO Depth / 127。エミュで測って合わせた。2026-09-23）
	const int wah_i = find({ "CutoffFreq" }), wq_i = find({ "Resonance" }), wd_i = find({ "LFO Depth" });
	const int ws_i = find({ "Sensitivty" });          // TOUCH WAH。触れて上へ（上限は表の上端）
	float wah = 0, wq = 1.0f, wah_top = 0;
	int wah_sens = 0;
	bool has_wah = false;
	if (wah_i >= 0 && wq_i >= 0) {
		int v = 0;
		if (value(wah_i, v)) {
			wah = wah_hz(v);
			has_wah = true;
			float q = 0;
			if (number_of(wq_i, q))
				wq = std::max(0.5f, q);
			int d = 0;
			if (wd_i >= 0 && value(wd_i, d) && d > 0)
				wah_top = wah_hz(int(std::lround(v + (127.0f - float(v)) * float(d) / 127.0f)));
			if (ws_i >= 0 && value(ws_i, wah_sens) && wah_sens > 0)
				wah_top = wah_hz(127);            // 強く弾くと上端まで上がる（どこまで上がるかは弾く強さ次第）
		}
	}
	if (bands.empty() && !has_lpf && !has_hpf && !has_xo && !has_wah)
		return;

	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float x0 = a.x, x1 = b.x, top = a.y + fs * 0.9f, bottom = b.y - fs * 0.7f;
	const float mid = (top + bottom) * 0.5f, half = (bottom - top) * 0.5f;
	const float DB = 18.0f;
	auto x_hz = [&](float f) { return x0 + (x1 - x0) * eq::t_of_hz(std::clamp(f, 20.0f, 20000.0f)); };
	auto y_db = [&](float db) { return mid - half * std::clamp(db, -DB, DB) / DB; };
	auto total_db = [&](float f) {
		float db = 0;
		for (const band &bd : bands)
			db += eq::band_db(bd.shape, bd.db, bd.hz, bd.q, f);
		if (has_lpf) {
			const float r = f / lpf;
			const float m2 = has_reso ? 1.0f / ((1.0f - r * r) * (1.0f - r * r) + (r / reso) * (r / reso))
			                          : 1.0f / (1.0f + r * r);
			db += 10.0f * std::log10(std::max(m2, 1e-9f));
		}
		if (has_hpf) {
			const float r = hpf / f;
			db += 10.0f * std::log10(1.0f / (1.0f + r * r));
		}
		if (has_wah) {
			const float r = f / wah;
			const float x = wq * (r - 1.0f / r);
			db += 20.0f * std::log10(wq / std::sqrt(1.0f + x * x));   // 2 次の山（頂は Q ぶん）
		}
		return db;
	};
	const ImU32 lc = IM_COL32(255, 200, 90, 230), fillc = IM_COL32(255, 200, 90, 40);
	// 0 dB の点線
	for (float x = x0; x < x1; x += fs * 0.6f)
		dl->AddLine(ImVec2(x, mid), ImVec2(std::min(x1, x + fs * 0.3f), mid), IM_COL32(255, 200, 90, 70), 1.0f);
	// 特性（0 dB との間を薄く塗る）
	const int np = std::max(24, int((x1 - x0) / 2.0f));
	std::vector<ImVec2> pts;
	pts.reserve(size_t(np) + 1);
	for (int i = 0; i <= np; i++) {
		const float t = float(i) / float(np);
		pts.push_back(ImVec2(x0 + (x1 - x0) * t, y_db(total_db(eq::hz_of_t(t)))));
	}
	for (size_t i = 1; i < pts.size(); i++)
		dl->AddQuadFilled(ImVec2(pts[i - 1].x, mid), pts[i - 1], pts[i], ImVec2(pts[i].x, mid), fillc);
	dl->AddPolyline(pts.data(), int(pts.size()), lc, 0, std::max(1.5f, fs * 0.1f));
	// 印と字
	const float tfs = fs * 0.6f;
	auto tag = [&](ImVec2 c, const std::string &t, ImU32 col, bool lit) {
		const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(tfs, FLT_MAX, 0.0f, t.c_str());
		const float tx = std::clamp(c.x - ts.x * 0.5f, x0 + 1.0f, x1 - ts.x - 1.0f);
		const float ty = c.y + fs * 0.35f + ts.y < bottom ? c.y + fs * 0.35f : c.y - fs * 0.35f - ts.y;
		dl->AddRectFilled(ImVec2(tx - 2, ty - 1), ImVec2(tx + ts.x + 2, ty + ts.y + 1), IM_COL32(0, 0, 0, 150), 3.0f);
		dl->AddText(ImGui::GetFont(), tfs, ImVec2(tx, ty), lit ? IM_COL32(255, 255, 255, 255) : col, t.c_str());
	};
	auto khz = [](float f) {
		char t[16];
		if (f >= 1000.0f) std::snprintf(t, sizeof(t), f >= 10000.0f ? "%.0fk" : "%.1fk", f / 1000.0f);
		else              std::snprintf(t, sizeof(t), "%.0f", f);
		return std::string(t);
	};
	auto is_focus = [&](int i) { return i >= 0 && focus == &def.params[i]; };
	for (const band &bd : bands) {
		const bool lit = is_focus(bd.fi) || is_focus(bd.gi) || is_focus(bd.wi);
		const ImVec2 c(x_hz(bd.hz), y_db(total_db(bd.hz)));
		const float r = std::max(3.0f, fs * 0.24f) * (lit ? 1.5f : 1.0f);
		dl->AddCircleFilled(c, r, lc);
		dl->AddCircle(c, r, IM_COL32(40, 30, 10, 255), 0, 1.5f);
		tag(c, std::string(bd.name) + " " + khz(bd.hz), lc, lit);
	}
	auto vline = [&](float f, const char *name, int idx) {
		const bool lit = is_focus(idx);
		const float x = x_hz(f);
		const ImU32 c = lit ? IM_COL32(255, 255, 255, 230) : IM_COL32(255, 170, 60, 200);
		for (float y = top; y < bottom; y += fs * 0.4f)
			dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(bottom, y + fs * 0.2f)), c, lit ? 2.0f : 1.2f);
		tag(ImVec2(x, top + fs * 0.1f), std::string(name) + " " + khz(f), c, lit);
	};
	if (has_lpf)
		vline(lpf, "LPF", lpf_i);
	if (has_hpf)
		vline(hpf, "HPF", hpf_i);
	if (has_xo)
		vline(xo, "X-over", xo_i);
	if (has_wah && wah_top > wah * 1.02f) {
		// LFO（AUTO WAH）や触れたぶん（TOUCH WAH）で山が上へ動く範囲
		const bool touch = wah_sens > 0;
		const bool lit = is_focus(touch ? ws_i : wd_i);
		const float xa = x_hz(wah), xb = x_hz(wah_top);
		const int a0 = touch ? 10 + wah_sens * 25 / 127 : 25;
		dl->AddRectFilled(ImVec2(xa, top), ImVec2(xb, bottom), IM_COL32(255, 200, 90, lit ? a0 + 20 : a0));
		for (float y = top; y < bottom; y += fs * 0.4f)
			dl->AddLine(ImVec2(xb, y), ImVec2(xb, std::min(bottom, y + fs * 0.2f)), IM_COL32(255, 200, 90, lit ? 200 : 120), 1.0f);
		char wah_label[64];
		std::snprintf(wah_label, sizeof(wah_label),
		    touch ? UI_TEXT(ps_fx_wah_touch_fmt, "Stronger hit goes to %s") : UI_TEXT(ps_fx_wah_lfo_fmt, "LFO to %s"),
		    khz(wah_top).c_str());
		tag(ImVec2((xa + xb) * 0.5f, top + fs * 0.1f),
		    wah_label, IM_COL32(255, 200, 90, 220), lit);
	}
	if (has_wah) {
		const bool lit = is_focus(wah_i) || is_focus(wq_i);
		const ImVec2 c(x_hz(wah), y_db(total_db(wah)));
		const float r = std::max(3.0f, fs * 0.24f) * (lit ? 1.5f : 1.0f);
		dl->AddCircleFilled(c, r, lc);
		dl->AddCircle(c, r, IM_COL32(40, 30, 10, 255), 0, 1.5f);
		tag(c, "Wah " + khz(wah), lc, lit);
	}
	// 目盛りと断り（右上）
	{
		const char *t = UI_TEXT(ps_fx_eq_title, "EQ/filter (guide)  +/-18 dB");
		const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(tfs, FLT_MAX, 0.0f, t);
		dl->AddText(ImGui::GetFont(), tfs, ImVec2(x0 + 2.0f, b.y - ts.y * 2.3f), IM_COL32(255, 200, 90, 200), t);
	}
}

// エフェクト 1 つ（メゾネット）。上の段に種類の名前と、通したあと（緑）・通す前（灰）のスペクトラム。
// 下の段に種類の選択、設定の窓を開くボタン、つまみ（システムエフェクトは戻りとパンを先に）。
// part_only は、このパートだけの音か（インサーション・インサーション接続のバリエーション）、
// 全パートの送りを混ぜた音か（システムのリバーブ・コーラス・バリエーション）。
// バリエーション（slot 7）は下の段の頭に「このパートのインサーションにする」のチェックを置き、
// 入っていなければこのパートの送り（Var Send）だけを触れるようにする（種類やパラメータは見るだけ）
void fx_cell(int slot, bool part_only, int part, xg::model &m, bridge &br, float w, float h)
{
	const bool is_var = slot == 7;
	const bool var_sys = get_value(m, "variation.connect") == 1;
	const int var_part = get_value(m, "variation.part");
	const bool var_mine = !var_sys && var_part == part;     // このパートのインサーションになっている
	const bool locked = is_var && !var_mine;
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float pad = fs * 0.25f;
	const float split = pos.y + h * overview::MAISON_SPLIT;
	const std::string prefix = fx_prefix(slot);
	const xg::param &ptype = P((prefix + ".type").c_str());
	int type = -1;
	if (!m.get(ptype, 0, type))
		type = -1;
	const int msb = type >= 0 ? type >> 7 : 0;
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);

	// ---- 上の段
	const float line = ImGui::GetTextLineHeight();
	float y = pos.y + pad;
	if (type >= 0)
		fx_icon(dl, ImVec2(pos.x + pad, y), line, msb, IM_COL32(250, 250, 240, 230));
	const std::string name = type >= 0 ? xg::fx_name(type) : std::string("--");
	dl->AddText(ImVec2(pos.x + pad + line * 1.3f, y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
	if (is_var) {
		// 右から [PART] [INS] と、掛かっているパートの字。INS はインサーション接続（切ればシステム接続）、
		// PART はこのパートに掛ける（切れば OFF）。PART はインサーション接続のときだけ意味があるので、そのときだけ触れる
		ImGui::PushFont(nullptr, fs * 0.7f);
		const float sfs = ImGui::GetFontSize();
		float rx = pos.x + w - pad;
		auto toggle = [&](const char *id, const char *text, bool on, bool enabled) {
			const ImVec2 ts = ImGui::CalcTextSize(text);
			const ImVec2 sz(ts.x + sfs * 0.8f, line * 0.9f);
			rx -= sz.x;
			const ImVec2 a(rx, y + (line - sz.y) * 0.5f), b(a.x + sz.x, a.y + sz.y);
			rx -= sfs * 0.3f;
			ImGui::SetCursorScreenPos(a);
			ImGui::BeginDisabled(!enabled);
			const bool clicked = ImGui::InvisibleButton(id, sz);
			const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
			ImGui::EndDisabled();
			const ImU32 fill = on ? (enabled ? IM_COL32(60, 150, 230, 255) : IM_COL32(50, 80, 110, 255))
			                      : (hov && enabled ? IM_COL32(70, 76, 90, 255) : IM_COL32(40, 44, 54, 255));
			dl->AddRectFilled(a, b, fill, 3.0f);
			dl->AddRect(a, b, on ? IM_COL32(150, 200, 255, enabled ? 255 : 120) : IM_COL32(120, 125, 140, enabled ? 200 : 90), 3.0f);
			dl->AddText(ImVec2(a.x + (sz.x - ts.x) * 0.5f, a.y + (sz.y - ts.y) * 0.5f),
			            on ? IM_COL32(255, 255, 255, enabled ? 255 : 150) : IM_COL32(200, 200, 210, enabled ? 220 : 110), text);
			return std::make_pair(clicked && enabled, hov);
		};
		const auto part_sw = toggle("##vpart", "PART", !var_sys && var_part == part, !var_sys);
		if (part_sw.first)
			br.send(m.set(P("variation.part"), 0, var_part == part ? 127 : part));
		if (part_sw.second)
			out_hover_param(P("variation.part"), 0);
		if (part_sw.second)
			hint(UI_TEXT(ps_var_part_hint, "%s\nOn puts the variation on this part (off: on no part). Only applies in insertion (INS) connection; with both INS and PART on, type and parameters are editable here"),
			     official_name("variation.part").c_str());
		const auto ins_sw = toggle("##vins", "INS", !var_sys, true);
		if (ins_sw.first)
			br.send(m.set(P("variation.connect"), 0, var_sys ? 0 : 1));
		if (ins_sw.second)
			out_hover_param(P("variation.connect"), 0);
		if (ins_sw.second)
			hint(UI_TEXT(ps_var_ins_hint, "%s\nOn: insertion connection (whole sound of the played part passes through, splitting into dry plus reverb/chorus sends); off: system connection (collects all parts' Var Send, mixes at return). Only one variation exists, shared with other parts. While INS and PART are not both on, only this part's Send is editable here"),
			     official_name("variation.connect").c_str());
		// 掛かっているパート
		std::string cap;
		if (var_sys)
			cap = UI_TEXT(ps_var_cap_sys, "SYSTEM (all parts' Send)");
		else if (var_part < XG_PARTS + 2)
			cap = "→ " + part_name(var_part);
		else
			cap = "→ OFF";
		const ImVec2 cs = ImGui::CalcTextSize(cap.c_str());
		const ImU32 cc = var_mine ? IM_COL32(150, 230, 190, 255) : IM_COL32(200, 200, 210, 200);
		dl->AddText(ImVec2(rx - cs.x, y + (line - cs.y) * 0.5f), cc, cap.c_str());
		ImGui::PopFont();
	}
	y += line * 1.25f;
	const int fx = scope_fx_of(slot);
	std::string label = part_only ? UI_TEXT(ps_fx_legend_part, "Green: processed  Gray: unprocessed (this part only)") : UI_TEXT(ps_fx_legend_sys, "Green: out  Gray: in (all parts' sends mixed)");
	if (is_var && !var_sys && !var_mine) {
		char other[160];
		if (var_part < XG_PARTS + 2)
			std::snprintf(other, sizeof(other), UI_TEXT(ps_fx_other_part_fmt, "Another part's (%s) insertion. This part's Var Send does nothing"), part_name(var_part).c_str());
		else
			std::snprintf(other, sizeof(other), "%s", UI_TEXT(ps_fx_other_none, "Insertion-connected but on no part. This part's Var Send does nothing"));
		label = other;
	}
	const ImVec2 spec_a(pos.x + pad, y), spec_b(pos.x + w - pad, split - pad);
	overview::spectrum_view(br, part, bridge::scope_src(fx, true), bridge::scope_src(fx, false), 10 + slot,
	                        spec_a, spec_b, label.c_str());
	dl->AddLine(ImVec2(pos.x, split), ImVec2(pos.x + w, split), ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

	// ---- 下の段: 種類と、設定の窓
	ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, split + pad));
	ImGui::BeginDisabled(locked);
	const std::vector<xg::fx_type> &types = slot == 5 ? xg::rev_types() : slot == 6 ? xg::cho_types() : xg::ins_types();
	// 送りの棒を横に置くとき（触れないバリエーション）は、種類の欄を短くして棒の幅を空ける
	ImGui::SetNextItemWidth(locked ? std::min(fs * 11.0f, w * 0.38f) : std::min(fs * 11.0f, w * 0.62f));
	if (begin_fx_combo("##type", type, ImGuiComboFlags_HeightLarge)) {
		int chosen = 0;
		if (fx_type_menu(types, type, chosen)) {
			br.send(m.set(ptype, 0, chosen));
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		out_hover_param(ptype, 0);               // Ctrl＋右クリックで種類を送る
	if (ImGui::IsItemHovered()) {
		const char *th = type >= 0 ? fx_type_help(msb, type & 0x7f) : nullptr;
		hint("%s  %s\n%s", official_name((prefix + ".type").c_str()).c_str(), name.c_str(), th ? th : UI_TEXT(ps_fx_type_fallback, "Effect type"));
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(UI_TEXT(ps_fx_details, "Details")))
		request_fx(slot);
	if (ImGui::IsItemHovered())
		hint("%s", UI_TEXT(ps_fx_details_hint, "Effect settings window\nA window with big knobs and explanations for this effect"));
	ImGui::EndDisabled();
	// このパートのインサーションにしていないバリエーションで触れるのは、このパートの送り（Var Send）だけ。
	// つまみの並びに足すと段が増えて全部が縮むので、種類の欄の右に横長の棒で置く
	if (locked) {
		const xg::param &ps = P("part.variation_send");
		int sv = ps.def;
		const bool known = m.get(ps, part, sv);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(std::max(fs * 3.0f, pos.x + w - pad - ImGui::GetCursorScreenPos().x));
		// インサーション接続（ほかのパートに掛けている・OFF）の間は送りが効かないので、動かせるが色を落とす
		const bool idle = !var_sys;
		if (idle) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
			ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(95, 98, 108, 255));
			ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(120, 124, 136, 255));
		}
		ImGui::BeginDisabled(!known);
		if (ImGui::SliderInt("##varsend", &sv, ps.min, ps.max, "Send %d") && known)
			drag_send(br, m.set(ps, part, sv));
		ImGui::EndDisabled();
		if (idle)
			ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && known)
			out_hover_param(ps, part);
		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			hint(idle ? UI_TEXT(ps_var_send_idle_fmt, "%s  %d (ineffective)\nVariation is insertion-connected, so part sends do nothing. Values stay movable; switching back to system connection (INS off) sends at this value")
			              : UI_TEXT(ps_var_send_hint, "%s  %d\nThis part's send to variation (works in system connection). Drag to change"), official_name("part.variation_send").c_str(), sv);
	}

	// ---- つまみ。入る大きさまで縮める
	struct knob_item { const xg::fx_param *fp; const xg::param *mp; const char *label; bool lock; };
	std::vector<knob_item> items;
	if (slot == 5 || slot == 6) {   // リバーブ・コーラスだけ（バリエーションは Send とパラメータに絞って大きさをそろえる）
		items.push_back({ nullptr, &P((prefix + ".return").c_str()), "Return", locked });
		items.push_back({ nullptr, &P((prefix + ".pan").c_str()), "Pan", locked });
	}
	const xg::fx_def *def = type >= 0 ? xg::fx_find(type) : nullptr;
	if (def)
		for (int i = 0; i < def->count; i++) {
			u32 a = 0;
			int sz = 0;
			if (fx_where(slot, def->params[i], a, sz))
				items.push_back({ &def->params[i], nullptr, def->params[i].label, locked });
		}
	const float kx0 = pos.x + pad, kx1 = pos.x + w - pad;
	const float ky0 = ImGui::GetCursorScreenPos().y + pad, ky1 = pos.y + h - pad;
	// 入る大きさを探す。まずつまみを縮め、それでも入らなければ字ごと縮める
	const int n = int(items.size());
	float kscale = 0.7f, ksize = 0, cw = 0, ch = 0;
	int per_row = 1;
	for (const float sc : { 0.7f, 0.62f, 0.55f, 0.48f }) {
		kscale = sc;
		const float kfs = fs * sc;
		float label_w = 0;
		for (const knob_item &it : items)
			label_w = std::max(label_w, ImGui::GetFont()->CalcTextSizeA(kfs, FLT_MAX, 0.0f, it.label).x);
		bool fits = false;
		for (ksize = kfs * 3.4f;; ksize -= kfs * 0.2f) {
			cw = std::max(ksize + kfs * 1.9f, label_w + kfs * 0.8f);
			ch = ksize + kfs * 2.8f;
			per_row = std::max(1, int((kx1 - kx0) / cw));
			const int rows = (n + per_row - 1) / per_row;
			fits = float(rows) * ch <= ky1 - ky0;
			if (fits || ksize <= kfs * 1.6f)
				break;
		}
		if (fits)
			break;
	}
	ImGui::PushFont(nullptr, fs * kscale);
	const xg::fx_param *focus_fp = nullptr;     // カーソルが載っている・つまんでいるパラメータ（上の段の印を大きく）
	for (int k = 0; k < n; k++) {
		const knob_item &it = items[size_t(k)];
		ImGui::SetCursorScreenPos(ImVec2(kx0 + float(k % per_row) * cw, ky0 + float(k / per_row) * ch));
		char id[8];
		std::snprintf(id, sizeof(id), "k%d", k);
		ImGui::BeginDisabled(it.lock);
		if (it.mp) {
			const int pp = it.mp->where == xg::area::part ? part : 0;
			int v = it.mp->def;
			const bool known = m.get(*it.mp, pp, v);
			const std::string text = known ? xg::format(*it.mp, v) : std::string("--");
			if (fx_editor::knob(id, v, it.mp->min, it.mp->max, ksize, it.label, text.c_str(), false, it.lock) && known)
				drag_send(br, m.set(*it.mp, pp, v));
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && known)
				out_hover_param(*it.mp, pp);          // Ctrl＋右クリックで送る
			if (it.lock && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				hint(UI_TEXT(ps_fx_viewonly_fmt, "%s  %s\nNot this part's insertion, so view-only here (turn on INS and PART above to edit)"),
				     official_name(it.mp->key).c_str(), text.c_str());
			else if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
				const char *help = help_for(it.mp->key);
				hint(UI_TEXT(ps_fx_param_fmt, "%s  %s\n%s (drag up/down, wheel, double-click to type a value)"), official_name(it.mp->key).c_str(), text.c_str(),
				     help ? help : "");
			}
		} else {
			u32 addr = 0;
			int size = 0;
			fx_where(slot, *it.fp, addr, size);
			int v = 0;
			const bool known = m.get_raw(addr, size, v);
			v = std::clamp(v, int(it.fp->lo), int(it.fp->hi));
			const std::string text = known ? fx_value_text(*it.fp, v) : std::string("--");
			if (fx_editor::knob(id, v, it.fp->lo, it.fp->hi, ksize, it.fp->label, text.c_str(), false, it.lock) && known)
				drag_send(br, m.set_raw(addr, size, v));
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && known)
				out_hover_raw(addr, size, it.fp->label);   // Ctrl＋右クリックで送る
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemActive())
				focus_fp = it.fp;
			if (it.lock && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				hint(UI_TEXT(ps_fx_viewonly_fmt, "%s  %s\nNot this part's insertion, so view-only here (turn on INS and PART above to edit)"),
				     it.fp->label, text.c_str());
			else if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
				const char *help = fx_param_help(it.fp->label);
				hint(UI_TEXT(ps_fx_param_fmt, "%s  %s\n%s (drag up/down, wheel, double-click to type a value)"), it.fp->label, text.c_str(),
				     help ? help : UI_TEXT(fxe_no_desc, "(no description yet)"));
			}
		}
		ImGui::EndDisabled();
	}
	if (!def || def->count == 0) {
		ImGui::SetCursorScreenPos(ImVec2(kx0, ky0 + (n ? float((n + per_row - 1) / per_row) * ch : 0.0f)));
		ImGui::TextDisabled("%s", msb == 0 ? UI_TEXT(fxe_noeffect, "NO EFFECT (pick a type to list its knobs here)")
		                          : msb == 0x40 ? UI_TEXT(fxe_thru, "THRU (no parameters)") : UI_TEXT(fxe_no_table, "No parameter table for this type yet"));
	}
	ImGui::PopFont();
	// 上の段のスペクトラムに、EQ・フィルタの特性（目安）を重ねる
	if (def)
		fx_response_overlay(slot, *def, m, spec_a, spec_b, focus_fp);
	ImGui::SetCursorScreenPos(pos);
	ImGui::Dummy(ImVec2(w, h));
}

// ---- つなぎの区画（メゾネット）。上の段に流れの絵、下の段に送りのフェーダーと、つなぎ方の型。
//
// XG のシステムエフェクトは、並びが「バリエーション → コーラス → リバーブ」に決まっていて、
// 前から後ろへの送り（VAR→CHO・V-R・C-R）の量だけを変えられる（後ろから前へは送れない）。
// だから「順序」は、この 3 本を開けるか閉じるかで選ぶ（並列・直列など）
void route_cell(int part, xg::model &m, bridge &br, float w, float h)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float pad = fs * 0.25f;
	const float split = pos.y + h * overview::MAISON_SPLIT;
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
	const bool var_sys = get_value(m, "variation.connect") == 1;
	const int var_part = get_value(m, "variation.part");

	// ---- 流れの絵。段違いに頭文字だけの節を置く: 左に細長い P（パート）、右に細長い O（出力）、
	// 間に V（バリエーション・上）・C（コーラス・中）・R（リバーブ・下）を右下がりに。線は縦横だけ（角は丸める）。
	// 送りは前から後ろ（左上から右下）にしか流れないので、この並びなら順序がそのまま形になる。
	// 線は送り元の色で、量が太さと明るさ。0 でない線には流れる粒を走らせる
	const float ax = pos.x + pad * 2.0f, ay = pos.y + pad * 2.0f, aw = w - pad * 4.0f, ah = split - pad * 4.0f - pos.y;
	struct box { float x0, y0, x1, y1; float cx() const { return (x0 + x1) * 0.5f; } float cy() const { return (y0 + y1) * 0.5f; } };
	const float rail = std::max(fs * 1.0f, aw * 0.045f);
	const float nw = std::clamp(aw * 0.1f, fs * 1.8f, fs * 3.2f);
	auto node_at = [&](float fx, float fy0, float fy1) { const float x = ax + aw * fx - nw * 0.5f; return box{ x, ay + ah * fy0, x + nw, ay + ah * fy1 }; };
	const box NP = { ax, ay, ax + rail, ay + ah }, NO = { ax + aw - rail, ay, ax + aw, ay + ah };
	const box NV = node_at(0.27f, 0.02f, 0.28f), NC = node_at(0.50f, 0.37f, 0.61f), NR = node_at(0.73f, 0.70f, 0.94f);
	auto at = [&](const box &bx, float f) { return bx.y0 + (bx.y1 - bx.y0) * f; };

	// 節の色。パートは金、出力は白っぽく、エフェクトは種類の系統ごと（設定の窓の筐体と同じ分け方）
	auto family = [](int type) -> ImU32 {
		const int msb = type >> 7;
		if (msb == 0)                      return IM_COL32(120, 120, 128, 255);
		if (msb >= 0x01 && msb <= 0x04)    return IM_COL32(95, 150, 255, 255);    // 残響
		if (msb >= 0x05 && msb <= 0x08)    return IM_COL32(60, 205, 195, 255);    // ディレイ
		if (msb >= 0x09 && msb <= 0x0c)    return IM_COL32(110, 140, 255, 255);   // 初期反射
		if ((msb >= 0x41 && msb <= 0x45) || msb == 0x48) return IM_COL32(185, 125, 255, 255);   // 揺れ
		if (msb >= 0x49 && msb <= 0x4b)    return IM_COL32(245, 100, 85, 255);    // 歪み
		if (msb == 0x4c || msb == 0x4d)    return IM_COL32(175, 180, 190, 255);   // EQ
		return IM_COL32(110, 215, 125, 255);
	};
	const int vtype = get_value(m, "variation.type"), ctype = get_value(m, "chorus.type"), rtype = get_value(m, "reverb.type");
	const ImU32 COL_P = IM_COL32(245, 190, 70, 255), COL_O = IM_COL32(170, 185, 210, 255);
	const ImU32 COL_V = family(vtype), COL_C = family(ctype), COL_R = family(rtype);
	auto with_alpha = [](ImU32 c, float a) { return (c & 0x00ffffffu) | (ImU32(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24); };

	// 背景に薄い点の格子
	{
		const float g = fs * 0.9f;
		for (float y = ay; y <= ay + ah; y += g)
			for (float x = ax; x <= ax + aw; x += g)
				dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 1.0f, y + 1.0f), IM_COL32(255, 255, 255, 18));
	}

	struct edge { const char *key; bool part_param; int n; ImVec2 pt[3]; float badge_x; ImU32 col; };
	const float rv = NR.cx() + nw * 0.22f, rc = NR.cx() - nw * 0.22f;
	const float bx_send = (NP.x1 + NV.x0) * 0.5f, bx_ret = NR.x1 + (NO.x0 - NR.x1) * 0.4f;
	const edge EDGES[] = {
		// パートからの送り（札は P と V の間の列にそろえる）
		{ "part.variation_send", true, 2, { { NP.x1, NV.cy() }, { NV.x0, NV.cy() } }, bx_send, COL_P },
		{ "part.chorus_send", true, 2, { { NP.x1, at(NC, 0.62f) }, { NC.x0, at(NC, 0.62f) } }, bx_send, COL_P },
		{ "part.reverb_send", true, 2, { { NP.x1, at(NR, 0.72f) }, { NR.x0, at(NR, 0.72f) } }, bx_send, COL_P },
		{ "part.dry_level", true, 2, { { NP.x1, ay + ah * 0.975f }, { NO.x0, ay + ah * 0.975f } }, bx_send, COL_P },
		// エフェクトからエフェクトへ（右へ出て、下の節の頭へ降りる）
		{ "variation.to_chorus", false, 3, { { NV.x1, at(NV, 0.8f) }, { NC.cx(), at(NV, 0.8f) }, { NC.cx(), NC.y0 } }, (NV.x1 + NC.cx()) * 0.5f, COL_V },
		{ "variation.to_reverb", false, 3, { { NV.x1, at(NV, 0.5f) }, { rv, at(NV, 0.5f) }, { rv, NR.y0 } }, (NC.x1 + rc) * 0.5f, COL_V },
		{ "chorus.to_reverb", false, 3, { { NC.x1, at(NC, 0.72f) }, { rc, at(NC, 0.72f) }, { rc, NR.y0 } }, (NC.x1 + rc) * 0.5f, COL_C },
		// 戻り（右の O へ。札は R と O の間の列にそろえる）
		{ "variation.return", false, 2, { { NV.x1, at(NV, 0.2f) }, { NO.x0, at(NV, 0.2f) } }, bx_ret, COL_V },
		{ "chorus.return", false, 2, { { NC.x1, at(NC, 0.25f) }, { NO.x0, at(NC, 0.25f) } }, bx_ret, COL_C },
		{ "reverb.return", false, 2, { { NR.x1, at(NR, 0.4f) }, { NO.x0, at(NR, 0.4f) } }, bx_ret, COL_R },
	};
	const int NE = int(sizeof(EDGES) / sizeof(EDGES[0]));
	// V→C と C→R は、札が同じ高さに来ないように V→R の札を C の右に寄せてある。V→C と V→R の札の高さは V の中で分けてある

	// 角を丸めた縦横の線の、先頭から d の位置（粒を走らせる）
	auto point_on = [](const edge &E, float d) {
		for (int i = 0; i + 1 < E.n; i++) {
			const ImVec2 p0 = E.pt[i], p1 = E.pt[i + 1];
			const float len = std::fabs(p1.x - p0.x) + std::fabs(p1.y - p0.y);
			if (d <= len || i + 2 == E.n) {
				const float t = len > 0.0f ? std::clamp(d / len, 0.0f, 1.0f) : 0.0f;
				return ImVec2(p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t);
			}
			d -= len;
		}
		return E.pt[E.n - 1];
	};
	auto length_of = [](const edge &E) {
		float L = 0;
		for (int i = 0; i + 1 < E.n; i++)
			L += std::fabs(E.pt[i + 1].x - E.pt[i].x) + std::fabs(E.pt[i + 1].y - E.pt[i].y);
		return L;
	};
	auto stroke = [&](const edge &E, ImU32 c, float thick) {
		const float r = fs * 0.45f;
		dl->PathLineTo(E.pt[0]);
		for (int i = 1; i + 1 < E.n; i++) {
			const ImVec2 c0 = E.pt[i], pa = E.pt[i - 1], pb = E.pt[i + 1];
			const float la = std::fabs(c0.x - pa.x) + std::fabs(c0.y - pa.y), lb = std::fabs(pb.x - c0.x) + std::fabs(pb.y - c0.y);
			const float rr = std::min({ r, la * 0.5f, lb * 0.5f });
			const ImVec2 da((c0.x - pa.x) / std::max(la, 1e-3f), (c0.y - pa.y) / std::max(la, 1e-3f));
			const ImVec2 db((pb.x - c0.x) / std::max(lb, 1e-3f), (pb.y - c0.y) / std::max(lb, 1e-3f));
			dl->PathLineTo(ImVec2(c0.x - da.x * rr, c0.y - da.y * rr));
			dl->PathBezierQuadraticCurveTo(c0, ImVec2(c0.x + db.x * rr, c0.y + db.y * rr), 8);
		}
		dl->PathLineTo(E.pt[E.n - 1]);
		dl->PathStroke(c, 0, thick);
	};

	ImGuiStorage *st = ImGui::GetStateStorage();
	struct badge { ImVec2 c; std::string text; ImU32 col; float t; int e; int v; bool known; bool var_ins; };
	std::vector<badge> badges;
	const float now = float(ImGui::GetTime());
	for (int e = 0; e < NE; e++) {
		const edge &E = EDGES[e];
		const xg::param &p = P(E.key);
		int v = p.def;
		const bool known = m.get(p, E.part_param ? part : 0, v);
		// バリエーションがインサーション接続の間は、パートの Var Send・Dry Level（どのパートも）・V>C・V>R・
		// Var Return は効かない（エミュで測った。XG でもシステム接続のときだけ有効な値）。その線は実際の流れで
		// 太さを固定して札を INS にする: このパートに掛けていれば P→V と V→O が最大・P→O が 0（音は丸ごと
		// バリエーションを通る）、掛けていなければ P→O が最大（乾いた音はいつも最大）・P→V と V→O が 0。
		// V→C・V→R はいつも 0
		const bool mine = var_part == part;
		bool var_ins_edge = false;
		float fixed_t = 0.0f;
		if (!var_sys) {
			switch (e) {
			case 0: var_ins_edge = true; fixed_t = mine ? 1.0f : 0.0f; break;   // Var Send
			case 3: var_ins_edge = true; fixed_t = mine ? 0.0f : 1.0f; break;   // Dry Level
			case 4: case 5: var_ins_edge = true; fixed_t = 0.0f; break;        // V>C・V>R
			case 7: var_ins_edge = true; fixed_t = mine ? 1.0f : 0.0f; break;   // Var Return
			default: break;
			}
		}
		const float t = var_ins_edge ? fixed_t : float(v) / 127.0f;
		const float thick = 1.2f + 2.4f * t;
		if (t > 0.0f) {
			stroke(E, with_alpha(E.col, 0.10f + 0.15f * t), thick * 3.2f);    // にじみ
			stroke(E, with_alpha(E.col, 0.55f + 0.45f * t), thick);
			// 流れる粒
			const float L = length_of(E), gap = fs * 1.6f;
			const float off = std::fmod(now * fs * (2.0f + 4.0f * t), gap);
			for (float d = off; d < L - fs * 0.5f; d += gap)
				dl->AddCircleFilled(point_on(E, d), 1.2f + 1.2f * t, with_alpha(IM_COL32(255, 255, 255, 255), 0.35f + 0.5f * t), 8);
		} else {
			stroke(E, IM_COL32(170, 175, 190, 55), 1.0f);
		}
		// 矢じり（最後の線の向き）
		const ImVec2 tip = E.pt[E.n - 1], from = E.pt[E.n - 2];
		const float hl = fs * (0.3f + 0.12f * t);
		const ImU32 hc = t > 0.0f ? with_alpha(E.col, 0.7f + 0.3f * t) : IM_COL32(170, 175, 190, 70);
		if (from.y == tip.y)
			dl->AddTriangleFilled(tip, ImVec2(tip.x - hl, tip.y - hl * 0.55f), ImVec2(tip.x - hl, tip.y + hl * 0.55f), hc);
		else
			dl->AddTriangleFilled(tip, ImVec2(tip.x - hl * 0.55f, tip.y - hl), ImVec2(tip.x + hl * 0.55f, tip.y - hl), hc);
		char text[16];
		if (var_ins_edge)
			std::snprintf(text, sizeof(text), "INS");
		else if (known)
			std::snprintf(text, sizeof(text), "%d", v);
		else
			std::snprintf(text, sizeof(text), "--");
		badges.push_back({ ImVec2(E.badge_x, E.pt[0].y), text, E.col, t, e, v, known, var_ins_edge });
	}

	// 節
	auto node_face = [&](const box &bx, ImU32 c, bool on, float round) {
		const ImVec2 a(bx.x0, bx.y0), b(bx.x1, bx.y1);
		dl->AddRectFilled(ImVec2(a.x + 1.5f, a.y + 2.5f), ImVec2(b.x + 1.5f, b.y + 2.5f), IM_COL32(0, 0, 0, 90), round);   // 影
		dl->AddRectFilled(a, b, on ? with_alpha(c, 0.28f) : IM_COL32(44, 46, 54, 255), round);
		dl->AddRectFilled(a, ImVec2(b.x, a.y + std::min(b.y - a.y, fs * 0.9f)), on ? with_alpha(c, 0.22f) : IM_COL32(60, 62, 70, 120), round,
		                  ImDrawFlags_RoundCornersTop);                                                                       // 上の照り
		dl->AddRect(a, b, on ? with_alpha(c, 0.95f) : IM_COL32(110, 112, 124, 200), round, 0, 1.5f);
	};
	static const char *const NODE[5] = { "P", "V", "C", "R", "O" };
	// Flow node display names in the UI language (P/V/C/R/O initials stay).
	auto node_name = [](int i) -> const char * {
		switch (i) {
		case 0: return UI_TEXT(ps_node_part, "Part");
		case 1: return UI_TEXT(ps_node_var, "Variation");
		case 2: return UI_TEXT(ps_node_cho, "Chorus");
		case 3: return UI_TEXT(ps_node_rev, "Reverb");
		default: return UI_TEXT(ps_node_out, "Out");
		}
	};
	const int TYPES[5] = { -1, vtype, ctype, rtype, -1 };
	const box *const NODES[5] = { &NP, &NV, &NC, &NR, &NO };
	const ImU32 COLS[5] = { COL_P, COL_V, COL_C, COL_R, COL_O };
	for (int i = 0; i < 5; i++) {
		const box &bx = *NODES[i];
		const bool on = TYPES[i] < 0 || (TYPES[i] >> 7) != 0;
		const bool rail_node = i == 0 || i == 4;
		node_face(bx, COLS[i], on, rail_node ? rail * 0.5f : fs * 0.35f);
		// 頭文字。細長い節は上に寄せる
		const float lfs = rail_node ? fs * 0.85f : fs * 1.15f;
		const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(lfs, FLT_MAX, 0.0f, NODE[i]);
		const ImVec2 tp(bx.cx() - ts.x * 0.5f, rail_node ? bx.y0 + fs * 0.35f : bx.cy() - ts.y * 0.5f);
		const ImU32 tc = on ? IM_COL32(250, 250, 252, 255) : IM_COL32(150, 150, 160, 255);
		dl->AddText(ImGui::GetFont(), lfs, tp, tc, NODE[i]);
		dl->AddText(ImGui::GetFont(), lfs, ImVec2(tp.x + 0.6f, tp.y), tc, NODE[i]);     // 太字の代わり
		if (!on) {                                                                      // NO EFFECT は斜線
			dl->AddLine(ImVec2(bx.x0 + 3.0f, bx.y1 - 3.0f), ImVec2(bx.x1 - 3.0f, bx.y0 + 3.0f), IM_COL32(150, 150, 160, 160), 1.2f);
		}
		std::string sub;
		if (TYPES[i] >= 0) {
			sub = on ? xg::fx_name(TYPES[i]) : std::string("NO EFFECT");
			if (i == 1 && !var_sys) {
				char ins[64];
				std::snprintf(ins, sizeof(ins), UI_TEXT(ps_node_ins_fmt, "(insertion: %s)"),
				              var_part < XG_PARTS + 2 ? part_name(var_part).c_str() : "OFF");
				sub += ins;
			}
		}
		ImGui::SetCursorScreenPos(ImVec2(bx.x0, bx.y0));
		ImGui::PushID(100 + i);
		ImGui::InvisibleButton("##node", ImVec2(bx.x1 - bx.x0, bx.y1 - bx.y0));
		if (ImGui::IsItemHovered())
			hint("%s\n%s", node_name(i), sub.empty() ? (i == 0 ? UI_TEXT(ps_node_part_hint, "This part's sound (after insertion)") : UI_TEXT(ps_node_dry_hint, "Dry plus returns mixed, to master EQ"))
			                                         : sub.c_str());
		ImGui::PopID();
	}

	// 値の札（丸い札）。上下にドラッグ・ホイール・ダブルクリックで 0 と既定を行き来
	for (const badge &bd : badges) {
		const edge &E = EDGES[bd.e];
		const xg::param &p = P(E.key);
		const float tfs = fs * 0.6f;
		const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(tfs, FLT_MAX, 0.0f, bd.text.c_str());
		const float hh = ts.y * 0.5f + 2.0f, hw = std::max(ts.x * 0.5f + fs * 0.3f, hh);
		const ImVec2 r0(bd.c.x - hw, bd.c.y - hh), r1(bd.c.x + hw, bd.c.y + hh);
		ImGui::SetCursorScreenPos(r0);
		ImGui::PushID(bd.e);
		ImGui::InvisibleButton("##edge", ImVec2(r1.x - r0.x, r1.y - r0.y));
		const ImGuiID id = ImGui::GetItemID();
		const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
		const int v = bd.v;
		if (bd.known && !bd.var_ins) {
			int nv = v;
			if (ImGui::IsItemActivated()) {
				st->SetInt(id, v);
				st->SetFloat(id + 1, io.MousePos.y);
			}
			if (act && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				nv = st->GetInt(id, v) + int((st->GetFloat(id + 1, io.MousePos.y) - io.MousePos.y) / 2.0f);
			if (hov) {
				ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
				if (io.MouseWheel != 0.0f)
					nv = v + (io.MouseWheel > 0 ? 1 : -1) * (io.KeyCtrl ? 10 : 1);
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					nv = v ? 0 : (p.def ? p.def : 64);
			}
			nv = std::clamp(nv, p.min, p.max);
			if (nv != v)
				drag_send(br, m.set(p, E.part_param ? part : 0, nv));
		}
		const bool lit = bd.t > 0.0f;
		dl->AddRectFilled(r0, r1, hov || act ? IM_COL32(40, 48, 62, 255) : IM_COL32(18, 21, 28, 240), hh);
		dl->AddRect(r0, r1, lit ? with_alpha(bd.col, hov || act ? 1.0f : 0.8f) : IM_COL32(120, 124, 140, hov || act ? 200 : 110), hh, 0, hov || act ? 1.6f : 1.0f);
		dl->AddText(ImGui::GetFont(), tfs, ImVec2(bd.c.x - ts.x * 0.5f, bd.c.y - ts.y * 0.5f),
		            lit ? IM_COL32(240, 244, 250, 255) : IM_COL32(160, 164, 176, 170), bd.text.c_str());
		if (hov || act) {
			if (bd.var_ins) {
				const bool mine = var_part == part;
				const char *why = "";
				switch (bd.e) {
				case 0: why = mine ? UI_TEXT(ps_why_0_mine, "This part's sound fully passes through variation (Var Send unused)")
				                   : UI_TEXT(ps_why_0_other, "Variation is on another part (or OFF); this part's sound does not pass through"); break;
				case 3: why = mine ? UI_TEXT(ps_why_3_mine, "This part's sound fully passes through variation, so there is no dry sound passing by (Dry Level unused)")
				                   : UI_TEXT(ps_why_3_other, "During insertion connection, Dry Level works on no part; dry sound is always max"); break;
				case 4: case 5: why = UI_TEXT(ps_why_45, "During insertion connection, sends from variation to chorus/reverb do nothing. Sound through variation is sent by the part's Cho Send and Rev Send"); break;
				case 7: why = mine ? UI_TEXT(ps_why_7_mine, "Sound through variation goes straight to output (Var Return unused)")
				                   : UI_TEXT(ps_why_7_other, "Variation does not pass this part's sound"); break;
				}
				hint(UI_TEXT(ps_badge_fixed_fmt, "%s (pinned by insertion connection)\n%s. Adjustable via %s; takes effect when back in system connection. Switch with [INS] in the variation panel"),
				     official_name(EDGES[bd.e].key).c_str(), why, bd.e == 7 ? UI_TEXT(ps_where_dialog, "the settings window (variation Details)") : UI_TEXT(ps_where_fader, "the fader below"));
			}
			else {
				const char *help = help_for(E.key);
				hint(UI_TEXT(ps_badge_fmt, "%s  %d\n%s (drag up/down, wheel, double-click to alternate 0 and default)"), official_name(E.key).c_str(), v, help ? help : "");
			}
		}
		ImGui::PopID();
	}
	dl->AddLine(ImVec2(pos.x, split), ImVec2(pos.x + w, split), ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

	// ---- 下の段: つなぎ方の型（前から後ろへの 3 本だけを変える）と、バリエーションの接続
	ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, split + pad));
	const int vc = get_value(m, "variation.to_chorus"), vr = get_value(m, "variation.to_reverb"), cr = get_value(m, "chorus.to_reverb");
	struct preset { const char *name, *about; int vc, vr, cr; };
	const preset PRESETS[] = {
		{ UI_TEXT(ps_pre_parallel, "Parallel"), UI_TEXT(ps_pre_parallel_about, "Play the 3 separately (all front-to-back sends at 0)"), 0, 0, 0 },
		{ UI_TEXT(ps_pre_vcr, "V-C-R"), UI_TEXT(ps_pre_vcr_about, "Variation out to chorus, chorus out to reverb (series)"), 127, 0, 127 },
		{ UI_TEXT(ps_pre_vr, "V-R"), UI_TEXT(ps_pre_vr_about, "Variation out to reverb only"), 0, 127, 0 },
		{ UI_TEXT(ps_pre_cr, "C-R"), UI_TEXT(ps_pre_cr_about, "Chorus out to reverb only"), 0, 0, 127 },
	};
	ImGui::PushFont(nullptr, fs * 0.8f);
	ImGui::TextDisabled("%s", UI_TEXT(ps_flow_ways, "Flow"));
	for (const preset &pr : PRESETS) {
		// 入らなければ次の行へ
		const float bw = ImGui::CalcTextSize(pr.name).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SameLine();
		if (ImGui::GetCursorScreenPos().x + bw > pos.x + w - pad)
			ImGui::NewLine();
		const bool cur = (vc > 0) == (pr.vc > 0) && (vr > 0) == (pr.vr > 0) && (cr > 0) == (pr.cr > 0);
		if (cur)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
		if (ImGui::SmallButton(pr.name)) {
			br.send(m.set(P("variation.to_chorus"), 0, pr.vc));
			br.send(m.set(P("variation.to_reverb"), 0, pr.vr));
			br.send(m.set(P("chorus.to_reverb"), 0, pr.cr));
		}
		if (cur)
			ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			hint(UI_TEXT(ps_flow_ways_hint, "Flow: %s\n%s. XG order is fixed VAR to CHO to REV, never back to front. Part sends and returns stay as they are"), pr.name, pr.about);
	}
	ImGui::PopFont();
	// 送りのフェーダー。左の 4 本がこのパートから、右の 3 本がエフェクトからエフェクトへ
	static const char *const KEYS[7] = { "part.dry_level", "part.variation_send", "part.chorus_send", "part.reverb_send",
	                                     "variation.to_chorus", "variation.to_reverb", "chorus.to_reverb" };
	static const char *const NAMES[7] = { "Dry", "Var", "Cho", "Rev", "V>C", "V>R", "C>R" };
	const ImVec2 fa = ImGui::GetCursorScreenPos();
	// バリエーションがインサーション接続なら、Dry・Var Send・V>C・V>R は効かない（動かせるが色を落とす）
	const unsigned idle = var_sys ? 0u : (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5);
	const int ff = overview::fader_strip("##sends", KEYS, NAMES, 7, 3, part, m, br,
	                                     ImVec2(w - pad * 2.0f, std::max(fs * 3.0f, pos.y + h - pad - fa.y)), idle);
	if (ff >= 0 && ((idle >> ff) & 1))
		hint("%s（効いていない）\nバリエーションがインサーション接続の間は効かない（XG でもシステム接続のときだけ有効な値）。"
		     "値は動かせて、システム接続に戻すとこの値で効く", official_name(KEYS[ff]).c_str());
	ImGui::SetCursorScreenPos(pos);
	ImGui::Dummy(ImVec2(w, h));
}

// ---- ドラムのタブ。パートモードが DRUMS1-4 のパートで、鍵ごとのセットアップ（3n rr pp）を絵つきで触る。
// エディタのドラムの面で行をダブルクリックすると、このタブがその鍵で開く
int g_drum_held = -1, g_drum_held_slot = -1;   // 「鳴らす」で押さえている鍵と、送った口 × 16 + ch
u64 g_drum_prev[2] = {};                       // 「弾いた鍵を追う」の前のコマの押さえ
bool g_drum_follow = false;

void drum_release(bridge &br)
{
	if (g_drum_held < 0)
		return;
	const u8 off[3] = { u8(0x80 | (g_drum_held_slot & 15)), u8(g_drum_held), 64 };
	br.send_port(g_drum_held_slot / 16, off, 3);
	g_drum_held = -1;
}

// パートモード（08 pp 07）の選択。NORMAL / DRUM / DRUMS1-4。ここで DRUMS の組を付け替える。
// 同じ組を使っているほかのパートがあれば、カーソルを載せると並べる
void part_mode_combo(int part, xg::model &m, const xg_snapshot &ram, bridge &br)
{
	const xg::param &pm = P("part.mode");
	int mode = ram.parts[part][0x07];
	m.get(pm, part, mode);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Part Mode");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.5f);
	if (ImGui::BeginCombo("##pmode", xg::format(pm, mode).c_str())) {
		for (int v = pm.min; v <= pm.max; v++) {
			std::string label = xg::format(pm, v);
			// ほかに同じ組を使っているパート
			if (v >= 2) {
				std::string users;
				for (int p = 0; p < XG_PARTS; p++)
					if (p != part && ram.parts[p][0x07] == v)
						users += (users.empty() ? "" : ", ") + part_name(p);
				if (!users.empty())
					label += "  (" + users + ")";
			}
			if (ImGui::Selectable(label.c_str(), v == mode) && v != mode)
				br.send(m.set(pm, part, v));
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		out_hover_param(pm, part);           // Ctrl＋右クリックでパートモードを送る
	if (ImGui::IsItemHovered())
		hint("Part Mode  %s\n%s", xg::format(pm, mode).c_str(),
		     UI_TEXT(ps_drum_mode_hint, "Which drum setup (DRUMS1-4) this part uses. DRUM (no number) takes no setup. Changing the kit resets the setup it uses"));
}

void drum_tab(int part, xg::model &m, const xg_snapshot &ram, bridge &br, float room_h)
{
	const float fs = ImGui::GetFontSize();
	const ImGuiStyle &st = ImGui::GetStyle();
	const int set = drum_set_of(ram, part);
	if (set < 0) {
		part_mode_combo(part, m, ram, br);
		ImGui::Spacing();
		ImGui::TextWrapped("%s", ram.parts[part][0x07] == 1
		                             ? UI_TEXT(ps_drum_plain, "Part mode DRUM (no number) ignores every drum setup. Set it to DRUMS1-4 to edit its keys here")
		                             : UI_TEXT(ps_drum_not, "This part is not a drum part (part mode DRUMS1-4). Choose a drum kit or set the part mode to DRUMS1-4 to edit its keys here"));
		return;
	}
	const float y0 = ImGui::GetCursorScreenPos().y;
	int key = shape_drum_key();
	const int slot = ram.parts[part][0x04];            // 受信の口 × 16 + ch（127 は OFF）
	// 弾いた鍵を追う: 新しく押さえた鍵のうち、セットアップのある鍵
	if (slot < XG_PARTS) {
		const u64 *now = ram.notes[slot];
		if (g_drum_follow)
			for (int k = XG_DRUM_KEY0; k < XG_DRUM_KEY0 + XG_DRUM_KEYS; k++) {
				const u64 bit = u64(1) << (k & 63);
				if ((now[k >> 6] & bit) && !(g_drum_prev[k >> 6] & bit) && k != g_drum_held) {
					set_shape_drum_key(key = k);
					break;
				}
			}
		g_drum_prev[0] = now[0];
		g_drum_prev[1] = now[1];
	}
	part_mode_combo(part, m, ram, br);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(drum_kit_name(m, part).c_str());
	ImGui::SameLine();
	// 鍵の名前は今のキットの楽器名（ROM の表）。音の無い鍵は番号だけ
	auto key_label = [&](int k) {
		std::string s = drum_key_text(k);
		const std::string g = drum_key_name(m, part, k);
		if (!g.empty())
			s += "  " + g;
		return s;
	};
	ImGui::SetNextItemWidth(fs * 13);
	if (ImGui::BeginCombo("##dkey", key_label(key).c_str(), ImGuiComboFlags_HeightLarge)) {
		for (int k = XG_DRUM_KEY0; k < XG_DRUM_KEY0 + XG_DRUM_KEYS; k++) {
			if (ImGui::Selectable(key_label(k).c_str(), k == key))
				set_shape_drum_key(key = k);
			if (k == key && ImGui::IsWindowAppearing())
				ImGui::SetScrollHereY();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##dprev", ImGuiDir_Left))
		set_shape_drum_key(key = std::max(XG_DRUM_KEY0, key - 1));
	ImGui::SameLine();
	if (ImGui::ArrowButton("##dnext", ImGuiDir_Right))
		set_shape_drum_key(key = std::min(XG_DRUM_KEY0 + XG_DRUM_KEYS - 1, key + 1));
	ImGui::SameLine();
	// 鳴らす（押している間）。受信チャンネルが OFF なら鳴らせない
	ImGui::BeginDisabled(slot >= XG_PARTS);
	ImGui::Button(UI_TEXT(ps_drum_play, "Play"));
	if (ImGui::IsItemActivated() && slot < XG_PARTS) {
		drum_release(br);
		const u8 on[3] = { u8(0x90 | (slot & 15)), u8(key), 100 };
		br.send_port(slot / 16, on, 3);
		g_drum_held = key;
		g_drum_held_slot = slot;
	}
	if (ImGui::IsItemDeactivated())
		drum_release(br);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::Checkbox(UI_TEXT(ps_drum_follow, "Follow played keys"), &g_drum_follow);
	// 鳴らし方の 4 つ（組・重ね方・離し・押し）は数が少ないので、ここに並べる
	{
		const drum_param *dp = drum_params();
		ImGui::SameLine(0, fs * 1.2f);
		int alt = drum_value(ram, set, key, 3);
		ImGui::SetNextItemWidth(fs * 4.5f);
		const std::string at = std::string("Alt ") + drum_value_text(3, alt);
		if (ImGui::DragInt("##alt", &alt, 0.2f, dp[3].lo, dp[3].hi, at.c_str(), ImGuiSliderFlags_AlwaysClamp))
			drum_write(br, set, key, 3, alt);
		if (ImGui::IsItemHovered())
			out_hover_drum(set, key, 3);
		if (ImGui::IsItemHovered())
			hint("Alt  %s\n%s", drum_value_text(3, alt).c_str(), help_for("drum.Alt") ? help_for("drum.Alt") : "");
		for (int i : { 8, 9, 10 }) {
			ImGui::SameLine();
			bool on = drum_value(ram, set, key, i) != 0;
			const std::string label = std::string(dp[i].head) + "##t" + std::to_string(i);
			if (ImGui::Checkbox(label.c_str(), &on))
				drum_write(br, set, key, i, on ? 1 : 0);
			if (ImGui::IsItemHovered()) {
				out_hover_drum(set, key, i);
				const std::string hk = std::string("drum.") + dp[i].head;
				hint("%s  %s\n%s", dp[i].head, drum_value_text(i, on ? 1 : 0).c_str(), help_for(hk.c_str()) ? help_for(hk.c_str()) : "");
			}
		}
	}

	const float h = std::max(fs * 8.0f, room_h - (ImGui::GetCursorScreenPos().y - y0));
	const float w = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * 2.0f) / 3.0f;
	auto box = [&](const char *id, const char *title, auto draw_cell) {
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(st.WindowPadding.x, fs * 0.1f));
		const bool open = ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders,
		                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleVar();
		if (open) {
			ImGui::PushFont(nullptr, fs * 0.8f);
			ImGui::TextUnformatted(title);
			ImGui::PopFont();
			const ImVec2 r = ImGui::GetContentRegionAvail();
			draw_cell(r.x, std::max(fs * 4.0f, r.y));
		}
		ImGui::EndChild();
	};
	box("dmix", UI_TEXT(ps_drum_title_mix, "Pitch, level, pan and sends"),
	    [&](float cw, float ch) { overview::drum_mix_cell(part, set, key, ram, br, cw, ch); });
	ImGui::SameLine();
	box("dfilter", UI_TEXT(ps_drum_title_filter, "Filter and EQ (this key)"),
	    [&](float cw, float ch) { overview::drum_filter_cell(part, set, key, ram, br, cw, ch); });
	ImGui::SameLine();
	box("denv", UI_TEXT(ps_drum_title_env, "EG (this key)"),
	    [&](float cw, float ch) { overview::drum_env_cell(part, set, key, ram, br, cw, ch); });
}

} // namespace


void part_shapes::drum_hidden(bridge &br)
{
	drum_release(br);
}

void part_shapes::draw(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	set_current_ram(&ram);            // 絵が音色の中身を読むため（ピッチ EG など）
	out_begin_frame();                // Ctrl＋右クリックで送るものは、部品がコマごとに名乗り直す
	begin_hint_bar();                 // 絵や名前の説明は、マウスのそばでなく下の帯に出す
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("part_shapes", nullptr, wf);
	ImGui::PopStyleVar();

	// 表示の大きさ。文字も絵も同じ倍率で縮む（editor.ini に覚える）
	float &zoom = shapes_zoom();
	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);

	const float fs = ImGui::GetFontSize();
	int part = shape_window_part();
	int scope = -1;                   // パートの音を拾うか（形のタブのフィルタが絵のときだけ）

	// ---- パートを選ぶ。音色の名前も出す
	ImGui::SetNextItemWidth(fs * 5);
	if (ImGui::BeginCombo("##part", part_name(part).c_str(), ImGuiComboFlags_HeightLarge)) {
		for (int p = 0; p < PARTS; p++) {
			if (p && p % 16 == 0)
				ImGui::Separator();          // 口の境目
			if (ImGui::Selectable(part_name(p).c_str(), p == part))
				set_shape_window_part(part = p);
			if (p == part && ImGui::IsWindowAppearing())
				ImGui::SetScrollHereY();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
		set_shape_window_part(part = (part + PARTS - 1) % PARTS);
	ImGui::SameLine();
	if (ImGui::ArrowButton("##next", ImGuiDir_Right))
		set_shape_window_part(part = (part + 1) % PARTS);
	ImGui::SameLine();
	int msb = 0, lsb = 0, prog = 0;
	std::string voice = "--";
	if (m.get(P("part.bank_msb"), part, msb) && m.get(P("part.bank_lsb"), part, lsb) && m.get(P("part.program"), part, prog)) {
		msb = shown_bank_msb(part, m, msb);      // GS のドラム（issue #52）
		voice = voice_text(msb, lsb, prog);
		if (const xg::voice_rom *vr = voices()) {
			const std::string real = vr->name(ram.parts[part], msb, prog);
			if (!real.empty()) {
				char buf[48];
				std::snprintf(buf, sizeof(buf), "%3d  %s", prog + 1, real.c_str());
				voice = buf;
			}
		}
	}
	ImGui::TextUnformatted(voice.c_str());

	// 送り先（Ctrl＋右クリックで送る先）、表示の大きさと、説明のチェックボックスは右端へ
	if (out_ready()) {
		ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - fs * 34);
		out_port_combo();
	}
	ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - fs * 18);
	if (ImGui::SmallButton("-"))
		set_shapes_zoom(zoom - 0.1f);
	ImGui::SameLine();
	ImGui::Text("%d%%", int(std::lround(zoom * 100)));
	ImGui::SameLine();
	if (ImGui::SmallButton("+"))
		set_shapes_zoom(zoom + 0.1f);
	ImGui::SameLine();
	help_checkbox();

	// ---- 上のペイン: エフェクト、棒、鍵盤
	const ImGuiStyle &st = ImGui::GetStyle();
	{
		// インサーションの行、見出しと棒の行、鍵盤の行
		const float strip_h = overview::part_strip_height() + st.WindowPadding.y * 2.0f + fs * 0.2f;
		if (ImGui::BeginChild("strip", ImVec2(0, strip_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar))
			m_strip.part_strip(part, m, ram, br);
		ImGui::EndChild();
	}

	// ---- 左に音色を選ぶ面、右はタブ: 「形」は 4 つの区画（2 × 2）、「すべて」はパートのパラメータ全部
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	// 音色を選ぶ面は、左に分類・右に音色とバンク違いの 2 列（xgui::program_pane）
	// 前の 6 割。ドラムのタブのとき（キットと 12 文字の楽器名）は少し広く
	const float pane_w = m_drum_tab ? std::min(fs * 18.0f, avail.x * 0.34f) : std::min(fs * 15.6f, avail.x * 0.3f);
	// 下の説明の帯（小さめの字で 3 行）。「説明を出す」を切っていれば帯ごと出さず、その高さを絵に回す
	const bool show_bar = help_on();
	ImGui::PushFont(nullptr, fs * BAR_SCALE);
	const float bar_h = show_bar ? ImGui::GetTextLineHeightWithSpacing() * 3.0f + st.WindowPadding.y * 2.0f + st.ItemSpacing.y : 0.0f;
	ImGui::PopFont();
	const float body_h = std::max(fs * 8.0f, avail.y - bar_h);

	// 音色を選ぶ面は左端に畳める（横幅を形のタブに回す）。面の右の細い取っ手を押すと畳む・開く。
	// 畳んだかどうかは editor.ini に覚える（shapes_knobs の FOLD_BIT）
	const bool folded = shapes_knobs(FOLD_BIT);
	if (!folded) {
		if (ImGui::BeginChild("voicepane", ImVec2(pane_w, body_h)))
		{
			ImGui::PushFont(nullptr, fs * 0.85f);   // 分類・音色・バンク違いの 3 つは小さめの字で
			// ドラムのタブのときは、左にキット・右にいまのキットの鍵ごとの楽器名
			if (m_drum_tab)
				drum_pane(part, m, br);
			else
				program_pane(part, m, &ram, br);
			// 面の上なら、Ctrl＋右クリックで今の音色（バンクセレクトとプログラムチェンジ）を送る
			if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
				out_hover_program(part);
			ImGui::PopFont();
		}
		ImGui::EndChild();
		ImGui::SameLine(0, st.ItemSpacing.x * 0.5f);
	}
	{
		const float hw = folded ? fs * 1.3f : fs * 0.8f;
		const ImVec2 a = ImGui::GetCursorScreenPos();
		const bool pressed = ImGui::InvisibleButton("##fold", ImVec2(hw, body_h));
		const bool hov = ImGui::IsItemHovered();
		if (pressed)
			set_shapes_knobs(FOLD_BIT, !folded);
		if (hov)
			hint("%s", folded ? UI_TEXT(ps_fold_open_hint, "Open the voice panel\nShow category, voice and bank columns on the left")
			            : UI_TEXT(ps_fold_close_hint, "Fold the voice panel\nFold category, voice and bank columns to the left edge, giving the width to the right tabs"));
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const ImVec2 b(a.x + hw, a.y + body_h);
		dl->AddRectFilled(a, b, ImGui::GetColorU32(hov ? ImGuiCol_ButtonHovered : ImGuiCol_FrameBg), fs * 0.25f);
		// 向きの三角（畳んでいれば右向き = 開く、開いていれば左向き = 畳む）
		const float cx = (a.x + b.x) * 0.5f, cy = a.y + fs * 0.9f, t = fs * 0.28f;
		const ImU32 tc = ImGui::GetColorU32(hov ? ImGuiCol_Text : ImGuiCol_TextDisabled);
		if (folded)
			dl->AddTriangleFilled(ImVec2(cx - t * 0.6f, cy - t), ImVec2(cx - t * 0.6f, cy + t), ImVec2(cx + t * 0.7f, cy), tc);
		else
			dl->AddTriangleFilled(ImVec2(cx + t * 0.6f, cy - t), ImVec2(cx + t * 0.6f, cy + t), ImVec2(cx - t * 0.7f, cy), tc);
		// 畳んでいるときは、縦書きで「音色」と今の音色の番号
		if (folded) {
			const char *const chars[] = { UI_TEXT(ps_fold_char1, "V"), UI_TEXT(ps_fold_char2, "o") };
			float y = cy + fs * 1.0f;
			for (const char *c : chars) {
				const ImVec2 ts = ImGui::CalcTextSize(c);
				dl->AddText(ImVec2(cx - ts.x * 0.5f, y), tc, c);
				y += ts.y;
			}
		}
		ImGui::SameLine();
	}

	ImGui::BeginGroup();
	const float top_y = ImGui::GetCursorScreenPos().y;
	const bool was_drum = m_drum_tab;
	m_drum_tab = false;
	if (ImGui::BeginTabBar("right")) {
		if (ImGui::BeginTabItem(UI_TEXT(ps_tab_shape, "Shape"))) {
			scope = part;
			scope = part;
			// 列を横へ並べ、画面に入るのは 3 列ぶん（残りは横に送って見る）。
			// 左に VIB（上）とモジュレーション（下）。フィルタと EQ、EG とピッチ EG、エフェクトは上下 2 段が
			// つながった区画（メゾネット。上の段が絵、下の段がフェーダーやつまみ）。
			// EG の右に、このパートに掛かっているインサーション、バリエーション（いつも。INS と PART の切り替えで
			// 位置が飛ばないように、つねにインサーションの次）、つなぎ（送りと順序）、コーラス・リバーブ（種類が
			// NO EFFECT でなければ、送りが 0 でも出す）。音の流れ（声 → インサーション → バリエーション）と同じ順。
			// ポルタメントは「すべて」のタブにある
			const int cho_type = get_value(m, "chorus.type"), rev_type = get_value(m, "reverb.type");
			const bool var_sys = get_value(m, "variation.connect") == 1;
			struct fx_col { int slot; bool part_only; };
			std::vector<fx_col> inline_fx, sys_fx;
			for (int n = 1; n <= 4; n++)
				if (get_value(m, std::string(fx_prefix(n)) + ".part") == part)
					inline_fx.push_back({ n, true });
			const fx_col var_col = { 7, !var_sys && get_value(m, "variation.part") == part };
			if ((cho_type >> 7) != 0) sys_fx.push_back({ 6, false });
			if ((rev_type >> 7) != 0) sys_fx.push_back({ 5, false });

			const float room_h = body_h - (ImGui::GetCursorScreenPos().y - top_y);
			const float w = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * 2.0f) / 3.0f;
			// 横の送りの棒のぶんを引く（いつもつなぎの列があるので、はみ出す）
			const float h = (room_h - st.ScrollbarSize - st.ItemSpacing.y) * 0.5f;
			const float tall = h * 2.0f + st.ItemSpacing.y;
			ImGui::BeginChild("flow", ImVec2(0, room_h), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
			// **ビブラートとモジュレーションは 1 つの区画にまとめ、ピッチベンドも足した**。
			// 上から「音色そのものの揺れ」「ホイールで足す揺れ」「ベンド」と、
			// 同じ『揺らすもの』が縦に並ぶ
			panel("wobble", UI_TEXT(ps_title_wobble, "Wobble (VIB/MW/BEND)"), w, tall, part, m, br,
			      { "part.vib_rate", "part.vib_depth", "part.vib_delay",
			        "part.mw_lfo_pmod", "part.mw_pitch", "part.mw_filter", "part.mw_amp",
			        "part.mw_lfo_fmod", "part.mw_lfo_amod" }, 0,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::wobble_cell(p, mm, b, pw, ph); },
			      UI_TEXT(ps_about_wobble, "One picture for the wobble. Sideways is the modulation wheel (0 at the left, 127 at the right), up/down is cents. The background wave is the vibrato itself, getting deeper towards the right. The orange dotted line is the voice's own depth (it moves with Vib Depth); where the curve passes it, the wheel starts to win. Below are Rate, Depth, Delay and MW LFO PM, plus the live MW (CC1) and pitch bend. Letting the bend go springs it back to the middle (Ctrl+release keeps it)"));
			ImGui::SameLine();
			panel("filter", UI_TEXT(ps_title_filterenv, "Filter and EQ (FILTER/EQ)"), w, tall, part, m, br,
			      { "part.cutoff", "part.resonance", "part.hpf_cutoff",
			        "part.eq_bass_gain", "part.eq_bass_freq", "part.eq_treble_gain", "part.eq_treble_freq" }, 1,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::filter_cell(p, mm, b, pw, ph, false); },
			      UI_TEXT(ps_about_filterenv, "Brightness. Sideways is real frequency; green is this part's spectrum now. Thick is filter plus part EQ (thin is filter alone, dotted EQ alone). Per voice, EQ right after the filter (before insertion). Faders change Cutoff, Resonance, HPF and the EQ bass/treble gains and frequencies"));
			ImGui::SameLine();
			panel("env", UI_TEXT(ps_title_env, "EG and pitch EG (EG/PEG)"), w, tall, part, m, br,
			      { "part.attack", "part.decay", "part.release",
			        "part.peg_init_level", "part.peg_attack_time", "part.peg_rel_level", "part.peg_rel_time" }, 2,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::env_cell(p, mm, b, pw, ph); },
			      UI_TEXT(ps_about_env, "Volume shape (blue: attack, decay, sustain, release) and pitch movement (orange) on one real-time axis with one release time. Left is volume (dB), right is pitch (cents). Faders change EG Attack, Decay, Release and pitch EG Init, Attack, Rel Lv, Rel Tm. Green background is the spectrum after the EG (horizontal is frequency, before insertion)."));
			// エフェクトの列
			auto fx_panel = [&](const fx_col &c) {
				ImGui::SameLine();
				char id[16], title[64];
				std::snprintf(id, sizeof(id), "fx%d", c.slot);
				if (c.slot <= 4)
					std::snprintf(title, sizeof(title), UI_TEXT(ps_fx_ins_fmt, "Insertion %d (INS %d)"), c.slot, c.slot);
				else if (c.slot == 7)
					std::snprintf(title, sizeof(title), "%s", c.part_only ? UI_TEXT(ps_fx_var_ins, "Variation (VAR, as insertion)") : UI_TEXT(ps_fx_var, "Variation (VAR)"));
				else
					std::snprintf(title, sizeof(title), "%s", c.slot == 6 ? UI_TEXT(ps_fx_cho, "Chorus (CHO)") : UI_TEXT(ps_fx_rev, "Reverb (REV)"));
				const int slot = c.slot;
				const bool only = c.part_only;
				panel(id, title, w, tall, part, m, br, {}, PANEL_FIXED,
				      [slot, only](int p, xg::model &mm, bridge &b, float pw, float ph) { fx_cell(slot, only, p, mm, b, pw, ph); },
				      only ? UI_TEXT(ps_about_fx_part, "The effect on this part. Top: this part's spectrum after (green) and before (gray) at the same scale. Bottom: change type and parameters. Details opens the settings window.")
				            : (slot == 7 ? UI_TEXT(ps_about_fx_var, "Variation. With both [INS] and [PART] on in the header row, it plays on this part and type and parameters are editable here. Otherwise, only this part's send (Var Send) changes at the bar right of the type row (knobs are view-only). Top is output (green) and input (gray) spectra.")
			                             : UI_TEXT(ps_about_fx_sys, "A system effect this part sends to. Top: output (green) and input (gray) spectra mixing all parts' sends. Bottom: change type, return, pan and parameters. Details opens the settings window.")));
			};
			for (const fx_col &c : inline_fx)
				fx_panel(c);
			fx_panel(var_col);
			ImGui::SameLine();
			panel("route", UI_TEXT(ps_title_route, "Routing (sends and order)"), w, tall, part, m, br, {}, PANEL_FIXED,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { route_cell(p, mm, b, pw, ph); },
			      UI_TEXT(ps_about_route, "Which effects this part's sound goes through. Drag the send amounts up/down, wheel, or double-click to change. XG order is fixed VAR to CHO to REV; Flow opens and closes front-to-back sends to pick parallel or series order. Shift plus wheel scrolls sideways."));
			for (const fx_col &c : sys_fx)
				fx_panel(c);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		// ドラムのタブ。エディタのドラムの面からの頼みなら前に出す
		const ImGuiTabItemFlags drum_flags = take_drum_tab() ? ImGuiTabItemFlags_SetSelected : 0;
		if (ImGui::BeginTabItem(UI_TEXT(ps_tab_drum, "Drum"), nullptr, drum_flags)) {
			scope = part;
			m_drum_tab = true;
			drum_tab(part, m, ram, br, body_h - (ImGui::GetCursorScreenPos().y - top_y));
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(UI_TEXT(ps_tab_matrix, "Matrix"))) {
			// 操作子 6 つ × 行き先 6 つ（モジュレーションのマトリクス）
			const float room_h = body_h - (ImGui::GetCursorScreenPos().y - top_y);
			if (ImGui::BeginChild("matrix", ImVec2(0, room_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar)) {
				const ImVec2 r = ImGui::GetContentRegionAvail();
				mod_matrix(part, m, br, r.x, r.y);
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(UI_TEXT(ps_tab_all, "All"))) {
			// エディタのパートの面と同じ組（xgui::PART_GROUPS）を、幅に合わせた列数で並べる
			if (ImGui::BeginChild("all", ImVec2(0, body_h - (ImGui::GetCursorScreenPos().y - top_y)))) {
				const int columns = std::clamp(int(ImGui::GetContentRegionAvail().x / (fs * 16.0f)), 1, 3);
				if (ImGui::BeginTable("groups", columns, ImGuiTableFlags_SizingStretchSame)) {
					int n = 0;
					for (const part_group &g : part_groups()) {
						if (n++ % columns == 0)
							ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::SeparatorText(g.title);
						ImGui::PushItemWidth(-fs * 6.0f);
						for (const char *key : g.keys) {
							if (!key)
								break;
							param_slider(key, part, m, br);
						}
						ImGui::PopItemWidth();
						if (!std::strcmp(g.title, texts().xgui_group_bend))
							bend_now_line(part, m, current_ram());
						ImGui::Spacing();
					}
					ImGui::EndTable();
				}
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	} else {
		m_drum_tab = was_drum;
	}
	ImGui::EndGroup();

	// ---- 説明の帯。カーソルを載せた項目の説明（無ければ使い方のひとこと）。「説明を出す」を切れば出さない
	if (show_bar) {
	if (ImGui::BeginChild("hint", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
		ImGui::PushFont(nullptr, fs * BAR_SCALE);
		const std::string &t = hint_text();
		if (t.empty()) {
			ImGui::TextDisabled("%s", UI_TEXT(ps_hint_bar, "Hover over an item for an explanation here (uncheck Show help to hide this bar)"));
		} else {
			// 1 行目（区画や項目の名前）は色を変える
			const size_t nl = t.find('\n');
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 214, 120, 255));
			ImGui::TextUnformatted(t.c_str(), t.c_str() + (nl == std::string::npos ? t.size() : nl));
			ImGui::PopStyleColor();
			if (nl != std::string::npos)
				ImGui::TextWrapped("%s", t.c_str() + nl + 1);
		}
		ImGui::PopFont();
	}
	ImGui::EndChild();
	}
	end_hint_bar();
	out_end_frame(m, ram, br);
	br.want_scope(scope);

	ImGui::PopFont();
	ImGui::End();
}

} // namespace ui

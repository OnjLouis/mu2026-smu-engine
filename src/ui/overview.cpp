// license:BSD-3-Clause

#include "overview.h"

#include "eq_curve.h"
#include "fx_icons.h"
#include "ui/texts.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "xg/fx_types.h"
#include "xg/ram.h"
#include "voice_shape.h"
#include "spectrum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ui {

using namespace xgui;

namespace {

constexpr int PARTS = XG_PARTS;   // 口 A-D の 64 パート

enum class src { param, exp, mod, bend, hold, vib, filter, eq, eg, ins };

// 絵で触る列（1 マスが広い）
bool wide(src s) { return s == src::vib || s == src::filter || s == src::eq || s == src::eg; }

ImU32 col(ImGuiCol c, float a = 1.0f) { return ImGui::GetColorU32(c, a); }

// 押さえている鍵の色。VEL メーターと同じ
const ImU32 NOTE_ON = IM_COL32(236, 116, 70, 255);

// マスターの鍵盤でのパートの色。パートの数だけ色相で振る（隣のパートが似ないよう 7 つ飛ばし）
ImU32 part_color(int part)
{
	float r, g, b;
	ImGui::ColorConvertHSVtoRGB(float((part * 7) % PARTS) / float(PARTS), 0.75f, 1.0f, r, g, b);
	return IM_COL32(int(r * 255), int(g * 255), int(b * 255), 255);
}

// 鍵盤の上の点が、どの鍵か。黒鍵を先に見る。外なら -1。vel に強さ（下ほど強い）
int key_at(ImVec2 pos, float w, float h, ImVec2 at, int &vel)
{
	const float fs = ImGui::GetFontSize();
	const float pad = fs * 0.2f;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	static const bool BLACK[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
	static const float WHITE_POS[12] = { 0, 0.6f, 1, 1.6f, 2, 3, 3.6f, 4, 4.6f, 5, 5.6f, 6 };
	const float kw = (w - pad * 2) / 75;
	const float left = pos.x + pad;
	if (at.y < top || at.y > bottom || at.x < left || at.x > left + kw * 75)
		return -1;
	const float frac = (at.y - top) / std::max(1.0f, bottom - top);
	vel = std::clamp(int(30 + 97 * frac), 1, 127);
	if (at.y < top + (bottom - top) * 0.6f) {
		for (int note = 0; note < 128; note++) {
			if (!BLACK[note % 12]) continue;
			const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
			if (at.x >= x && at.x < x + kw * 0.8f)
				return note;
		}
	}
	for (int note = 0; note < 128; note++) {
		if (BLACK[note % 12]) continue;
		const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
		if (at.x >= x && at.x < x + kw)
			return note;
	}
	return -1;
}

// 鍵の横の範囲（白鍵なら白鍵の幅、黒鍵なら黒鍵の幅）と、下の端
void key_span(ImVec2 pos, float w, float h, int note, float &x0, float &x1, float &bottom)
{
	const float fs = ImGui::GetFontSize();
	const float pad = fs * 0.2f;
	const float top = pos.y + pad;
	static const bool BLACK[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
	static const float WHITE_POS[12] = { 0, 0.6f, 1, 1.6f, 2, 3, 3.6f, 4, 4.6f, 5, 5.6f, 6 };
	const float kw = (w - pad * 2) / 75;
	x0 = pos.x + pad + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
	x1 = x0 + (BLACK[note % 12] ? kw * 0.8f : kw - 1);
	bottom = BLACK[note % 12] ? top + (pos.y + h - pad - top) * 0.6f : pos.y + h - pad;
}

// 128 鍵の鍵盤。color は鍵ごとの色（0 なら押さえていない）
template <typename F>
void draw_keys(ImDrawList *dl, ImVec2 pos, float w, float h, F color)
{
	const float fs = ImGui::GetFontSize();
	const float pad = fs * 0.2f;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	static const bool BLACK[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
	static const float WHITE_POS[12] = { 0, 0.6f, 1, 1.6f, 2, 3, 3.6f, 4, 4.6f, 5, 5.6f, 6 };
	constexpr int WHITES = 75;                    // 0-127 の白鍵
	const float kw = (w - pad * 2) / WHITES;
	const float left = pos.x + pad;
	for (int note = 0; note < 128; note++) {
		if (BLACK[note % 12]) continue;
		const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
		const ImU32 c = color(note);
		dl->AddRectFilled(ImVec2(x, top), ImVec2(x + kw - 1, bottom), c ? c : IM_COL32(220, 220, 215, 255));
	}
	for (int note = 0; note < 128; note++) {
		if (!BLACK[note % 12]) continue;
		const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
		const ImU32 c = color(note);
		dl->AddRectFilled(ImVec2(x, top), ImVec2(x + kw * 0.8f, top + (bottom - top) * 0.6f), c ? c : IM_COL32(30, 30, 32, 255));
	}
}

} // namespace

// 小さなマスの説明に足す一言。一覧の小さな絵は見るだけで、触るのは大きな窓で
// Compact cells show this instead of the full hint below.
#define BIG_HINT UI_TEXT(ov_bighint, "\nDouble-click to edit in a big window")

// ---- 絵の点と字（パートの音色の窓の VIB・FILTER・EG・ピッチ EG）
//
// 触れる点は黄の芯に赤の縁、触れない点（形の折れ目）は紺。線は、つまみで長さが変わる区間を橙、
// 変わらない区間を青にする。大きな窓では、触れる点のそばに「名前 : 値 (音色の元の値と合わせた実際の量)」
// を出す。実際の量は音色の中身から出す（ui/voice_shape.h。鍵 60・強さ 100。実機と突き合わせた式）
constexpr ImU32 TOUCH_FILL = IM_COL32(255, 225, 40, 255), TOUCH_RING = IM_COL32(225, 30, 25, 255);
constexpr ImU32 FIXED_POINT = IM_COL32(40, 85, 130, 255);
constexpr ImU32 SEG_KNOB = IM_COL32(240, 125, 50, 255), SEG_FIXED = IM_COL32(50, 125, 180, 255);

static void touch_point(ImDrawList *dl, ImVec2 p, float r, bool grabbed)
{
	const float rr = grabbed ? r * 1.4f : r;
	dl->AddCircleFilled(p, rr * 1.35f, TOUCH_RING, 20);
	dl->AddCircleFilled(p, rr, TOUCH_FILL, 20);
}

static void fixed_point(ImDrawList *dl, ImVec2 p, float r)
{
	dl->AddCircleFilled(p, r * 1.1f, FIXED_POINT, 20);
}

// 点のそばの字。上に置けなければ下に。枠（a-b）からはみ出さないよう寄せる
// 文字を避けさせるもの。点（まわりの四角）、線、先に置いた文字の枠
struct label_box { ImVec2 lo, hi; };
struct label_avoid {
	std::vector<label_box> boxes;                 // 点と、置いた文字
	std::vector<std::pair<ImVec2, ImVec2>> segs;  // 線
	void point(ImVec2 p, float r) { boxes.push_back({ ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r) }); }
	void line(ImVec2 p, ImVec2 q) { segs.push_back({ p, q }); }
};

// 線分が四角にかかる長さの目安（線の上を刻んで数える）
static int seg_hits(ImVec2 p, ImVec2 q, ImVec2 lo, ImVec2 hi)
{
	int n = 0;
	for (int i = 0; i <= 24; i++) {
		const float t = float(i) / 24.0f;
		const float x = p.x + (q.x - p.x) * t, y = p.y + (q.y - p.y) * t;
		n += x >= lo.x && x <= hi.x && y >= lo.y && y <= hi.y;
	}
	return n;
}

// 避けるものを渡すと、点のまわりの置き場所（上下、中央・右寄せ・左寄せ、離れる幅）を順に試して、
// 点・線・ほかの文字にいちばんかからないところに置く。置いた枠を避けるものに足す
static void point_label(ImDrawList *dl, ImVec2 p, const char *text, bool above, ImVec2 a, ImVec2 b,
                        label_avoid *avoid = nullptr)
{
	const float fs = ImGui::GetFontSize() * 0.75f;      // 点の字は本文より小さく
	ImFont *font = ImGui::GetFont();
	const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
	shape_value(text);                            // 出せても出せなくても、区画の値の一覧に
	if (ts.x + 4.0f > b.x - a.x || ts.y + 2.0f > b.y - a.y)
		return;                                   // 窓が狭くて入らない字は出さない
	auto clamp_x =[&](float x) { return std::clamp(x, a.x + 2.0f, std::max(a.x + 2.0f, b.x - ts.x - 2.0f)); };
	float x = clamp_x(p.x - ts.x * 0.5f);
	float y = above ? p.y - fs * 0.7f - ts.y : p.y + fs * 0.7f;
	if (y < a.y) y = p.y + fs * 0.7f;
	if (y + ts.y > b.y) y = p.y - fs * 0.7f - ts.y;
	if (avoid) {
		auto cost = [&](float cx, float cy) {
			const ImVec2 lo(cx - 2, cy - 1), hi(cx + ts.x + 2, cy + ts.y + 1);
			if (lo.y < a.y || hi.y > b.y)
				return 1 << 20;
			int c = 0;
			for (const label_box &o : avoid->boxes)
				if (lo.x < o.hi.x && hi.x > o.lo.x && lo.y < o.hi.y && hi.y > o.lo.y)
					c += 1000;
			for (const auto &s : avoid->segs)
				c += seg_hits(s.first, s.second, lo, hi) * 10;
			return c;
		};
		int best = 1 << 30;
		float bx = x, by = y;
		const float xs[3] = { clamp_x(p.x - ts.x * 0.5f), clamp_x(p.x + fs * 0.5f), clamp_x(p.x - ts.x - fs * 0.5f) };
		for (int k = 0; k < 16 && best > 0; k++) {
			const float off = fs * (0.5f + 0.35f * float(k));
			for (int side = 0; side < 2 && best > 0; side++) {
				const bool up = (side == 0) == above;
				const float cy = up ? p.y - off - ts.y : p.y + off;
				for (float cx : xs) {
					const int c = cost(cx, cy) + k + side * 8;   // 近いほどよい。頼まれた側（上か下）を先に
					if (c < best) { best = c; bx = cx; by = cy; }
				}
			}
		}
		if (best >= 1000) {
			return;
		}
		x = bx;
		y = by;
		avoid->boxes.push_back({ ImVec2(x - 2, y - 1), ImVec2(x + ts.x + 2, y + ts.y + 1) });
	}
	dl->AddRectFilled(ImVec2(x - 2, y - 1), ImVec2(x + ts.x + 2, y + ts.y + 1), IM_COL32(0, 0, 0, 150), 3.0f);
	dl->AddText(font, fs, ImVec2(x, y), IM_COL32(245, 245, 235, 255), text);
}

// 今のパートの音色の中身。パートの塊（写し）に、層の値（触った直後の値）を重ねて渡す
struct voice_ctx {
	const u8 *rom = nullptr;
	u32 rec = 0;
	u8 blk[XG_PART_COPY] = {};
};

static bool voice_of(int part, voice_ctx &v)
{
	const xg::voice_rom *vr = voices();
	const xg_snapshot *ram = current_ram();
	v.rom = vr ? vr->data() : nullptr;
	v.rec = (v.rom && ram) ? vr->voice_record(ram->parts[part]) : 0;
	if (!v.rec)
		return false;
	std::memcpy(v.blk, ram->parts[part], sizeof(v.blk));
	return true;
}

// 鳴る要素のうち最初のもの（無ければ 0 番）
template <typename Line>
static const Line &lead_line(const std::vector<Line> &lines)
{
	for (const Line &l : lines)
		if (l.active)
			return l;
	return lines.front();
}

struct overview::column {
	const char *title;
	src from;
	const char *key;      // from が param のとき
};

// 列の並び。前半は Domino の並び（VOL EXP PAN P.BEND MOD HOLD）。後半は音の流れの順に、
// 音色を作るもの（揺れ → フィルタ → 音量の形 → パートの EQ）、インサーション、
// 送り（バリエーション → コーラス → リバーブ。前のものは後ろへも送れる）
static const overview::column COLUMNS[] = {
	{ "VOL",    src::param, "part.volume" },
	{ "EXP",    src::exp,   nullptr },
	{ "PAN",    src::param, "part.pan" },
	{ "P.BEND", src::bend,  nullptr },
	{ "MOD",    src::mod,   nullptr },
	{ "HOLD",   src::hold,  nullptr },
	{ "VIB",    src::vib,   nullptr },          // ビブラートの速さ・深さ・掛かり始めを 1 マスで
	{ "FILTER", src::filter, nullptr },         // カットオフとレゾナンスを 1 マスで（Domino の CUT RESO）
	{ "EG",     src::eg,    nullptr },          // アタック・ディケイ・リリースを 1 マスで
	{ "EQ",     src::eq,    nullptr },          // パートの EQ（低音・高音の周波数とゲイン）を 1 マスで
	{ "INS",    src::ins,   nullptr },          // 掛かっているインサーション
	{ "VAR",    src::param, "part.variation_send" },
	{ "CHO",    src::param, "part.chorus_send" },
	{ "REV",    src::param, "part.reverb_send" },
};
static constexpr int NCOLS = int(sizeof(COLUMNS) / sizeof(COLUMNS[0]));

// マスターの行で、その列に出すもの。無ければ空欄
static const char *master_key(const char *title)
{
	if (!std::strcmp(title, "VOL")) return "system.master_volume";
	if (!std::strcmp(title, "REV")) return "reverb.return";
	if (!std::strcmp(title, "CHO")) return "chorus.return";
	if (!std::strcmp(title, "VAR")) return "variation.return";
	return nullptr;
}


void overview::cell(const column &c, int part, xg::model &m, const xg_snapshot &ram, bridge &br,
                    float w, float h, cell_text *value_out)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();

	if (wide(c.from) || c.from == src::ins) {
		if (part < 0)
			ImGui::Dummy(ImVec2(w, h));
		else if (c.from == src::ins)
			ins_cell(part, m, br, h);
		else {
			if (c.from == src::eg)
				eg_cell(part, m, br, w, h, true);
			else if (c.from == src::filter)
				filter_cell(part, m, br, w, h, true);
			else if (c.from == src::eq)
				eq_cell(part, m, br, w, h, true);
			else
				vib_cell(part, m, br, w, h, true);
			// 小さなマスは見るだけ。ダブルクリックでパートの音色の窓に大きく出して、そこで触る
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				select_part(part);
				request_part(part);
			}
		}
		return;
	}

	// part が -1 ならマスターの行。列ごとに、システムやエフェクトの戻りの値を出す
	const bool master = part < 0;
	src from = c.from;
	const char *key = c.key;
	if (master) {
		key = master_key(c.title);
		from = src::param;
		if (!key) {
			ImGui::Dummy(ImVec2(w, h));
			return;
		}
	}
	const int at = master ? 0 : part;
	const u8 *blk = master ? nullptr : ram.parts[part];

	// バリエーションの接続が INSERTION のとき、送り（パートの VAR）も戻り（マスターの VAR）も
	// 使われない。触れはするが薄く出す
	int conn = 1;
	const bool dim = !std::strcmp(c.title, "VAR") && m.get(P("variation.connect"), 0, conn) && conn == 0;

	// 値と、見せ方
	int v = 0, lo = 0, hi = 127;
	int cc_slot = -1, cc_num = -1;       // EXP・MOD の列: CC を流す先（受信チャンネル）と CC の番号
	int bend_slot = -1;                  // P.BEND の列: ピッチベンドを流す先
	int wheel_step = 1;                  // ホイール 1 目盛りぶん（ベンドは 16384 段なので粗く）
	float drag_px = 200.0f;              // 全域を動かすのに要る画面の幅（ベンドは段が細かいので広く）
	const ImGuiID sent_id = ImGui::GetID(c.title) + ImGuiID(part + 1) * 2;   // 送った値と時刻を覚える所
	bool known = true, bipolar = false, editable = false;
	std::string text;
	const xg::param *p = from == src::param ? &P(key) : nullptr;
	switch (from) {
	case src::param:
		known = m.get(*p, at, v);
		lo = p->min; hi = p->max;
		bipolar = p->how == xg::view::center || p->how == xg::view::pan;
		editable = known;
		text = known ? xg::format(*p, v) : "--";
		break;
	case src::exp:
	case src::mod: {
		// 演奏の値（CC11・CC1）。触ると、そのパートが受けているチャンネルへ CC を流す。
		// RAM の写しは 25ms ごとなので、送ったばかりの間は送った値を出す
		v = blk[from == src::exp ? xg::ram::PART_EXP : xg::ram::PART_MOD] & 0x7f;
		int rcv = 127;
		m.get(P("part.rcv_channel"), part, rcv);
		if (m_saved_rcv[part] >= 0)
			rcv = m_saved_rcv[part];
		cc_slot = rcv >= 0 && rcv < PARTS ? rcv : -1;
		cc_num = from == src::exp ? 11 : 1;
		ImGuiStorage *st = ImGui::GetStateStorage();
		if (ImGui::GetTime() - st->GetFloat(sent_id + 1, -10.0f) < 0.3f)
			v = st->GetInt(sent_id, v);
		editable = cc_slot >= 0;
		text = std::to_string(v);
		break;
	}
	case src::bend: {
		// **入ってきた MIDI から取る**（ui/driver.h）。式だけの口では firmware に
		// ベンドを渡さない（音程はこちらで作る）ので、ワーク RAM の PART_BEND は
		// 動かない。受信チャンネルが分からないパートだけ、RAM の値で代わりにする。
		// **16384 段のまま**扱う（真ん中からの離れ。0 が真ん中）
		int rcv = 127;
		m.get(P("part.rcv_channel"), part, rcv);
		bend_slot = rcv >= 0 && rcv < PARTS ? rcv : -1;
		if (bend_slot >= 0)
			v = ram.bend[bend_slot];
		else {
			// RAM には MSB の半分と、下のバイトの最下位ビットに MSB の残り
			const int msb = (blk[xg::ram::PART_BEND] & 0x3f) * 2 + (blk[xg::ram::PART_BEND + 1] & 1);
			v = (msb - 64) * 128;
		}
		// 送ったばかりの間は送った値を出す（写しは 25ms ごと）
		ImGuiStorage *st = ImGui::GetStateStorage();
		if (ImGui::GetTime() - st->GetFloat(sent_id + 1, -10.0f) < 0.3f)
			v = st->GetInt(sent_id, v);
		lo = -8192; hi = 8191;
		bipolar = true;
		editable = bend_slot >= 0;
		wheel_step = 128;                          // ホイール 1 目盛りで MSB 1 つぶん
		drag_px = 600.0f;                          // 端まで 300px。細かいので緩やかに
		char buf[12];
		std::snprintf(buf, sizeof(buf), "%+d", v);
		text = v == 0 ? "0" : buf;
		break;
	}
	case src::hold: v = blk[xg::ram::PART_HOLD] ? 127 : 0; text = v ? "ON" : "OFF"; break;
	default: break;
	}

	ImGui::PushID(c.title);
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##cell", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();

	int nv = v;
	if (editable) {
		if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
			// 横にも縦にも効く。全域を 200px ほどで（Shift で細かく）
			// a Get*Ref reference goes stale when an insert grows the storage,
			// so take a value and write it back
			ImGuiStorage *st = ImGui::GetStateStorage();
			float acc = st->GetFloat(id, 0.0f);
			acc += (io.MouseDelta.x - io.MouseDelta.y) * float(hi - lo) / (io.KeyShift ? drag_px * 4.0f : drag_px);
			const int step = int(acc);
			if (step) { nv = std::clamp(nv + step, lo, hi); acc -= float(step); }
			st->SetFloat(id, acc);
		}
		if (ImGui::IsItemDeactivated())
			ImGui::GetStateStorage()->SetFloat(id, 0.0f);
		if (hovered && ImGui::GetTime() - m_scrolled_at > 0.5) {
			ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
			if (io.MouseWheel != 0.0f) {
				nv = std::clamp(nv + (io.MouseWheel > 0 ? 1 : -1) * wheel_step * (io.KeyCtrl ? 10 : 1), lo, hi);
				m_wheel_taken = true;
			}
		}
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			ImGui::OpenPopup("##type");
		if (ImGui::BeginPopup("##type")) {
			ImGui::TextDisabled(UI_TEXT(cap_range_fmt, "%s %s (%d-%d)"), master ? "MASTER" : part_name(part).c_str(), p ? p->label : c.title, lo, hi);
			const ImGuiID typed_id = ImGui::GetID("typed");
			ImGuiStorage *st = ImGui::GetStateStorage();
			int typed = st->GetInt(typed_id, v);
			if (ImGui::IsWindowAppearing()) { typed = v; ImGui::SetKeyboardFocusHere(); }
			ImGui::SetNextItemWidth(fs * 6);
			if (ImGui::InputInt("##n", &typed, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
				nv = std::clamp(typed, lo, hi);
				ImGui::CloseCurrentPopup();
			}
			st->SetInt(typed_id, typed);
			ImGui::EndPopup();
		}
		if (nv != v && p) {
			drag_send(br, m.set(*p, at, nv));
			text = xg::format(*p, nv);
		} else if (nv != v && bend_slot >= 0) {
			// **ピッチベンド**（0xE0）。真ん中からの離れを 14bit に戻して流す
			const int raw = std::clamp(nv + 8192, 0, 16383);
			const u8 pb[3] = { u8(0xe0 | (bend_slot & 15)), u8(raw & 0x7f), u8((raw >> 7) & 0x7f) };
			br.send_port(bend_slot / 16, pb, 3);
			ImGuiStorage *st = ImGui::GetStateStorage();
			st->SetInt(sent_id, nv);
			st->SetFloat(sent_id + 1, float(ImGui::GetTime()));
			char buf[12];
			std::snprintf(buf, sizeof(buf), "%+d", nv);
			text = nv == 0 ? "0" : buf;
		} else if (nv != v && cc_num >= 0) {
			const u8 cc[3] = { u8(0xb0 | (cc_slot & 15)), u8(cc_num), u8(nv) };
			br.send_port(cc_slot / 16, cc, 3);
			ImGuiStorage *st = ImGui::GetStateStorage();
			st->SetInt(sent_id, nv);
			st->SetFloat(sent_id + 1, float(ImGui::GetTime()));
			text = std::to_string(nv);
		}
	}

	// 描く。上に棒、下に数
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float pad = fs * 0.2f;
	const float bar_h = std::max(3.0f, fs * 0.45f);
	const ImVec2 b0(pos.x + pad, pos.y + pad);
	// 数を外（見出しの行）に出すときは、棒が高さいっぱいを使う
	const ImVec2 b1(pos.x + w - pad, value_out ? pos.y + h - pad : b0.y + bar_h);
	dl->AddRectFilled(b0, b1, col(ImGuiCol_FrameBg));
	if (known && hi > lo) {
		const float frac = std::clamp(float(nv - lo) / float(hi - lo), 0.0f, 1.0f);
		ImU32 fill = editable ? col(active || hovered ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab)
		                      : IM_COL32(200, 70, 60, 255);
		if (dim)
			fill = col(ImGuiCol_TextDisabled, 0.5f);
		const float x = b0.x + (b1.x - b0.x) * frac;
		if (bipolar) {
			const float mid = (b0.x + b1.x) * 0.5f;
			dl->AddRectFilled(ImVec2(std::min(mid, x) - 1, b0.y), ImVec2(std::max(mid, x) + 1, b1.y), fill);
		} else {
			dl->AddRectFilled(b0, ImVec2(x, b1.y), fill);
		}
	}
	if (hovered)
		dl->AddRect(ImVec2(pos.x + 1, pos.y + 1), ImVec2(pos.x + w - 1, pos.y + h - 1), col(ImGuiCol_Border));
	if (value_out) {
		value_out->text = text;
		value_out->bright = known && !dim;
		value_out->hovered = hovered;
	} else {
		const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
		dl->AddText(ImVec2(pos.x + w - pad - ts.x, b1.y + (pos.y + h - b1.y - ts.y) * 0.5f),
		            known && !dim ? col(ImGuiCol_Text) : col(ImGuiCol_TextDisabled), text.c_str());
	}

	if (hovered && p)
		out_hover_param(*p, at);
	if (hovered && !active) {
		const char *what = master ? p->label : c.title;
		if (dim)
			hint(UI_TEXT(ov_var_off_fmt, "%s  %s\nVariation is connected as INSERTION, so this value is unused"), what, text.c_str());
		else
			hint(editable ? UI_TEXT(ov_slider_fmt, "%s  %s\nDrag sideways or up/down, wheel, or double-click to type a value")
			              : UI_TEXT(ov_slider_ro_fmt, "%s  %s\nPlayed value (display only)"), what, text.c_str());
	}
	ImGui::PopID();
}



namespace {

// INS 列で扱うエフェクト。1-4 がインサーション、5 がバリエーション（接続が INSERTION のとき）
struct fx_slot { int id; const char *mark; ImU32 color; const char *part_key; const char *type_key; };

const fx_slot FX_SLOTS[] = {
	{ 1, "1", IM_COL32(214, 160, 48, 255),  "insertion1.part", "insertion1.type" },
	{ 2, "2", IM_COL32(214, 160, 48, 255),  "insertion2.part", "insertion2.type" },
	{ 3, "3", IM_COL32(214, 160, 48, 255),  "insertion3.part", "insertion3.type" },
	{ 4, "4", IM_COL32(214, 160, 48, 255),  "insertion4.part", "insertion4.type" },
	{ 5, "V", IM_COL32(150, 110, 220, 255), "variation.part",  "variation.type" },
};

// Slot display name in the UI language (the table above stays language-free).
const char *fx_slot_title(const fx_slot &f)
{
	switch (f.id) {
	case 1: return UI_TEXT(ov_ins1, "Insertion 1");
	case 2: return UI_TEXT(ov_ins2, "Insertion 2");
	case 3: return UI_TEXT(ov_ins3, "Insertion 3");
	case 4: return UI_TEXT(ov_ins4, "Insertion 4");
	default: return UI_TEXT(sys_variation, "Variation");
	}
}

constexpr const char *DRAG_FX = "S_MU2000_FX";

// そのエフェクトが今どのパートに掛かっているか。掛かっていなければ -1
int fx_target(const fx_slot &f, xg::model &m)
{
	int who = 127, conn = 1;
	if (!m.get(P(f.part_key), 0, who) || who >= PARTS + 2)
		return -1;
	if (f.id == 5 && (!m.get(P("variation.connect"), 0, conn) || conn != 0))
		return -1;                               // SYSTEM のバリエーションはパートに掛からない
	return who;
}

// エフェクトを別のパートへ（バリエーションは INSERTION にもする）
void fx_move(const fx_slot &f, int part, xg::model &m, bridge &br)
{
	if (f.id == 5)
		br.send(m.set(P("variation.connect"), 0, 0));
	br.send(m.set(P(f.part_key), 0, part));
}

void fx_menu(int part, xg::model &m, bridge &br)
{
	ImGui::TextDisabled(UI_TEXT(ov_fx_for_part_fmt, "Effects on part %s"), part_name(part).c_str());
	ImGui::Separator();
	for (const fx_slot &f : FX_SLOTS) {
		int type = 0;
		const bool has_type = m.get(P(f.type_key), 0, type);
		const int where = fx_target(f, m);
		char label[128];
		if (f.id == 5 && where < 0)
			std::snprintf(label, sizeof(label), UI_TEXT(ov_fx_now_sys_fmt, "%s (now SYSTEM, %s)"), fx_slot_title(f), has_type ? xg::fx_name(type).c_str() : "--");
		else
			std::snprintf(label, sizeof(label), UI_TEXT(ov_fx_now_fmt, "%s (now %s, %s)"), fx_slot_title(f),
			              where >= 0 ? part_name(where).c_str() : "OFF", has_type ? xg::fx_name(type).c_str() : "--");
		if (!ImGui::BeginMenu(label))
			continue;
		if (where == part) {
			if (ImGui::MenuItem(f.id == 5 ? UI_TEXT(ov_unhook_sys, "Remove from this part, back to SYSTEM") : UI_TEXT(ov_unhook, "Remove from this part"))) {
				if (f.id == 5) br.send(m.set(P("variation.connect"), 0, 1));
				else           br.send(m.set(P(f.part_key), 0, 127));
			}
		} else if (ImGui::MenuItem(f.id == 5 ? UI_TEXT(ov_hook_sys, "Insert on this part") : UI_TEXT(ov_hook, "Apply to this part"))) {
			fx_move(f, part, m, br);
		}
		ImGui::Separator();
		ImGui::TextDisabled("%s", UI_TEXT(fx_kind, "Type"));
		int chosen = 0;
		if (fx_type_menu(xg::ins_types(), has_type ? type : -1, chosen))
			br.send(m.set(P(f.type_key), 0, chosen));
		ImGui::EndMenu();
	}
	ImGui::Separator();
	ImGui::TextDisabled("%s", UI_TEXT(ov_drag_note, "Drag the mark onto another part's INS cell to move it.\n"
	                                          "Applying it while still NO EFFECT silences the part."));
}

} // namespace


void overview::ins_cell(int part, xg::model &m, bridge &br, float h, bool names, fx_which which)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();

	struct on_part { const fx_slot *slot; std::string name; int msb; };
	std::vector<on_part> on;
	for (const fx_slot &f : FX_SLOTS) {
		if ((which == fx_which::insertions && f.id == 5) || (which == fx_which::variation && f.id != 5))
			continue;
		int type = 0;
		if (fx_target(f, m) == part && m.get(P(f.type_key), 0, type))
			on.push_back({ &f, xg::fx_name(type), type >> 7 });
	}

	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##ins", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool cell_hovered = ImGui::IsItemHovered();
	// 落とし先。別のパートから印を持ってきたら、そのエフェクトをこのパートへ
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload(DRAG_FX)) {
			const int id = *static_cast<const int *>(pl->Data);
			for (const fx_slot &f : FX_SLOTS)
				if (f.id == id)
					fx_move(f, part, m, br);
		}
		ImGui::EndDragDropTarget();
	}
	if (ImGui::BeginPopupContextItem("fxmenu")) {
		fx_menu(part, m, br);
		ImGui::EndPopup();
	}
	if (cell_hovered && on.empty() && !ImGui::IsDragDropActive())
		ImGui::SetItemTooltip("%s", UI_TEXT(ov_rclick_fx, "Right-click to apply an effect"));

	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
	// 一覧では印（1-4、V）だけを横に並べる。names（パートの音色の窓）なら印の後ろに種類の名前も出し、
	// 幅が足りなければ次の行へ折り返す
	const float line = fs * 1.05f;
	const float bw = fs * 1.0f;
	const float left = pos.x + fs * 0.2f;
	float x = left;
	float y = names ? pos.y + fs * 0.1f : pos.y + (h - fs) * 0.5f;
	if (names && on.empty() && which != fx_which::variation)
		dl->AddText(ImVec2(left, y), col(ImGuiCol_TextDisabled), UI_TEXT(ov_not_fx, "Not applied (right-click to apply)"));
	for (size_t i = 0; i < on.size(); i++) {
		const fx_slot &f = *on[i].slot;
		const std::string &name = on[i].name;
		const float icon_w = fs * 1.25f;
		const float item_w = names ? bw + fs * 0.35f + icon_w + ImGui::CalcTextSize(name.c_str()).x + fs * 0.9f : bw + fs * 0.2f;
		if (names && x > left && x + item_w > pos.x + w) {
			x = left;
			y += line;
		}

		// 印はつかめる（ドラッグで移す）
		ImGui::SetCursorScreenPos(ImVec2(x, y));
		ImGui::PushID(f.id);
		ImGui::InvisibleButton("##fx", ImVec2(names ? item_w - fs * 0.5f : bw, line), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
		if (f.id <= 4 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			request_fx(f.id);                    // 設定の窓を出す
		if (ImGui::BeginDragDropSource()) {
			ImGui::SetDragDropPayload(DRAG_FX, &f.id, sizeof(f.id));
			ImGui::Text(UI_TEXT(ov_move_fmt, "Move %s (%s)"), fx_slot_title(f), name.c_str());
			ImGui::EndDragDropSource();
		}
		ImGui::OpenPopupOnItemClick("fxmenu_badge", ImGuiPopupFlags_MouseButtonRight);
		if (ImGui::BeginPopup("fxmenu_badge")) {
			fx_menu(part, m, br);
			ImGui::EndPopup();
		}
		if (ImGui::IsItemHovered() && !ImGui::IsDragDropActive())
			ImGui::SetItemTooltip(f.id <= 4 ? UI_TEXT(ov_tip_ins_fmt, "%s: %s\nDouble-click for settings, drag to another part, right-click for type and removal")
			                                : UI_TEXT(ov_tip_var_fmt, "%s: %s\nDrag to another part, right-click for type and removal"), fx_slot_title(f), on[i].name.c_str());
		ImGui::PopID();

		dl->AddRectFilled(ImVec2(x, y + 1), ImVec2(x + bw, y + fs), f.color, 3.0f);
		if (hot)
			dl->AddRect(ImVec2(x - 1, y), ImVec2(x + bw + 1, y + fs + 1), col(ImGuiCol_Text), 3.0f);
		const ImVec2 ms = ImGui::CalcTextSize(f.mark);
		dl->AddText(ImVec2(x + (bw - ms.x) * 0.5f, y), IM_COL32(20, 20, 20, 255), f.mark);
		if (names) {
			const ImU32 c = hot ? col(ImGuiCol_SliderGrabActive) : col(ImGuiCol_Text);
			fx_icon(dl, ImVec2(x + bw + fs * 0.3f, y), fs, on[i].msb, c);
			dl->AddText(ImVec2(x + bw + fs * 0.35f + icon_w, y), c, name.c_str());
		}
		x += item_w;
	}
	// 落とせる欄を光らせる
	if (cell_hovered && ImGui::GetDragDropPayload() && ImGui::GetDragDropPayload()->IsDataType(DRAG_FX))
		dl->AddRect(ImVec2(pos.x + 1, pos.y + 1), ImVec2(pos.x + w - 1, pos.y + h - 1), col(ImGuiCol_DragDropTarget), 3.0f, 0, 2.0f);
	dl->PopClipRect();
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
	ImGui::Dummy(ImVec2(0, 0));
}


// ピッチ EG の 1 マス。横は時間、縦は音程で、真ん中の横線が本来の音程。
//   押した瞬間はイニシャルレベルの高さから始まり、アタックの時間で本来の音程へ移る。
//   離したところ（縦の点線）からリリースの時間でリリースレベルの高さへ移る。
// 3 つの点をつまむ（大きな窓だけ）:
//   左の点（出だし）      縦でイニシャルレベル
//   真ん中の点            横でアタックの時間
//   右の点（リリースの先）横でリリースの時間、縦でリリースレベル
// XG の値は音色の元の値に対する増減（64 が音色のまま）。高さは値に比例、長さは EG と同じ
// 2 の (値 - 64) / 24 乗で伸び縮みさせた見た目で、実際の半音や秒数ではない
void overview::peg_small(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const xg::param &pi = P("part.peg_init_level"), &pa = P("part.peg_attack_time");
	const xg::param &pl = P("part.peg_rel_level"), &pr = P("part.peg_rel_time");
	int vi = 64, va = 64, vl = 64, vr = 64;
	const bool known = m.get(pi, part, vi) && m.get(pa, part, va) && m.get(pl, part, vl) && m.get(pr, part, vr);

	ImGui::PushID("peg");
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##peg", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = !compact && ImGui::IsItemActive();   // 一覧の小さなマスでは触らせない

	const float pad = fs * 0.25f;
	const float x0 = pos.x + pad, x1 = pos.x + w - pad;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	const float mid = (top + bottom) * 0.5f;
	const float half = (bottom - top) * 0.5f * 0.92f;
	auto y_of = [&](int v) { return mid - half * float(v - 64) / 64.0f; };
	auto v_of = [&](float y) { return int(std::lround(64 - (y - mid) / half * 64.0f)); };
	const float unit = (x1 - x0) / 4.0f;                    // 真ん中の値のときの 1 区間
	auto len = [&](int v) { return unit * 0.5f * std::pow(2.0f, float(v - 64) / 24.0f); };
	const float hold = unit;                                 // 押している間（固定）

	float la = len(va), lr = len(vr);
	const float total = la + hold + lr + unit * 0.3f;
	const float squeeze = total > (x1 - x0) ? (x1 - x0) / total : 1.0f;
	const float xa = x0 + la * squeeze;
	const float xs = xa + hold * squeeze;
	const float xr = xs + lr * squeeze;
	const float yi = y_of(vi), yl = y_of(vl);

	// つかむ点。押した瞬間に一番近い点を選び、離すまで同じ点を動かす
	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(id, -1);
	if (active && ImGui::IsItemActivated() && known) {
		const ImVec2 mp = io.MousePos;
		const float dists[3] = { std::hypot(mp.x - x0, mp.y - yi), std::hypot(mp.x - xa, mp.y - mid),
		                         std::hypot(mp.x - xr, mp.y - yl) };
		grab = int(std::min_element(dists, dists + 3) - dists);
	}
	if (!active)
		grab = -1;
	st->SetInt(id, grab);
	if (active && known && grab >= 0 && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
		auto time_of = [&](float length) {
			return int(std::lround(64 + 24 * std::log2(std::max(1.0f, length) / (unit * 0.5f))));
		};
		auto send = [&](const xg::param &p, int v, int nv) {
			nv = std::clamp(nv, p.min, p.max);
			if (nv != v)
				drag_send(br, m.set(p, part, nv));
		};
		const ImVec2 mp = io.MousePos;
		if (grab == 0)
			send(pi, vi, v_of(mp.y));
		if (grab == 1)
			send(pa, va, time_of((mp.x - x0) / squeeze));
		if (grab == 2) {
			send(pr, vr, time_of((mp.x - xs) / squeeze));
			send(pl, vl, v_of(mp.y));
		}
	}

	// 描く
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	if (known) {
		const ImU32 guide = col(ImGuiCol_TextDisabled, 0.35f);
		dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), guide);                       // 本来の音程
		for (float y = top; y < bottom; y += fs * 0.5f)                             // 鍵盤を離したところ
			dl->AddLine(ImVec2(xs, y), ImVec2(xs, std::min(bottom, y + fs * 0.25f)), guide);
		const ImVec2 pts[] = { { x0, yi }, { xa, mid }, { xs, mid }, { xr, yl }, { x1, yl } };
		// アタック・リリースの区間が橙、押している間と離し終えた後が青
		const float th = std::max(1.5f, fs * 0.1f);
		for (int i = 0; i < 4; i++)
			dl->AddLine(pts[i], pts[i + 1], (i == 0 || i == 2) ? SEG_KNOB : SEG_FIXED, th);
		const float r = std::max(2.5f, fs * 0.22f);
		fixed_point(dl, pts[2], r);
		const ImVec2 handles[] = { pts[0], pts[1], pts[3] };
		for (int i = 0; i < 3; i++)
			touch_point(dl, handles[i], r, i == grab);
		// 字（大きな窓だけ）。音程と時間は音色の元の値と合わせた実際のもの
		voice_ctx v;
		if (!compact && voice_of(part, v)) {
			v.blk[0x62] = u8(vi); v.blk[0x63] = u8(va); v.blk[0x64] = u8(vl); v.blk[0x65] = u8(vr);
			const std::vector<shape::peg_line> ls = shape::peg_lines(v.rom, v.rec, v.blk, 0.0f);
			if (!ls.empty()) {
				const shape::peg_line &L = lead_line(ls);
				// 行き着くまで: 押してから、離す前の最後の点まで
				float atk_ms = 0;
				for (const shape::pt &p : L.pts)
					if (p.ms <= L.keyoff_ms)
						atk_ms = p.ms;
				const float rel_ms = L.pts.back().ms - L.keyoff_ms;
				char s[80];
				const ImVec2 a(pos.x, pos.y), b(pos.x + w, pos.y + h);
				std::snprintf(s, sizeof(s), "Init : %s (%+.0f cent)", xg::format(pi, vi).c_str(), L.pts.front().cents);
				label_avoid boxes;
				for (int i = 0; i < 5; i++)
					boxes.point(pts[i], r + 2.0f);
				for (int i = 0; i < 4; i++)
					boxes.line(pts[i], pts[i + 1]);
				point_label(dl, pts[0], s, yi > mid, a, b, &boxes);
				std::snprintf(s, sizeof(s), "Attack : %s (%.0f ms)", xg::format(pa, va).c_str(), atk_ms);
				point_label(dl, pts[1], s, true, a, b, &boxes);
				std::snprintf(s, sizeof(s), "Release : %s / %s (%.0f ms, %+.0f cent)", xg::format(pr, vr).c_str(),
				              xg::format(pl, vl).c_str(), rel_ms, L.pts.back().cents);
				if (ImGui::CalcTextSize(s).x * 0.75f > w - fs * 0.5f)   // 狭い窓では詰めて書く（点の字は 0.75 倍）
					std::snprintf(s, sizeof(s), "Rel %s/%s (%.0fms %+.0fc)", xg::format(pr, vr).c_str(),
					              xg::format(pl, vl).c_str(), rel_ms, L.pts.back().cents);
				point_label(dl, pts[3], s, true, a, b, &boxes);     // 離しは点の上に
			}
		}
	} else {
		const ImVec2 ts = ImGui::CalcTextSize("--");
		dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), col(ImGuiCol_TextDisabled), "--");
	}

	if ((hovered || active) && known)
		hint("PITCH EG INITIAL LEVEL %s ／ ATTACK TIME %s ／ RELEASE LEVEL %s ／ RELEASE TIME %s%s",
		                      xg::format(pi, vi).c_str(), xg::format(pa, va).c_str(),
		                      xg::format(pl, vl).c_str(), xg::format(pr, vr).c_str(),
		                      compact ? BIG_HINT : UI_TEXT(ov_peg_hint, "\nLeft dot: up/down for the starting pitch\nMiddle dot: sideways for the attack time\n"
		                                                                "Right dot: sideways for the release time, up/down for the release level"));
	ImGui::PopID();
}


// EG の 1 マス。音量の形（立ち上がり → 落ち着き → 伸ばし → 離して消える）を折れ線で描き、
// 3 つの点をつまんで横に動かすと、アタック・ディケイ・リリースが変わる（大きな窓だけ。compact なら見るだけ）。
// XG の値は音色の元の値に対する増減（64 が音色のまま）。形の長さは 2 の (値 - 64) / 24 乗で伸び縮みさせ、
// 真ん中の値で各区間が同じくらいの長さになるようにした（見た目だけ。実際の秒数ではない）
void overview::eg_small(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const xg::param &pa = P("part.attack"), &pd = P("part.decay"), &pr = P("part.release");
	int va = 64, vd = 64, vr = 64;
	const bool known = m.get(pa, part, va) && m.get(pd, part, vd) && m.get(pr, part, vr);

	ImGui::PushID("eg");
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##eg", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = !compact && ImGui::IsItemActive();   // 一覧の小さなマスでは触らせない

	const float pad = fs * 0.25f;
	const float x0 = pos.x + pad, x1 = pos.x + w - pad;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	const float sustain_y = top + (bottom - top) * 0.45f;
	const float unit = (x1 - x0) / 4.0f;                    // 真ん中の値のときの 1 区間
	auto len = [&](int v) { return unit * 0.5f * std::pow(2.0f, float(v - 64) / 24.0f); };
	const float hold = unit * 0.5f;                          // 伸ばしている間（固定）

	// 3 つの点の位置。はみ出すときは全体を縮める
	float la = len(va), ld = len(vd), lr = len(vr);
	const float total = la + ld + hold + lr;
	const float squeeze = total > (x1 - x0) ? (x1 - x0) / total : 1.0f;
	const float xa = x0 + la * squeeze;
	const float xd = xa + ld * squeeze;
	const float xs = xd + hold * squeeze;
	const float xr = xs + lr * squeeze;

	// つかむ点。押した瞬間に一番近い点を選び、離すまで同じ点を動かす
	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(id, -1);
	if (active && ImGui::IsItemActivated() && known) {
		const float mx = io.MousePos.x;
		const float dists[3] = { std::fabs(mx - xa), std::fabs(mx - xd), std::fabs(mx - xr) };
		grab = int(std::min_element(dists, dists + 3) - dists);
	}
	if (!active)
		grab = -1;
	st->SetInt(id, grab);
	if (active && known && grab >= 0 && io.MouseDelta.x != 0.0f) {
		// 動かした幅を値に直す。2 倍の長さが 24 目盛り
		auto apply = [&](const xg::param &p, int v, float from, float to_len) {
			const float cur = std::max(1.0f, from);
			const float want = std::max(1.0f, to_len);
			int nv = std::clamp(int(std::lround(64 + 24 * std::log2(want / (unit * 0.5f)))), p.min, p.max);
			(void)cur;
			if (nv != v)
				drag_send(br, m.set(p, part, nv));
		};
		const float mx = io.MousePos.x;
		if (grab == 0) apply(pa, va, la, (mx - x0) / squeeze);
		if (grab == 1) apply(pd, vd, ld, (mx - xa) / squeeze);
		if (grab == 2) apply(pr, vr, lr, (mx - xs) / squeeze);
	}

	// 描く
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	if (known) {
		const ImVec2 pts[] = { { x0, bottom }, { xa, top }, { xd, sustain_y }, { xs, sustain_y }, { xr, bottom } };
		// 面を薄く塗ってから線。立ち上がり・ディケイ・リリースの区間が橙、伸ばしている間が青
		dl->PathClear();
		for (const ImVec2 &p : pts) dl->PathLineTo(p);
		dl->PathFillConcave(col(ImGuiCol_SliderGrab, 0.25f));
		const float th = std::max(1.5f, fs * 0.1f);
		for (int i = 0; i < 4; i++)
			dl->AddLine(pts[i], pts[i + 1], i == 2 ? SEG_FIXED : SEG_KNOB, th);
		const float r = std::max(2.5f, fs * 0.22f);
		fixed_point(dl, pts[0], r);
		fixed_point(dl, pts[3], r);
		const ImVec2 handles[] = { pts[1], pts[2], pts[4] };
		for (int i = 0; i < 3; i++)
			touch_point(dl, handles[i], r, i == grab);
		// 字（大きな窓だけ）。時間は音色の元の値と合わせた実際のもの
		voice_ctx v;
		if (!compact && voice_of(part, v)) {
			v.blk[0x1a] = u8(va); v.blk[0x1b] = u8(vd); v.blk[0x1c] = u8(vr);
			const std::vector<shape::amp_line> ls = shape::amp_lines(v.rom, v.rec, v.blk, 300.0f);
			if (!ls.empty()) {
				size_t li = 0;
				while (li + 1 < ls.size() && !ls[li].active)
					li++;
				const shape::amp_line &L = ls[li];
				// ディケイ: 立ち上がりの後、伸ばしている音量に落ち着くまで（離さずに回す。2 秒で打ち切り）
				const shape::amp_line held = shape::amp_run(v.rom, xg::nv::element(v.rom, v.rec, int(li)), v.blk, -1.0f, L.attack_ms + 2000.0f);   // 立ち上がりの後 2 秒まで
				const float dec_end = held.pts.back().ms;
				float rel_ms = 0;
				for (const shape::pt &p : L.pts)
					if (p.ms > L.keyoff_ms) {
						rel_ms = p.ms - L.keyoff_ms;
						if (p.cents <= -60.0f)
							break;
					}
				const float dec_ms = std::max(0.0f, dec_end - L.attack_ms);
				char s[64];
				const ImVec2 a(pos.x, pos.y), b(pos.x + w, pos.y + h);
				label_avoid boxes;
				for (int i = 0; i < 5; i++)
					boxes.point(pts[i], r + 2.0f);
				for (int i = 0; i < 4; i++)
					boxes.line(pts[i], pts[i + 1]);
				std::snprintf(s, sizeof(s), "Attack : %s (%.0f ms)", xg::format(pa, va).c_str(), L.attack_ms);
				point_label(dl, pts[1], s, false, a, b, &boxes);
				std::snprintf(s, sizeof(s), dec_end >= L.attack_ms + 1990.0f ? "Decay : %s (2000+ ms)" : "Decay : %s (%.0f ms)",
				              xg::format(pd, vd).c_str(), dec_ms);
				point_label(dl, pts[2], s, false, a, b, &boxes);
				std::snprintf(s, sizeof(s), "Release : %s (%.0f ms)", xg::format(pr, vr).c_str(), rel_ms);
				point_label(dl, pts[4], s, true, a, b, &boxes);
			}
		}
	} else {
		const ImVec2 ts = ImGui::CalcTextSize("--");
		dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), col(ImGuiCol_TextDisabled), "--");
	}

	if ((hovered || active) && known)
		hint("EG ATTACK TIME %s ／ EG DECAY TIME %s ／ EG RELEASE TIME %s%s",
		                      xg::format(pa, va).c_str(), xg::format(pd, vd).c_str(), xg::format(pr, vr).c_str(),
		                      compact ? BIG_HINT : UI_TEXT(ov_porta_hint, "\nDrag a dot sideways (right for longer, left for shorter)"));
	ImGui::PopID();
}

// フィルタの 1 マス。低い音から高い音への通り方（2 次のローパスと 2 次のハイパスを重ねたもの）を描き、
// 2 つの点をつまむ（大きな窓だけ）。押した瞬間に近いほうの点をつかむ。
//   ローパスの点  横でカットオフ、縦でレゾナンス（山の高さ）
//   ハイパスの点  横でカットオフ（左の低いところにあり、右へ動かすと低音が削れる）
// 横軸は値に比例で、1 マスの幅が 8 オクターブ。ローパスは 64 が真ん中（音色のまま）。
// ハイパスは 64 を左の端の近く（真ん中から 3.4 オクターブ下）に置き、同じ目盛りで動かす。
// ハイパスにレゾナンスは効かない（Q は 0.707 の平ら）。
// 点の高さは、カットオフでの持ち上がり 20log10(Q) dB。Q = 2 の (値 - 64) / 16 乗なので、
// 高さも値に比例する。どれも見た目だけで、実際の周波数や Q ではない
void overview::filter_small(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const xg::param &pc = P("part.cutoff"), &pq = P("part.resonance"), &ph = P("part.hpf_cutoff");
	int vc = 64, vq = 64, vh = 64;
	const bool known = m.get(pc, part, vc) && m.get(pq, part, vq);
	const bool known_h = m.get(ph, part, vh);

	ImGui::PushID("filter");
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##filter", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = !compact && ImGui::IsItemActive();   // 一覧の小さなマスでは触らせない

	const float pad = fs * 0.25f;
	const float x0 = pos.x + pad, x1 = pos.x + w - pad;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	const float DB_TOP = 26.0f, DB_BOTTOM = -30.0f;
	auto y_of = [&](float db) { return top + (bottom - top) * (DB_TOP - std::clamp(db, DB_BOTTOM, DB_TOP)) / (DB_TOP - DB_BOTTOM); };
	auto db_of_value = [](int v) { return 6.0206f * float(v - 64) / 16.0f; };
	const float y0db = y_of(0.0f);
	const float span = x1 - x0;
	const float HPF_BASE = 64.0f - 0.425f * 127.0f;          // ハイパスの 64 を置く目盛り（真ん中から 3.4 オクターブ下）
	const float xc = x0 + span * float(vc) / 127.0f;
	const float yq = y_of(db_of_value(vq));
	const float xh = x0 + span * (float(vh) - 64.0f + HPF_BASE) / 127.0f;
	const float yh = y_of(-3.0103f);

	// つかんだときの、点とマウスのずれと、どちらの点か（0 ローパス、1 ハイパス）を覚えておく
	// a Get*Ref reference goes stale when an insert grows the storage, so take
	// a value and write it back
	ImGuiStorage *st = ImGui::GetStateStorage();
	float gx = st->GetFloat(id, 0.0f), gy = st->GetFloat(id + 1, 0.0f);
	int grab = st->GetInt(id + 2, 0);
	if (active && ImGui::IsItemActivated() && known) {
		const float dc = std::hypot(io.MousePos.x - xc, io.MousePos.y - yq);
		const float dh = std::hypot(io.MousePos.x - xh, io.MousePos.y - yh);
		grab = known_h && dh < dc ? 1 : 0;
		gx = (grab ? xh : xc) - io.MousePos.x;
		gy = (grab ? yh : yq) - io.MousePos.y;
		st->SetFloat(id, gx);
		st->SetFloat(id + 1, gy);
		st->SetInt(id + 2, grab);
	}
	if (active && known && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
		const float fx = io.MousePos.x + gx, fy = io.MousePos.y + gy;
		if (grab == 1) {
			const int nh = std::clamp(int(std::lround((fx - x0) / span * 127.0f - HPF_BASE + 64.0f)), ph.min, ph.max);
			if (nh != vh)
				drag_send(br, m.set(ph, part, nh));
		} else {
			const int nc = std::clamp(int(std::lround((fx - x0) / span * 127.0f)), pc.min, pc.max);
			const float db = DB_TOP - (fy - top) / (bottom - top) * (DB_TOP - DB_BOTTOM);
			const int nq = std::clamp(int(std::lround(64 + db * 16.0f / 6.0206f)), pq.min, pq.max);
			if (nc != vc)
				drag_send(br, m.set(pc, part, nc));
			if (nq != vq)
				drag_send(br, m.set(pq, part, nq));
		}
	}

	// 描く
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);

	if (known) {
		// 目安の線。0 dB と、音色のままのカットオフ
		const ImU32 guide = col(ImGuiCol_TextDisabled, 0.35f);
		dl->AddLine(ImVec2(x0, y0db), ImVec2(x1, y0db), guide);
		const float xmid = x0 + span * 64.0f / 127.0f;
		dl->AddLine(ImVec2(xmid, top), ImVec2(xmid, bottom), guide);

		const float q = std::pow(2.0f, float(vq - 64) / 16.0f);
		const int n = std::max(8, int(span) / 2);
		std::vector<ImVec2> pts;
		pts.reserve(n + 1);
		for (int i = 0; i <= n; i++) {
			const float x = x0 + span * float(i) / float(n);
			const float r = std::pow(2.0f, (x - xc) / span * 8.0f);          // 周波数 / ローパスのカットオフ
			const float r2 = r * r;
			float mag = 1.0f / std::sqrt((1 - r2) * (1 - r2) + r2 / (q * q));
			if (known_h) {
				const float s = std::pow(2.0f, (xh - x) / span * 8.0f);      // ハイパスのカットオフ / 周波数
				const float s2 = s * s;
				mag *= 1.0f / std::sqrt((1 - s2) * (1 - s2) + 2.0f * s2);    // Q = 0.707
			}
			pts.push_back(ImVec2(x, y_of(20.0f * std::log10(std::max(mag, 1e-4f)))));
		}
		const ImU32 line = col(ImGuiCol_SliderGrabActive);
		dl->PathClear();
		dl->PathLineTo(ImVec2(x0, bottom));
		for (const ImVec2 &p : pts) dl->PathLineTo(p);
		dl->PathLineTo(ImVec2(x1, bottom));
		dl->PathFillConcave(col(ImGuiCol_SliderGrab, 0.25f));
		dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
		dl->AddPolyline(pts.data(), int(pts.size()), line, 0, std::max(1.5f, fs * 0.1f));
		const float r = std::max(2.5f, fs * 0.22f);
		const bool hold_c = active && grab == 0, hold_h = active && grab == 1;
		touch_point(dl, ImVec2(xc, yq), r, hold_c);
		if (known_h)
			touch_point(dl, ImVec2(xh, yh), r, hold_h);
		// 字（大きな窓だけ）。周波数は、音色の元の値と合わせたフィルタをチップの式で通して
		// 出した特性の、いちばん大きい所から 3 dB 下がる所（山があれば山の頂）
		voice_ctx v;
		if (!compact && voice_of(part, v)) {
			v.blk[0x18] = u8(vc); v.blk[0x19] = u8(vq);
			if (known_h)
				v.blk[xg::ram::PART_HPF_RAM] = u8(vh);
			const std::vector<shape::filter_line> ls = shape::filter_lines(v.rom, v.rec, v.blk);
			if (!ls.empty()) {
				const shape::filter_line &L = lead_line(ls);
				float peak = -1e9f, peak_hz = 20.0f;
				for (const shape::pt &p : L.pts)
					if (p.cents > peak) { peak = p.cents; peak_hz = p.ms; }
				float lpf_hz = 0;
				for (size_t i = L.pts.size(); i-- > 0;)
					if (L.pts[i].cents >= peak - 3.0f) { lpf_hz = L.pts[i].ms; break; }
				if (peak > 3.0f)
					lpf_hz = peak_hz;
				float hpf_hz = 0;
				for (const shape::pt &p : L.pts)
					if (p.cents >= peak - 3.0f) { hpf_hz = p.ms; break; }
				auto hz_text = [](float f) {
					char t[24];
					if (f >= 19000.0f) std::snprintf(t, sizeof(t), "%s", UI_TEXT(ov_khz, "20 kHz or more"));
					else if (f >= 1000.0f) std::snprintf(t, sizeof(t), "%.1f kHz", f / 1000.0f);
					else std::snprintf(t, sizeof(t), "%.0f Hz", f);
					return std::string(t);
				};
				char s[96];
				const ImVec2 a(pos.x, pos.y), b(pos.x + w, pos.y + h);
				std::snprintf(s, sizeof(s), "Cutoff : %s (%s)\nReso : %s", xg::format(pc, vc).c_str(),
				              hz_text(lpf_hz).c_str(), xg::format(pq, vq).c_str());
				label_avoid boxes;
				boxes.point(ImVec2(xc, yq), r + 2.0f);
				if (known_h)
					boxes.point(ImVec2(xh, yh), r + 2.0f);
				point_label(dl, ImVec2(xc, yq), s, true, a, b, &boxes);
				if (known_h) {
					std::snprintf(s, sizeof(s), L.hpf ? UI_TEXT(ov_hpf_on_fmt, "HPF : %s (%s)") : UI_TEXT(ov_hpf_off_fmt, "HPF : %s (off)"),
					              xg::format(ph, vh).c_str(), hz_text(hpf_hz).c_str());
					point_label(dl, ImVec2(xh, yh), s, false, a, b, &boxes);
				}
			}
		}
		dl->PopClipRect();
	} else {
		const ImVec2 ts = ImGui::CalcTextSize("--");
		dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), col(ImGuiCol_TextDisabled), "--");
	}

	if ((hovered || active) && known)
		hint("FILTER CUTOFF FREQUENCY %s ／ FILTER RESONANCE %s ／ HPF CUTOFF FREQUENCY %s%s",
		                      xg::format(pc, vc).c_str(), xg::format(pq, vq).c_str(),
		                      known_h ? xg::format(ph, vh).c_str() : "--",
		                      compact ? BIG_HINT : UI_TEXT(ov_filter_hint, "\nRight dot: sideways for cutoff (right is brighter), up/down for resonance (up is stronger)\n"
		                                                                  "Left dot: sideways for HPF (right cuts more bass)"));
	ImGui::PopID();
}

namespace {

using eq::HZ;
using eq::hz_text;
using eq::t_of_hz;
using eq::hz_of_t;
using eq::index_near;
using eq::band_db;
using band_shape = eq::shape;

struct eq_band {
	band_shape shape;
	const xg::param *gain, *freq, *q;     // q は無ければ nullptr
	int part;
	int vg, vf, vq;
	bool known;
};

// EQ の絵の 1 マス。帯ごとの点をつまんで、横で周波数、縦でゲイン。ホイールで Q（あれば）。
// 戻り値はつかんでいる帯（無ければ -1）。edit が false なら描くだけで、つまみもホイールも効かない
int eq_plot(const char *id, eq_band *bands, int n, xg::model &m, bridge &br, float w, float h, const char *tip,
            bool edit)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	bool known = true;
	for (int i = 0; i < n; i++) {
		eq_band &b = bands[i];
		b.known = m.get(*b.gain, b.part, b.vg) && m.get(*b.freq, b.part, b.vf) && (!b.q || m.get(*b.q, b.part, b.vq));
		known &= b.known;
	}

	ImGui::PushID(id);
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##eq", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID iid = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = edit && ImGui::IsItemActive();

	const float pad = fs * 0.25f;
	const float x0 = pos.x + pad, x1 = pos.x + w - pad;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	const float DB = 15.0f;
	auto x_of = [&](float hz) { return x0 + (x1 - x0) * t_of_hz(hz); };
	auto y_of = [&](float db) { return (top + bottom) * 0.5f - (bottom - top) * 0.5f * std::clamp(db, -DB, DB) / DB; };
	auto handle = [&](const eq_band &b) { return ImVec2(x_of(float(HZ[b.vf])), y_of(float(b.vg - 64))); };

	// 押した瞬間に一番近い点を選ぶ。ずれを覚えて、点が指に飛ばないようにする
	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(iid, -1);
	float gx = st->GetFloat(iid + 1, 0.0f), gy = st->GetFloat(iid + 2, 0.0f);
	auto nearest = [&]() {
		int best = -1; float bd = 1e9f;
		for (int i = 0; i < n; i++) {
			if (!bands[i].known) continue;
			const ImVec2 hp = handle(bands[i]);
			const float d = (hp.x - io.MousePos.x) * (hp.x - io.MousePos.x) + (hp.y - io.MousePos.y) * (hp.y - io.MousePos.y);
			if (d < bd) { bd = d; best = i; }
		}
		return best;
	};
	if (active && ImGui::IsItemActivated() && known) {
		grab = nearest();
		if (grab >= 0) {
			const ImVec2 hp = handle(bands[grab]);
			gx = hp.x - io.MousePos.x;
			gy = hp.y - io.MousePos.y;
		}
	}
	if (!active)
		grab = -1;
	st->SetInt(iid, grab);
	st->SetFloat(iid + 1, gx);
	st->SetFloat(iid + 2, gy);
	if (active && grab >= 0 && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
		const eq_band &b = bands[grab];
		const float t = (io.MousePos.x + gx - x0) / (x1 - x0);
		const int nf = index_near(t, b.freq->min, b.freq->max);
		const float db = -((io.MousePos.y + gy) - (top + bottom) * 0.5f) / ((bottom - top) * 0.5f) * DB;
		const int ng = std::clamp(int(std::lround(64 + db)), b.gain->min, b.gain->max);
		if (nf != b.vf) drag_send(br, m.set(*b.freq, b.part, nf));
		if (ng != b.vg) drag_send(br, m.set(*b.gain, b.part, ng));
	}
	// ホイールで Q。カーソルに一番近い帯
	const int hot = !edit ? -1 : hovered && !active ? nearest() : grab;
	if (hovered && known && hot >= 0 && bands[hot].q) {
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		if (io.MouseWheel != 0.0f) {
			const eq_band &b = bands[hot];
			const int nq = std::clamp(b.vq + (io.MouseWheel > 0 ? 1 : -1) * (io.KeyCtrl ? 10 : 2), b.q->min, b.q->max);
			if (nq != b.vq) drag_send(br, m.set(*b.q, b.part, nq));
		}
	}

	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	if (known) {
		const ImU32 guide = col(ImGuiCol_TextDisabled, 0.35f);
		dl->AddLine(ImVec2(x0, y_of(0)), ImVec2(x1, y_of(0)), guide);
		for (float hz : { 100.0f, 1000.0f, 10000.0f })
			dl->AddLine(ImVec2(x_of(hz), top), ImVec2(x_of(hz), bottom), guide);
		const int np = std::max(8, int(x1 - x0) / 2);
		std::vector<ImVec2> pts;
		pts.reserve(np + 1);
		for (int i = 0; i <= np; i++) {
			const float t = float(i) / float(np);
			const float f = hz_of_t(t);
			float db = 0;
			for (int k = 0; k < n; k++)
				db += band_db(bands[k].shape, float(bands[k].vg - 64), float(HZ[bands[k].vf]),
				              bands[k].q ? bands[k].vq / 10.0f : 0.7f, f);
			pts.push_back(ImVec2(x0 + (x1 - x0) * t, y_of(db)));
		}
		const ImU32 line = col(ImGuiCol_SliderGrabActive);
		dl->PathClear();
		dl->PathLineTo(ImVec2(x0, y_of(0)));
		for (const ImVec2 &p : pts) dl->PathLineTo(p);
		dl->PathLineTo(ImVec2(x1, y_of(0)));
		dl->PathFillConcave(col(ImGuiCol_SliderGrab, 0.25f));
		dl->AddPolyline(pts.data(), int(pts.size()), line, 0, std::max(1.5f, fs * 0.1f));
		const float r = std::max(2.5f, fs * 0.2f);
		for (int k = 0; k < n; k++) {
			const ImVec2 hp = handle(bands[k]);
			const bool on = k == grab || (k == hot && hovered);
			dl->AddCircleFilled(hp, on ? r * 1.4f : r, on ? col(ImGuiCol_Text) : line);
		}
	} else {
		const ImVec2 ts = ImGui::CalcTextSize("--");
		dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), col(ImGuiCol_TextDisabled), "--");
	}

	if ((hovered || active) && known) {
		std::string text;
		for (int k = 0; k < n; k++) {
			char buf[96];
			const eq_band &b = bands[k];
			std::snprintf(buf, sizeof(buf), "%s%d: %sHz %+ddB", k ? "\n" : "", k + 1, hz_text(b.vf).c_str(), b.vg - 64);
			text += buf;
			if (b.q) {
				std::snprintf(buf, sizeof(buf), "  Q %.1f", b.vq / 10.0);
				text += buf;
			}
		}
		hint("%s%s%s", text.c_str(), *tip == '\n' ? "" : "\n", tip);
	}
	ImGui::PopID();
	return grab;
}

} // namespace


// パートの EQ の 1 マス。低音（シェルフ）と高音（シェルフ）の 2 つの点
void overview::eq_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	eq_band bands[] = {
		{ band_shape::low_shelf,  &P("part.eq_bass_gain"),   &P("part.eq_bass_freq"),   nullptr, part, 64, 12, 0, false },
		{ band_shape::high_shelf, &P("part.eq_treble_gain"), &P("part.eq_treble_freq"), nullptr, part, 64, 54, 0, false },
	};
	eq_plot("eq", bands, 2, m, br, w, h, compact ? BIG_HINT : UI_TEXT(ov_eq_pt_hint, "Drag a dot: sideways for frequency, up/down for gain (1 is bass, 2 is treble)"),
	        !compact);
}


// マスター EQ の 1 マス。5 つの帯。1 と 5 は形（シェルフ／ピーク）を右クリックで選ぶ
void overview::master_eq_plot(xg::model &m, bridge &br, float w, float h, bool edit)
{
	int s1 = 0, s5 = 0;
	m.get(P("master_eq.shape1"), 0, s1);
	m.get(P("master_eq.shape5"), 0, s5);
	eq_band bands[] = {
		{ s1 ? band_shape::peak : band_shape::low_shelf,  &P("master_eq.gain1"), &P("master_eq.freq1"), &P("master_eq.q1"), 0, 64, 12, 7, false },
		{ band_shape::peak,                               &P("master_eq.gain2"), &P("master_eq.freq2"), &P("master_eq.q2"), 0, 64, 28, 7, false },
		{ band_shape::peak,                               &P("master_eq.gain3"), &P("master_eq.freq3"), &P("master_eq.q3"), 0, 64, 34, 7, false },
		{ band_shape::peak,                               &P("master_eq.gain4"), &P("master_eq.freq4"), &P("master_eq.q4"), 0, 64, 46, 7, false },
		{ s5 ? band_shape::peak : band_shape::high_shelf, &P("master_eq.gain5"), &P("master_eq.freq5"), &P("master_eq.q5"), 0, 64, 52, 7, false },
	};
	eq_plot("meq", bands, 5, m, br, w, h,
	        edit ? UI_TEXT(ov_eq_master_hint, "Drag a dot: sideways for frequency, up/down for gain. Wheel near a dot for width (Q)")
	             : UI_TEXT(ov_bighint_master, "\nDouble-click to edit in the master window"),
	        edit);
}

// 一覧のマスター EQ は見るだけ。ダブルクリックでマスターの窓（種類・帯の形・値もそこで）
void overview::master_eq_cell(xg::model &m, bridge &br, float h)
{
	master_eq_plot(m, br, ImGui::GetContentRegionAvail().x, h, false);
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		request_master();
}


// ビブラートの 1 マス。弾いてからの揺れの形。平らな所が掛かり始めるまで（Delay）、
// そのあとの波の山の点をつまんで、横で速さ（山が近いほど速い）、縦で深さ。
// 平らな所の終わりの点を横に動かすと Delay（つまめるのは大きな窓だけ）。どれも音色の元の値に対する増減（64 が音色のまま）で、
// 形は 2 の (値 - 64) / 24 乗で伸び縮みさせた見た目だけのもの
void overview::vib_small(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const xg::param &pr = P("part.vib_rate"), &pd = P("part.vib_depth"), &pl = P("part.vib_delay");
	int vr = 64, vd = 64, vl = 64;
	const bool known = m.get(pr, part, vr) && m.get(pd, part, vd) && m.get(pl, part, vl);

	ImGui::PushID("vib");
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##vib", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = !compact && ImGui::IsItemActive();   // 一覧の小さなマスでは触らせない

	const float pad = fs * 0.25f;
	const float x0 = pos.x + pad, x1 = pos.x + w - pad;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	const float mid = (top + bottom) * 0.5f, half = (bottom - top) * 0.5f;
	const float unit = (x1 - x0) / 5.0f;
	auto scale = [](int v) { return std::pow(2.0f, float(v - 64) / 24.0f); };
	const float delay = std::min(unit * scale(vl), (x1 - x0) * 0.8f);
	const float period = std::clamp(unit * 0.8f / scale(vr), 3.0f, (x1 - x0));
	const float amp = std::min(half * 0.45f * scale(vd), half);
	const float xd = x0 + delay;
	const ImVec2 crest(xd + period * 0.25f, mid - amp);

	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(id, -1);
	float gx = st->GetFloat(id + 1, 0.0f), gy = st->GetFloat(id + 2, 0.0f);
	if (active && ImGui::IsItemActivated() && known) {
		const float dd = std::fabs(io.MousePos.x - xd) + std::fabs(io.MousePos.y - mid);
		const float dc = std::fabs(io.MousePos.x - crest.x) + std::fabs(io.MousePos.y - crest.y);
		grab = dd < dc ? 0 : 1;
		gx = (grab == 0 ? xd : crest.x) - io.MousePos.x;
		gy = (grab == 0 ? mid : crest.y) - io.MousePos.y;
	}
	if (!active)
		grab = -1;
	st->SetInt(id, grab);
	st->SetFloat(id + 1, gx);
	st->SetFloat(id + 2, gy);
	if (active && known && grab >= 0 && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
		auto value = [](float ratio, const xg::param &p) {
			return std::clamp(int(std::lround(64 + 24 * std::log2(std::max(ratio, 1e-3f)))), p.min, p.max);
		};
		const float fx = io.MousePos.x + gx, fy = io.MousePos.y + gy;
		if (grab == 0) {
			const int nl = value((fx - x0) / unit, pl);
			if (nl != vl) drag_send(br, m.set(pl, part, nl));
		} else {
			const int nr = value(unit * 0.8f / std::max(1.0f, (fx - xd) * 4.0f), pr);
			const int nd = value((mid - fy) / (half * 0.45f), pd);
			if (nr != vr) drag_send(br, m.set(pr, part, nr));
			if (nd != vd) drag_send(br, m.set(pd, part, nd));
		}
	}

	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	if (known) {
		const ImU32 line = col(ImGuiCol_SliderGrabActive);
		dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), col(ImGuiCol_TextDisabled, 0.35f));
		std::vector<ImVec2> pts;
		pts.push_back(ImVec2(x0, mid));
		pts.push_back(ImVec2(xd, mid));
		for (float x = xd + 1.0f; x <= x1; x += 1.0f)
			pts.push_back(ImVec2(x, mid - amp * std::sin((x - xd) / period * 2.0f * IM_PI)));
		dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
		dl->AddPolyline(pts.data(), int(pts.size()), line, 0, std::max(1.5f, fs * 0.1f));
		const float r = std::max(2.5f, fs * 0.2f);
		fixed_point(dl, ImVec2(x0, mid), r);
		touch_point(dl, ImVec2(xd, mid), r, grab == 0);
		touch_point(dl, crest, r, grab == 1);
		// 字（大きな窓だけ）。速さ・深さ・遅れは音色の元の値と合わせた実際のもの（チップの LFO を回して出す）
		voice_ctx v;
		if (!compact && voice_of(part, v)) {
			v.blk[0x15] = u8(vr); v.blk[0x16] = u8(vd); v.blk[0x17] = u8(vl);
			const std::vector<shape::vib_line> ls = shape::vib_lines(v.rom, v.rec, v.blk, 1500.0f);
			if (!ls.empty()) {
				const shape::vib_line &L = lead_line(ls);
				char s[96];
				const ImVec2 a(pos.x, pos.y), b(pos.x + w, pos.y + h);
				std::snprintf(s, sizeof(s), "Rate : %s (%.2f Hz)\nDepth : %s (±%.0f cent)", xg::format(pr, vr).c_str(),
				              L.hz, xg::format(pd, vd).c_str(), L.depth_cents);
				label_avoid boxes;
				boxes.point(ImVec2(x0, mid), r + 2.0f);
				boxes.point(ImVec2(xd, mid), r + 2.0f);
				boxes.point(crest, r + 2.0f);
				point_label(dl, crest, s, true, a, b, &boxes);
				std::snprintf(s, sizeof(s), "Delay : %s (%.0f ms)", xg::format(pl, vl).c_str(), L.delay_ms);
				point_label(dl, ImVec2(xd, mid), s, false, a, b, &boxes);
			}
		}
		dl->PopClipRect();
	} else {
		const ImVec2 ts = ImGui::CalcTextSize("--");
		dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), col(ImGuiCol_TextDisabled), "--");
	}
	if ((hovered || active) && known)
		hint("Rate %s   Depth %s   Delay %s%s",
		                      xg::format(pr, vr).c_str(), xg::format(pd, vd).c_str(), xg::format(pl, vl).c_str(),
		                      compact ? BIG_HINT : UI_TEXT(ov_vib_hint, "\nTop of the wave: sideways for speed, up/down for depth\nEnd of the flat part: sideways for the delay"));
	ImGui::PopID();
}

// ミキサーのフェーダー風の絵。縦の溝と目盛り、つまみ（白い線入り）が値の高さにある。
// 真ん中（64）の目盛りだけ明るく。上に小さく名前と値の字
static void fader_picture(ImDrawList *dl, float x0, float x1, float top, float bottom, int value, int lo, int hi,
                          const char *name, const char *text, bool lit, ImU32 name_col = 0, bool dim = false)
{
	// dim は「動かせるが今は効かない」値（色を落とす）
	const float fs = ImGui::GetFontSize();
	const float gfs = fs * 0.6f;
	const float cx = (x0 + x1) * 0.5f;
	const float cap_h = std::max(6.0f, fs * 0.55f);
	const float a = top + cap_h * 0.5f, b = bottom - cap_h * 0.5f;     // つまみの中心が動く範囲
	// 目盛り（8 つに割る。真ん中は長く明るく）
	for (int i = 0; i <= 8; i++) {
		const float y = b - (b - a) * float(i) / 8.0f;
		const bool mid = i == 4;
		const float len = (x1 - x0) * (mid ? 0.5f : 0.3f);
		const ImU32 c = mid ? IM_COL32(170, 170, 176, 255) : IM_COL32(90, 90, 96, 255);
		dl->AddLine(ImVec2(x0, y), ImVec2(x0 + len, y), c, 1.0f);
		dl->AddLine(ImVec2(x1 - len, y), ImVec2(x1, y), c, 1.0f);
	}
	// 溝
	const float gw = std::max(2.0f, fs * 0.14f);
	dl->AddRectFilled(ImVec2(cx - gw, top), ImVec2(cx + gw, bottom), IM_COL32(10, 10, 12, 255), gw);
	// つまみ
	const float y = b - (b - a) * float(value - lo) / float(std::max(1, hi - lo));
	const ImVec2 c0(x0 + 1.0f, y - cap_h * 0.5f), c1(x1 - 1.0f, y + cap_h * 0.5f);
	const ImU32 top_c = dim ? (lit ? IM_COL32(110, 110, 116, 255) : IM_COL32(88, 88, 94, 255))
	                        : (lit ? IM_COL32(200, 200, 208, 255) : IM_COL32(160, 160, 168, 255));
	const ImU32 bot_c = dim ? (lit ? IM_COL32(70, 70, 76, 255) : IM_COL32(56, 56, 62, 255))
	                        : (lit ? IM_COL32(110, 110, 118, 255) : IM_COL32(80, 80, 88, 255));
	dl->AddRectFilledMultiColor(c0, c1, top_c, top_c, bot_c, bot_c);
	dl->AddRect(c0, c1, IM_COL32(30, 30, 34, 255), 1.5f);
	dl->AddLine(ImVec2(c0.x + 1.0f, y), ImVec2(c1.x - 1.0f, y), dim ? IM_COL32(140, 140, 146, 255) : IM_COL32(250, 250, 245, 255), 1.5f);
	// 上に名前と値（2 行）
	ImFont *font = ImGui::GetFont();
	const ImVec2 ts = font->CalcTextSizeA(gfs, FLT_MAX, 0.0f, text);
	const ImVec2 ns = font->CalcTextSizeA(gfs, FLT_MAX, 0.0f, name);
	dl->AddText(font, gfs, ImVec2(cx - ts.x * 0.5f, top - ts.y - 1.0f), ImGui::GetColorU32(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text), text);
	dl->AddText(font, gfs, ImVec2(cx - ns.x * 0.5f, top - ts.y - ns.y - 1.0f), name_col ? name_col : ImGui::GetColorU32(ImGuiCol_TextDisabled), name);
}

// ホイールの絵（モジュレーションとピッチベンド）。**輪を横から見た形**で、
// 刻みの筋が値に連れて上下する（回っているのが分かる）。端に行くほど筋が詰まるので、
// 円筒に見える。名前と値はフェーダーと同じく上に 2 行、高さもフェーダーとそろえる
static void wheel_picture(ImDrawList *dl, float x0, float x1, float top, float bottom,
                          float frac, bool bipolar, const char *name, const char *text, bool lit)
{
	const float fs = ImGui::GetFontSize();
	const float gfs = fs * 0.6f;
	const float cx = (x0 + x1) * 0.5f;
	const float r = std::min((x1 - x0) * 0.4f, fs * 0.45f);
	const float mid = (top + bottom) * 0.5f, half = (bottom - top) * 0.5f;
	frac = std::clamp(frac, 0.0f, 1.0f);

	// 土台と、円筒の陰影（真ん中が明るく、上下の端が暗い）
	dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom), IM_COL32(18, 18, 22, 255), r);
	dl->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), true);
	constexpr int BANDS = 20;
	for (int i = 0; i < BANDS; i++) {
		const float t0 = float(i) / BANDS, t1 = float(i + 1) / BANDS;
		const float sh = std::sin(3.14159265f * (t0 + t1) * 0.5f);          // 端 0、真ん中 1
		const int v = int(30.0f + 58.0f * sh) + (lit ? 18 : 0);
		dl->AddRectFilled(ImVec2(x0, top + (bottom - top) * t0), ImVec2(x1, top + (bottom - top) * t1),
		                  IM_COL32(v, v, v + 4, 255));
	}
	// 刻みの筋。値に連れて回る（端ほど詰まる ＝ 円筒の見え方）
	constexpr int RIDGES = 9;
	const float turn = frac * 2.0f;                                          // 全域で 2 回り
	for (int k = 0; k < RIDGES; k++) {
		float ph = float(k) / RIDGES - turn;
		ph -= std::floor(ph);                                                // 0-1 に畳む
		const float y = mid - half * std::cos(3.14159265f * ph);
		const float edge = std::sin(3.14159265f * ph);                       // 端は薄く
		const int a = int(40.0f + 150.0f * edge);
		dl->AddLine(ImVec2(x0 + 1.0f, y), ImVec2(x1 - 1.0f, y), IM_COL32(0, 0, 0, a), 1.0f);
		dl->AddLine(ImVec2(x0 + 1.0f, y + 1.0f), ImVec2(x1 - 1.0f, y + 1.0f), IM_COL32(210, 210, 216, a / 3), 1.0f);
	}
	// 真ん中の印（ベンドの戻る先）
	if (bipolar)
		dl->AddLine(ImVec2(x0 + 1.0f, mid), ImVec2(x1 - 1.0f, mid), IM_COL32(120, 120, 128, 160), 1.0f);
	// いまの位置（つまみの線と、両脇の印）
	const float y = bottom - (bottom - top) * frac;
	const ImU32 grip = lit ? IM_COL32(250, 250, 245, 255) : IM_COL32(200, 200, 208, 230);
	dl->AddLine(ImVec2(x0 + 1.0f, y), ImVec2(x1 - 1.0f, y), grip, 2.0f);
	dl->PopClipRect();
	dl->AddRect(ImVec2(x0, top), ImVec2(x1, bottom), lit ? IM_COL32(150, 150, 158, 255) : IM_COL32(70, 70, 78, 255), r);
	// 両脇の印。端まで回したときに枠からはみ出さないよう、中へ寄せる
	const float ay = std::clamp(y, top + 3.0f, bottom - 3.0f);
	for (float sx : { x0 - 2.0f, x1 + 2.0f })
		dl->AddTriangleFilled(ImVec2(sx, ay), ImVec2(sx + (sx < cx ? -3.0f : 3.0f), ay - 3.0f),
		                      ImVec2(sx + (sx < cx ? -3.0f : 3.0f), ay + 3.0f), grip);

	// 上に名前と値（フェーダーと同じ並び）
	ImFont *font = ImGui::GetFont();
	const ImVec2 ts = font->CalcTextSizeA(gfs, FLT_MAX, 0.0f, text);
	const ImVec2 ns = font->CalcTextSizeA(gfs, FLT_MAX, 0.0f, name);
	dl->AddText(font, gfs, ImVec2(cx - ts.x * 0.5f, top - ts.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text), text);
	dl->AddText(font, gfs, ImVec2(cx - ns.x * 0.5f, top - ts.y - ns.y - 1.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), name);
}

// ビブラート（音色の窓の大きな区画）。左に実際の揺れ（チップの LFO を回した波。横が時間 1.5 秒、
// 縦がセント）、右に Rate・Depth・Delay のフェーダー。絵は見るだけで、フェーダーをドラッグ
// （つまんだ高さがそのまま値）か、フェーダーの上でマウスホイール（1 目で 1、Ctrl で 10）で動かす。
// 一覧の小さなマスは vib_small（前の絵）
void overview::vib_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	if (compact) {
		vib_small(part, m, br, w, h, compact);
		return;
	}
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const xg::param *ps[3] = { &P("part.vib_rate"), &P("part.vib_depth"), &P("part.vib_delay") };
	static const char *const NAMES[3] = { "Rate", "Depth", "Delay" };
	int vals[3] = { 64, 64, 64 };
	const bool known = m.get(*ps[0], part, vals[0]) && m.get(*ps[1], part, vals[1]) && m.get(*ps[2], part, vals[2]);

	ImGui::PushID("vibbig");
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##vib", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();

	// 置き場所。右にフェーダー 3 本、残りが波の絵
	const float pad = fs * 0.25f;
	const float gfs = fs * 0.6f;
	const float fw = std::min(fs * 1.3f, w * 0.1f);
	const float fgap = fs * 0.35f;
	const float fx_right = pos.x + w - pad;
	float fx0[3];
	for (int i = 0; i < 3; i++)
		fx0[i] = fx_right - fw * float(3 - i) - fgap * float(2 - i);
	const float ftop = pos.y + pad + gfs * 2.4f, fbot = pos.y + h - pad;
	const float x0 = pos.x + pad, x1 = fx0[0] - fs * 0.5f;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	const float mid = (top + bottom) * 0.5f, half = (bottom - top) * 0.5f;
	auto fader_at = [&](float x) {
		for (int i = 0; i < 3; i++)
			if (x >= fx0[i] - fgap * 0.5f && x <= fx0[i] + fw + fgap * 0.5f)
				return i;
		return -1;
	};
	const float cap_h = std::max(6.0f, fs * 0.55f);
	auto value_at = [&](float y, const xg::param &p) {
		const float a = ftop + cap_h * 0.5f, b = fbot - cap_h * 0.5f;
		return std::clamp(p.min + int(std::lround((b - y) / std::max(1.0f, b - a) * float(p.max - p.min))), p.min, p.max);
	};
	const int over = hovered ? fader_at(io.MousePos.x) : -1;

	// マウスホイール（フェーダーの上）
	if (over >= 0 && known) {
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		if (io.MouseWheel != 0.0f) {
			const int nv = std::clamp(vals[over] + wheel_steps(io.MouseWheel, io.KeyCtrl), ps[over]->min, ps[over]->max);
			if (nv != vals[over]) {
				br.send(m.set(*ps[over], part, nv));
				vals[over] = nv;
			}
		}
	}
	// つまんで上下（つまんだ高さがそのまま値）
	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(id, -1);
	if (active && ImGui::IsItemActivated())
		grab = fader_at(io.MousePos.x);
	if (!active)
		grab = -1;
	st->SetInt(id, grab);
	if (grab >= 0 && known) {
		const int nv = value_at(io.MousePos.y, *ps[grab]);
		if (nv != vals[grab]) {
			drag_send(br, m.set(*ps[grab], part, nv));
			vals[grab] = nv;
		}
	}

	// 説明（フェーダーの上）
	{
		static const char *const KEYS[3] = { "part.vib_rate", "part.vib_depth", "part.vib_delay" };
		const int i = grab >= 0 ? grab : over;
		if (over >= 0 && known)
			out_hover_param(*ps[over], part);
		if (i >= 0 && known) {
			const char *help = help_for(KEYS[i]);
			hint(UI_TEXT(ov_value_tip_fmt, "%s  %s\n%s (drag or wheel)"), official_name(KEYS[i]).c_str(), xg::format(*ps[i], vals[i]).c_str(),
			     help ? help : "");
		}
	}
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
	for (int i = 0; i < 3; i++) {
		const std::string t = known ? xg::format(*ps[i], vals[i]) : std::string("--");
		fader_picture(dl, fx0[i], fx0[i] + fw, ftop, fbot, vals[i], ps[i]->min, ps[i]->max, NAMES[i], t.c_str(),
		              grab == i || over == i);
	}

	// ---- 波（実際の揺れ）
	voice_ctx v;
	std::vector<shape::vib_line> ls, own_ls;
	// 横（時間）の幅。Delay を上げると掛かり始めが 4 秒先まで行くので、パートに合わせて伸ばす（6.232）
	float tspan = 1500.0f;
	if (known && voice_of(part, v)) {
		v.blk[0x15] = u8(vals[0]); v.blk[0x16] = u8(vals[1]); v.blk[0x17] = u8(vals[2]);
		tspan = shape::vib_span_ms(v.rom, v.rec, v.blk);
		ls = shape::vib_lines(v.rom, v.rec, v.blk, tspan);
		// 音色自身の揺れ（Depth を既定の 64 にしたもの）。背景に薄く出して、Depth で足した・引いたぶんを見せる
		if (vals[1] != 64) {
			u8 own_blk[XG_PART_COPY];
			std::memcpy(own_blk, v.blk, sizeof(own_blk));
			own_blk[0x16] = 64;
			own_ls = shape::vib_lines(v.rom, v.rec, own_blk, tspan);
		}
	}
	if (!ls.empty()) {
		const shape::vib_line &L = lead_line(ls);
		const shape::vib_line *O = own_ls.empty() ? nullptr : &lead_line(own_ls);
		// 縦の目盛り（片側）。最低 ±220 セント、深い音色はそれに合わせて広げる
		const float span = std::max({ 220.0f, L.depth_cents * 1.15f, O ? O->depth_cents * 1.15f : 0.0f });
		auto y_of = [&](float c) { return mid - half * c / span; };
		dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), col(ImGuiCol_TextDisabled, 0.35f));
		for (float c : { 50.0f, 100.0f, 200.0f, 400.0f }) {
			if (c > span)
				break;
			for (float sgn : { 1.0f, -1.0f })
				dl->AddLine(ImVec2(x0, y_of(sgn * c)), ImVec2(x1, y_of(sgn * c)), col(ImGuiCol_TextDisabled, 0.15f));
			char g[16];
			std::snprintf(g, sizeof(g), "%.0f", c);
			dl->AddText(ImGui::GetFont(), fs * 0.6f, ImVec2(x0 + 2.0f, y_of(c) - fs * 0.6f), col(ImGuiCol_TextDisabled, 0.55f), g);
		}
		// 掛かり始め
		if (L.delay_ms > 0.0f) {
			const float xd = x0 + (x1 - x0) * std::min(1.0f, L.delay_ms / tspan);
			for (float y = top; y < bottom; y += fs * 0.5f)
				dl->AddLine(ImVec2(xd, y), ImVec2(xd, std::min(bottom, y + fs * 0.25f)), col(ImGuiCol_TextDisabled, 0.5f));
		}
		// 音色自身の揺れ（Depth 64）を背景に薄く、その上に Depth 込みの実際の揺れ
		if (O) {
			std::vector<ImVec2> op;
			op.reserve(O->pts.size());
			for (const shape::pt &p : O->pts)
				op.push_back(ImVec2(x0 + (x1 - x0) * p.ms / tspan, y_of(p.cents)));
			if (op.size() >= 2)
				dl->AddPolyline(op.data(), int(op.size()), col(ImGuiCol_TextDisabled, 0.35f), 0, 1.0f);
		}
		std::vector<ImVec2> pts;
		pts.reserve(L.pts.size());
		for (const shape::pt &p : L.pts)
			pts.push_back(ImVec2(x0 + (x1 - x0) * p.ms / tspan, y_of(p.cents)));
		if (pts.size() >= 2)
			dl->AddPolyline(pts.data(), int(pts.size()), col(ImGuiCol_SliderGrabActive), 0, std::max(1.5f, fs * 0.1f));
		// 実際の量（左上に小さく）と、帯の一覧
		char s[96];
		std::snprintf(s, sizeof(s), "%.2f Hz   ±%.0f cent   %.0f ms", L.hz, L.depth_cents, L.delay_ms);
		dl->AddText(ImGui::GetFont(), fs * 0.7f, ImVec2(x0 + fs * 1.6f, top + 2.0f), col(ImGuiCol_Text, 0.85f), s);
		std::snprintf(s, sizeof(s), "Rate : %s (%.2f Hz)", xg::format(*ps[0], vals[0]).c_str(), L.hz);
		shape_value(s);
		std::snprintf(s, sizeof(s), "Depth : %s (±%.0f cent)", xg::format(*ps[1], vals[1]).c_str(), L.depth_cents);
		shape_value(s);
		std::snprintf(s, sizeof(s), "Delay : %s (%.0f ms)", xg::format(*ps[2], vals[2]).c_str(), L.delay_ms);
		shape_value(s);
	} else {
		const ImVec2 ts = ImGui::CalcTextSize("--");
		dl->AddText(ImVec2(x0 + (x1 - x0 - ts.x) * 0.5f, mid - ts.y * 0.5f), col(ImGuiCol_TextDisabled), "--");
	}
	dl->PopClipRect();
	ImGui::PopID();
}

// ---- 音色の窓の大きな区画の、フェーダーの並び（フィルタと EQ、EG とピッチ EG）
namespace {

// フェーダーを n 本、a-b の四角の中に横に並べて、操作（ドラッグ・マウスホイール）と描画をする。
// group_after の後ろは少し空けて組を分ける（-1 なら分けない）。値の字は値の棒と同じ書き方（EQ の周波数は Hz）。
// 戻り値は、カーソルが載っているかつまんでいるフェーダー（無ければ -1）
int fader_row(const char *const *keys, const char *const *names, int n, int group_after, int part, xg::model &m, bridge &br,
              ImVec2 a, ImVec2 b, bool hovered, bool active, ImGuiID id, int *vals, bool *have,
              ImU32 col_first = 0, ImU32 col_second = 0, unsigned dim_mask = 0)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float gfs = fs * 0.6f;
	const float gap = fs * 0.5f, group = fs * 1.4f;
	const float room = b.x - a.x - gap * float(n - 1) - (group_after >= 0 ? group - gap : 0.0f);
	const float fw = std::clamp(room / float(n), fs * 1.0f, fs * 1.8f);
	const float total = fw * float(n) + gap * float(n - 1) + (group_after >= 0 ? group - gap : 0.0f);
	std::vector<float> fx0(static_cast<size_t>(n));
	{
		float x = a.x + std::max(0.0f, (b.x - a.x - total) * 0.5f);
		for (int i = 0; i < n; i++) {
			fx0[size_t(i)] = x;
			x += fw + (i == group_after ? group : gap);
		}
	}
	const float ftop = a.y + gfs * 2.4f, fbot = b.y;
	const float cap_h = std::max(6.0f, fs * 0.55f);
	std::vector<const xg::param *> ps(static_cast<size_t>(n));
	for (int i = 0; i < n; i++)
		ps[size_t(i)] = &P(keys[i]);
	auto fader_at = [&](ImVec2 p) {
		if (p.y < a.y || p.y > b.y)
			return -1;
		for (int i = 0; i < n; i++)
			if (p.x >= fx0[size_t(i)] - gap * 0.5f && p.x <= fx0[size_t(i)] + fw + gap * 0.5f)
				return i;
		return -1;
	};
	auto value_at = [&](float y, const xg::param &p) {
		const float lo = ftop + cap_h * 0.5f, hi = fbot - cap_h * 0.5f;
		return std::clamp(p.min + int(std::lround((hi - y) / std::max(1.0f, hi - lo) * float(p.max - p.min))), p.min, p.max);
	};
	const int over = hovered ? fader_at(io.MousePos) : -1;
	if (over >= 0 && have[over]) {
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		if (io.MouseWheel != 0.0f) {
			// 1 目で 1（Ctrl で 10）。overview::wheel_steps と同じ
			const int step = std::max(1, int(std::lround(std::fabs(io.MouseWheel)))) * (io.KeyCtrl ? 10 : 1);
			const int nv = std::clamp(vals[over] + (io.MouseWheel > 0 ? step : -step), ps[size_t(over)]->min, ps[size_t(over)]->max);
			if (nv != vals[over]) {
				br.send(m.set(*ps[size_t(over)], part, nv));
				vals[over] = nv;
			}
		}
	}
	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(id, -1);
	if (active && ImGui::IsItemActivated())
		grab = fader_at(io.MousePos);
	if (!active)
		grab = -1;
	st->SetInt(id, grab);
	if (grab >= 0 && have[grab]) {
		const int nv = value_at(io.MousePos.y, *ps[size_t(grab)]);
		if (nv != vals[grab]) {
			drag_send(br, m.set(*ps[size_t(grab)], part, nv));
			vals[grab] = nv;
		}
	}
	const int focus = grab >= 0 ? grab : over;
	if (over >= 0 && have[over])
		out_hover_param(*ps[size_t(over)], part);
	if (focus >= 0 && have[focus]) {
		const char *help = help_for(keys[focus]);
		hint(UI_TEXT(ov_value_tip_fmt, "%s  %s\n%s (drag or wheel)"), official_name(keys[focus]).c_str(), value_text(keys[focus], vals[focus]).c_str(),
		     help ? help : "");
	}
	for (int i = 0; i < n; i++) {
		std::string t = have[i] ? value_text(keys[i], vals[i]) : std::string("--");
		if (t.size() > 3 && t.compare(t.size() - 3, 3, " Hz") == 0)
			t.resize(t.size() - 3);
		// 名前の色は組ごと（EG の組とピッチ EG の組を絵の線と同じ色にする）
		const ImU32 nc = (group_after >= 0 && i > group_after) ? col_second : col_first;
		fader_picture(dl, fx0[size_t(i)], fx0[size_t(i)] + fw, ftop, fbot, vals[i], ps[size_t(i)]->min, ps[size_t(i)]->max, names[i],
		              t.c_str(), focus == i, nc, ((dim_mask >> i) & 1) != 0);
	}
	return focus;
}

// 音量と音程の動きを、同じ鍵を離す時刻で組む
struct env_timeline {
	std::vector<shape::amp_line> amp;
	std::vector<shape::peg_line> peg;
	float t_off = 0, t_end = 1;
	float settle = 0;             // 音量が伸ばしの高さに落ち着くまで（2 秒で打ち切り）
	bool ok = false;
};

env_timeline make_timeline(const voice_ctx &v)
{
	namespace nv = xg::nv;
	env_timeline t;
	if (!v.rom || !v.rec)
		return t;
	const int n = nv::element_count(v.rom, v.rec);
	// 落ち着くまで。音量は減衰 2 の終わり（2 秒で打ち切り）、音程は段 2 の終わり
	float amp_settle = 0;
	for (int e = 0; e < n; e++) {
		const shape::amp_line a = shape::amp_run(v.rom, nv::element(v.rom, v.rec, e), v.blk, -1.0f, 2000.0f);
		amp_settle = std::max(amp_settle, a.pts.back().ms);
	}
	const std::vector<shape::peg_line> p0 = shape::peg_lines(v.rom, v.rec, v.blk, 0.0f);
	float peg_settle = 0;
	for (const shape::peg_line &l : p0)
		peg_settle = std::max(peg_settle, l.keyoff_ms);
	t.settle = amp_settle;
	t.t_off = std::max({ amp_settle, peg_settle, 50.0f }) + 250.0f;
	for (int e = 0; e < n; e++)
		t.amp.push_back(shape::amp_run(v.rom, nv::element(v.rom, v.rec, e), v.blk, t.t_off, t.t_off + 6000.0f));
	t.peg = shape::peg_lines(v.rom, v.rec, v.blk, t.t_off - peg_settle);
	float end = t.t_off + 100.0f;
	for (const shape::amp_line &a : t.amp)
		end = std::max(end, a.pts.back().ms);
	for (const shape::peg_line &l : t.peg)
		end = std::max(end, l.pts.back().ms);
	t.t_end = std::min(end, t.t_off + 5000.0f);
	t.ok = !t.amp.empty();
	return t;
}

// 時間 → 横の位置（log(1 + t / 10ms) の目盛り。数 ms の立ち上がりから数秒の余韻まで 1 枚に入る）
float time_x(float t, float t_end, float x0, float x1)
{
	const float T0 = 10.0f;
	return x0 + (x1 - x0) * std::log1p(std::max(t, 0.0f) / T0) / std::log1p(std::max(t_end, 1.0f) / T0);
}

// 時間の目盛り（10 ms・100 ms・1 s・2 s・4 s）と、鍵を離す時刻の縦の点線
void time_grid(ImDrawList *dl, float t_end, float t_off, float x0, float x1, float top, float bottom)
{
	const float fs = ImGui::GetFontSize();
	for (float t : { 10.0f, 100.0f, 1000.0f, 2000.0f, 4000.0f }) {
		if (t > t_end)
			break;
		const float x = time_x(t, t_end, x0, x1);
		dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), col(ImGuiCol_TextDisabled, 0.15f));
		char g[16];
		if (t >= 1000.0f) std::snprintf(g, sizeof(g), "%.0fs", t / 1000.0f);
		else              std::snprintf(g, sizeof(g), "%.0fms", t);
		dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x + 2.0f, bottom - fs * 0.6f), col(ImGuiCol_TextDisabled, 0.7f), g);
	}
	const float xo = time_x(t_off, t_end, x0, x1);
	for (float y = top; y < bottom; y += fs * 0.5f)
		dl->AddLine(ImVec2(xo, y), ImVec2(xo, std::min(bottom, y + fs * 0.25f)), col(ImGuiCol_TextDisabled, 0.5f));
	dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(xo + 2.0f, bottom - fs * 1.2f), col(ImGuiCol_TextDisabled, 0.8f), UI_TEXT(ov_discrete, "Rel"));
}

// spectrum_view の線 1 本ぶんの状態。下がるときはゆっくり（1 コマ 1.5 dB）、山の高さはさらにゆっくり
struct spec_curve {
	int part = -1;
	std::vector<float> sm;
	std::vector<float> pw;        // 力（線形）の移動平均。コマごとの雑音の揺れをならす
	float peak = -200.0f;
	bool ok = false;
};

// 鳴っていないとみなす大きさと、目盛りの上端の下限（どちらも FFT の大きさの dB。ハン窓・2048 点では、
// 振幅 A の正弦波の山がおよそ 20 log10(A × 512)）。MEG のリバーブは音が止まったあとも振幅 40 ほどの
// 直流のずれと 1-2 の揺れが残り続ける（固定小数点の丸め。2026-09-22 にエミュで測った）。
// 直流は引き、振幅 16 ほどより小さいものは鳴っていないことにし、目盛りの上端は振幅 400 ほどより下げない
// （小さな残りかすを「いちばん大きい所から 60 dB」で画面いっぱいに引き伸ばさない）
constexpr float SPEC_SILENT_DB = 78.0f;
// 20Hz より下の bin は見ない（音ではなく、窓の中での音量の変わりぶん＝包絡線がここに出る）
constexpr size_t SPEC_BIN0 = size_t(20.0 * double(bridge::SCOPE_N) / 44100.0) + 1;
constexpr float SPEC_REF_MIN_DB = 106.0f;

void spec_update(bridge &br, int part, int src, spec_curve &c)
{
	static std::vector<float> wave(bridge::SCOPE_N);
	c.ok = false;
	if (br.read_scope(wave.data(), src) != part)
		return;
	double mean = 0;
	for (float v : wave)
		mean += v;
	mean /= double(wave.size());
	for (float &v : wave)
		v -= float(mean);
	std::vector<float> db;
	spectrum::magnitude_db(wave.data(), bridge::SCOPE_N, db);
	if (c.part != part || c.sm.size() != db.size()) {
		c.part = part;
		c.sm.assign(db.size(), -200.0f);
		c.pw.assign(db.size(), 0.0f);
		c.peak = -200.0f;
	}
	// **力でならす**（雑音のコマごとの揺れを落とす。山はほとんど動かない）。
	// 前は「下がるときは 1 コマ 1.5dB まで」と持ちこたえさせていたが、押した瞬間の立ち上がりには
	// 低いほうまで音が入っているので、その持ちこたえが 0.8 秒ほど平たい山として居座っていた（2026-09-23）。
	// 持ちこたえはやめて、ならしだけにする（1 コマで 5dB ほど下がる）
	float frame_peak = -200.0f;
	for (size_t k = SPEC_BIN0; k < db.size(); k++) {
		const float p = float(std::pow(10.0, double(db[k]) / 10.0));
		c.pw[k] = c.pw[k] > 0.0f ? c.pw[k] * 0.3f + p * 0.7f : p;
		c.sm[k] = float(10.0 * std::log10(std::max(double(c.pw[k]), 1e-20)));
		frame_peak = std::max(frame_peak, c.sm[k]);
	}
	c.peak = std::max(frame_peak, c.peak - 0.5f);
	c.ok = c.peak > SPEC_SILENT_DB;
}

// 横の位置ごとに、その幅（最低でも 3 本ぶん）に入る bin を**力で平均**して折れ線に。
// いちばん大きい値を拾うと、低いほうは 1 本の bin が画面の広い範囲に引き伸ばされて、
// 窓のにじみや雑音（山より 40-60 dB 下）がそのまま棒になって激しく揺れた（2026-09-23）
std::vector<ImVec2> spec_points(const spec_curve &c, float floor_db, float x0, float x1, float top, float bottom)
{
	const float F_LO = 20.0f, F_HI = 20000.0f;
	const float span = x1 - x0;
	std::vector<ImVec2> sp;
	const float step = std::max(1.5f, ImGui::GetFontSize() * 0.12f);
	const long last = long(c.sm.size()) - 1;
	for (float x = x0; x <= x1; x += step) {
		const float f0 = F_LO * std::pow(F_HI / F_LO, (x - x0) / span);
		const float f1 = F_LO * std::pow(F_HI / F_LO, (x + step - x0) / span);
		long k0 = long(f0 * float(bridge::SCOPE_N) / 44100.0f), k1 = long(f1 * float(bridge::SCOPE_N) / 44100.0f);
		if (k1 - k0 < 2) {                       // 最低 3 本ぶん（低いほうの 1 本飛びをならす）
			const long mid = (k0 + k1) / 2;
			k0 = mid - 1;
			k1 = mid + 1;
		}
		k0 = std::clamp<long>(k0, long(SPEC_BIN0), last);
		k1 = std::clamp<long>(std::max(k1, k0), 1, last);
		double pw = 0;
		for (long k = k0; k <= k1; k++)
			pw += std::pow(10.0, double(c.sm[size_t(k)]) / 10.0);
		const float v = float(10.0 * std::log10(std::max(pw / double(k1 - k0 + 1), 1e-20)));
		const float t = std::clamp((v - floor_db) / 60.0f, 0.0f, 1.0f);
		sp.push_back(ImVec2(x, bottom - (bottom - top) * t));
	}
	return sp;
}

// ---- 一覧の小さなスペクトラム（パートごとの声の和と、マスターの最終の出力）
// 1024 点（23ms）なので低いほうは粗いが、欄が小さいので足りる。力でならし、山の高さはゆっくり下げる
struct mini_spec {
	unsigned serial = 0;
	std::vector<float> pw;
	float peak = -200.0f;
	bool ok = false;
};

mini_spec &mini_spec_of(int src)
{
	static std::array<mini_spec, bridge::PSCOPE_SRCS> all;
	return all[size_t(std::clamp(src, 0, bridge::PSCOPE_SRCS - 1))];
}

// 1024 点では 4096 点より 12dB 小さく出る（SPEC_SILENT_DB・SPEC_REF_MIN_DB は 4096 点の値）
constexpr float MINI_SILENT_DB = SPEC_SILENT_DB - 12.0f;
constexpr float MINI_REF_MIN_DB = SPEC_REF_MIN_DB - 12.0f;

void mini_spec_update(bridge &br, int src, mini_spec &c)
{
	const unsigned serial = br.part_scopes_serial();
	if (serial == c.serial)
		return;
	c.serial = serial;
	static std::vector<float> wave(bridge::PSCOPE_N);
	if (!br.read_part_scope(src, wave.data())) {
		c.ok = false;
		return;
	}
	double mean = 0;
	for (float v : wave)
		mean += v;
	mean /= double(wave.size());
	for (float &v : wave)
		v -= float(mean);
	std::vector<float> db;
	spectrum::magnitude_db(wave.data(), bridge::PSCOPE_N, db);
	if (c.pw.size() != db.size())
		c.pw.assign(db.size(), 0.0f);
	float frame_peak = -200.0f;
	for (size_t k = 1; k < db.size(); k++) {
		const float p = float(std::pow(10.0, double(db[k]) / 10.0));
		c.pw[k] = c.pw[k] > 0.0f ? c.pw[k] * 0.4f + p * 0.6f : p;
		frame_peak = std::max(frame_peak, float(10.0 * std::log10(std::max(double(c.pw[k]), 1e-20))));
	}
	c.peak = std::max(frame_peak, c.peak - 1.0f);
	c.ok = frame_peak > MINI_SILENT_DB;
}

void mini_spec_draw(ImDrawList *dl, const mini_spec &c, ImVec2 a, ImVec2 b, ImU32 color)
{
	dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 50), 2.0f);
	if (!c.ok || c.pw.empty() || b.x - a.x < 4.0f)
		return;
	const float F_LO = 30.0f, F_HI = 16000.0f;
	const float floor_db = std::max(c.peak, MINI_REF_MIN_DB) - 60.0f;
	const float span = b.x - a.x;
	const float step = std::max(1.0f, span / 64.0f);
	const long last = long(c.pw.size()) - 1;
	const float bin_hz = 44100.0f / float(bridge::PSCOPE_N);
	std::vector<ImVec2> sp;
	for (float x = a.x; x <= b.x + 0.01f; x += step) {
		const float f0 = F_LO * std::pow(F_HI / F_LO, (x - a.x) / span);
		const float f1 = F_LO * std::pow(F_HI / F_LO, std::min(x + step - a.x, span) / span);
		const long k0 = std::clamp<long>(long(f0 / bin_hz), 1, last);
		const long k1 = std::clamp<long>(std::max(long(f1 / bin_hz), k0), 1, last);
		double pw = 0;
		for (long k = k0; k <= k1; k++)
			pw += double(c.pw[size_t(k)]);
		const float v = float(10.0 * std::log10(std::max(pw / double(k1 - k0 + 1), 1e-20)));
		const float t = std::clamp((v - floor_db) / 60.0f, 0.0f, 1.0f);
		sp.push_back(ImVec2(std::min(x, b.x), b.y - (b.y - a.y) * t));
	}
	const ImU32 fill = (color & ~IM_COL32_A_MASK) | (ImU32(90) << IM_COL32_A_SHIFT);
	const ImU32 edge = (color & ~IM_COL32_A_MASK) | (ImU32(220) << IM_COL32_A_SHIFT);
	for (size_t i = 1; i < sp.size(); i++)
		dl->AddQuadFilled(ImVec2(sp[i - 1].x, b.y), sp[i - 1], sp[i], ImVec2(sp[i].x, b.y), fill);
	dl->AddPolyline(sp.data(), int(sp.size()), edge, 0, 1.0f);
}

} // namespace

int overview::fader_strip(const char *id, const char *const *keys, const char *const *names, int n, int group_after, int part,
                          xg::model &m, bridge &br, ImVec2 size, unsigned dim_mask)
{
	std::vector<int> vals(static_cast<size_t>(n));
	std::unique_ptr<bool[]> have(new bool[size_t(n)]);
	for (int i = 0; i < n; i++) {
		vals[size_t(i)] = P(keys[i]).def;
		have[size_t(i)] = m.get(P(keys[i]), part, vals[size_t(i)]);
	}
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft);
	const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
	return fader_row(keys, names, n, group_after, part, m, br, pos, ImVec2(pos.x + size.x, pos.y + size.y), hovered, active,
	                 ImGui::GetItemID(), vals.data(), have.get(), 0, 0, dim_mask);
}

void overview::spectrum_view(bridge &br, int part, int src, int ghost_src, int key, ImVec2 a, ImVec2 b, const char *label,
                             bool backdrop)
{
	static std::map<int, spec_curve> curves;       // key * 2 が出す線、key * 2 + 1 が重ねる線
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float F_LO = 20.0f, F_HI = 20000.0f;
	const float x0 = a.x, x1 = b.x, top = a.y, bottom = b.y;
	auto x_hz = [&](float f) { return x0 + (x1 - x0) * std::log(std::clamp(f, F_LO, F_HI) / F_LO) / std::log(F_HI / F_LO); };
	if (!backdrop)
		dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 60), 3.0f);
	for (float f : { 100.0f, 1000.0f, 10000.0f }) {
		if (backdrop)
			break;
		const float x = x_hz(f);
		dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), col(ImGuiCol_TextDisabled, 0.15f));
		const char *t = f >= 10000.0f ? "10k" : f >= 1000.0f ? "1k" : "100";
		dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x + 2.0f, bottom - fs * 0.6f), col(ImGuiCol_TextDisabled, 0.7f), t);
	}
	spec_curve &main = curves[key * 2];
	spec_update(br, part, src, main);
	spec_curve *ghost = nullptr;
	if (ghost_src >= 0) {
		ghost = &curves[key * 2 + 1];
		spec_update(br, part, ghost_src, *ghost);
		if (!ghost->ok)
			ghost = nullptr;
	}
	// 目盛りは 2 本のうち大きいほうにそろえる（入口と出口の大きさの違いがそのまま見える）
	float ref = main.ok ? main.peak : -200.0f;
	if (ghost)
		ref = std::max(ref, ghost->peak);
	if (ref > SPEC_SILENT_DB) {
		const float floor_db = std::max(ref, SPEC_REF_MIN_DB) - 60.0f;
		if (ghost) {
			const std::vector<ImVec2> gp = spec_points(*ghost, floor_db, x0, x1, top, bottom);
			dl->AddPolyline(gp.data(), int(gp.size()), IM_COL32(200, 200, 210, 110), 0, 1.0f);
		}
		if (main.ok) {
			const std::vector<ImVec2> sp = spec_points(main, floor_db, x0, x1, top, bottom);
			const ImU32 fill = IM_COL32(120, 220, 170, 55), edge = IM_COL32(140, 240, 190, 170);
			for (size_t i = 1; i < sp.size(); i++)
				dl->AddQuadFilled(ImVec2(sp[i - 1].x, bottom), sp[i - 1], sp[i], ImVec2(sp[i].x, bottom), fill);
			dl->AddPolyline(sp.data(), int(sp.size()), edge, 0, 1.0f);
		}
	} else if (!backdrop) {
		const char *t = UI_TEXT(ov_silent, "(silent)");
		const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs * 0.6f, FLT_MAX, 0.0f, t);
		dl->AddText(ImGui::GetFont(), fs * 0.6f, ImVec2((x0 + x1 - ts.x) * 0.5f, (top + bottom - ts.y) * 0.5f), col(ImGuiCol_TextDisabled, 0.6f), t);
	}
	if (label)
		dl->AddText(ImGui::GetFont(), fs * 0.6f, ImVec2(x0 + 3.0f, top + 2.0f), col(ImGuiCol_TextDisabled, 0.9f), label);
}

// フィルタとパートの EQ（音色の窓の中央の列。上下 2 段がつながったメゾネット）。上の段が絵、下の段が
// Cutoff・Resonance・HPF と EQ の 4 つ（低音・高音のゲインと周波数）のフェーダー。
// 絵の横は**実際の周波数**（20 Hz-20 kHz の対数）で、次を同じ目盛りで重ねる:
//   * このパートが今出している音のスペクトラム（緑。声ごとの出力をパートに振り分けて足したもの。6.216）
//   * フィルタとパートの EQ を合わせた実際の周波数特性（太線）。フィルタだけ（細線）と EQ だけ（点線）も。
//     どちらも声ごとに掛かり、EQ はフィルタのすぐ後ろ（インサーションより前）
// 実際に切る周波数に縦の点線（LPF 橙・HPF 桃色）、EQ の周波数に ● 印。絵は見るだけ。
// 一覧の小さなマスは filter_small（目安の形）
void overview::filter_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	if (compact) {
		filter_small(part, m, br, w, h, compact);
		return;
	}
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	constexpr int NF = 7;
	static const char *const KEYS[NF] = { "part.cutoff", "part.resonance", "part.hpf_cutoff",
		"part.eq_bass_gain", "part.eq_bass_freq", "part.eq_treble_gain", "part.eq_treble_freq" };
	static const char *const NAMES[NF] = { "Cutoff", "Reso", "HPF", "Lo G", "Lo F", "Hi G", "Hi F" };
	const xg::param *ps[NF];
	int vals[NF];
	bool have[NF];
	for (int i = 0; i < NF; i++) {
		ps[i] = &P(KEYS[i]);
		vals[i] = ps[i]->def;
		have[i] = m.get(*ps[i], part, vals[i]);
	}
	const bool known = have[0] && have[1];

	ImGui::PushID("filterbig");
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##filter", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();

	// メゾネット: 上の段に絵、下の段にフェーダー。境目に床の線
	const float pad = fs * 0.25f;
	const float split = pos.y + h * MAISON_SPLIT;
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
	const int focus = fader_row(KEYS, NAMES, NF, 2, part, m, br, ImVec2(pos.x + pad, split + pad), ImVec2(pos.x + w - pad, pos.y + h - pad),
	                            hovered, active, id, vals, have);
	dl->AddLine(ImVec2(pos.x, split), ImVec2(pos.x + w, split), col(ImGuiCol_Border), 1.0f);
	const float x0 = pos.x + pad, x1 = pos.x + w - pad;
	const float top = pos.y + pad, bottom = split - pad;
	const float span = x1 - x0;
	const float F_LO = 20.0f, F_HI = 20000.0f;
	auto x_hz = [&](float f) { return x0 + span * std::log(std::clamp(f, F_LO, F_HI) / F_LO) / std::log(F_HI / F_LO); };
	const float DB_TOP = 24.0f, DB_BOTTOM = -36.0f;
	auto y_db = [&](float db) { return top + (bottom - top) * (DB_TOP - std::clamp(db, DB_BOTTOM, DB_TOP)) / (DB_TOP - DB_BOTTOM); };

	// ---- 目盛り（実際の周波数と 0 dB）
	for (float f : { 100.0f, 1000.0f, 10000.0f }) {
		const float x = x_hz(f);
		dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), col(ImGuiCol_TextDisabled, 0.15f));
		const char *t = f >= 10000.0f ? "10k" : f >= 1000.0f ? "1k" : "100";
		dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x + 2.0f, bottom - fs * 0.6f), col(ImGuiCol_TextDisabled, 0.7f), t);
	}
	dl->AddLine(ImVec2(x0, y_db(0.0f)), ImVec2(x1, y_db(0.0f)), col(ImGuiCol_TextDisabled, 0.35f));

	// ---- このパートの音のスペクトラム（緑）。spectrum_view と同じ作り（直流を引き、幅ごとに力で平均、
	// 縦はいちばん大きい所から 60 dB 下まで、下がるときはゆっくり）
	{
		static spec_curve fc;
		spec_update(br, part, 0, fc);
		if (fc.ok) {
			const std::vector<ImVec2> sp = spec_points(fc, fc.peak - 60.0f, x0, x1, top, bottom);
			const ImU32 fill = IM_COL32(120, 220, 170, 55), edge = IM_COL32(140, 240, 190, 140);
			for (size_t i = 1; i < sp.size(); i++)
				dl->AddQuadFilled(ImVec2(sp[i - 1].x, bottom), sp[i - 1], sp[i], ImVec2(sp[i].x, bottom), fill);
			dl->AddPolyline(sp.data(), int(sp.size()), edge, 0, 1.0f);
		}
	}

	// ---- フィルタの実際の周波数特性と、切る周波数の印
	voice_ctx v;
	if (known && voice_of(part, v)) {
		v.blk[0x18] = u8(vals[0]);
		v.blk[0x19] = u8(vals[1]);
		if (have[2])
			v.blk[xg::ram::PART_HPF_RAM] = u8(vals[2]);
		if (have[3]) v.blk[xg::ram::PART_EQ_LGAIN] = u8(vals[3]);
		if (have[4]) v.blk[xg::ram::PART_EQ_LFREQ] = u8(vals[4]);
		if (have[5]) v.blk[xg::ram::PART_EQ_HGAIN] = u8(vals[5]);
		if (have[6]) v.blk[xg::ram::PART_EQ_HFREQ] = u8(vals[6]);
		const std::vector<shape::filter_line> ls = shape::filter_lines(v.rom, v.rec, v.blk, 160);
		if (!ls.empty()) {
			const shape::filter_line &L = lead_line(ls);
			std::vector<float> hz;
			for (const shape::pt &p : L.pts)
				hz.push_back(p.ms);
			const std::vector<shape::pt> eq = shape::eq_response(v.rom, v.blk, hz);
			std::vector<ImVec2> fpts, epts, tpts;
			for (size_t i = 0; i < L.pts.size(); i++) {
				const float x = x_hz(L.pts[i].ms);
				const float edb = i < eq.size() ? eq[i].cents : 0.0f;
				fpts.push_back(ImVec2(x, y_db(L.pts[i].cents)));
				epts.push_back(ImVec2(x, y_db(edb)));
				tpts.push_back(ImVec2(x, y_db(L.pts[i].cents + edb)));
			}
			// 合わせた特性を塗って太線、フィルタだけを細線、EQ だけを点線
			dl->PathClear();
			dl->PathLineTo(ImVec2(tpts.front().x, bottom));
			for (const ImVec2 &p : tpts)
				dl->PathLineTo(p);
			dl->PathLineTo(ImVec2(tpts.back().x, bottom));
			dl->PathFillConcave(col(ImGuiCol_SliderGrab, 0.18f));
			dl->AddPolyline(fpts.data(), int(fpts.size()), IM_COL32(150, 190, 255, 150), 0, 1.0f);
			for (size_t i = 1; i < epts.size(); i += 2)
				dl->AddLine(epts[i - 1], epts[i], IM_COL32(255, 210, 110, 200), 1.2f);
			dl->AddPolyline(tpts.data(), int(tpts.size()), col(ImGuiCol_SliderGrabActive), 0, std::max(2.0f, fs * 0.14f));
			// パートの EQ の周波数の位置に ● 印（EQ だけの点線の上）。触れないが、フェーダーとの関係を見せる。
			// そのフェーダー（ゲインか周波数）にカーソルが載っているか、つまんでいる間は大きく
			if (have[4] && have[6]) {
				const float fl = float(eq::HZ[std::clamp(vals[4], 0, 60)]);
				const float fh = float(eq::HZ[std::clamp(vals[6], 0, 60)]);
				const std::vector<shape::pt> at = shape::eq_response(v.rom, v.blk, { fl, fh });
				const float r0 = std::max(3.0f, fs * 0.26f);
				for (int k = 0; k < 2 && k < int(at.size()); k++) {
					const bool lit = focus == 3 + k * 2 || focus == 4 + k * 2;
					const ImVec2 c(x_hz(at[size_t(k)].ms), y_db(at[size_t(k)].cents));
					const float rr = lit ? r0 * 1.5f : r0;
					dl->AddCircleFilled(c, rr, IM_COL32(255, 210, 110, 255));
					dl->AddCircle(c, rr, IM_COL32(40, 30, 10, 255), 0, 1.5f);
					char t[16];
					std::snprintf(t, sizeof(t), "%s %s", k ? "Hi" : "Lo", eq::hz_text(k ? vals[6] : vals[4]).c_str());
					const float tfs = fs * 0.6f;
					const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(tfs, FLT_MAX, 0.0f, t);
					const float tx = std::clamp(c.x - ts.x * 0.5f, x0 + 1.0f, x1 - ts.x - 1.0f);
					const float ty = c.y + rr + 2.0f + ts.y < bottom ? c.y + rr + 2.0f : c.y - rr - 2.0f - ts.y;
					dl->AddRectFilled(ImVec2(tx - 2, ty - 1), ImVec2(tx + ts.x + 2, ty + ts.y + 1), IM_COL32(0, 0, 0, 140), 3.0f);
					dl->AddText(ImGui::GetFont(), tfs, ImVec2(tx, ty), IM_COL32(255, 210, 110, lit ? 255 : 210), t);
				}
			}
			// 切る周波数: いちばん大きい所から 3 dB 下がる所（山があれば山の頂）。filter_small と同じ決め方
			float peak = -1e9f, peak_hz = 20.0f;
			for (const shape::pt &p : L.pts)
				if (p.cents > peak) { peak = p.cents; peak_hz = p.ms; }
			float lpf_hz = 0;
			for (size_t i = L.pts.size(); i-- > 0;)
				if (L.pts[i].cents >= peak - 3.0f) { lpf_hz = L.pts[i].ms; break; }
			if (peak > 3.0f)
				lpf_hz = peak_hz;
			float hpf_hz = 0;
			for (const shape::pt &p : L.pts)
				if (p.cents >= peak - 3.0f) { hpf_hz = p.ms; break; }
			auto hz_text = [](float f) {
				char t[24];
				if (f >= 19000.0f) std::snprintf(t, sizeof(t), "%s", UI_TEXT(ov_khz, "20 kHz or more"));
				else if (f >= 1000.0f) std::snprintf(t, sizeof(t), "%.1f kHz", f / 1000.0f);
				else std::snprintf(t, sizeof(t), "%.0f Hz", f);
				return std::string(t);
			};
			auto mark = [&](float f, ImU32 c, const char *label, int row) {
				if (f <= 0.0f || f >= 19000.0f)
					return;
				const float x = x_hz(f);
				for (float y = top; y < bottom; y += fs * 0.4f)
					dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(bottom, y + fs * 0.2f)), c, 1.5f);
				char t[40];
				std::snprintf(t, sizeof(t), "%s %s", label, hz_text(f).c_str());
				const float tfs = fs * 0.65f;
				const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(tfs, FLT_MAX, 0.0f, t);
				const float tx = std::min(x + 3.0f, x1 - ts.x - 2.0f);
				const float ty = top + 2.0f + float(row) * (ts.y + 2.0f);
				dl->AddRectFilled(ImVec2(tx - 2, ty - 1), ImVec2(tx + ts.x + 2, ty + ts.y + 1), IM_COL32(0, 0, 0, 140), 3.0f);
				dl->AddText(ImGui::GetFont(), tfs, ImVec2(tx, ty), c, t);
			};
			mark(lpf_hz, IM_COL32(255, 170, 60, 230), "LPF", 0);
			if (L.hpf)
				mark(hpf_hz, IM_COL32(255, 120, 200, 230), "HPF", 1);
			char s[96];
			std::snprintf(s, sizeof(s), "Cutoff : %s (%s)", xg::format(*ps[0], vals[0]).c_str(), hz_text(lpf_hz).c_str());
			shape_value(s);
			std::snprintf(s, sizeof(s), "Reso : %s", xg::format(*ps[1], vals[1]).c_str());
			shape_value(s);
			if (have[2]) {
				std::snprintf(s, sizeof(s), L.hpf ? UI_TEXT(ov_hpf_on_fmt, "HPF : %s (%s)") : UI_TEXT(ov_hpf_off_fmt, "HPF : %s (off)"),
				              xg::format(*ps[2], vals[2]).c_str(), hz_text(hpf_hz).c_str());
				shape_value(s);
			}
			// 凡例（右下に小さく）
			{
				const float lfs = fs * 0.6f;
				const char *const items[3] = { UI_TEXT(ov_legend_combined, "Combined"), UI_TEXT(ov_legend_filter, "Filter"), "EQ" };
				const ImU32 cols[3] = { col(ImGuiCol_SliderGrabActive), IM_COL32(150, 190, 255, 200), IM_COL32(255, 210, 110, 220) };
				float ly = bottom - lfs * 4.2f;
				for (int k = 0; k < 3; k++) {
					const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(lfs, FLT_MAX, 0.0f, items[k]);
					const float lx = x1 - ts.x - fs * 0.3f;
					dl->AddLine(ImVec2(lx - fs * 0.9f, ly + ts.y * 0.5f), ImVec2(lx - fs * 0.2f, ly + ts.y * 0.5f), cols[k], k == 0 ? 2.0f : 1.2f);
					dl->AddText(ImGui::GetFont(), lfs, ImVec2(lx, ly), col(ImGuiCol_TextDisabled, 0.9f), items[k]);
					ly += ts.y + 1.0f;
				}
			}
		}
	} else if (!known) {
		const ImVec2 ts = ImGui::CalcTextSize("--");
		dl->AddText(ImVec2(x0 + (span - ts.x) * 0.5f, (top + bottom - ts.y) * 0.5f), col(ImGuiCol_TextDisabled), "--");
	}
	dl->PopClipRect();
	ImGui::PopID();
}

// EG とピッチ EG（音色の窓の右の列。上下 2 段がつながったメゾネット）。上の段に、音量の形（青・塗り）と
// 音程の動き（橙の線）を**同じ実際の時間の目盛り・同じ鍵を離す時刻**で 1 枚に描く。縦は、左が音量
// （0 dB-−60 dB）、右が音程（± セント）。下の段に EG の 3 つとピッチ EG の 4 つのフェーダー。絵は見るだけ。
// 一覧の小さなマスは eg_small・peg_small（目安の形）
void overview::env_cell(int part, xg::model &m, bridge &br, float w, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	constexpr int NF = 7;
	static const char *const KEYS[NF] = { "part.attack", "part.decay", "part.release",
		"part.peg_init_level", "part.peg_attack_time", "part.peg_rel_level", "part.peg_rel_time" };
	// ピッチ EG の組は名前を短く（組の色で EG と分ける）
	static const char *const NAMES[NF] = { "Attack", "Decay", "Release", "Init", "Atk", "RelLv", "RelTm" };
	int vals[NF];
	bool have[NF];
	bool known = true;
	for (int i = 0; i < NF; i++) {
		vals[i] = P(KEYS[i]).def;
		have[i] = m.get(P(KEYS[i]), part, vals[i]);
		known = known && have[i];
	}

	ImGui::PushID("envbig");
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##env", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
	const float pad = fs * 0.25f;
	const float split = pos.y + h * MAISON_SPLIT;
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
	const int focus = fader_row(KEYS, NAMES, NF, 2, part, m, br, ImVec2(pos.x + pad, split + pad), ImVec2(pos.x + w - pad, pos.y + h - pad),
	          hovered, active, id, vals, have, IM_COL32(150, 190, 255, 255), IM_COL32(255, 170, 130, 255));
	dl->AddLine(ImVec2(pos.x, split), ImVec2(pos.x + w, split), col(ImGuiCol_Border), 1.0f);
	// 絵。上に実際の時間の字を 2 行置くぶん空け
	const float line = fs * 0.75f;
	const float x0 = pos.x + pad + fs * 1.6f, x1 = pos.x + w - pad - fs * 1.6f;
	const float top = pos.y + pad + line * 2.2f, bottom = split - pad;
	// 背景に、EG を通したあとの音（インサーションの前）のスペクトラム。フィルタの絵と同じく横は実際の周波数
	spectrum_view(br, part, 0, -1, 1, ImVec2(pos.x + pad, top), ImVec2(pos.x + w - pad, bottom), nullptr, true);
	const float mid = (top + bottom) * 0.5f, half = (bottom - top) * 0.46f;

	voice_ctx v;
	if (known && voice_of(part, v)) {
		v.blk[0x1a] = u8(vals[0]); v.blk[0x1b] = u8(vals[1]); v.blk[0x1c] = u8(vals[2]);
		v.blk[0x62] = u8(vals[3]); v.blk[0x63] = u8(vals[4]); v.blk[0x64] = u8(vals[5]); v.blk[0x65] = u8(vals[6]);
		const env_timeline t = make_timeline(v);
		if (t.ok) {
			time_grid(dl, t.t_end, t.t_off, x0, x1, top, bottom);
			// 縦の目盛り。左は音量（0 dB が上、-60 dB が下）、右は音程（真ん中が 0 セント）
			auto y_db = [&](float db) { return top + (bottom - top) * std::clamp(-db / 60.0f, 0.0f, 1.0f); };
			float span = 50.0f;
			for (const shape::peg_line &l : t.peg)
				for (const shape::pt &p : l.pts)
					span = std::max(span, std::fabs(p.cents) * 1.1f);
			auto y_c = [&](float c) { return mid - half * c / span; };
			const float tfs = fs * 0.55f;
			dl->AddText(ImGui::GetFont(), tfs, ImVec2(pos.x + pad, top - tfs * 0.5f), IM_COL32(120, 170, 255, 200), "0dB");
			dl->AddText(ImGui::GetFont(), tfs, ImVec2(pos.x + pad, bottom - tfs), IM_COL32(120, 170, 255, 200), "-60");
			char g[24];
			std::snprintf(g, sizeof(g), "+%.0f", span);
			dl->AddText(ImGui::GetFont(), tfs, ImVec2(x1 + 2.0f, y_c(span) - tfs * 0.5f), IM_COL32(255, 160, 120, 220), g);
			dl->AddText(ImGui::GetFont(), tfs, ImVec2(x1 + 2.0f, mid - tfs * 0.5f), IM_COL32(255, 160, 120, 220), "0c");
			std::snprintf(g, sizeof(g), "-%.0f", span);
			dl->AddText(ImGui::GetFont(), tfs, ImVec2(x1 + 2.0f, y_c(-span) - tfs * 0.5f), IM_COL32(255, 160, 120, 220), g);
			dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), IM_COL32(255, 160, 120, 50));
			// 音量の形（鳴る要素ごと。最初のものを塗って太く）
			const shape::amp_line &A = lead_line(t.amp);
			for (const shape::amp_line &a : t.amp) {
				std::vector<ImVec2> pts;
				for (const shape::pt &p : a.pts)
					pts.push_back(ImVec2(time_x(p.ms, t.t_end, x0, x1), y_db(p.cents)));
				if (&a == &A) {
					dl->PathClear();
					dl->PathLineTo(ImVec2(pts.front().x, bottom));
					for (const ImVec2 &p : pts)
						dl->PathLineTo(p);
					dl->PathLineTo(ImVec2(pts.back().x, bottom));
					dl->PathFillConcave(col(ImGuiCol_SliderGrab, 0.22f));
				}
				dl->AddPolyline(pts.data(), int(pts.size()), &a == &A ? col(ImGuiCol_SliderGrabActive) : col(ImGuiCol_SliderGrabActive, 0.4f),
				                0, &a == &A ? std::max(2.0f, fs * 0.12f) : 1.0f);
			}
			// 音程の動き
			const shape::peg_line &L = lead_line(t.peg);
			for (const shape::peg_line &l : t.peg) {
				std::vector<ImVec2> pts;
				for (size_t i = 0; i < l.pts.size(); i++) {
					pts.push_back(ImVec2(time_x(l.pts[i].ms, t.t_end, x0, x1), y_c(l.pts[i].cents)));
					if (i + 1 == l.pts.size())
						pts.push_back(ImVec2(x1, pts.back().y));
				}
				dl->AddPolyline(pts.data(), int(pts.size()), &l == &L ? IM_COL32(255, 160, 120, 255) : IM_COL32(255, 160, 120, 110),
				                0, &l == &L ? std::max(2.0f, fs * 0.12f) : 1.0f);
			}
			// 実際の時間（上に 2 行）と、帯の一覧
			float rel = 0;
			for (const shape::pt &p : A.pts)
				if (p.ms > t.t_off) {
					rel = p.ms - t.t_off;
					if (p.cents <= -60.0f)
						break;
				}

			// ---- フェーダーに対応する点（触れない）。そのフェーダーにカーソルが載っているか、つまんでいる間は大きく
			{
				const float r0 = std::max(3.0f, fs * 0.26f);
				auto dot = [&](ImVec2 c, ImU32 fill, const char *mark, bool lit) {
					const float rr = lit ? r0 * 1.5f : r0;
					dl->AddCircleFilled(c, rr, fill);
					dl->AddCircle(c, rr, IM_COL32(20, 20, 30, 255), 0, 1.5f);
					const float tfs = fs * 0.6f;
					const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(tfs, FLT_MAX, 0.0f, mark);
					const float tx = std::clamp(c.x + rr + 1.0f, x0, x1 - ts.x);
					const float ty = std::clamp(c.y - rr - ts.y, top, bottom - ts.y);
					dl->AddText(ImGui::GetFont(), tfs, ImVec2(tx, ty), fill, mark);
				};
				// 音量: Attack は立ち上がりきった所、Decay は伸ばしの高さに落ち着いた所（2 秒で打ち切り）、
				// Release は離してから -60 dB まで下がった所
				auto amp_at = [&](float ms) {
					float db = A.pts.front().cents;
					for (const shape::pt &p : A.pts)
						if (p.ms <= ms)
							db = p.cents;
					return db;
				};
				const ImU32 blue = IM_COL32(150, 190, 255, 255), orange = IM_COL32(255, 170, 130, 255);
				dot(ImVec2(time_x(A.attack_ms, t.t_end, x0, x1), y_db(0.0f)), blue, "A", focus == 0);
				const float dec_t = std::min(t.settle, t.t_off);
				dot(ImVec2(time_x(dec_t, t.t_end, x0, x1), y_db(amp_at(dec_t))), blue, "D", focus == 1);
				dot(ImVec2(time_x(t.t_off + rel, t.t_end, x0, x1), y_db(amp_at(t.t_off + rel))), blue, "R", focus == 2);
				// 音程: Init は出だし、Atk は最初の段（アタック）を終えた所、RelLv・RelTm は離したあとの行き着く先
				if (!L.pts.empty()) {
					dot(ImVec2(time_x(0.0f, t.t_end, x0, x1), y_c(L.pts.front().cents)), orange, "I", focus == 3);
					if (L.pts.size() > 1 && L.pts[1].ms < L.keyoff_ms - 0.5f)
						dot(ImVec2(time_x(L.pts[1].ms, t.t_end, x0, x1), y_c(L.pts[1].cents)), orange, "A", focus == 4);
					dot(ImVec2(time_x(L.pts.back().ms, t.t_end, x0, x1), y_c(L.pts.back().cents)), orange, "R", focus == 5 || focus == 6);
				}
			}
			const float dec = std::max(0.0f, t.settle - A.attack_ms);
			char s[128];
			if (t.settle >= 1990.0f)
				std::snprintf(s, sizeof(s), UI_TEXT(ov_env_vol_long, "Volume  Attack %.0f ms   Decay 2000+ ms   Release %.0f ms"), A.attack_ms, rel);
			else
				std::snprintf(s, sizeof(s), UI_TEXT(ov_env_vol, "Volume  Attack %.0f ms   Decay %.0f ms   Release %.0f ms"), A.attack_ms, dec, rel);
			dl->AddText(ImGui::GetFont(), line, ImVec2(pos.x + pad + 2.0f, pos.y + pad), IM_COL32(150, 190, 255, 255), s);
			shape_value(s);
			float atk = 0;
			for (const shape::pt &p : L.pts)
				if (p.ms < L.keyoff_ms - 0.5f)      // 離す時刻の点は数えない
					atk = p.ms;
			std::snprintf(s, sizeof(s), UI_TEXT(ov_env_pitch, "Pitch  Init %+.0f cent   Attack %.0f ms   Release %.0f ms → %+.0f cent"),
			              L.pts.front().cents, atk, L.pts.back().ms - L.keyoff_ms, L.pts.back().cents);
			dl->AddText(ImGui::GetFont(), line, ImVec2(pos.x + pad + 2.0f, pos.y + pad + line * 1.1f), IM_COL32(255, 170, 130, 255), s);
			shape_value(s);
		}
	}
	dl->PopClipRect();
	ImGui::PopID();
}

// ---- ドラムの 1 打（音色の窓のドラムのタブ）
namespace {

// ドラムの打のフェーダー（fader_row のドラム版）。idx はワーク RAM の並びの番号（xgui::drum_params）。
// 値は xgui::drum_value、書くのは drum_write（ドラッグの間は間引く）。戻り値はカーソルが載っているか
// つまんでいるフェーダー（無ければ -1）
int drum_fader_row(const int *idx, int n, int group_after, int set, int key, bridge &br,
                   ImVec2 a, ImVec2 b, bool hovered, bool active, ImGuiID id, int *vals,
                   ImU32 col_first = 0, ImU32 col_second = 0)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const drum_param *dp = drum_params();
	const float gfs = fs * 0.6f;
	const float gap = fs * 0.5f, group = fs * 1.4f;
	const float room = b.x - a.x - gap * float(n - 1) - (group_after >= 0 ? group - gap : 0.0f);
	const float fw = std::clamp(room / float(n), fs * 1.0f, fs * 1.8f);
	const float total = fw * float(n) + gap * float(n - 1) + (group_after >= 0 ? group - gap : 0.0f);
	std::vector<float> fx0(static_cast<size_t>(n));
	{
		float x = a.x + std::max(0.0f, (b.x - a.x - total) * 0.5f);
		for (int i = 0; i < n; i++) {
			fx0[size_t(i)] = x;
			x += fw + (i == group_after ? group : gap);
		}
	}
	const float ftop = a.y + gfs * 2.4f, fbot = b.y;
	const float cap_h = std::max(6.0f, fs * 0.55f);
	auto fader_at = [&](ImVec2 p) {
		if (p.y < a.y || p.y > b.y)
			return -1;
		for (int i = 0; i < n; i++)
			if (p.x >= fx0[size_t(i)] - gap * 0.5f && p.x <= fx0[size_t(i)] + fw + gap * 0.5f)
				return i;
		return -1;
	};
	auto value_at = [&](float y, const drum_param &d) {
		const float lo = ftop + cap_h * 0.5f, hi = fbot - cap_h * 0.5f;
		return std::clamp(d.lo + int(std::lround((hi - y) / std::max(1.0f, hi - lo) * float(d.hi - d.lo))), d.lo, d.hi);
	};
	const int over = hovered ? fader_at(io.MousePos) : -1;
	if (over >= 0) {
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		if (io.MouseWheel != 0.0f) {
			const drum_param &d = dp[idx[over]];
			const int step = std::max(1, int(std::lround(std::fabs(io.MouseWheel)))) * (io.KeyCtrl ? 10 : 1);
			const int nv = std::clamp(vals[over] + (io.MouseWheel > 0 ? step : -step), d.lo, d.hi);
			if (nv != vals[over]) {
				drum_write(br, set, key, idx[over], nv);
				vals[over] = nv;
			}
		}
	}
	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(id, -1);
	if (active && ImGui::IsItemActivated())
		grab = fader_at(io.MousePos);
	if (!active)
		grab = -1;
	st->SetInt(id, grab);
	if (grab >= 0) {
		const int nv = value_at(io.MousePos.y, dp[idx[grab]]);
		if (nv != vals[grab]) {
			drum_write(br, set, key, idx[grab], nv, true);
			vals[grab] = nv;
		}
	}
	const int focus = grab >= 0 ? grab : over;
	if (over >= 0)
		out_hover_drum(set, key, idx[over]);
	if (focus >= 0) {
		const std::string hk = std::string("drum.") + dp[idx[focus]].head;
		const char *help = help_for(hk.c_str());
		hint(UI_TEXT(ov_value_tip_fmt, "%s  %s\n%s (drag or wheel)"), dp[idx[focus]].head,
		     drum_value_text(idx[focus], vals[focus]).c_str(), help ? help : "");
	}
	for (int i = 0; i < n; i++) {
		const drum_param &d = dp[idx[i]];
		std::string t = drum_value_text(idx[i], vals[i]);
		if (t.size() > 3 && t.compare(t.size() - 3, 3, " Hz") == 0)
			t.resize(t.size() - 3);
		const ImU32 nc = (group_after >= 0 && i > group_after) ? col_second : col_first;
		fader_picture(dl, fx0[size_t(i)], fx0[size_t(i)] + fw, ftop, fbot, vals[i], d.lo, d.hi, d.head,
		              t.c_str(), focus == i, nc);
	}
	return focus;
}

// その打の 23 個（書いたばかりの値を含む）
void drum_vals(const xg_snapshot &ram, int set, int key, u8 *out)
{
	for (int i = 0; i < XG_DRUM_PARAMS; i++)
		out[i] = u8(drum_value(ram, set, key, i));
}

// 絵の地（メゾネットの上の段）の四角と、下の段の境目。区画ごとに同じ作り
struct maison {
	ImVec2 pos;
	float split, pad;
	bool hovered, active;
	ImGuiID id;
};

maison begin_maison(const char *id, float w, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	maison m;
	ImGui::PushID(id);
	m.pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##cell", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	m.id = ImGui::GetItemID();
	m.hovered = ImGui::IsItemHovered();
	m.active = ImGui::IsItemActive();
	m.pad = fs * 0.25f;
	m.split = m.pos.y + h * overview::MAISON_SPLIT;
	dl->AddRectFilled(m.pos, ImVec2(m.pos.x + w, m.pos.y + h), col(m.hovered || m.active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	dl->PushClipRect(m.pos, ImVec2(m.pos.x + w, m.pos.y + h), true);
	return m;
}

void end_maison(const maison &m, float w)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddLine(ImVec2(m.pos.x, m.split), ImVec2(m.pos.x + w, m.split), col(ImGuiCol_Border), 1.0f);
	dl->PopClipRect();
	ImGui::PopID();
}

void center_text(ImDrawList *dl, float x0, float x1, float top, float bottom, const char *t)
{
	const ImVec2 ts = ImGui::CalcTextSize(t);
	dl->AddText(ImVec2((x0 + x1 - ts.x) * 0.5f, (top + bottom - ts.y) * 0.5f), col(ImGuiCol_TextDisabled), t);
}

} // namespace

// フィルタと EQ。上の段に、打のフィルタ（切る高さ・共振・HPF）と打ごとの EQ を合わせた実際の特性
// （太線。細線がフィルタだけ、点線が EQ だけ）と、パートの音のスペクトラム（背景）。
// 下の段に Cutoff・Reso・VelCut・HPF と EQ の 4 本
void overview::drum_filter_cell(int part, int set, int key, const xg_snapshot &ram, bridge &br, float w, float h)
{
	static const int IDX[8] = { 11, 12, 22, 20, 16, 18, 17, 19 };
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	u8 all[XG_DRUM_PARAMS];
	drum_vals(ram, set, key, all);
	int vals[8];
	for (int i = 0; i < 8; i++)
		vals[i] = all[IDX[i]];
	const maison mz = begin_maison("drumfilter", w, h);
	const int focus = drum_fader_row(IDX, 8, 3, set, key, br, ImVec2(mz.pos.x + mz.pad, mz.split + mz.pad),
	                                 ImVec2(mz.pos.x + w - mz.pad, mz.pos.y + h - mz.pad), mz.hovered, mz.active, mz.id, vals,
	                                 IM_COL32(150, 190, 255, 255), IM_COL32(255, 210, 110, 255));
	for (int i = 0; i < 8; i++)
		all[IDX[i]] = u8(vals[i]);
	const float x0 = mz.pos.x + mz.pad, x1 = mz.pos.x + w - mz.pad;
	const float top = mz.pos.y + mz.pad, bottom = mz.split - mz.pad;
	const float span = x1 - x0;
	const float F_LO = 20.0f, F_HI = 20000.0f;
	auto x_hz = [&](float f) { return x0 + span * std::log(std::clamp(f, F_LO, F_HI) / F_LO) / std::log(F_HI / F_LO); };
	const float DB_TOP = 24.0f, DB_BOTTOM = -36.0f;
	auto y_db = [&](float db) { return top + (bottom - top) * (DB_TOP - std::clamp(db, DB_BOTTOM, DB_TOP)) / (DB_TOP - DB_BOTTOM); };
	spectrum_view(br, part, 0, -1, 3, ImVec2(x0, top), ImVec2(x1, bottom), nullptr, true);
	for (float f : { 100.0f, 1000.0f, 10000.0f }) {
		const float x = x_hz(f);
		dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), col(ImGuiCol_TextDisabled, 0.15f));
		const char *t = f >= 10000.0f ? "10k" : f >= 1000.0f ? "1k" : "100";
		dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x + 2.0f, bottom - fs * 0.6f), col(ImGuiCol_TextDisabled, 0.7f), t);
	}
	dl->AddLine(ImVec2(x0, y_db(0.0f)), ImVec2(x1, y_db(0.0f)), col(ImGuiCol_TextDisabled, 0.35f));

	const xg::voice_rom *vr = voices();
	const shape::drum_line L = shape::drum_shape(vr ? vr->data() : nullptr, ram.kit[part], key, all,
	                                             ram.parts[part]);
	if (L.ok) {
		std::vector<ImVec2> fpts, epts, tpts;
		for (size_t i = 0; i < L.filter.size(); i++) {
			const float x = x_hz(L.filter[i].ms);
			const float edb = i < L.eq.size() ? L.eq[i].cents : 0.0f;
			fpts.push_back(ImVec2(x, y_db(L.filter[i].cents)));
			epts.push_back(ImVec2(x, y_db(edb)));
			tpts.push_back(ImVec2(x, y_db(L.filter[i].cents + edb)));
		}
		dl->PathClear();
		dl->PathLineTo(ImVec2(tpts.front().x, bottom));
		for (const ImVec2 &p : tpts)
			dl->PathLineTo(p);
		dl->PathLineTo(ImVec2(tpts.back().x, bottom));
		dl->PathFillConcave(col(ImGuiCol_SliderGrab, 0.18f));
		dl->AddPolyline(fpts.data(), int(fpts.size()), IM_COL32(150, 190, 255, 150), 0, 1.0f);
		for (size_t i = 1; i < epts.size(); i += 2)
			dl->AddLine(epts[i - 1], epts[i], IM_COL32(255, 210, 110, 200), 1.2f);
		dl->AddPolyline(tpts.data(), int(tpts.size()), col(ImGuiCol_SliderGrabActive), 0, std::max(2.0f, fs * 0.14f));
		// EQ の周波数の位置に ● 印（EQ だけの点線の上）。そのフェーダーを触っている間は大きく
		const float fl = float(eq::HZ[std::clamp(int(all[18]), 0, 60)]);
		const float fh = float(eq::HZ[std::clamp(int(all[19]), 0, 60)]);
		const std::vector<shape::pt> at = shape::eq_response_vals(vr->data(), all[16], all[17], all[18], all[19], { fl, fh });
		const float r0 = std::max(3.0f, fs * 0.26f);
		for (int k = 0; k < 2 && k < int(at.size()); k++) {
			const bool lit = focus == 4 + k * 2 || focus == 5 + k * 2;
			const ImVec2 c(x_hz(at[size_t(k)].ms), y_db(at[size_t(k)].cents));
			const float rr = lit ? r0 * 1.5f : r0;
			dl->AddCircleFilled(c, rr, IM_COL32(255, 210, 110, 255));
			dl->AddCircle(c, rr, IM_COL32(40, 30, 10, 255), 0, 1.5f);
		}
		// 切る高さ: いちばん大きい所から 3 dB 下がる所（山があれば山の頂）。filter_cell と同じ決め方
		float peak = -1e9f, peak_hz = 20.0f;
		for (const shape::pt &p : L.filter)
			if (p.cents > peak) { peak = p.cents; peak_hz = p.ms; }
		float lpf_hz = 0;
		for (size_t i = L.filter.size(); i-- > 0;)
			if (L.filter[i].cents >= peak - 3.0f) { lpf_hz = L.filter[i].ms; break; }
		if (peak > 3.0f)
			lpf_hz = peak_hz;
		float hpf_hz = 0;
		for (const shape::pt &p : L.filter)
			if (p.cents >= peak - 3.0f) { hpf_hz = p.ms; break; }
		auto hz_text = [](float f) {
			char t[24];
			if (f >= 19000.0f) std::snprintf(t, sizeof(t), "%s", UI_TEXT(ov_khz, "20 kHz or more"));
			else if (f >= 1000.0f) std::snprintf(t, sizeof(t), "%.1f kHz", f / 1000.0f);
			else std::snprintf(t, sizeof(t), "%.0f Hz", f);
			return std::string(t);
		};
		auto mark = [&](float f, ImU32 c, const char *label, int row) {
			if (f <= 0.0f || f >= 19000.0f)
				return;
			const float x = x_hz(f);
			for (float y = top; y < bottom; y += fs * 0.4f)
				dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(bottom, y + fs * 0.2f)), c, 1.5f);
			char t[40];
			std::snprintf(t, sizeof(t), "%s %s", label, hz_text(f).c_str());
			const float tfs = fs * 0.65f;
			const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(tfs, FLT_MAX, 0.0f, t);
			const float tx = std::min(x + 3.0f, x1 - ts.x - 2.0f);
			const float ty = top + 2.0f + float(row) * (ts.y + 2.0f);
			dl->AddRectFilled(ImVec2(tx - 2, ty - 1), ImVec2(tx + ts.x + 2, ty + ts.y + 1), IM_COL32(0, 0, 0, 140), 3.0f);
			dl->AddText(ImGui::GetFont(), tfs, ImVec2(tx, ty), c, t);
		};
		mark(lpf_hz, IM_COL32(255, 170, 60, 230), "LPF", 0);
		if (L.hpf)
			mark(hpf_hz, IM_COL32(255, 120, 200, 230), "HPF", 1);
		char s[96];
		std::snprintf(s, sizeof(s), "Cutoff : %s (%s)", drum_value_text(11, all[11]).c_str(), hz_text(lpf_hz).c_str());
		shape_value(s);
		std::snprintf(s, sizeof(s), "Reso : %s", drum_value_text(12, all[12]).c_str());
		shape_value(s);
		// 凡例（右下に小さく）
		const float lfs = fs * 0.6f;
		const char *const items[3] = { UI_TEXT(ov_legend_combined, "Combined"), UI_TEXT(ov_legend_filter, "Filter"), "EQ" };
		const ImU32 cols[3] = { col(ImGuiCol_SliderGrabActive), IM_COL32(150, 190, 255, 200), IM_COL32(255, 210, 110, 220) };
		float ly = bottom - lfs * 4.2f;
		for (int k = 0; k < 3; k++) {
			const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(lfs, FLT_MAX, 0.0f, items[k]);
			const float lx = x1 - ts.x - fs * 0.3f;
			dl->AddLine(ImVec2(lx - fs * 0.9f, ly + ts.y * 0.5f), ImVec2(lx - fs * 0.2f, ly + ts.y * 0.5f), cols[k], k == 0 ? 2.0f : 1.2f);
			dl->AddText(ImGui::GetFont(), lfs, ImVec2(lx, ly), col(ImGuiCol_TextDisabled, 0.9f), items[k]);
			ly += ts.y + 1.0f;
		}
	} else {
		center_text(dl, x0, x1, top, bottom, UI_TEXT(ps_drum_no_shape, "No picture for this key (no sound in this kit)"));
	}
	end_maison(mz, w);
}

// EG。上の段に音量の形（実際の時間。打ちっぱなしなら減衰 2 の終わりまで、離しを受けるなら 0.5 秒で離す）、
// 下の段に立ち上がり・減衰 1・減衰 2
void overview::drum_env_cell(int part, int set, int key, const xg_snapshot &ram, bridge &br, float w, float h)
{
	static const int IDX[3] = { 13, 14, 15 };
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	u8 all[XG_DRUM_PARAMS];
	drum_vals(ram, set, key, all);
	int vals[3];
	for (int i = 0; i < 3; i++)
		vals[i] = all[IDX[i]];
	const maison mz = begin_maison("drumenv", w, h);
	const int focus = drum_fader_row(IDX, 3, -1, set, key, br, ImVec2(mz.pos.x + mz.pad, mz.split + mz.pad),
	                                 ImVec2(mz.pos.x + w - mz.pad, mz.pos.y + h - mz.pad), mz.hovered, mz.active, mz.id, vals,
	                                 IM_COL32(150, 190, 255, 255));
	for (int i = 0; i < 3; i++)
		all[IDX[i]] = u8(vals[i]);
	const float line = fs * 0.75f;
	const float x0 = mz.pos.x + mz.pad + fs * 1.6f, x1 = mz.pos.x + w - mz.pad - fs * 0.6f;
	const float top = mz.pos.y + mz.pad + line * 1.2f, bottom = mz.split - mz.pad;
	spectrum_view(br, part, 0, -1, 4, ImVec2(mz.pos.x + mz.pad, top), ImVec2(mz.pos.x + w - mz.pad, bottom), nullptr, true);

	const xg::voice_rom *vr = voices();
	const shape::drum_line L = shape::drum_shape(vr ? vr->data() : nullptr, ram.kit[part], key, all,
	                                             ram.parts[part]);
	if (L.ok && !L.amp.pts.empty()) {
		const shape::amp_line &A = L.amp;
		float t_end = std::max(50.0f, A.pts.back().ms);
		t_end = std::min(t_end, 6000.0f);
		// 時間の目盛り（time_grid と同じ。離す時刻の線は、離しを受けるときだけ）
		for (float t : { 10.0f, 100.0f, 1000.0f, 2000.0f, 4000.0f }) {
			if (t > t_end)
				break;
			const float x = time_x(t, t_end, x0, x1);
			dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), col(ImGuiCol_TextDisabled, 0.15f));
			char g[16];
			if (t >= 1000.0f) std::snprintf(g, sizeof(g), "%.0fs", t / 1000.0f);
			else              std::snprintf(g, sizeof(g), "%.0fms", t);
			dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x + 2.0f, bottom - fs * 0.6f), col(ImGuiCol_TextDisabled, 0.7f), g);
		}
		if (A.keyoff_ms >= 0) {
			const float xo = time_x(A.keyoff_ms, t_end, x0, x1);
			for (float y = top; y < bottom; y += fs * 0.5f)
				dl->AddLine(ImVec2(xo, y), ImVec2(xo, std::min(bottom, y + fs * 0.25f)), col(ImGuiCol_TextDisabled, 0.5f));
			dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(xo + 2.0f, bottom - fs * 1.2f), col(ImGuiCol_TextDisabled, 0.8f), UI_TEXT(ov_discrete, "Rel"));
		}
		auto y_db = [&](float db) { return top + (bottom - top) * std::clamp(-db / 60.0f, 0.0f, 1.0f); };
		const float tfs = fs * 0.55f;
		dl->AddText(ImGui::GetFont(), tfs, ImVec2(mz.pos.x + mz.pad, top - tfs * 0.5f), IM_COL32(120, 170, 255, 200), "0dB");
		dl->AddText(ImGui::GetFont(), tfs, ImVec2(mz.pos.x + mz.pad, bottom - tfs), IM_COL32(120, 170, 255, 200), "-60");
		std::vector<ImVec2> pts;
		for (const shape::pt &p : A.pts)
			if (p.ms <= t_end)
				pts.push_back(ImVec2(time_x(p.ms, t_end, x0, x1), y_db(p.cents)));
		if (pts.size() >= 2) {
			dl->PathClear();
			dl->PathLineTo(ImVec2(pts.front().x, bottom));
			for (const ImVec2 &p : pts)
				dl->PathLineTo(p);
			dl->PathLineTo(ImVec2(pts.back().x, bottom));
			dl->PathFillConcave(col(ImGuiCol_SliderGrab, 0.22f));
			dl->AddPolyline(pts.data(), int(pts.size()), col(ImGuiCol_SliderGrabActive), 0, std::max(2.0f, fs * 0.12f));
		}
		// フェーダーに対応する点（触れない）。触っている間は大きく
		auto amp_at = [&](float ms) {
			float db = A.pts.front().cents;
			for (const shape::pt &p : A.pts)
				if (p.ms <= ms)
					db = p.cents;
			return db;
		};
		const float r0 = std::max(3.0f, fs * 0.26f);
		auto dot = [&](float ms, const char *mark, bool lit) {
			const ImVec2 c(time_x(ms, t_end, x0, x1), y_db(amp_at(ms)));
			const float rr = lit ? r0 * 1.5f : r0;
			const ImU32 fill = IM_COL32(150, 190, 255, 255);
			dl->AddCircleFilled(c, rr, fill);
			dl->AddCircle(c, rr, IM_COL32(20, 20, 30, 255), 0, 1.5f);
			const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs * 0.6f, FLT_MAX, 0.0f, mark);
			dl->AddText(ImGui::GetFont(), fs * 0.6f, ImVec2(std::clamp(c.x + rr + 1.0f, x0, x1 - ts.x), std::clamp(c.y - rr - ts.y, top, bottom - ts.y)), fill, mark);
		};
		// 減衰 2 の終わり: 形の最後（離しを受けるなら離す前の最後）
		float d2_end = A.pts.back().ms;
		if (A.keyoff_ms >= 0)
			d2_end = std::min(d2_end, A.keyoff_ms);
		dot(A.attack_ms, "A", focus == 0);
		if (A.decay1_ms > 0)
			dot(A.decay1_ms, "D1", focus == 1);
		dot(d2_end, "D2", focus == 2);
		char s[128];
		std::snprintf(s, sizeof(s), UI_TEXT(ps_drum_env_fmt, "Attack %.0f ms   Decay 1 %.0f ms   Decay 2 to %.0f dB"),
		              A.attack_ms, std::max(0.0f, A.decay1_ms - A.attack_ms), A.sustain_db);
		dl->AddText(ImGui::GetFont(), line, ImVec2(mz.pos.x + mz.pad + 2.0f, mz.pos.y + mz.pad), IM_COL32(150, 190, 255, 255), s);
		shape_value(s);
	} else {
		center_text(dl, x0, x1, top, bottom, UI_TEXT(ps_drum_no_shape, "No picture for this key (no sound in this kit)"));
	}
	end_maison(mz, w);
}

// 高さ・音量・パン・送り。上の段に、音の置き場所（左右がパン、点の大きさが音量、3 本の送りの棒）と高さの字。
// 下の段に Pitch・Fine・VelPit と Level・Pan・Rev・Cho・Var
void overview::drum_mix_cell(int part, int set, int key, const xg_snapshot &ram, bridge &br, float w, float h)
{
	(void)part;
	static const int IDX[8] = { 0, 1, 21, 2, 4, 5, 6, 7 };
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	u8 all[XG_DRUM_PARAMS];
	drum_vals(ram, set, key, all);
	int vals[8];
	for (int i = 0; i < 8; i++)
		vals[i] = all[IDX[i]];
	const maison mz = begin_maison("drummix", w, h);
	const int focus = drum_fader_row(IDX, 8, 2, set, key, br, ImVec2(mz.pos.x + mz.pad, mz.split + mz.pad),
	                                 ImVec2(mz.pos.x + w - mz.pad, mz.pos.y + h - mz.pad), mz.hovered, mz.active, mz.id, vals,
	                                 IM_COL32(255, 170, 130, 255), IM_COL32(150, 220, 170, 255));
	for (int i = 0; i < 8; i++)
		all[IDX[i]] = u8(vals[i]);
	const float x0 = mz.pos.x + mz.pad * 4, x1 = mz.pos.x + w - mz.pad * 4;
	const float top = mz.pos.y + mz.pad, bottom = mz.split - mz.pad;
	const float line = fs * 0.75f;
	// 高さの字（半音とセント）
	{
		const xg::voice_rom *vr = voices();
		const shape::drum_line L = shape::drum_shape(vr ? vr->data() : nullptr, ram.kit[part], key, all,
		                                             ram.parts[part]);
		char s[96];
		std::snprintf(s, sizeof(s), UI_TEXT(ps_drum_pitch_fmt, "Pitch %+.2f semitones"), (L.ok ? L.cents : 0.0f) / 100.0f);
		dl->AddText(ImGui::GetFont(), line, ImVec2(mz.pos.x + mz.pad + 2.0f, top), IM_COL32(255, 170, 130, 255), s);
		shape_value(s);
	}
	// 左右の置き場所。横線の上に点（大きさが音量）。Rnd は点を並べて見せる
	const float py = top + line * 2.6f;
	dl->AddLine(ImVec2(x0, py), ImVec2(x1, py), col(ImGuiCol_TextDisabled, 0.5f), 1.0f);
	dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x0, py + 3.0f), col(ImGuiCol_TextDisabled, 0.8f), "L");
	dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x1 - fs * 0.4f, py + 3.0f), col(ImGuiCol_TextDisabled, 0.8f), "R");
	dl->AddLine(ImVec2((x0 + x1) * 0.5f, py - fs * 0.3f), ImVec2((x0 + x1) * 0.5f, py + fs * 0.3f), col(ImGuiCol_TextDisabled, 0.6f), 1.0f);
	const float rr = std::max(2.0f, fs * 0.2f + fs * 0.45f * float(all[2]) / 127.0f);
	const bool lit = focus == 3 || focus == 4;
	const ImU32 dotc = IM_COL32(150, 220, 170, lit ? 255 : 210);
	if (all[4] == 0) {
		for (int k = 0; k < 5; k++)
			dl->AddCircle(ImVec2(x0 + (x1 - x0) * (0.1f + 0.2f * float(k)), py), rr, dotc, 0, 1.5f);
	} else {
		const float px = x0 + (x1 - x0) * float(all[4] - 1) / 126.0f;
		dl->AddCircleFilled(ImVec2(px, py), rr, dotc);
		dl->AddCircle(ImVec2(px, py), rr, IM_COL32(20, 30, 20, 255), 0, 1.5f);
	}
	// 送りの 3 本（横の棒）
	const char *const names[3] = { "REV", "CHO", "VAR" };
	const float by0 = py + fs * 1.2f;
	const float bh = std::max(3.0f, std::min(fs * 0.5f, (bottom - by0) / 3.0f - 2.0f));
	for (int k = 0; k < 3; k++) {
		const float y = by0 + float(k) * (bh + fs * 0.25f);
		if (y + bh > bottom)
			break;
		const float bx = x0 + fs * 1.6f;
		dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x0, y + (bh - fs * 0.55f) * 0.5f), col(ImGuiCol_TextDisabled, 0.9f), names[k]);
		dl->AddRectFilled(ImVec2(bx, y), ImVec2(x1, y + bh), col(ImGuiCol_FrameBg), 2.0f);
		dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + (x1 - bx) * float(all[5 + k]) / 127.0f, y + bh),
		                  IM_COL32(150, 220, 170, focus == 5 + k ? 230 : 150), 2.0f);
	}
	end_maison(mz, w);
}

void overview::eg_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	if (compact)
		eg_small(part, m, br, w, h, compact);
	else
		env_cell(part, m, br, w, h);
}

void overview::peg_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	if (compact)
		peg_small(part, m, br, w, h, compact);
	else
		env_cell(part, m, br, w, h);
}






// 鍵盤のモジュレーションホイール風の絵。溝の中に円筒が縦に立っていて、刻みが値に合わせて流れる。
// 白い印が今の位置（下が 0、上が maxv）。上に小さく名前と数を出す
static void wheel_picture(ImDrawList *dl, float x0, float x1, float top, float bottom, int value, int maxv,
                          const char *name, bool lit)
{
	const float fs = ImGui::GetFontSize();
	const float gfs = fs * 0.6f;
	const float r = fs * 0.25f;
	dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom), IM_COL32(18, 18, 20, 255), r);
	dl->AddRect(ImVec2(x0, top), ImVec2(x1, bottom), lit ? IM_COL32(140, 140, 150, 255) : IM_COL32(70, 70, 76, 255), r);
	const float in = std::max(2.0f, fs * 0.15f);
	const ImVec2 a(x0 + in, top + in), b(x1 - in, bottom - in);
	// 円筒の陰（上下の端は暗く、真ん中は明るく）
	const float midy = (a.y + b.y) * 0.5f;
	const ImU32 dark = IM_COL32(35, 35, 40, 255), lite = IM_COL32(92, 92, 100, 255);
	dl->AddRectFilledMultiColor(a, ImVec2(b.x, midy), dark, dark, lite, lite);
	dl->AddRectFilledMultiColor(ImVec2(a.x, midy), b, lite, lite, dark, dark);
	// 刻み。値が 1 増えると円筒が少し回ったように、上へ流れる
	const float step = std::max(3.0f, fs * 0.32f);
	const float phase = std::fmod(float(value) * step * 0.5f, step);
	for (float y = b.y - phase; y > a.y; y -= step) {
		const float t = 1.0f - std::fabs(y - midy) / std::max(1.0f, (b.y - a.y) * 0.5f);
		dl->AddLine(ImVec2(a.x + 1, y), ImVec2(b.x - 1, y), IM_COL32(20, 20, 24, int(60 + 150 * t)), 1.0f);
	}
	const float my = b.y - (b.y - a.y) * float(value) / float(std::max(1, maxv));
	dl->AddRectFilled(ImVec2(a.x, my - 1.5f), ImVec2(b.x, my + 1.5f), IM_COL32(240, 240, 235, 255));
	// 上に名前と数（2 行）
	char n[8];
	std::snprintf(n, sizeof(n), "%d", value);
	ImFont *font = ImGui::GetFont();
	const ImVec2 ns = font->CalcTextSizeA(gfs, FLT_MAX, 0.0f, n);
	const ImVec2 ms = font->CalcTextSizeA(gfs, FLT_MAX, 0.0f, name);
	dl->AddText(font, gfs, ImVec2((x0 + x1 - ns.x) * 0.5f, top - ns.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text), n);
	dl->AddText(font, gfs, ImVec2((x0 + x1 - ms.x) * 0.5f, top - ns.y - ms.y - 1.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), name);
}

// モジュレーションのビブラート。左端にモジュレーションホイール（CC1）、右端に MW LFO PM を、
// どちらも鍵盤のホイール風の絵で置き、間に 2 次元の絵（横がホイールの位置 0-127、縦が揺れの深さ＝片側のセント）。
// ホイールはドラッグか、絵の上でマウスホイール（速く回すほど大きく動く）で回す。右のホイールの上で
// 回せば MW LFO PM、それ以外の所では CC1（受信チャンネルへ送る）。
// 実機は「ホイールのぶん」と「音色自身の揺れ（Vib Depth 込み）」を足さず、大きいほうを使う
// （doc/native-engine.md の 6.215）。音色自身の揺れは触れない薄い横線と帯で背景に描く
// （その線より上でだけ、ホイールが効く）。ホイールのぶんを階段、効く深さを太線で
void overview::mod_cell(int part, xg::model &m, bridge &br, float w, float h, bool compact)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const xg::param &pm = P("part.mw_lfo_pmod"), &pd = P("part.vib_depth");
	int vm = 10, vd = 64;
	const bool known = m.get(pm, part, vm) && m.get(pd, part, vd);

	ImGui::PushID("mod");
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##mod", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = !compact && ImGui::IsItemActive();

	int rcv = 127;
	m.get(P("part.rcv_channel"), part, rcv);
	const int slot = rcv >= 0 && rcv < PARTS ? rcv : -1;
	const xg_snapshot *snap = current_ram();
	int wheel_now = snap ? mod_now(part, snap->parts[part][xg::ram::PART_MOD] & 0x7f) : 0;

	// 置き場所。左右の端にホイール、間が 2 次元の絵
	const float pad = fs * 0.25f;
	const float gfs = fs * 0.6f;
	const float wt = pos.y + pad + gfs * 2.4f;       // ホイールの溝の上端（上に名前と数を置く）
	const float top = pos.y + pad + fs * 0.3f, bottom = pos.y + h - pad;
	const float wheel_w = std::min(fs * 1.5f, w * 0.15f);
	const float lx0 = pos.x + pad, lx1 = lx0 + wheel_w;             // 左: CC1
	const float rx1 = pos.x + w - pad, rx0 = rx1 - wheel_w;         // 右: MW LFO PM
	const float x0 = lx1 + fs * 0.5f, x1 = rx0 - fs * 0.5f;
	const float in = std::max(2.0f, fs * 0.15f);
	const float wa = wt + in, wb = bottom - in;      // ホイールの動く範囲
	const bool over_right = io.MousePos.x >= rx0 - fs * 0.25f;
	const bool over_left = io.MousePos.x <= lx1 + fs * 0.25f;

	// **マウスホイールで回す**（速く回すほど大きく。Ctrl でさらに大きく）。右のホイールの上なら MW LFO PM
	if (hovered && !compact) {
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		if (io.MouseWheel != 0.0f) {
			const int d = wheel_steps(io.MouseWheel, io.KeyCtrl);
			if (over_right) {
				const int nv = std::clamp(vm + d, pm.min, pm.max);
				if (known && nv != vm)
					br.send(m.set(pm, part, nv));
				vm = known ? nv : vm;
			} else if (slot >= 0) {
				const int nv = std::clamp(wheel_now + d, 0, 127);
				if (nv != wheel_now) {
					mod_send(part, slot, nv, br);
					wheel_now = nv;
				}
			}
		}
	}
	// **ホイールの絵をつまんで上下**（1 が左、2 が右）。
	// **押した所へ飛ばさず、動かした距離で増減する**。実物のホイールと同じ手触りで、
	// つまんだ瞬間に値が跳ぶこともない（溝の高さいっぱいで全域。Shift で 4 倍細かく）
	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(id, 0);
	if (active && ImGui::IsItemActivated())
		grab = over_left ? 1 : (over_right ? 2 : 0);
	if (!active)
		grab = 0;
	st->SetInt(id, grab);
	if (!grab)
		st->SetFloat(id + 1, 0.0f);
	if (grab && io.MouseDelta.y != 0.0f) {
		const int maxv = grab == 1 ? 127 : pm.max;
		float acc = st->GetFloat(id + 1, 0.0f);
		// 溝の高さいっぱいで全域。ただし溝が短いときは 1 画素あたりが荒くなり
		// すぎるので、最低でも 150 画素は動かす（Shift でさらに 4 倍細かく）
		acc += -io.MouseDelta.y * float(maxv) /
		       std::max(150.0f, (wb - wa)) / (io.KeyShift ? 4.0f : 1.0f);
		const int step = int(acc);
		if (step) {
			acc -= float(step);
			if (grab == 1 && slot >= 0) {
				const int nv = std::clamp(wheel_now + step, 0, 127);
				if (nv != wheel_now) {
					mod_send(part, slot, nv, br);
					wheel_now = nv;
				}
			} else if (grab == 2 && known) {
				const int nv = std::clamp(vm + step, pm.min, pm.max);
				if (nv != vm) {
					drag_send(br, m.set(pm, part, nv));
					vm = nv;
				}
			}
		}
		st->SetFloat(id + 1, acc);
	}

	voice_ctx v;
	const bool have = known && voice_of(part, v);
	std::vector<shape::mod_line> ls;
	if (have) {
		v.blk[0x20] = u8(vm);
		v.blk[0x16] = u8(vd);
		ls = shape::mod_lines(v.rom, v.rec, v.blk);
	}
	const shape::mod_line *L = ls.empty() ? nullptr : &lead_line(ls);
	// 縦の目盛り。いちばん深い所が上から少し下に来るように（最低でも ±220 セント）
	float span = 220.0f;
	if (L) {
		span = std::max(span, L->own_cents * 1.15f);
		for (float c : L->eff)
			span = std::max(span, c * 1.15f);
	}
	// 縦は平方根の目盛り（浅い揺れも見分けられるように）
	auto y_of = [&](float cents) { return bottom - (bottom - top) * std::sqrt(std::clamp(cents / span, 0.0f, 1.0f)); };
	auto x_of = [&](int wheel) { return x0 + (x1 - x0) * float(wheel) / 127.0f; };

	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);

	// 説明（ホイールの上）。2 次元の絵の上では区画の説明（part_shapes）
	if (!compact && (grab == 1 || (hovered && over_left)))
		hint(UI_TEXT(ov_mod_wheel_hint, "MODULATION WHEEL (CC1)  %d\nModulation wheel. Drag or wheel up/down. Higher adds more of the right side's MW LFO PMOD DEPTH vibrato (sends CC1 to the receive channel)"), wheel_now);
	else if (!compact && (grab == 2 || (hovered && over_right))) {
		const char *help = help_for("part.mw_lfo_pmod");
		hint(UI_TEXT(ov_param_tip_fmt, "%s  %d\n%s (drag or wheel)"), official_name("part.mw_lfo_pmod").c_str(), vm, help ? help : "");
	}
	if (!compact && hovered && over_left && slot >= 0)
		out_hover_live(slot, false, wheel_now);
	else if (!compact && hovered && over_right && known)
		out_hover_param(pm, part);
	wheel_picture(dl, lx0, lx1, wt, bottom, wheel_now, 127, "MW", grab == 1 || (hovered && over_left));
	if (!compact)
		wheel_picture(dl, rx0, rx1, wt, bottom, vm, pm.max, "PM", grab == 2 || (hovered && over_right));

	if (L) {
		// ---- 背景: 音色自身の揺れ（Vib Depth 込み）。触れない薄い横線と、その下の帯。
		// ホイールのぶんがこの線を越えた所から、モジュレーションが効き始める
		if (L->own_cents > 0.0f) {
			const float oy = y_of(L->own_cents);
			dl->AddRectFilled(ImVec2(x0, oy), ImVec2(x1, bottom), col(ImGuiCol_TextDisabled, 0.10f));
			dl->AddLine(ImVec2(x0, oy), ImVec2(x1, oy), col(ImGuiCol_TextDisabled, 0.55f), 1.0f);
		}
		dl->AddLine(ImVec2(x0, bottom), ImVec2(x1, bottom), col(ImGuiCol_TextDisabled, 0.35f));
		// 目安の線（セント）
		for (float c : { 25.0f, 50.0f, 100.0f, 200.0f, 400.0f }) {
			if (c > span)
				break;
			const float y = y_of(c);
			dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), col(ImGuiCol_TextDisabled, 0.18f));
			char g[16];
			std::snprintf(g, sizeof(g), "%.0f", c);
			dl->AddText(ImGui::GetFont(), fs * 0.65f, ImVec2(x0 + 2.0f, y - fs * 0.65f), col(ImGuiCol_TextDisabled, 0.6f), g);
		}
		// 今のホイールの位置
		const int now = wheel_now;
		dl->AddLine(ImVec2(x_of(now), top), ImVec2(x_of(now), bottom), col(ImGuiCol_Text, 0.3f));
		const float th = std::max(1.5f, fs * 0.1f);
		std::vector<ImVec2> wl, el;
		for (int k = 0; k < 128; k++) {
			wl.push_back(ImVec2(x_of(k), y_of(L->wheel[size_t(k)])));
			el.push_back(ImVec2(x_of(k), y_of(L->eff[size_t(k)])));
		}
		dl->AddPolyline(wl.data(), int(wl.size()), SEG_KNOB, 0, th * 0.7f);
		dl->AddPolyline(el.data(), int(el.size()), col(ImGuiCol_SliderGrabActive), 0, th * 2.0f);
		const float r = std::max(2.5f, fs * 0.22f);
		const ImVec2 cur(x_of(now), y_of(L->eff[size_t(now)]));
		dl->AddCircleFilled(cur, r * 0.8f, col(ImGuiCol_Text));
		if (!compact) {
			char s[96];
			const ImVec2 a(x0, pos.y), b(x1, pos.y + h);
			label_avoid boxes;
			boxes.point(cur, r + 2.0f);
			std::snprintf(s, sizeof(s), UI_TEXT(ov_wheel_fmt, "Wheel %d: ±%.0f cent"), now, L->eff[size_t(now)]);
			point_label(dl, cur, s, true, a, b, &boxes);
			std::snprintf(s, sizeof(s), UI_TEXT(ov_mw_fmt, "MW LFO PM: %d (max ±%.0f cent)"), vm, L->wheel[127]);
			point_label(dl, ImVec2(x1, y_of(L->wheel[127])), s, true, a, b, &boxes);
			if (L->own_cents > 0.0f) {
				std::snprintf(s, sizeof(s), UI_TEXT(ov_own_fmt, "Voice wobble ±%.0f cent"), L->own_cents);
				shape_value(s);                  // 絵には出さず、帯の一覧にだけ
			}
		}
	} else {
		const ImVec2 ts = ImGui::CalcTextSize("--");
		dl->AddText(ImVec2(x0 + (x1 - x0 - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), col(ImGuiCol_TextDisabled), "--");
	}
	dl->PopClipRect();
	ImGui::PopID();
}


void overview::row(int part, xg::model &m, const xg_snapshot &ram, bridge &br, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImGui::PushID(part);

	// ---- パートと音色
	ImGui::TableNextColumn();
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##name", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			select_part(part);
		if (ImGui::BeginPopupContextItem("program")) {
			program_menu(part, m, &ram, br);
			ImGui::EndPopup();
		}
		if (m_part == part)
			dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(ImGuiCol_Header));
		else if (ImGui::IsItemHovered())
			dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(ImGuiCol_HeaderHovered, 0.4f));

		int msb = 0, lsb = 0, prog = 0, rcv = 0;
		const bool voice = m.get(P("part.bank_msb"), part, msb) && m.get(P("part.bank_lsb"), part, lsb) &&
		                   m.get(P("part.program"), part, prog);
		msb = shown_bank_msb(part, m, msb);      // GS のドラム（issue #52）
		bool has_rcv = m.get(P("part.rcv_channel"), part, rcv);
		const bool silenced = this->silenced(part);
		if (silenced) {
			rcv = m_saved_rcv[part];                 // 表示は元のチャンネル
			has_rcv = true;
		}
		const std::string name = part_name(part);
		dl->AddText(ImVec2(pos.x + fs * 0.3f, pos.y + fs * 0.1f), silenced ? col(ImGuiCol_TextDisabled) : col(ImGuiCol_Text), name.c_str());
		// 音色の名前と楽器の絵。利用者の ROM から読めれば MU2000 の本当の名前、
		// 読めなければ GM の名前（xg/voices.h）
		std::string vt = voice ? voice_text(msb, lsb, prog) : "--";
		const xg::voice_rom *vr = voices();
		const u8 *blk = ram.parts[part];
		if (voice && vr) {
			const std::string real = vr->name(blk, msb, prog);
			if (!real.empty()) {
				char buf[40];
				std::snprintf(buf, sizeof(buf), "%3d  %s", prog + 1, real.c_str());
				vt = buf;
			}
		}
		dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
		const float icon_x = pos.x + fs * 2.2f;
		// 実機の LCD に寄せて、横に 2 倍（1 ドットが横 2 : 縦 1）
		const float dot = std::max(1.0f, std::floor((h - fs * 0.3f) / 16.0f));
		const float dot_w = dot * 2;
		u16 rows[16];
		if (vr) {
			// パネルの LCD と同じ色（draw_imgui.h の LCD_BACK / LCD_GHOST / LCD_DOT）。
			// 絵が引けないもの（ROM の版が違うなど）も、LCD の枠だけ出して並びを揃える
			static const ImU32 LCD_BACK  = IM_COL32(150, 205, 45, 255);
			static const ImU32 LCD_GHOST = IM_COL32(140, 194, 44, 255);
			static const ImU32 LCD_DOT   = IM_COL32(18, 22, 14, 255);
			const bool has = voice && vr->icon(blk, msb, prog, rows);
			const float top = pos.y + (h - dot * 16) * 0.5f;
			const float frame = std::max(1.0f, dot);
			dl->AddRectFilled(ImVec2(icon_x - frame, top - frame),
			                  ImVec2(icon_x + dot_w * 16 + frame, top + dot * 16 + frame), LCD_BACK, 2.0f);
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++) {
					const bool on = has && ((rows[y] >> (15 - x)) & 1);
					dl->AddRectFilled(ImVec2(icon_x + x * dot_w, top + y * dot),
					                  ImVec2(icon_x + (x + 1) * dot_w, top + (y + 1) * dot),
					                  on ? LCD_DOT : LCD_GHOST);
				}
		}
		const float text_x = icon_x + dot_w * 16 + fs * 0.4f;
		dl->AddText(ImVec2(text_x, pos.y + fs * 0.1f), col(ImGuiCol_Text), vt.c_str());
		char sub[64];
		if (silenced)
			std::snprintf(sub, sizeof(sub), UI_TEXT(ov_rcv_mute_fmt, "Receive %s (%s)"), channel_name(rcv).c_str(),
			              m_mute[part] ? UI_TEXT(ov_mute_word, "muted") : UI_TEXT(ov_solo_off_word, "not soloed"));
		else
			std::snprintf(sub, sizeof(sub), UI_TEXT(ov_rcv_fmt, "Receive %s   M %d  L %d"), has_rcv ? channel_name(rcv).c_str() : "--", msb, lsb);
		dl->AddText(ImVec2(text_x, pos.y + fs * 1.15f), col(ImGuiCol_TextDisabled), sub);
		dl->PopClipRect();
		mute_buttons(part, pos.x, pos.y, w, h);
	}

	// 受信チャンネルから、見張りの口×チャンネル（ミュート中は元のチャンネル）
	int rcv = 127;
	m.get(P("part.rcv_channel"), part, rcv);
	if (m_saved_rcv[part] >= 0)
		rcv = m_saved_rcv[part];
	const int slot = rcv >= 0 && rcv < PARTS ? rcv : -1;

	// ---- VEL メーター
	ImGui::TableNextColumn();
	{
		if (slot >= 0 && ram.note_ons[slot] != m_seen_ons[part]) {
			m_seen_ons[part] = ram.note_ons[slot];
			m_level[part] = std::max(m_level[part], ram.velocity[slot] / 127.0f);
		}
		m_level[part] = std::max(0.0f, m_level[part] - ImGui::GetIO().DeltaTime * 1.6f);
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(w, h));
		const float pad = fs * 0.2f;
		const ImVec2 a(pos.x + pad, pos.y + pad), b(pos.x + w - pad, pos.y + h - pad);
		dl->AddRectFilled(a, b, col(ImGuiCol_FrameBg));
		const float top = b.y - (b.y - a.y) * m_level[part];
		dl->AddRectFilled(ImVec2(a.x, top), b, NOTE_ON);
	}

	// ---- スペクトラム（このパートの声の和。エフェクトの前）
	ImGui::TableNextColumn();
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(w, h));
		const float pad = fs * 0.2f;
		mini_spec &c = mini_spec_of(part);
		mini_spec_update(br, part, c);
		mini_spec_draw(dl, c, ImVec2(pos.x + pad, pos.y + pad), ImVec2(pos.x + w - pad, pos.y + h - pad), part_color(part));
	}

	// ---- 値の棒
	for (const column &c : COLUMNS) {
		ImGui::TableNextColumn();
		cell(c, part, m, ram, br, ImGui::GetContentRegionAvail().x, h);
	}

	// ---- 鍵盤。128 鍵を全部並べる
	ImGui::TableNextColumn();
	keys_cell(part, slot, ram, br, ImGui::GetContentRegionAvail().x, h);

	ImGui::PopID();
}


// 1 パートの鍵盤。押さえている鍵が光り、押すと鳴らす（左でも右でも）。押したまま横に動かすと鍵が替わる。
// 離すとノートオフ。送り先はこのパートの受信チャンネル（slot = 口 × 16 + ch。口 B なら口 B へ）
void overview::keys_cell(int part, int slot, const xg_snapshot &ram, bridge &br, float w, float h,
                         bool marker, int pc_low)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##keys", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	// 目印を置く窓では、右クリックは試聴の鍵の印を**入れたり消したり**するだけ
	// （鳴らさない）。何鍵でも付けられるので、和音で試聴できる
	if (marker && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
		int dummy = 0;
		const int note = key_at(pos, w, h, ImGui::GetIO().MousePos, dummy);
		if (note >= 0)
			toggle_audition_key(part, note);
	}
	const bool down = ImGui::IsItemActive() && slot >= 0 &&
	                  (ImGui::IsMouseDown(ImGuiMouseButton_Left) || (!marker && ImGui::IsMouseDown(ImGuiMouseButton_Right)));
	int vel = 100;
	const int want = down ? key_at(pos, w, h, ImGui::GetIO().MousePos, vel) : -1;
	if (want != m_playing[part]) {
		auto send = [&](const u8 msg[3]) {
			br.send_port(m_playing_slot[part] / 16, msg, 3);
		};
		if (m_playing[part] >= 0) {
			const u8 off[3] = { u8(0x80 | (m_playing_slot[part] & 15)), u8(m_playing[part]), 64 };
			send(off);
		}
		m_playing[part] = want;
		if (want >= 0) {
			m_playing_slot[part] = slot;
			const u8 on[3] = { u8(0x90 | (slot & 15)), u8(want), u8(vel) };
			send(on);
		}
	}
	if (ImGui::IsItemHovered() && !down && slot >= 0) {
		if (marker)
			ImGui::SetItemTooltip("%s", UI_TEXT(ov_kb_audition_tip, "Left-click to play (lower is louder). Right-click to mark a key for voice audition, right-click again to clear it\n"
			                                                  "Mark as many keys as you like for a chord; with no mark, changing voice plays nothing. Marks are per part and are not remembered\n"
			                                                  "PC keyboard plays too, white keys only: the A row from C3, the Q row an octave up, the number row another octave up. Hold Z to flatten or X to sharpen what you play. Shift holds the modulation wheel up. PageUp / PageDown shift the octave"));
		else
			ImGui::SetItemTooltip("%s", UI_TEXT(ov_kb_play_tip, "Press to play (either mouse button). Lower is louder"));
	}
	draw_keys(dl, pos, w, h, [&](int note) -> ImU32 {
		return slot >= 0 && ((ram.notes[slot][note >> 6] >> (note & 63)) & 1) ? NOTE_ON : 0;
	});
	const float fs = ImGui::GetFontSize();
	// PC のキーボードで弾ける範囲。鍵盤の下に細い線
	if (pc_low >= 0) {
		float a0, a1, ab, b0, b1, bb;
		key_span(pos, w, h, pc_low, a0, a1, ab);
		key_span(pos, w, h, std::min(127, pc_low + 16), b0, b1, bb);
		const float y = pos.y + h - std::max(2.0f, fs * 0.12f);
		dl->AddRectFilled(ImVec2(a0, y), ImVec2(b1, pos.y + h), IM_COL32(90, 170, 255, 200));
	}
	// 試聴の鍵の目印。鍵の下の方に丸。**印の付いた鍵ぜんぶ**に描く
	if (marker)
		for (int n = 0; n < 128; n++) {
			if (!audition_key(part, n))
				continue;
			float x0, x1, bottom;
			key_span(pos, w, h, n, x0, x1, bottom);
			const float r = std::max(2.0f, std::min((x1 - x0) * 0.45f, fs * 0.3f));
			const ImVec2 c((x0 + x1) * 0.5f, bottom - r - fs * 0.15f);
			dl->AddCircleFilled(c, r + 1.0f, IM_COL32(20, 20, 20, 255));
			dl->AddCircleFilled(c, r, IM_COL32(60, 200, 120, 255));
		}
}

int overview::mod_now(int part, int ram_value)
{
	const bool fresh = m_mod_sent_part == part && m_mod_sent >= 0 && ImGui::GetTime() - m_mod_sent_at < 0.3;
	return fresh ? m_mod_sent : ram_value;
}

int overview::wheel_steps(float wheel, bool big)
{
	// マウスホイールの 1 目で 1（Ctrl で 10）。加速はしない（大きく動かすのはドラッグで）
	if (wheel == 0.0f)
		return 0;
	const int step = std::max(1, int(std::lround(std::fabs(wheel)))) * (big ? 10 : 1);
	return wheel > 0 ? step : -step;
}

// ---- ゆれ（VIB・MW・BEND）。**1 枚の絵**にまとめた区画（音色の窓）
//
// 横はモジュレーションホイールの位置（左が 0、右が 127）、縦はセント（真ん中が 0）。
//   * 背景  … ビブラートの波そのもの。**振幅がその位置での実際の深さ**になる
//              （右へ行くほど深くなる）。波の細かさは Rate に連れる
//   * 前面  … モジュレーションの曲線（±の包み）
//   * 横の線… **音色自身の揺れの深さ**（Vib Depth で動く）。曲線がこの線を超えた所から
//              ホイールのぶんが勝つ ＝ 効き始め。その位置に印を出す
//   * 縦の線… いまのホイールの位置（CC1）
// 下の段は Rate・Depth・Delay・MW LFO PM のフェーダーと、CC1 とピッチベンドの
// 生のフェーダー（この 2 本は音源へ直に流す）
void overview::wobble_cell(int part, xg::model &m, bridge &br, float w, float h)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();

	constexpr int NF = 4;
	static const char *const KEYS[NF] = { "part.vib_rate", "part.vib_depth", "part.vib_delay", "part.mw_lfo_pmod" };
	static const char *const NAMES[NF] = { "Rate", "Depth", "Delay", "MW PM" };
	const xg::param *ps[NF];
	int vals[NF];
	bool have[NF];
	for (int i = 0; i < NF; i++) {
		ps[i] = &P(KEYS[i]);
		vals[i] = ps[i]->def;
		have[i] = m.get(*ps[i], part, vals[i]);
	}
	const bool known = have[0] && have[1] && have[2];

	int rcv = 127;
	m.get(P("part.rcv_channel"), part, rcv);
	const int slot = rcv >= 0 && rcv < PARTS ? rcv : -1;
	const xg_snapshot *snap = current_ram();
	int wheel_now = snap ? mod_now(part, snap->parts[part][xg::ram::PART_MOD] & 0x7f) : 0;
	int bend = bend_now(part, slot);
	int range = 0x42;
	m.get(P("part.bend_pitch"), part, range);

	ImGui::PushID("wobble");
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##wobble", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();

	// メゾネット: 上の段に絵、下の段にフェーダー（ほかの区画と同じ）
	const float pad = fs * 0.25f;
	const float split = pos.y + h * MAISON_SPLIT;
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(hovered || active ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);

	// ---- 下の段。左に層のフェーダー 4 本、右に生の 2 本（CC1 とベンド）
	// ホイールは細め（輪を横から見ているので太くならない）。右端は印のぶんだけ空ける
	const float live_w = std::min(fs * 1.05f, w * 0.08f);
	const float live_gap = fs * 0.7f;
	const float live_right = pos.x + w - pad - fs * 0.3f;
	const float mw_x0 = live_right - live_w * 2.0f - live_gap;
	const float bend_x0 = live_right - live_w;
	// **高さは層のフェーダーとそろえる**。fader_row は渡した枠の上に名前と値の
	// 2 行（gfs * 2.4）を置くので、こちらも同じだけ下げる
	const float gfs = fs * 0.6f;
	const float ftop = split + pad + gfs * 2.4f, fbot = pos.y + h - pad;
	const int focus = fader_row(KEYS, NAMES, NF, 2, part, m, br, ImVec2(pos.x + pad, split + pad),
	                            ImVec2(mw_x0 - fs * 0.6f, fbot), hovered, active, id, vals, have);

	// 生の 2 本。**つまんだ距離で増減**する（押した所へ飛ばない）
	const bool over_mw = hovered && io.MousePos.x >= mw_x0 - live_gap * 0.5f && io.MousePos.x < bend_x0 - live_gap * 0.5f;
	const bool over_bend = hovered && io.MousePos.x >= bend_x0 - live_gap * 0.5f;
	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(id + 1, 0);
	if (active && ImGui::IsItemActivated())
		grab = over_mw ? 1 : (over_bend ? 2 : 0);
	if (!active) {
		if (grab == 2 && !io.KeyCtrl && bend != 0 && slot >= 0) {
			bend_send(part, slot, 0, br);        // 離したら真ん中へ（実物のホイールと同じ）
			bend = 0;
		}
		grab = 0;
		st->SetFloat(id + 2, 0.0f);
	}
	st->SetInt(id + 1, grab);
	const float travel = std::max(1.0f, fbot - ftop);
	if (grab && io.MouseDelta.y != 0.0f && slot >= 0) {
		float acc = st->GetFloat(id + 2, 0.0f);
		const float full = grab == 1 ? 127.0f : 16384.0f;
		acc += -io.MouseDelta.y * full / std::max(150.0f, travel) / (io.KeyShift ? 4.0f : 1.0f);
		const int step = int(acc);
		if (step) {
			acc -= float(step);
			if (grab == 1) {
				const int nv = std::clamp(wheel_now + step, 0, 127);
				if (nv != wheel_now) { mod_send(part, slot, nv, br); wheel_now = nv; }
			} else {
				const int nv = std::clamp(bend + step, -8192, 8191);
				if (nv != bend) { bend_send(part, slot, nv, br); bend = nv; }
			}
		}
		st->SetFloat(id + 2, acc);
	}
	// ホイールで（CC1 は 1 つずつ、ベンドは MSB 1 つぶん）
	if ((over_mw || over_bend) && slot >= 0) {
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		if (io.MouseWheel != 0.0f) {
			const int d = wheel_steps(io.MouseWheel, io.KeyCtrl);
			if (over_mw) {
				const int nv = std::clamp(wheel_now + d, 0, 127);
				if (nv != wheel_now) { mod_send(part, slot, nv, br); wheel_now = nv; }
			} else {
				const int nv = std::clamp(bend + d * 128, -8192, 8191);
				if (nv != bend) { bend_send(part, slot, nv, br); bend = nv; }
			}
		}
	}
	// Ctrl＋右クリックで外へ送るもの（MW は CC1、ベンドはピッチベンド）
	if (over_mw && slot >= 0)
		out_hover_live(slot, false, wheel_now);
	else if (over_bend && slot >= 0)
		out_hover_live(slot, true, bend);
	{
		char t[24];
		std::snprintf(t, sizeof(t), "%d", wheel_now);
		wheel_picture(dl, mw_x0, mw_x0 + live_w, ftop, fbot, float(wheel_now) / 127.0f, false,
		              "MW", t, grab == 1 || over_mw);
		std::snprintf(t, sizeof(t), "%+d", bend);
		wheel_picture(dl, bend_x0, bend_x0 + live_w, ftop, fbot, float(bend + 8192) / 16383.0f, true,
		              "BEND", t, grab == 2 || over_bend);
	}
	if (over_mw || grab == 1)
		hint(UI_TEXT(ov_mod_wheel_hint, "MODULATION WHEEL (CC1)  %d\nModulation wheel. Drag or wheel up/down. Higher adds more of the right side's MW LFO PMOD DEPTH vibrato (sends CC1 to the receive channel)"), wheel_now);
	else if (over_bend || grab == 2)
		hint(UI_TEXT(ps_bend_hint_fmt, "PITCH BEND  %+d (%+.2f semitones)\nDrag sideways to bend (Shift for finer, wheel for steps). Letting go springs back to the middle, Ctrl+release keeps it. Sends pitch bend to the receive channel"),
		     bend, double(bend) / 8192.0 * double(range - 0x40));
	(void)focus;
	dl->AddLine(ImVec2(pos.x, split), ImVec2(pos.x + w, split), col(ImGuiCol_Border), 1.0f);

	// ---- 上の段。1 枚の絵
	const float x0 = pos.x + pad, x1 = pos.x + w - pad;
	const float top = pos.y + pad, bottom = split - pad;
	const float mid = (top + bottom) * 0.5f, half = (bottom - top) * 0.5f;

	voice_ctx v;
	std::vector<shape::mod_line> ms_lines;
	std::vector<shape::vib_line> vs_lines;	float tspan = 1500.0f;
	if (known && voice_of(part, v)) {
		v.blk[0x15] = u8(vals[0]);
		v.blk[0x16] = u8(vals[1]);
		v.blk[0x17] = u8(vals[2]);
		if (have[3])
			v.blk[0x20] = u8(vals[3]);
		ms_lines = shape::mod_lines(v.rom, v.rec, v.blk);
		// 横（時間）の幅は Delay に合わせて伸ばす。つまみ 127 の 4.1 秒が
		// 1.5 秒の窓に入らず、掛かり始めの線が右端に張り付いていた（6.232）
		tspan = shape::vib_span_ms(v.rom, v.rec, v.blk);
		vs_lines = shape::vib_lines(v.rom, v.rec, v.blk, tspan);
	}
	if (ms_lines.empty()) {
		const ImVec2 ts = ImGui::CalcTextSize("--");
		dl->AddText(ImVec2(x0 + (x1 - x0 - ts.x) * 0.5f, mid - ts.y * 0.5f), col(ImGuiCol_TextDisabled), "--");
		dl->PopClipRect();
		ImGui::PopID();
		return;
	}
	const shape::mod_line &L = lead_line(ms_lines);
	const shape::vib_line *V = vs_lines.empty() ? nullptr : &lead_line(vs_lines);

	// **音色自身の揺れ**（Depth を既定の 64 にしたもの）。Depth で足した・引いたぶんを
	// 見せるための下敷き。vib_cell と同じ作り
	std::vector<shape::vib_line> own_ls;
	if (V && vals[1] != 64) {
		u8 own_blk[XG_PART_COPY];
		std::memcpy(own_blk, v.blk, sizeof(own_blk));
		own_blk[0x16] = 64;
		own_ls = shape::vib_lines(v.rom, v.rec, own_blk, tspan);
	}
	const shape::vib_line *O = own_ls.empty() ? nullptr : &lead_line(own_ls);

	// 縦の目盛り。どちらの層もいちばん深い所が少し余るように（最低でも ±220 セント）
	float span = 220.0f;
	span = std::max(span, L.own_cents * 1.15f);
	for (float c : L.eff)
		span = std::max(span, c * 1.15f);
	if (V)
		span = std::max(span, V->depth_cents * 1.15f);
	if (O)
		span = std::max(span, O->depth_cents * 1.15f);
	auto y_of = [&](float cents) { return mid - half * std::clamp(cents / span, -1.0f, 1.0f); };
	auto x_wheel = [&](float wheel) { return x0 + (x1 - x0) * std::clamp(wheel, 0.0f, 127.0f) / 127.0f; };
	auto x_ms = [&](float ms) { return x0 + (x1 - x0) * std::clamp(ms / tspan, 0.0f, 1.0f); };

	float delay_x = -1.0f;      // 掛かり始めの縦線の場所（字はいちばん最後に書く）
	dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), col(ImGuiCol_TextDisabled, 0.35f));
	for (float c : { 50.0f, 100.0f, 200.0f, 400.0f }) {
		if (c > span)
			break;
		for (float sgn : { 1.0f, -1.0f })
			dl->AddLine(ImVec2(x0, y_of(sgn * c)), ImVec2(x1, y_of(sgn * c)), col(ImGuiCol_TextDisabled, 0.12f));
		char g[16];
		std::snprintf(g, sizeof(g), "%.0f", c);
		// 上 2 行は「MW 0」と数字の行、下 1 行は時間の軸の字。そこへ掛かる
		// 目盛りの字は**書かない**（寄せると重なって読めなくなる）
		const float gy = y_of(c) - fs * 0.6f;
		if (gy < top + fs * 1.3f || gy > bottom - fs * 1.8f)
			continue;
		dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x0 + 2.0f, gy), col(ImGuiCol_TextDisabled, 0.5f), g);
	}

	// ================= 下の層: ビブラートの波（横は時間 1.5 秒）=================
	// **Rate・Depth・Delay がここに出る**。横軸がホイールだった頃は Delay を
	// 描く場所が無かった（issue の指摘）
	if (V) {
		// 掛かり始め（Delay）。縦の点線
		if (V->delay_ms > 0.0f) {
			const float xd = x_ms(V->delay_ms);
			for (float y = top; y < bottom; y += fs * 0.5f)
				dl->AddLine(ImVec2(xd, y), ImVec2(xd, std::min(bottom, y + fs * 0.25f)),
				            col(ImGuiCol_TextDisabled, 0.55f));
			// 字は**下側**（時間の軸のほう）に、**いちばん最後**に書く
			// （前の層に書くと、あとから来るモジュレーションの線に潰される）
			delay_x = xd;
		}
		// 音色自身の揺れ（Depth 64）をいちばん下に薄く
		if (O) {
			std::vector<ImVec2> op;
			op.reserve(O->pts.size());
			for (const shape::pt &p : O->pts)
				op.push_back(ImVec2(x_ms(p.ms), y_of(p.cents)));
			if (op.size() >= 2)
				dl->AddPolyline(op.data(), int(op.size()), col(ImGuiCol_TextDisabled, 0.30f), 0, 1.0f);
		}
		// いまの Depth での揺れ
		std::vector<ImVec2> wp;
		wp.reserve(V->pts.size());
		for (const shape::pt &p : V->pts)
			wp.push_back(ImVec2(x_ms(p.ms), y_of(p.cents)));
		if (wp.size() >= 2)
			dl->AddPolyline(wp.data(), int(wp.size()), col(ImGuiCol_Text, 0.42f), 0, 1.2f);
	}

	// ================= 上の層: モジュレーションの曲線（横はホイール）=================
	for (float sgn : { 1.0f, -1.0f }) {
		std::vector<ImVec2> pts;
		pts.reserve(128);
		for (int i = 0; i < 128; i++)
			pts.push_back(ImVec2(x_wheel(float(i)), y_of(sgn * L.eff[size_t(i)])));
		dl->AddPolyline(pts.data(), int(pts.size()), col(ImGuiCol_SliderGrabActive), 0, std::max(1.5f, fs * 0.09f));
	}
	// 効き始め（音色自身の深さ。Vib Depth で上下する）と、曲線が超える位置
	if (L.own_cents > 0.0f) {
		for (float sgn : { 1.0f, -1.0f }) {
			const float y = y_of(sgn * L.own_cents);
			for (float x = x0; x < x1; x += fs * 0.6f)
				dl->AddLine(ImVec2(x, y), ImVec2(std::min(x1, x + fs * 0.3f), y), IM_COL32(230, 180, 90, 200));
		}
		int cross = -1;
		for (int i = 0; i < 128; i++)
			if (L.eff[size_t(i)] > L.own_cents + 0.5f) { cross = i; break; }
		if (cross > 0) {
			const float x = x_wheel(float(cross));
			dl->AddLine(ImVec2(x, y_of(L.own_cents)), ImVec2(x, y_of(-L.own_cents)), IM_COL32(230, 180, 90, 120));
			char t[24];
			std::snprintf(t, sizeof(t), "%d", cross);
			dl->AddText(ImGui::GetFont(), fs * 0.55f, ImVec2(x + 2.0f, y_of(L.own_cents) - fs * 0.65f),
			            IM_COL32(230, 180, 90, 220), t);
		}
	}
	// いまのホイールの位置
	{
		const float x = x_wheel(float(wheel_now));
		dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), col(ImGuiCol_Text, 0.5f), 1.0f);
		dl->AddCircleFilled(ImVec2(x, y_of(L.eff[size_t(std::clamp(wheel_now, 0, 127))])), std::max(2.0f, fs * 0.16f),
		                    col(ImGuiCol_Text, 0.9f));
	}

	// ---- 軸の名前。**層ごとの色**で（下＝時間、上＝ホイール）
	{
		ImFont *font = ImGui::GetFont();
		const float afs = fs * 0.55f;
		dl->AddText(font, afs, ImVec2(x0 + 2.0f, bottom - afs - 1.0f), col(ImGuiCol_Text, 0.40f), "0 ms");
		if (delay_x >= 0.0f) {
			char t[24];
			std::snprintf(t, sizeof(t), "%.0f ms", V->delay_ms);
			const ImVec2 ds = font->CalcTextSizeA(afs, FLT_MAX, 0.0f, t);
			const ImVec2 dp(std::clamp(delay_x + 2.0f, x0 + 2.0f, x1 - ds.x - 2.0f), bottom - afs - 1.0f);
			// 波と曲線の上に重なるので、字の下だけ敷く（FrameBg は
			// 半分すけているので、下地には WindowBg のほうを使う）
			dl->AddRectFilled(ImVec2(dp.x - 1.0f, dp.y), ImVec2(dp.x + ds.x + 1.0f, dp.y + ds.y),
			                  col(ImGuiCol_WindowBg), 2.0f);
			dl->AddText(font, afs, dp, col(ImGuiCol_Text, 0.6f), t);
		}
		char tl[16];
		std::snprintf(tl, sizeof(tl), tspan < 1000.0f ? "%.0f ms" : "%.1f s",
		              tspan < 1000.0f ? tspan : tspan / 1000.0f);
		const ImVec2 ts = font->CalcTextSizeA(afs, FLT_MAX, 0.0f, tl);
		dl->AddText(font, afs, ImVec2(x1 - ts.x - 2.0f, bottom - afs - 1.0f), col(ImGuiCol_Text, 0.40f), tl);
		dl->AddText(font, afs, ImVec2(x0 + 2.0f, top + 1.0f), col(ImGuiCol_SliderGrabActive), "MW 0");
		const ImVec2 ws = font->CalcTextSizeA(afs, FLT_MAX, 0.0f, "127");
		dl->AddText(font, afs, ImVec2(x1 - ws.x - 2.0f, top + 1.0f), col(ImGuiCol_SliderGrabActive), "127");
	}

	// ---- 数字（真ん中の上）。**軸の字の 1 行下**に置く（上の角は
	// 「MW 0」「127」で埋まっていて、重なって読めなかった）
	char s2[128];
	std::snprintf(s2, sizeof(s2), "%.2f Hz  ±%.0f c  %.0f ms   MW %d → ±%.0f c",
	              V ? V->hz : 0.0f, V ? V->depth_cents : L.own_cents, V ? V->delay_ms : 0.0f,
	              wheel_now, L.eff[size_t(std::clamp(wheel_now, 0, 127))]);
	{
		ImFont *font = ImGui::GetFont();
		const ImVec2 ns = font->CalcTextSizeA(fs * 0.65f, FLT_MAX, 0.0f, s2);
		dl->AddText(font, fs * 0.65f, ImVec2((x0 + x1 - ns.x) * 0.5f, top + fs * 0.55f + 3.0f),
		            col(ImGuiCol_Text, 0.85f), s2);
	}

	dl->PopClipRect();
	ImGui::PopID();
}


// ---- ピッチベンド（音色の窓の「ゆれ」の区画のつまみ、と一覧の列）
//
// 値は **16384 段のまま**（真ん中からの離れ、-8192〜+8191）扱う。画面に出す値は
// 入ってきた MIDI から取る（ui/driver.h）ので、式だけの口でも firmware の道でも同じ

int overview::bend_now(int part, int slot)
{
	// 送ったばかりの間は送った値（写しは 25ms ごとなので、その間は古い）
	if (m_bend_sent_part == part && ImGui::GetTime() - m_bend_sent_at < 0.3)
		return m_bend_sent;
	const xg_snapshot *snap = current_ram();
	return snap && slot >= 0 ? snap->bend[slot] : 0;
}

void overview::bend_send(int part, int slot, int value, bridge &br)
{
	if (slot < 0)
		return;
	const int raw = std::clamp(value + 8192, 0, 16383);
	const u8 pb[3] = { u8(0xe0 | (slot & 15)), u8(raw & 0x7f), u8((raw >> 7) & 0x7f) };
	br.send_port(slot / 16, pb, 3);
	m_bend_sent = value;
	m_bend_sent_part = part;
	m_bend_sent_at = ImGui::GetTime();
}



void overview::mod_send(int part, int slot, int value, bridge &br)
{
	if (slot < 0)
		return;
	const u8 cc[3] = { u8(0xb0 | (slot & 15)), 1, u8(value) };
	br.send_port(slot / 16, cc, 3);
	m_mod_sent = value;
	m_mod_sent_part = part;
	m_mod_sent_at = ImGui::GetTime();
}


// PC のキーボードで弾く。3 段とも白鍵だけを並べる（キーは文字でなく位置で読むので、配列が違っても同じ並び）:
//   A 段（A から 12 個。日本語配列で A〜」）   m_pc_base（C3）からの白鍵
//   Q 段（Q から 12 個。Q〜「）               その 1 オクターブ上
//   数字の段（1 から 13 個。1〜￥）            さらに 1 オクターブ上
// Z を押している間に弾いた鍵は半音下、X は半音上（黒鍵はこれで弾く）。Shift を押している間は
// モジュレーション（CC1）を PC_MOD にし、離すと 0 に戻す。PageUp / PageDown で全体を 1 オクターブ動かす
void overview::pc_keys(int part, int slot, bridge &br)
{
	static constexpr ImGuiKey KEYS[PC_KEYS] = {
		ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_F, ImGuiKey_G, ImGuiKey_H, ImGuiKey_J, ImGuiKey_K,
		ImGuiKey_L, ImGuiKey_Semicolon, ImGuiKey_Apostrophe, ImGuiKey_Backslash,
		ImGuiKey_Q, ImGuiKey_W, ImGuiKey_E, ImGuiKey_R, ImGuiKey_T, ImGuiKey_Y, ImGuiKey_U, ImGuiKey_I,
		ImGuiKey_O, ImGuiKey_P, ImGuiKey_LeftBracket, ImGuiKey_RightBracket,
		ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8,
		ImGuiKey_9, ImGuiKey_0, ImGuiKey_Minus, ImGuiKey_Equal, ImGuiKey_Oem102,
	};
	// 段の中の何個目か → C からの半音（白鍵だけ）
	static constexpr int WHITE[13] = { 0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21 };
	auto offset = [](int i) { return i < 12 ? WHITE[i] : i < 24 ? 12 + WHITE[i - 12] : 24 + WHITE[i - 24]; };
	ImGuiIO &io = ImGui::GetIO();
	// 離したキー（窓から外れたときも ImGui がキーを離したことにする）
	for (int i = 0; i < PC_KEYS; i++) {
		if (m_pc_note[i] >= 0 && !ImGui::IsKeyDown(KEYS[i])) {
			const u8 off[3] = { u8(0x80 | (m_pc_slot[i] & 15)), u8(m_pc_note[i]), 64 };
			br.send_port(m_pc_slot[i] / 16, off, 3);
			m_pc_note[i] = -1;
		}
	}
	// Shift を離したら（文字の箱に入ったときも）モジュレーションを戻す
	const bool typing = io.WantTextInput || io.KeyCtrl || io.KeyAlt || slot < 0;
	if (m_pc_mod_slot >= 0 && (!io.KeyShift || typing)) {
		mod_send(m_pc_mod_part, m_pc_mod_slot, 0, br);
		m_pc_mod_slot = -1;
	}
	// 文字を打っている最中（数を打つ箱など）と、Ctrl・Alt を押しているときは弾かない
	if (typing)
		return;
	if (io.KeyShift && m_pc_mod_slot < 0) {
		mod_send(part, slot, PC_MOD, br);
		m_pc_mod_part = part;
		m_pc_mod_slot = slot;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false))
		m_pc_base = std::max(0, m_pc_base - 12);
	if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false))
		m_pc_base = std::min(84, m_pc_base + 12);
	const int shift = (ImGui::IsKeyDown(ImGuiKey_X) ? 1 : 0) - (ImGui::IsKeyDown(ImGuiKey_Z) ? 1 : 0);
	for (int i = 0; i < PC_KEYS; i++) {
		if (!ImGui::IsKeyPressed(KEYS[i], false) || m_pc_note[i] >= 0)
			continue;
		const int note = m_pc_base + offset(i) + shift;
		if (note < 0 || note > 127)
			continue;
		const u8 on[3] = { u8(0x90 | (slot & 15)), u8(note), 100 };
		br.send_port(slot / 16, on, 3);
		m_pc_note[i] = note;
		m_pc_slot[i] = slot;
	}
}

void overview::release_pc_keys(bridge &br)
{
	for (int i = 0; i < PC_KEYS; i++) {
		if (m_pc_note[i] < 0)
			continue;
		const u8 off[3] = { u8(0x80 | (m_pc_slot[i] & 15)), u8(m_pc_note[i]), 64 };
		br.send_port(m_pc_slot[i] / 16, off, 3);
		m_pc_note[i] = -1;
	}
	if (m_pc_mod_slot >= 0) {
		mod_send(m_pc_mod_part, m_pc_mod_slot, 0, br);
		m_pc_mod_slot = -1;
	}
}


namespace {

const overview::column &column_of(const char *title)
{
	for (const overview::column &c : COLUMNS)
		if (!std::strcmp(c.title, title))
			return c;
	return COLUMNS[0];
}

// 種類の品書き（分類 → 系統 → LSB 違い）。今の種類に印
void type_menu(const std::vector<xg::fx_type> &types, const char *key, xg::model &m, bridge &br)
{
	int cur = 0, chosen = 0;
	const bool has = m.get(P(key), 0, cur);
	if (fx_type_menu(types, has ? cur : -1, chosen))
		br.send(m.set(P(key), 0, chosen));
}

// 掛け先のパートの品書き（A1-B16 と OFF）
void part_menu(const char *key, xg::model &m, bridge &br, bool with_off)
{
	int cur = 127;
	m.get(P(key), 0, cur);
	static const char *PORT_MENU[4] = { "A1-A16", "B1-B16", "C1-C16", "D1-D16" };
	for (int port = 0; port < PARTS / 16; port++) {
		if (!ImGui::BeginMenu(PORT_MENU[port]))
			continue;
		for (int i = port * 16; i < port * 16 + 16; i++)
			if (ImGui::MenuItem(part_name(i).c_str(), nullptr, cur == i))
				br.send(m.set(P(key), 0, i));
		ImGui::EndMenu();
	}
	// 64 パートの後ろに A/D INPUT が 2 つ並ぶ（実機で確かめた）
	for (int i = PARTS; i < PARTS + 2; i++)
		if (ImGui::MenuItem(part_name(i).c_str(), nullptr, cur == i))
			br.send(m.set(P(key), 0, i));
	if (with_off && ImGui::MenuItem(UI_TEXT(ov_off_no_part, "OFF (applies to no part)"), nullptr, cur >= PARTS + 2))
		br.send(m.set(P(key), 0, 127));
}

} // namespace


// システムのエフェクト（リバーブ・コーラス・バリエーション）の 1 マス。
// 上の行が種類（右クリックで選ぶ）、下が戻り量の棒
void overview::system_fx_cell(const char *title, const std::vector<xg::fx_type> &types, const char *type_key,
                              const char *return_col, bool variation, xg::model &m, const xg_snapshot &ram,
                              bridge &br, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	const float line = fs * 1.1f;

	ImGui::PushID(title);
	ImGui::InvisibleButton("##type", ImVec2(w, line), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool hot = ImGui::IsItemHovered();
	if (ImGui::BeginPopupContextItem("typemenu", ImGuiPopupFlags_MouseButtonRight)) {
		ImGui::TextDisabled(UI_TEXT(ov_fx_kind_fmt, "%s type"), title);
		ImGui::Separator();
		type_menu(types, type_key, m, br);
		if (variation) {
			int conn = 1;
			m.get(P("variation.connect"), 0, conn);
			ImGui::Separator();
			ImGui::TextDisabled("%s", UI_TEXT(fx_connect, "Connection"));
			if (ImGui::MenuItem(UI_TEXT(ov_conn_sys, "SYSTEM (all parts send to it)"), nullptr, conn == 1))
				br.send(m.set(P("variation.connect"), 0, 1));
			if (ImGui::BeginMenu(UI_TEXT(ov_conn_ins, "INSERTION (applies to one part)"))) {
				part_menu("variation.part", m, br, false);
				ImGui::EndMenu();
			}
			if (conn == 0 && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", UI_TEXT(ov_conn_tip, "Applies to the chosen part"));
		}
		ImGui::EndPopup();
	}
	if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("typemenu"))
		ImGui::SetItemTooltip("%s", UI_TEXT(ov_rclick_kind, "Right-click to pick the type"));

	int type = 0, conn = 1, vpart = 127;
	std::string name = m.get(P(type_key), 0, type) ? xg::fx_name(type) : "--";
	bool dim = false;
	if (variation && m.get(P("variation.connect"), 0, conn) && conn == 0) {
		m.get(P("variation.part"), 0, vpart);
		name += vpart < PARTS + 2 ? " → " + part_name(vpart) : " → OFF";
		dim = false;
	}
	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + line), true);
	if (hot)
		dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + line), col(ImGuiCol_HeaderHovered, 0.35f));
	dl->AddText(ImVec2(pos.x + fs * 0.3f, pos.y + (line - fs) * 0.5f), dim ? col(ImGuiCol_TextDisabled) : col(ImGuiCol_Text), name.c_str());
	dl->PopClipRect();
	ImGui::PopID();

	// 戻り量
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + line));
	cell(column_of(return_col), -1, m, ram, br, w, h - line);
}


// インサーション（とバリエーション）の 1 マス。上の行が印と種類、下が掛け先。
// 右クリックで種類と掛け先、印をつかんでパートの INS 欄に落とすと掛け先が変わる
void overview::insertion_cell(int slot_index, xg::model &m, bridge &br, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const fx_slot &f = FX_SLOTS[slot_index];
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;

	ImGui::PushID(f.id);
	ImGui::InvisibleButton("##slot", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
	int type = 0;
	const bool has_type = m.get(P(f.type_key), 0, type);
	const std::string name = has_type ? xg::fx_name(type) : "--";
	const int where = fx_target(f, m);
	if (f.id <= 4 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		request_fx(f.id);                        // 設定の窓を出す
	if (ImGui::BeginDragDropSource()) {
		ImGui::SetDragDropPayload(DRAG_FX, &f.id, sizeof(f.id));
		ImGui::Text(UI_TEXT(ov_drag_to_fmt, "Drop %s (%s) onto a part's INS cell"), fx_slot_title(f), name.c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginPopupContextItem("slotmenu", ImGuiPopupFlags_MouseButtonRight)) {
		ImGui::TextDisabled("%s", fx_slot_title(f));
		ImGui::Separator();
		if (ImGui::BeginMenu(UI_TEXT(fx_kind, "Type"))) {
			type_menu(xg::ins_types(), f.type_key, m, br);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(UI_TEXT(fx_part, "Part"))) {
			part_menu(f.part_key, m, br, true);
			ImGui::EndMenu();
		}
		ImGui::Separator();
		ImGui::TextDisabled("%s", UI_TEXT(ov_drag_note2, "Grab it and drop it onto a part's INS cell to apply it.\n"
		                                           "Applying it while still NO EFFECT silences the part."));
		ImGui::EndPopup();
	}
	if (ImGui::IsItemHovered() && !ImGui::IsDragDropActive())
		ImGui::SetItemTooltip(UI_TEXT(ov_move_tip_fmt, "%s: %s → %s\nDouble-click for settings, right-click for type and part, drag onto a part's INS cell"),
		                      fx_slot_title(f), name.c_str(), where >= 0 ? part_name(where).c_str() : "OFF");
	ImGui::PopID();

	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
	if (hot)
		dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(ImGuiCol_HeaderHovered, 0.35f));
	const float bw = fs * 1.0f;
	const float y = pos.y + fs * 0.1f;
	const ImU32 badge = where >= 0 ? f.color : col(ImGuiCol_TextDisabled, 0.5f);
	dl->AddRectFilled(ImVec2(pos.x + fs * 0.2f, y + 1), ImVec2(pos.x + fs * 0.2f + bw, y + fs), badge, 3.0f);
	const ImVec2 ms = ImGui::CalcTextSize(f.mark);
	dl->AddText(ImVec2(pos.x + fs * 0.2f + (bw - ms.x) * 0.5f, y), IM_COL32(20, 20, 20, 255), f.mark);
	dl->AddText(ImVec2(pos.x + fs * 1.5f, y), where >= 0 ? col(ImGuiCol_Text) : col(ImGuiCol_TextDisabled), name.c_str());
	const std::string to = where >= 0 ? "→ " + part_name(where) : "OFF";
	dl->AddText(ImVec2(pos.x + fs * 1.5f, y + fs * 1.05f), col(ImGuiCol_TextDisabled), to.c_str());
	dl->PopClipRect();
}


// マスターの表。パートの表とは見出しを分ける
void overview::master_pane(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	const float fs = ImGui::GetFontSize();
	const float h = fs * 2.3f;
	ImDrawList *dl = ImGui::GetWindowDrawList();

	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH |
	                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
	constexpr int NCOL = 12;
	if (!ImGui::BeginTable("master", NCOL, flags))
		return;
	ImGui::TableSetupColumn(UI_TEXT(bar_master, "Master"), ImGuiTableColumnFlags_WidthFixed, fs * 18.5f);
	ImGui::TableSetupColumn("M.VOL", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
	// 音の流れの順（インサーション → バリエーション → コーラス → リバーブ → マスター EQ）
	ImGui::TableSetupColumn("INS 1", ImGuiTableColumnFlags_WidthFixed, fs * 7);
	ImGui::TableSetupColumn("INS 2", ImGuiTableColumnFlags_WidthFixed, fs * 7);
	ImGui::TableSetupColumn("INS 3", ImGuiTableColumnFlags_WidthFixed, fs * 7);
	ImGui::TableSetupColumn("INS 4", ImGuiTableColumnFlags_WidthFixed, fs * 7);
	ImGui::TableSetupColumn("VARIATION", ImGuiTableColumnFlags_WidthFixed, fs * 9.5f);
	ImGui::TableSetupColumn("CHORUS", ImGuiTableColumnFlags_WidthFixed, fs * 7.5f);
	ImGui::TableSetupColumn("REVERB", ImGuiTableColumnFlags_WidthFixed, fs * 7.5f);
	ImGui::TableSetupColumn("MASTER EQ", ImGuiTableColumnFlags_WidthFixed, fs * 11);
	ImGui::TableSetupColumn("SPECTRUM", ImGuiTableColumnFlags_WidthFixed, fs * 11);
	ImGui::TableSetupColumn("##mkeys", ImGuiTableColumnFlags_WidthStretch);
	// Help keys, parallel to the displays above (the マスター display is
	// translated; the rest are ASCII and double as their own keys).
	static const char *const MASTER_COL_KEYS[] = {
		"マスター", "M.VOL", "INS 1", "INS 2", "INS 3", "INS 4",
		"VARIATION", "CHORUS", "REVERB", "MASTER EQ", "SPECTRUM", "##mkeys",
	};
	headers_with_help(NCOL, MASTER_COL_KEYS);
	ImGui::TableNextRow(0, h);
	ImGui::PushID("master");

	// ---- 名前。移調とマスターチューンも
	ImGui::TableNextColumn();
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::InvisibleButton("##mastername", ImVec2(w, h));
		if (ImGui::IsItemHovered()) {
			dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(ImGuiCol_HeaderHovered, 0.4f));
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				request_master();
			ImGui::SetItemTooltip("%s", UI_TEXT(ov_master_name_tip, "Double-click for the master window (master volume, transpose, effect returns, master EQ)"));
		}
		dl->AddText(ImVec2(pos.x + fs * 0.3f, pos.y + fs * 0.1f), col(ImGuiCol_Text), "MASTER");
		int tr = 0x40, tune = 0x400;
		char sub[64];
		if (m.get(P("system.transpose"), 0, tr) && m.get(P("system.master_tune"), 0, tune))
			std::snprintf(sub, sizeof(sub), "Transpose %s   Tune %s",
			              xg::format(P("system.transpose"), tr).c_str(), xg::format(P("system.master_tune"), tune).c_str());
		else
			std::snprintf(sub, sizeof(sub), "--");
		dl->AddText(ImVec2(pos.x + fs * 0.3f, pos.y + fs * 1.15f), col(ImGuiCol_TextDisabled), sub);
	}

	ImGui::TableNextColumn();
	cell(column_of("VOL"), -1, m, ram, br, ImGui::GetContentRegionAvail().x, h);
	for (int i = 0; i < 4; i++) {
		ImGui::TableNextColumn();
		insertion_cell(i, m, br, h);
	}
	ImGui::TableNextColumn();
	system_fx_cell(UI_TEXT(sys_variation, "Variation"), xg::ins_types(), "variation.type", "VAR", true, m, ram, br, h);
	ImGui::TableNextColumn();
	system_fx_cell(UI_TEXT(sys_chorus, "Chorus"), xg::cho_types(), "chorus.type", "CHO", false, m, ram, br, h);
	ImGui::TableNextColumn();
	system_fx_cell(UI_TEXT(sys_reverb, "Reverb"), xg::rev_types(), "reverb.type", "REV", false, m, ram, br, h);
	ImGui::TableNextColumn();
	master_eq_cell(m, br, h);

	// ---- 最終の出力のスペクトラム（エフェクトとマスター EQ のあと）
	ImGui::TableNextColumn();
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(w, h));
		const float pad = fs * 0.2f;
		mini_spec &c = mini_spec_of(mu2000::PSCOPE_OUT);
		mini_spec_update(br, mu2000::PSCOPE_OUT, c);
		mini_spec_draw(dl, c, ImVec2(pos.x + pad, pos.y + pad), ImVec2(pos.x + w - pad, pos.y + h - pad),
		               IM_COL32(140, 240, 190, 255));
	}

	// ---- 鍵盤。全パートで鳴っている鍵を重ねる。色はパートごと、重なったら混ぜる
	ImGui::TableNextColumn();
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(w, h));
		int slots[PARTS];
		for (int p = 0; p < PARTS; p++) {
			int rcv = 127;
			slots[p] = m.get(P("part.rcv_channel"), p, rcv) && rcv < PARTS ? rcv : -1;
		}
		draw_keys(dl, pos, w, h, [&](int note) -> ImU32 {
			int r = 0, g = 0, b = 0, n = 0;
			for (int p = 0; p < PARTS; p++) {
				const int sl = slots[p];
				if (sl < 0 || !((ram.notes[sl][note >> 6] >> (note & 63)) & 1))
					continue;
				const ImU32 c = part_color(p);
				r += (c >> IM_COL32_R_SHIFT) & 0xff;
				g += (c >> IM_COL32_G_SHIFT) & 0xff;
				b += (c >> IM_COL32_B_SHIFT) & 0xff;
				n++;
			}
			return n ? IM_COL32(r / n, g / n, b / n, 255) : 0;
		});
	}
	ImGui::PopID();
	ImGui::EndTable();
}


// パートの音色の窓の上のペイン。1 行目に掛かっているエフェクト（名前付き）、
// 2 行目に VOL〜HOLD と VAR〜REV の棒（一覧と同じく触れる）と、このパートの鍵盤
float overview::part_strip_height()
{
	// part_strip と同じ積み方: 1 行目（フレームの高さ）、見出しと数の行、棒、鍵盤
	const float fs = ImGui::GetFontSize();
	const ImGuiStyle &st = ImGui::GetStyle();
	ImGui::PushFont(nullptr, fs * LABEL_SCALE);
	const float label_h = ImGui::GetTextLineHeight() + fs * 0.1f;
	ImGui::PopFont();
	return ImGui::GetFrameHeight() + st.ItemSpacing.y + label_h + fs * METER_H + st.ItemSpacing.y + fs * 2.3f;
}

void overview::part_strip(int part, xg::model &m, const xg_snapshot &ram, bridge &br)
{
	m_wheel_taken = false;
	const float fs = ImGui::GetFontSize();
	const float h = fs * 2.3f;
	const ImGuiStyle &st = ImGui::GetStyle();
	ImGui::PushID("strip");
	ImGui::PushID(part);

	// 棒の並び。窓の幅いっぱいに同じ幅で割り振る（VOL〜HOLD の 6 本、間を空けて VAR・CHO・REV の 3 本）
	static const char *const LEFT[]  = { "VOL", "EXP", "PAN", "P.BEND", "MOD", "HOLD" };
	static const char *const RIGHT[] = { "VAR", "CHO", "REV" };
	const float gap = fs * 1.0f;
	const ImVec2 top = ImGui::GetCursorScreenPos();
	const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
	const float unit = std::max(fs * 4.2f, (right - top.x - gap) / 9.0f);
	const float var_x = top.x + unit * 6.0f + gap;         // VAR の棒の左端

	// ---- 1 行目: 左にインサーション、VAR の棒の真上からバリエーション
	const float line_h = ImGui::GetFrameHeight();
	ImGui::SetCursorScreenPos(top);
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", UI_TEXT(ov_ins_section, "Insertion"));
	help_tip("INS");
	ImGui::SameLine();
	{
		const ImVec2 at = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(at.x, top.y));
		// 印の並びは VAR の手前まで（はみ出す分は切る）
		ImGui::BeginChild("##ins_row", ImVec2(std::max(fs, var_x - gap * 0.5f - at.x), line_h), ImGuiChildFlags_None,
		                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
		const ImVec2 in = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(in.x, in.y + st.FramePadding.y - fs * 0.1f));
		ins_cell(part, m, br, fs * 1.25f, true, fx_which::insertions);
		ImGui::EndChild();
	}
	ImGui::SetCursorScreenPos(ImVec2(var_x, top.y));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", UI_TEXT(sys_variation, "Variation"));
	help_tip("VARIATION");
	variation_label(part, m, br, var_x + ImGui::CalcTextSize((std::string(UI_TEXT(sys_variation, "Variation")) + " ").c_str()).x, top.y + st.FramePadding.y, right);

	// ---- 2 行目: 見出しと数（小さめの字で同じ行に）、その下に棒。
	// 見出しと数が重なるほど狭ければ見出しを出さない（カーソルを載せると下の帯に名前と説明）
	ImGui::PushFont(nullptr, fs * LABEL_SCALE);
	const float label_h = ImGui::GetTextLineHeight() + fs * 0.1f;
	ImGui::PopFont();
	const float meter_h = fs * METER_H;
	const ImVec2 origin(top.x, top.y + line_h + st.ItemSpacing.y);
	ImDrawList *sdl = ImGui::GetWindowDrawList();
	float x = origin.x;
	auto one = [&](const char *title) {
		const float w = unit - fs * 0.25f;
		const float pad = fs * 0.2f;
		cell_text ct;
		ImGui::SetCursorScreenPos(ImVec2(x, origin.y + label_h));
		cell(column_of(title), part, m, ram, br, w, meter_h, &ct);
		ImGui::PushFont(nullptr, fs * LABEL_SCALE);
		const ImVec2 vs = ImGui::CalcTextSize(ct.text.c_str());
		const ImVec2 ts = ImGui::CalcTextSize(title);
		const bool room = ts.x + fs * 0.4f + vs.x <= w - pad * 2.0f;
		if (room)
			sdl->AddText(ImVec2(x + pad, origin.y), col(ImGuiCol_TextDisabled), title);
		sdl->AddText(ImVec2(x + w - pad - vs.x, origin.y), ct.bright ? col(ImGuiCol_Text) : col(ImGuiCol_TextDisabled),
		             ct.text.c_str());
		ImGui::PopFont();
		// 見出しの行か棒にカーソルが載ったら、名前と今の値と説明を下の帯へ
		const bool over = ct.hovered || ImGui::IsMouseHoveringRect(ImVec2(x, origin.y), ImVec2(x + w, origin.y + label_h));
		if (over && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
			// 1 行目は正式名（XG のパートの項目なら番地も、CC なら番号も）
			static const std::pair<const char *, const char *> OFFICIAL[] = {
				{ "VOL", "part.volume" }, { "EXP", "EXPRESSION（CC11）" }, { "PAN", "part.pan" },
				{ "P.BEND", "PITCH BEND" }, { "MOD", "MODULATION WHEEL（CC1）" }, { "HOLD", "HOLD1（CC64）" },
				{ "VAR", "part.variation_send" }, { "CHO", "part.chorus_send" }, { "REV", "part.reverb_send" },
			};
			std::string name = title;
			for (const auto &o : OFFICIAL)
				if (!std::strcmp(o.first, title))
					name = std::strncmp(o.second, "part.", 5) ? std::string(o.second) : official_name(o.second);
			if (const char *help = help_for(title))
				hint("%s  %s\n%s", name.c_str(), ct.text.c_str(), help);
			else if (!ct.hovered)
				hint("%s  %s", name.c_str(), ct.text.c_str());
		}
		x += unit;
	};
	for (const char *t : LEFT)
		one(t);
	x += gap;
	for (const char *t : RIGHT)
		one(t);

	// ---- 3 行目: 鍵盤。受信チャンネルから見張りの口×チャンネル（一覧でミュートしていても、この窓は受信チャンネルのまま）
	const float keys_y = origin.y + label_h + meter_h + st.ItemSpacing.y;
	int rcv = 127;
	m.get(P("part.rcv_channel"), part, rcv);
	const int slot = rcv >= 0 && rcv < PARTS ? rcv : -1;
	// 鍵盤（右クリックで試聴の鍵、PC のキーボードでも弾ける）。
	// **左端にあったモジュレーションホイールは外した**（同じものが下の
	// 「ゆれ」の区画にあり、鍵盤の幅を削ってまで置くものではない）
	ImGui::SetCursorScreenPos(ImVec2(origin.x, keys_y));
	keys_cell(part, slot, ram, br, std::max(fs * 8.0f, right - origin.x), h, true, m_pc_base);
	pc_keys(part, slot, br);

	ImGui::SetCursorScreenPos(ImVec2(origin.x, keys_y + h));
	ImGui::Dummy(ImVec2(0, 0));
	ImGui::PopID();
	ImGui::PopID();
}

// バリエーションの種類と繋がり方（パートの帯の 1 行目、VAR の棒の真上）。このパートに INSERTION で掛かっていれば
// 一覧の INS 欄と同じ V の印（ドラッグ・右クリックで触れる）、そうでなければ文字で（SYSTEM なら送りの棒が効く）
void overview::variation_label(int part, xg::model &m, bridge &br, float x0, float y, float x1)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	int type = 0, conn = 1, who = 127;
	const bool has_type = m.get(P("variation.type"), 0, type);
	m.get(P("variation.connect"), 0, conn);
	m.get(P("variation.part"), 0, who);
	if (conn == 0 && who == part) {
		ImGui::SetCursorScreenPos(ImVec2(x0 - fs * 0.2f, y));
		ImGui::PushID("var");
		const float w = x1 - x0 + fs * 0.2f;
		ImGui::BeginChild("##varlabel", ImVec2(w, ImGui::GetTextLineHeight() + 2), ImGuiChildFlags_None,
		                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
		ins_cell(part, m, br, ImGui::GetTextLineHeight() + 2, true, fx_which::variation);
		ImGui::EndChild();
		ImGui::PopID();
		return;
	}
	char text[96];
	const std::string name = has_type ? xg::fx_name(type) : std::string("--");
	if (conn == 0)
		std::snprintf(text, sizeof(text), UI_TEXT(cap_insertion_fmt, "%s (INSERTION -> %s)"), name.c_str(),
		              who < PARTS + 2 ? part_name(who).c_str() : "OFF");
	else
		std::snprintf(text, sizeof(text), "%s", name.c_str());
	dl->PushClipRect(ImVec2(x0, y), ImVec2(x1, y + fs * 1.5f), true);
	if (has_type) {
		fx_icon(dl, ImVec2(x0, y), fs, type >> 7, col(ImGuiCol_Text));
		x0 += fs * 1.25f;
	}
	dl->AddText(ImVec2(x0, y), col(ImGuiCol_Text), text);
	dl->PopClipRect();
}

void overview::select_part(int part)
{
	m_part = part;
	set_shape_window_part(part);
}


void overview::release_keys(bridge &br)
{
	for (int part = 0; part < PARTS; part++) {
		if (m_playing[part] < 0)
			continue;
		const u8 off[3] = { u8(0x80 | (m_playing_slot[part] & 15)), u8(m_playing[part]), 64 };
		br.send_port(m_playing_slot[part] / 16, off, 3);
		m_playing[part] = -1;
	}
}


void overview::mute_buttons(int part, float px, float py, float w, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 pos(px, py);
	const float bw = fs * 1.25f, bh = (h - fs * 0.3f) * 0.5f;
	const float x = pos.x + w - bw - fs * 0.15f;
	struct { const char *id, *mark; bool *on; ImU32 lit; float y; const char *tip; } b[] = {
		{ "##mute", "M", &m_mute[part], IM_COL32(230, 80, 60, 255),  pos.y + fs * 0.1f,
		  UI_TEXT(ov_tip_mute, "Mute (silence this part)") },
		{ "##solo", "S", &m_solo[part], IM_COL32(240, 200, 60, 255), pos.y + fs * 0.2f + bh,
		  UI_TEXT(ov_tip_solo, "Solo (play only soloed parts)") },
	};
	for (auto &e : b) {
		ImGui::SetCursorScreenPos(ImVec2(x, e.y));
		if (ImGui::InvisibleButton(e.id, ImVec2(bw, bh)))
			*e.on = !*e.on;
		const bool hot = ImGui::IsItemHovered();
		if (hot)
			ImGui::SetItemTooltip("%s", e.tip);
		dl->AddRectFilled(ImVec2(x, e.y), ImVec2(x + bw, e.y + bh),
		                  *e.on ? e.lit : col(hot ? ImGuiCol_ButtonHovered : ImGuiCol_Button), 3.0f);
		const ImVec2 ts = ImGui::CalcTextSize(e.mark);
		dl->AddText(ImVec2(x + (bw - ts.x) * 0.5f, e.y + (bh - ts.y) * 0.5f),
		            *e.on ? IM_COL32(20, 20, 20, 255) : col(ImGuiCol_Text), e.mark);
	}
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
	ImGui::Dummy(ImVec2(0, 0));
}


// ミュートとソロ。消すパートを音源に伝え、音源がそのパートの声をミックスの手前で 0 にする（mu2000::set_part_mute）。
// 前は XG の受信チャンネルを OFF にしていたが、firmware が MIDI を受けないデモ曲の再生中は効かなかった（イシュー #113）。
// いまは MIDI を通さないので、曲の設定も書き換えない
void overview::apply_mutes(xg::model &m, bridge &br)
{
	(void)m;
	u64 mask = 0;
	for (int p = 0; p < PARTS; p++)
		if (silenced(p))
			mask |= u64(1) << p;
	br.set_part_mute(mask);
}


void overview::hidden(bridge &br)
{
	release_keys(br);
	// ミュートとソロは、この窓で聞き比べるためのもの。閉じたら外す
	for (int p = 0; p < PARTS; p++)
		m_mute[p] = m_solo[p] = false;
	br.set_part_mute(0);
}


// 上の帯の右端の、同時発音数と CPU の負荷。数字の後ろに棒を敷く。
// 演奏中に桁が変わっても文字が動かないよう、数字は桁数ぶんの幅の枠に右寄せで置く
// （0-9 のうち一番広い字の幅 × 桁数。字の幅が違う書体でも位置が揺れない）
void overview::meters(bridge &br)
{
	snapshot s;
	br.read(s);
	const int master = s.voices_master, slave = s.voices_slave, total = master + slave;
	const float cpu = br.cpu();

	float dw = 0.0f;
	for (char c = '0'; c <= '9'; c++) {
		const char d[2] = { c, 0 };
		dw = std::max(dw, ImGui::CalcTextSize(d).x);
	}
	// 部品: 文字そのもの（digits == 0）か、digits 桁の枠に右寄せした数
	struct piece { const char *text; int value; int digits; };
	auto width = [&](std::initializer_list<piece> ps) {
		float w = 0.0f;
		for (const piece &q : ps)
			w += q.digits ? dw * float(q.digits) : ImGui::CalcTextSize(q.text).x;
		return w;
	};
	ImDrawList *dl = ImGui::GetWindowDrawList();
	auto put = [&](float x, float y, std::initializer_list<piece> ps) {
		const ImU32 ink = col(ImGuiCol_Text);
		for (const piece &q : ps) {
			if (!q.digits) {
				dl->AddText(ImVec2(x, y), ink, q.text);
				x += ImGui::CalcTextSize(q.text).x;
				continue;
			}
			char n[16];
			std::snprintf(n, sizeof(n), "%d", q.value);
			const float slot = dw * float(q.digits);
			dl->AddText(ImVec2(x + slot - ImGui::CalcTextSize(n).x, y), ink, n);
			x += slot;
		}
	};

	const std::initializer_list<piece> voices = {
		{ UI_TEXT(ov_voices_prefix, "Voices "), 0, 0 }, { nullptr, total, 3 }, { "/128  (M:", 0, 0 }, { nullptr, master, 2 },
		{ ", S:", 0, 0 }, { nullptr, slave, 2 }, { ")", 0, 0 },
	};
	const int cpu_pct = cpu >= 0.0f ? int(std::lround(cpu)) : 0;
	const std::initializer_list<piece> load = { { "CPU ", 0, 0 }, { nullptr, cpu_pct, 3 }, { "%", 0, 0 } };

	const float fs = ImGui::GetFontSize();
	const float pad = fs * 0.5f, gap = fs * 0.8f;
	const float vw = width(voices) + pad * 2.0f;
	const float cw = cpu >= 0.0f ? width(load) + pad * 2.0f : 0.0f;
	const float all = vw + (cpu >= 0.0f ? gap + cw : 0.0f);
	const float h = ImGui::GetFrameHeight();

	// **どちらの口で鳴らしているか**（F4 で切り替わる）。
	// 聞き比べのとき、いまどちらを聞いているのか分からないと困る
	const int eng = br.engine();
	char eng_text[32];
	std::snprintf(eng_text, sizeof(eng_text), UI_TEXT(ov_engine_fmt, "Engine: %s"), eng == 1 ? "native" : "firmware");
	const float ew = eng >= 0 ? ImGui::CalcTextSize(eng_text).x + fs : 0.0f;
	ImGui::SameLine(std::max(ImGui::GetCursorPosX() + fs,
	                         ImGui::GetWindowContentRegionMax().x - all
	                         - (eng >= 0 ? ew + gap : 0.0f)));
	if (eng >= 0) {
		const ImVec2 e0 = ImGui::GetCursorScreenPos(), e1(e0.x + ew, e0.y + h);
		dl->AddRectFilled(e0, e1, eng == 1 ? IM_COL32(150, 90, 30, 200)
		                                   : IM_COL32(50, 90, 60, 200), fs * 0.25f);
		dl->AddText(ImVec2(e0.x + fs * 0.5f, e0.y + (h - fs) * 0.5f),
		            col(ImGuiCol_Text), eng_text);
		ImGui::InvisibleButton("##engine", ImVec2(ew, h));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(ov_engine_tip, "Sounding engine (F4 switches)\n"
			                                         "firmware: the real firmware plays (effects as on hardware)\n"
			                                         "native: SH-2 stopped, registers built from formulas"));
		ImGui::SameLine(0.0f, gap);
	}
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float ty = pos.y + (h - fs) * 0.5f;
	// 枠は角を丸める。中の棒は、枠の端に着いている側だけ枠に合わせて丸め、伸びる先の端は四角いまま
	const float round = fs * 0.25f;
	auto bar = [&](ImVec2 a, ImVec2 b, ImU32 c, bool at_left, bool at_right) {
		const ImDrawFlags corners = (at_left ? ImDrawFlags_RoundCornersLeft : 0) | (at_right ? ImDrawFlags_RoundCornersRight : 0);
		dl->AddRectFilled(a, b, c, corners ? round : 0.0f, corners ? corners : ImDrawFlags_RoundCornersNone);
	};

	// 発音数の棒。マスタの分とスレーブの分を色を分けて積む（全体が 128）
	{
		const ImVec2 p0 = pos, p1(pos.x + vw, pos.y + h);
		dl->AddRectFilled(p0, p1, col(ImGuiCol_FrameBg), round);
		const float xm = p0.x + vw * float(master) / 128.0f;
		const float xs = xm + vw * float(slave) / 128.0f;
		if (master)
			bar(p0, ImVec2(xm, p1.y), IM_COL32(66, 120, 200, 200), true, total >= 128 && !slave);
		if (slave)
			bar(ImVec2(xm, p0.y), ImVec2(std::min(xs, p1.x), p1.y), IM_COL32(210, 130, 50, 200), !master, total >= 128);
		put(p0.x + pad, ty, voices);
		ImGui::InvisibleButton("##voices", ImVec2(vw, h));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(ov_voices_tip, "Voices: sounding voices, including releases. Some voices use two or more voices per note\n"
			                                         "M is the SWP30 master (up to 64, blue), S the slave (up to 64, orange). Overflow goes to the slave"));
	}

	// CPU の棒。0-100%。重くなるほど黄、赤にする
	if (cpu >= 0.0f) {
		ImGui::SameLine(0.0f, gap);
		const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + cw, p0.y + h);
		dl->AddRectFilled(p0, p1, col(ImGuiCol_FrameBg), round);
		const float f = std::clamp(cpu / 100.0f, 0.0f, 1.0f);
		const ImU32 fill = cpu < 60.0f ? IM_COL32(60, 150, 90, 200) : cpu < 85.0f ? IM_COL32(190, 160, 40, 210)
		                                                                        : IM_COL32(210, 60, 50, 220);
		if (f > 0.0f)
			bar(p0, ImVec2(p0.x + cw * f, p1.y), fill, true, f >= 1.0f);
		put(p0.x + pad, ty, load);
		ImGui::InvisibleButton("##cpu", ImVec2(cw, h));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(ov_cpu_tip, "CPU: audio processing time vs deadline (over 100%% breaks up)"));
	}
}


void overview::draw(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	m_wheel_taken = false;
	m_model = &m;
	apply_mutes(m, br);
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("overview", nullptr, wf);
	ImGui::PopStyleVar();

	// 表示の大きさ。32 パートを見渡すための窓なので、既定は小さめ（文字 10px）。
	// 棒や絵も文字の大きさから決まるので、全部が一緒に縮む
	float &zoom = overview_zoom();
	help_checkbox();
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	if (ImGui::SmallButton("-"))
		set_overview_zoom(zoom - 0.125f);
	ImGui::SameLine();
	ImGui::Text("%d%%", int(std::lround(zoom * 100)));
	ImGui::SameLine();
	if (ImGui::SmallButton("+"))
		set_overview_zoom(zoom + 0.125f);
	ImGui::SameLine();
	ImGui::TextDisabled("%s", UI_TEXT(ov_zoom_label, "Display size (double-click a small picture to enlarge)"));

	// 同時発音数と CPU の負荷は右端へ。発音数は SWP30 2 個の声のスロット（64 ずつ、合わせて 128）のうち鳴っているもの。
	// firmware はマスタの 64 から使い、埋まるとスレーブに回す（112 音を重ねるとマスタ 64 + スレーブ 48 になった）。
	// CPU は gui が音声を回しているときだけ出す（プラグインではホストの持ち物なので出さない）
	meters(br);

	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);
	const float fs = ImGui::GetFontSize();
	const float h = fs * 2.3f;

	// マスターの表（見出しは別）。インサーションとバリエーションの設定もここ
	br.want_part_scopes();            // 一覧のスペクトラム。見えているあいだだけ音源が溜める
	master_pane(m, ram, br);
	ImGui::Spacing();

	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
	                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1, 1));
	if (ImGui::BeginTable("rows", NCOLS + 4, flags)) {
		ImGui::TableSetupScrollFreeze(1, 1);            // 見出しは流さない
		ImGui::TableSetupColumn(UI_TEXT(ov_part_col, "Part (right-click for voice)"), ImGuiTableColumnFlags_WidthFixed, fs * 18.5f);
		ImGui::TableSetupColumn("VEL", ImGuiTableColumnFlags_WidthFixed, fs * 2.2f);
		ImGui::TableSetupColumn("SPEC", ImGuiTableColumnFlags_WidthFixed, fs * 6.0f);
		for (const column &c : COLUMNS)
			ImGui::TableSetupColumn(c.title, ImGuiTableColumnFlags_WidthFixed,
			                        wide(c.from) ? fs * 3.6f : c.from == src::ins ? fs * 6.2f : fs * 3.4f);
		ImGui::TableSetupColumn("##keys", ImGuiTableColumnFlags_WidthStretch);   // 見出しは要らない
		// Help keys, parallel to the displays above (the part display is
		// translated; the rest double as their own keys).
		const char *part_keys[NCOLS + 4];
		part_keys[0] = "パート（右クリックで音色）";
		part_keys[1] = "VEL";
		part_keys[2] = "SPEC";
		for (int i = 0; i < NCOLS; i++) part_keys[3 + i] = COLUMNS[i].title;
		part_keys[NCOLS + 3] = "##keys";
		headers_with_help(NCOLS + 4, part_keys);

		for (int part = 0; part < PARTS; part++) {
			ImGui::TableNextRow(0, h);
			row(part, m, ram, br, h);
		}
		// 行のどこを左クリックしても、その行を選ぶ（パートの音色の窓もそのパートに替わる）。
		// 載っている行は前のコマのもの（0 は見出し）。品書きなどが上に出ているときは窓が載っていない扱い
		const int hovered_row = ImGui::TableGetHoveredRow();
		if (hovered_row >= 1 && hovered_row <= PARTS && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
		    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			select_part(hovered_row - 1);
		ImGui::EndTable();
	}
	ImGui::PopStyleVar();
	ImGui::PopFont();
	ImGui::End();

	if (ImGui::GetIO().MouseWheel != 0.0f && !m_wheel_taken)
		m_scrolled_at = ImGui::GetTime();
}

} // namespace ui

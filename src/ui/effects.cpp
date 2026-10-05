// license:BSD-3-Clause
//
// エフェクトの面。リバーブ／コーラス／バリエーション（XG のシステム側）と、
// MU2000 が持つインサーション 2 系統。
//
// ここも **音源に手を入れず SysEx を送るだけ**。番地は資料から拾ったのではなく、
// 送って音を測って確かめた。確かめ方は doc/effects.md に書いてある。
//
// 値は画面では覚えない。パラメータの層（xg::model）が音源に問い合わせた返事を読む。
// 前は「電源投入直後に近いはず」の値を決め打ちしていたが、firmware に聞くと
// インサーションの種別は DISTORTION、コーラスは CHORUS 1、バリエーションの接続は
// INSERTION で、決め打ちとは違っていた。
//
//   02 01 00  リバーブ種別（MSB, LSB）
//   02 01 0C  リバーブ リターン
//   02 01 20  コーラス種別
//   02 01 2C  コーラス リターン
//   02 01 40  バリエーション種別
//   02 01 5A  バリエーション接続（0 インサーション / 1 システム）
//   02 01 5B  バリエーションを掛けるパート
//   03 00 00  インサーション 1 種別      03 00 0C  そのパート
//   03 01 00  インサーション 2 種別      03 01 0C  そのパート

#include "panel.h"
#include "draw_imgui.h"
#include "texts.h"
#include "xg/fx_types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

using xg::fx_type;

// 面の欄とパラメータの層の名前
const char *fx_key(int ctl)
{
	switch (ctl) {
	case CTL_REV_TYPE:  return "reverb.type";
	case CTL_REV_RET:   return "reverb.return";
	case CTL_CHO_TYPE:  return "chorus.type";
	case CTL_CHO_RET:   return "chorus.return";
	case CTL_VAR_TYPE:  return "variation.type";
	case CTL_VAR_CONN:  return "variation.connect";
	case CTL_VAR_PART:  return "variation.part";
	case CTL_INS1_TYPE: return "insertion1.type";
	case CTL_INS1_PART: return "insertion1.part";
	case CTL_INS2_TYPE: return "insertion2.type";
	case CTL_INS2_PART: return "insertion2.part";
	default:            return nullptr;
	}
}

const xg::param *fx_param(int ctl)
{
	const char *k = fx_key(ctl);
	return k ? xg::find(k) : nullptr;
}

// 種別の値（MSB × 128 + LSB）が表の何番目か。表に無ければ -1
int type_index(const fx_type *t, int n, int value)
{
	for (int i = 0; i < n; i++)
		if ((t[i].msb << 7 | t[i].lsb) == value)
			return i;
	return -1;
}

const fx_type *type_table(int ctl, int &n)
{
	switch (ctl) {
	case CTL_REV_TYPE: n = int(xg::rev_types().size()); return xg::rev_types().data();
	case CTL_CHO_TYPE: n = int(xg::cho_types().size()); return xg::cho_types().data();
	case CTL_VAR_TYPE:
	case CTL_INS1_TYPE:
	case CTL_INS2_TYPE: n = int(xg::ins_types().size()); return xg::ins_types().data();
	default: n = 0; return nullptr;
	}
}

// 面の並び。1 行が 1 つのブロック
struct fx_row {
	const char *title;
	double y;
	int ctl[3];
	const char *label[3];
	double w[3];
};
const fx_row ROWS[] = {
	{ "REVERB",      44,  { CTL_REV_TYPE,  CTL_REV_RET,   CTL_NONE },
	  { "TYPE", "RETURN", "" }, { 250, 120, 0 } },
	{ "CHORUS",     110,  { CTL_CHO_TYPE,  CTL_CHO_RET,   CTL_NONE },
	  { "TYPE", "RETURN", "" }, { 250, 120, 0 } },
	{ "VARIATION",  176,  { CTL_VAR_TYPE,  CTL_VAR_CONN,  CTL_VAR_PART },
	  { "TYPE", "CONNECT", "PART" }, { 250, 120, 120 } },
	{ "INSERTION 1", 242, { CTL_INS1_TYPE, CTL_INS1_PART, CTL_NONE },
	  { "TYPE", "PART", "" }, { 250, 120, 0 } },
	{ "INSERTION 2", 300, { CTL_INS2_TYPE, CTL_INS2_PART, CTL_NONE },
	  { "TYPE", "PART", "" }, { 250, 120, 0 } },
};

constexpr double COL_X = 150;   // 名札の右端

} // namespace


void panel::fx_bounds(int ctl, bool &at_min, bool &at_max) const
{
	at_min = at_max = true;
	const xg::param *p = fx_param(ctl);
	int v = 0;
	if (!p || !m_xg.get(*p, 0, v))
		return;
	int n = 0;
	if (const fx_type *t = type_table(ctl, n)) {
		const int i = type_index(t, n, v);
		at_min = i == 0;
		at_max = i == n - 1;
		return;
	}
	if (p->special >= 0) {                     // パート。0-63 の次が OFF
		at_min = v == p->min;
		at_max = v == p->special;
		return;
	}
	at_min = v <= p->min;
	at_max = v >= p->max;
}

std::string panel::fx_text(int ctl) const
{
	const xg::param *p = fx_param(ctl);
	int v = 0;
	if (!p || !m_xg.get(*p, 0, v))
		return "--";
	int n = 0;
	if (const fx_type *t = type_table(ctl, n)) {
		const int i = type_index(t, n, v);
		if (i >= 0)
			return t[i].name;
		// 面の表に無い種別（パネルや曲が選んだもの）。番号で出す
		char buf[32];
		std::snprintf(buf, sizeof(buf), "TYPE %02X-%02X", v >> 7, v & 0x7f);
		return buf;
	}
	return xg::format(*p, v);
}

// 1 つ隣の値へ。パラメータチェンジを 1 つ送る
void panel::step_fx(int ctl, int step, bridge &br)
{
	const xg::param *p = fx_param(ctl);
	int v = 0;
	if (!p || !step || !m_xg.get(*p, 0, v))
		return;                                // 読めていないうちは動かさない

	int next = v;
	int n = 0;
	if (const fx_type *t = type_table(ctl, n)) {
		int i = type_index(t, n, v);
		if (i < 0) {
			// 表に無い種別からは、値の並びで隣にあるものへ
			i = step > 0 ? n : -1;
			for (int k = 0; k < n; k++) {
				const int tv = t[k].msb << 7 | t[k].lsb;
				if (step > 0 && tv > v) { i = k - 1; break; }
				if (step < 0 && tv < v) i = k + 1;
			}
		}
		i = std::clamp(i + step, 0, n - 1);
		next = t[i].msb << 7 | t[i].lsb;
	} else if (p->special >= 0) {
		// パート 1-32 の次が OFF
		int i = (v == p->special) ? p->max + 1 : v;
		i = std::clamp(i + step, p->min, p->max + 1);
		next = (i > p->max) ? p->special : i;
	} else {
		next = std::clamp(v + step, p->min, p->max);
	}
	if (next != v)
		br.send(m_xg.set(*p, 0, next));
}

// The effects list through an ImDrawList.
void panel::draw_list(ImDrawList *dl, const spot &sp) const
{
	im::round_box(dl, sp.r, RGB(40, 43, 48), RGB(88, 93, 100), float(4 * m_scale));

	const int edge = (sp.r.right - sp.r.left) / 6;
	RECT inner = sp.r;
	inner.left  += edge;
	inner.right -= edge;
	const std::string label = fx_text(sp.ctl);
	im::text_in(dl, im::pos_of(inner), im::size_of(inner), label.c_str(),
	            label == "--" ? TEXT_DIM : TEXT, m_fonts.small, m_fonts.small_px, true, true, false);

	RECT l = sp.r, r = sp.r;
	l.right = l.left + edge;
	r.left  = r.right - edge;
	bool at_min, at_max;
	fx_bounds(sp.ctl, at_min, at_max);
	im::text_in(dl, im::pos_of(l), im::size_of(l), "<", at_min ? RGB(80, 84, 90) : ACCENT,
	            m_fonts.small, m_fonts.small_px, true, true, false);
	im::text_in(dl, im::pos_of(r), im::size_of(r), ">", at_max ? RGB(80, 84, 90) : ACCENT,
	            m_fonts.small, m_fonts.small_px, true, true, false);
}


void panel::build_effect_spots()
{
	for (const fx_row &row : ROWS) {
		double x = COL_X;
		for (int i = 0; i < 3; i++) {
			if (row.ctl[i] == CTL_NONE)
				continue;
			m_spots.push_back({ spot_kind::list, mu2000::button::count, row.ctl[i],
			                    scale(x, row.y, row.w[i], 24), row.label[i], "" });
			x += row.w[i] + 24;
		}
	}
}

// effects page through ui/draw_imgui.h.
void panel::paint_effects(ImDrawList *dl, const char *status) const
{
	RECT all{ 0, 0, m_w, m_h };
	im::fill(dl, all, BODY);
	RECT top{ 0, 0, m_w, m_oy + int(26 * m_scale) };
	im::fill(dl, top, BODY_TOP);

	im::text_in(dl, im::pos_of(scale(20, 6, 460, 16)), im::size_of(scale(20, 6, 460, 16)),
	            UI_TEXT(effects_title, "Effects (sending XG parameter changes)"), TEXT_DIM,
	            m_fonts.small, m_fonts.small_px, false, true, false);

	for (const fx_row &row : ROWS) {
		im::text_in(dl, im::pos_of(scale(20, row.y + 4, 120, 16)),
		            im::size_of(scale(20, row.y + 4, 120, 16)),
		            row.title, TEXT, m_fonts.small, m_fonts.small_px);
		double x = COL_X;
		for (int i = 0; i < 3; i++) {
			if (row.ctl[i] == CTL_NONE)
				continue;
			im::text_in(dl, im::pos_of(scale(x, row.y - 13, row.w[i], 12)),
			            im::size_of(scale(x, row.y - 13, row.w[i], 12)),
			            row.label[i], TEXT_DIM, m_fonts.small, m_fonts.small_px);
			x += row.w[i] + 24;
		}
	}

	for (const spot &sp : m_spots) {
		if (sp.kind == spot_kind::list)
			draw_list(dl, sp);
		else if (sp.kind == spot_kind::action) {
			im::round_box(dl, sp.r, BTN_FACE, BTN_EDGE, float(5 * m_scale));
			im::text_in(dl, im::pos_of(sp.r), im::size_of(sp.r), sp.label, TEXT,
			            m_fonts.small, m_fonts.small_px, true, true, false);
		}
	}

	im::text_in(dl, im::pos_of(scale(150, 356, 830, 20)), im::size_of(scale(150, 356, 830, 20)),
	            UI_TEXT(effects_values_note, "Values are read back from the MU2000. Changes from the panel or songs appear here too."),
	            RGB(104, 109, 116), m_fonts.small, m_fonts.small_px);
	im::text_in(dl, im::pos_of(scale(150, 324, 830, 34)), im::size_of(scale(150, 324, 830, 34)),
	            UI_TEXT(effects_insertion_note, "Insertion works on the selected part. Variation can be used as insertion\n"
	                                            "by setting CONNECT to INSERTION."),
	            RGB(104, 109, 116), m_fonts.small, m_fonts.small_px, false, false, true);

	if (status && status[0])
		im::text_in(dl, im::pos_of(m_status), im::size_of(m_status), status,
		            TEXT_DIM, m_fonts.small, m_fonts.small_px, false, true, false);

	draw_tabs(dl);
}

} // namespace ui

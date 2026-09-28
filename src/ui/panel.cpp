// license:BSD-3-Clause
//
// パネルの面。**実機の写真から採寸して並べ直した**。
//
// 論理座標の 1000 × 385 が本体の前面ぜんたい（実機の縦横比はおよそ 2.6:1）。
// 残りの 15 は面を切り替える帯で、本体の外。
//
//   左   A/D INPUT のジャックとつまみ、VOLUME、電源、MIDI IN A、PHONES、カード
//   中   LCD、その下に PART / BANK・PGM# / VOL / EXP / PAN / REV / CHO / VAR / KEY
//        の見出しと、音色カテゴリのボタン 18 個
//   右   PLAY EDIT / UTIL EFFECT / SAMPLING SEQ の 6 個（LED 入り）、
//        MUTE PART−+ / ENTER SELECT−+ / EXIT VALUE−+ の 9 個、
//        SELECT と AUDITION、そして**大きなダイヤル**

#include "panel.h"
#include "draw.h"
#include "texts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace ui {

namespace {

constexpr double PI = 3.14159265358979;

// **位置と大きさは ui::layout（src/ui/layout.*）が持っている**。
// panel.txt があればそちらで上書きされる。ここに残してあるのは
// 「どのボタンか」「札に何と書くか」だけ

struct place { mu2000::button b; const char *label; const char *sub; };

const mu2000::button CAT_B[18] = {
	mu2000::button::piano,      mu2000::button::chrom_perc, mu2000::button::organ,
	mu2000::button::guitar,     mu2000::button::bass,       mu2000::button::strings,
	mu2000::button::ensemble,   mu2000::button::brass,      mu2000::button::reed,
	mu2000::button::pipe,       mu2000::button::synth_lead, mu2000::button::synth_pad,
	mu2000::button::synth_effects, mu2000::button::ethnic,  mu2000::button::percussive,
	mu2000::button::sfx,        mu2000::button::model_excl, mu2000::button::drum,
};
const char *CAT_LABEL[18] = {
	"Piano", "Chrom. perc.", "Organ", "Guitar", "Bass", "Strings",
	"Ensemble", "Brass", "Reed", "Pipe", "Synth lead", "Synth pad",
	"Synth effects", "Ethnic", "Percussive", "SFX", "Model excl.", "Drum",
};

// LCD の下段に並ぶもの。窓の内側の左端からの割合で置く。
// 窓の下に印刷されている札も、ここから位置を取って揃える
// 位置と幅は、上の面の**点 1 つぶん**を単位にした、窓の内側の左端からの数
// （上の面は 17 桁 × 6 点 − 1 = 101 点）。
//
// 下の面の位置と幅は実機の写真から採寸した（layout.cpp の low.x / low.w）。
// 「01」「A01」の 5 桁は字間が左から 1・2・1・1 点

// n 番のバーの左端。A1 が 0、A2 が 1、パート 1 が 2 …（点の単位）
constexpr int bar_x(int i) { return (i / 2) * (CELL_W + 1) + ((i & 1) ? 3 : 0); }
constexpr int part_x(int n) { return bar_x(n + 1); }

// 実機の窓は、上の面の左に 2.7 点、右に 5.4 点ぶんの余白がある。
// 右の余白にモードの ▶ が入る（写真から採寸。単位は上の面の点の間隔）
constexpr double LCD_LEFT = 2.7, LCD_RIGHT = 5.4;
constexpr double LCD_SPAN = LCD_LEFT + (TOP_COLS * (CELL_W + 1) - 1) + LCD_RIGHT;

// 下の面の点（「01」「A01」）は上の面の点より少し小さい。間隔は上の面の 0.92 倍。
// 下の面のセグメントの高さも、この下の面の点の間隔で測ってある
constexpr double LOW_DOT = 0.92;

// 上の面と下の面のあいだの目盛りの帯の高さ（点の単位）と、その中の並び
// （帯の上端を 0、下端を 1 とした割合）。写真から採寸した
constexpr double BAND = 7.4;
constexpr double BAND_NUM[2]  = { 0.07, 0.32 };  // パート番号 A1 A2 1-32（上は目盛りの線）
constexpr double BAND_MIC[2]  = { 0.36, 0.625 }; // MIC の箱。BANK / PGM# も同じ行
constexpr double BAND_LINE[2] = { 0.655, 0.92 }; // LINE の箱

// 点と点の隙間。実機は点の間隔の 1 割ほどしかない
constexpr double DOT_GAP = 0.10;

// 右端の ▶ の高さ（下の面の上端から、点の間隔の単位）。
// いちばん上は札のない ▶、残りが XG / GS / PERFORM
constexpr double MODE_Y[4] = { -2.2, 0.6, 3.4, 6.2 };
const char *const MODE_LABEL[3] = { "XG", "GS", "PERFORM" };

// 23 桁目の制御ビット。列 A-D は bit3-bit0、行は上の桁の 0-7 と
// 下の桁の 0-7 をつないだ 0-15。番地は実測（doc/gui.md）
enum { CA = 0, CB = 1, CC = 2, CD = 3 };
bool lcd_ctl(const snapshot &s, int col, int row)
{
	if (!s.lcd_on)
		return false;
	const u8 v = s.dots[((row / 8) * LCD_COLS + TOP_COLS + 6) * CELL_H + (row % 8)];
	return BIT(v, 3 - col) != 0;
}

// キートップの記号の絵。黒い丸に白で − ＋ ◀ ▶ を抜く。GDI の円や多角形は
// 縁がぼけずにギザギザになるので、画素ごとに形までの距離を測り、縁の 1 画素を
// 半透明にした絵を作って貼る。kind: 0 − 1 ＋ 2 ◀ 3 ▶
std::vector<uint32_t> key_symbol(int kind, double R, int &S)
{
	S = int(std::ceil(2 * R + 2));
	const double c = S / 2.0;
	const double A = 0.6 * R, T = 0.14 * R;
	auto box = [](double x, double y, double hx, double hy) {
		const double qx = std::fabs(x) - hx, qy = std::fabs(y) - hy;
		const double out = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
		return out + std::min(std::max(qx, qy), 0.0);
	};
	auto tri = [&](double x, double y) {
		if (kind == 2)
			x = -x;                                     // ◀ は ▶ を裏返したもの
		const double p[3][2] = { { A, 0 }, { -A / 2, -A }, { -A / 2, A } };
		double d = 1e9;
		int pos = 0, neg = 0;                               // 3 辺のどちら側か
		for (int i = 0; i < 3; i++) {
			const double *a = p[i], *b = p[(i + 1) % 3];
			const double ex = b[0] - a[0], ey = b[1] - a[1];
			const double wx = x - a[0], wy = y - a[1];
			const double t = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0, 1.0);
			d = std::min(d, std::hypot(wx - ex * t, wy - ey * t));
			(ex * wy - ey * wx < 0 ? neg : pos)++;
		}
		return (pos == 3 || neg == 3) ? -d : d;
	};
	const uint32_t ink[3] = { 62, 60, 54 }, white[3] = { 236, 234, 226 };
	std::vector<uint32_t> px(size_t(S) * S);
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			const double dx = x + 0.5 - c, dy = y + 0.5 - c;
			const double ac = std::clamp(0.5 - (std::hypot(dx, dy) - R), 0.0, 1.0);
			double ds;
			if (kind == 0)      ds = box(dx, dy, A, T);
			else if (kind == 1) ds = std::min(box(dx, dy, A, T), box(dx, dy, T, A));
			else                ds = tri(dx, dy);
			const double as = std::clamp(0.5 - ds, 0.0, 1.0);
			uint32_t v = uint32_t(std::lround(ac * 255)) << 24;
			for (int k = 0; k < 3; k++)
				v |= uint32_t(std::lround(ink[k] + (double(white[k]) - ink[k]) * as)) << (16 - 8 * k);
			px[size_t(y) * S + x] = v;
		}
	return px;
}

// 多角形を縁をぼかして、α つきの絵（ox, oy から w × h、α をかけた ARGB）に
// 重ねる（偶奇の規則）。GDI の Polygon は縁が段々になるので、1 画素を縦 4 段に
// 分け、段ごとに辺との交点を求めて、横は正確な重なりの長さで覆い具合を足す
void fill_aa(std::vector<uint32_t> &img, int ox, int oy, int w, int h,
             const std::vector<double> &xy, COLORREF ink)
{
	const size_t n = xy.size() / 2;
	if (n < 3)
		return;
	double minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
	for (size_t i = 1; i < n; i++) {
		minx = std::min(minx, xy[2 * i]);     maxx = std::max(maxx, xy[2 * i]);
		miny = std::min(miny, xy[2 * i + 1]); maxy = std::max(maxy, xy[2 * i + 1]);
	}
	const int x0 = std::max(ox, int(std::floor(minx))), y0 = std::max(oy, int(std::floor(miny)));
	const int x1 = std::min(ox + w, int(std::ceil(maxx))), y1 = std::min(oy + h, int(std::ceil(maxy)));
	const int sw = x1 - x0, sh = y1 - y0;
	if (sw <= 0 || sh <= 0)
		return;
	constexpr int SS = 4;
	std::vector<float> cov(size_t(sw) * sh, 0.0f);
	std::vector<double> xs;
	for (int r = 0; r < sh * SS; r++) {
		const double ys = y0 + (r + 0.5) / SS;
		xs.clear();
		for (size_t i = 0; i < n; i++) {
			const double ax = xy[2 * i], ay = xy[2 * i + 1];
			const double bx = xy[2 * ((i + 1) % n)], by = xy[2 * ((i + 1) % n) + 1];
			if ((ay <= ys && by > ys) || (by <= ys && ay > ys))
				xs.push_back(ax + (ys - ay) * (bx - ax) / (by - ay));
		}
		std::sort(xs.begin(), xs.end());
		float *row = &cov[size_t(r / SS) * sw];
		for (size_t i = 0; i + 1 < xs.size(); i += 2) {
			const double a = xs[i] - x0, b = xs[i + 1] - x0;
			for (int c = std::max(0, int(std::floor(a))); c < std::min(sw, int(std::ceil(b))); c++) {
				const double o = std::min(b, c + 1.0) - std::max(a, double(c));
				if (o > 0)
					row[c] += float(o / SS);
			}
		}
	}
	// 上に重ねる（α をかけた色どうしの「上に置く」）
	const double R = GetRValue(ink), G = GetGValue(ink), B = GetBValue(ink);
	for (int y = 0; y < sh; y++)
		for (int x = 0; x < sw; x++) {
			const double a = std::min(1.0f, cov[size_t(y) * sw + x]);
			if (a <= 0)
				continue;
			uint32_t &d = img[size_t(y0 - oy + y) * w + (x0 - ox + x)];
			auto ch = [&](int sh2, double c) {
				return uint32_t(std::lround(c * a + ((d >> sh2) & 0xff) * (1 - a))) << sh2;
			};
			d = ch(24, 255) | ch(16, R) | ch(8, G) | ch(0, B);
		}
}

COLORREF mix(COLORREF a, COLORREF b, double t)
{
	auto ch = [&](int x, int y) { return int(std::lround(x + (y - x) * t)); };
	return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)),
	           ch(GetBValue(a), GetBValue(b)));
}

// UTIL > SYS の Contrast（1-8）で変わる LCD の色。**値が小さいほど濃い**。
// 工場出荷の 2 がいままでの色。1 で写真（コントラストを上げて撮ったもの）の
// 濃さ（消えている点が背景と点いた点のあいだの 35% ほど）になる。
// 3 から上は薄れていき、8 では消えている点がほぼ見えず、点いた点も半分ほど
struct lcd_ink { COLORREF dot, ghost, faint; };
lcd_ink lcd_palette(int c)
{
	const COLORREF faint2 = RGB(147, 202, 45);           // 絵の区画の消え点
	if (c == 2)
		return { LCD_DOT, LCD_GHOST, faint2 };
	c = std::clamp(c, 1, 8);
	const double ghost_a = c == 1 ? 0.35 : 0.06 * (8 - c) / 6.0;
	const double dot_a   = c == 1 ? 1.0 : 1.0 - 0.08 * (c - 2);
	return { mix(LCD_BACK, LCD_DOT, dot_a),
	         mix(LCD_BACK, LCD_DOT, ghost_a),
	         mix(LCD_BACK, LCD_DOT, ghost_a * 0.3) };
}


// 窓の下に印刷されている札。どの並びの真ん中に置くか
struct column { int at; const char *label; };
const column COLUMNS[] = {
	{ LOW_PART, "PART" }, { LOW_ICON, "BANK/PGM#" }, { LOW_VOL, "VOL" },
	{ LOW_EXP,  "EXP"  }, { LOW_PAN,  "PAN" },       { LOW_REV, "REV" },
	{ LOW_CHO,  "CHO"  }, { LOW_VAR,  "VAR" },       { LOW_KEY, "KEY" },
};

// 右上の 6 個。丸い押しボタンで、中に LED が入っている。
// LED の番号は MAME の mulcd.lay の並び（左列 0,2,4 / 右列 1,3,5）
struct mode_button { mu2000::button b; int led; const char *label; };
const mode_button MODES[6] = {
	{ mu2000::button::play,          0, "PLAY"     },
	{ mu2000::button::edit,          1, "EDIT"     },
	{ mu2000::button::util,          2, "UTIL"     },
	{ mu2000::button::effect,        3, "EFFECT"   },
	{ mu2000::button::sampling_mode, 4, "SAMPLING" },
	{ mu2000::button::seq,           5, "SEQ"      },
};

// 右端の 9 個
const place NAV[9] = {
	{ mu2000::button::mute_solo,    "MUTE",   "SOLO" },
	{ mu2000::button::part_minus,   "PART",   "-" },
	{ mu2000::button::part_plus,    "PART",   "+" },
	{ mu2000::button::enter,        "ENTER",  "" },
	{ mu2000::button::select_left,  "SELECT", "-" },
	{ mu2000::button::select_right, "SELECT", "+" },
	{ mu2000::button::exit,         "EXIT",   "" },
	{ mu2000::button::value_minus,  "VALUE",  "-" },
	{ mu2000::button::value_plus,   "VALUE",  "+" },
};

// 音色カテゴリの右にある小さな丸ボタン 2 つ
const place ROUND[2] = {
	{ mu2000::button::select,   "SELECT",   "" },
	{ mu2000::button::audition, "AUDITION", "" },
};

// 実機の色
const COLORREF PANEL_FACE = RGB(196, 189, 170);
const COLORREF PANEL_INK  = RGB(46, 44, 40);
const COLORREF KEY_FACE   = RGB(216, 205, 165);
const COLORREF KEY_EDGE   = RGB(126, 118, 92);
const COLORREF KEY_DOWN   = RGB(150, 140, 95);

} // namespace


panel::panel()
{
	resize(LOGICAL_W, LOGICAL_H);
}

panel::~panel()
{
	if (m_font_label) DeleteObject(m_font_label);
	if (m_font_small) DeleteObject(m_font_small);
	if (m_font_tiny)  DeleteObject(m_font_tiny);
	if (m_font_tag)   DeleteObject(m_font_tag);
	if (m_font_num)   DeleteObject(m_font_num);
	if (m_font_key)   DeleteObject(m_font_key);
}

RECT panel::scale(double x, double y, double w, double h) const
{
	RECT r;
	r.left   = m_ox + int(std::lround(x * m_scale));
	r.top    = m_oy + int(std::lround(y * m_scale));
	r.right  = m_ox + int(std::lround((x + w) * m_scale));
	r.bottom = m_oy + int(std::lround((y + h) * m_scale));
	return r;
}

// パネルに描いてある MIDI IN A のジャック（丸と札）を囲む枠
RECT panel::midi_jack() const
{
	return scale(92, 230, 96, 90);
}

bool panel::on_midi_jack(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = midi_jack();
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_card_slot(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = scale(m_lay.card[0], m_lay.card[1], m_lay.card[2], m_lay.card[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_ad_input(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = scale(m_lay.adin[0], m_lay.adin[1], m_lay.adin[2], m_lay.adin[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_phones(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = scale(m_lay.phones[0], m_lay.phones[1], m_lay.phones[2], m_lay.phones[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// 論理座標の点を実座標へ
POINT panel::at(double x, double y) const
{
	POINT p;
	p.x = m_ox + int(std::lround(x * m_scale));
	p.y = m_oy + int(std::lround(y * m_scale));
	return p;
}

void panel::resize(int w, int h)
{
	m_w = std::max(w, 200);
	m_h = std::max(h, 60);
	// **帯のぶんを差し引いてから合わせる**。当たりも描きも
	// `scale()` / `at()` を通るので、ここだけ直せば全部ついてくる
	const int body_h = std::max(m_h - m_top_inset, 60);

	if (m_lcd_only) {
		m_scale = std::min(double(m_w) / m_lay.lcd[2], double(body_h) / m_lay.lcd[3]);
		m_ox = int(-m_lay.lcd[0] * m_scale);
		m_oy = m_top_inset - int(m_lay.lcd[1] * m_scale);
	} else {
		m_scale = std::min(double(m_w) / LOGICAL_W, double(body_h) / LOGICAL_H);
		m_ox = int((m_w - LOGICAL_W * m_scale) / 2);
		m_oy = m_top_inset + int((body_h - LOGICAL_H * m_scale) / 2);
	}

	m_lcd    = scale(m_lay.lcd[0], m_lay.lcd[1], m_lay.lcd[2], m_lay.lcd[3]);
	if (m_lcd_only)
		m_lcd = RECT{ 0, m_top_inset, m_w, m_h };
	m_volume = scale(m_lay.volume[0] - m_lay.volume[2], m_lay.volume[1] - m_lay.volume[2],
	                 m_lay.volume[2] * 2, m_lay.volume[2] * 2);   // 当たりは丸で見る
	m_adgain = scale(m_lay.adgain[0] - m_lay.adgain[2], m_lay.adgain[1] - m_lay.adgain[2],
	                 m_lay.adgain[2] * 2, m_lay.adgain[2] * 2);
	m_status = scale(20, 372, 700, 13);
	m_hint   = scale(20, 386, 700, 13);
	m_wheel  = scale(m_lay.dial[0] - m_lay.dial[2], m_lay.dial[1] - m_lay.dial[2],
	                 m_lay.dial[2] * 2, m_lay.dial[2] * 2);
	for (int i = 0; i < 6; i++)
		m_leds[i] = scale(m_lay.mode[i][0] - m_lay.mode_r, m_lay.mode[i][1] - m_lay.mode_r,
		                  m_lay.mode_r * 2, m_lay.mode_r * 2);

	if (m_font_label) DeleteObject(m_font_label);
	if (m_font_small) DeleteObject(m_font_small);
	if (m_font_tiny)  DeleteObject(m_font_tiny);
	auto make_font = [&](double px, int weight, int floor_px = 7) {
		return CreateFontA(-std::max(floor_px, int(px * m_scale)), 0, 0, 0, weight,
		                   FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
		                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
		                   "Segoe UI");
	};
	m_font_label = make_font(13, FW_BOLD);
	m_font_small = make_font(8.5, FW_NORMAL);
	// 目盛りは 34 個の番号をバーの真下に並べるので、思い切り小さくする
	m_font_tiny  = make_font(6.5, FW_NORMAL, 5);

	// キートップの記号
	{
		const double R = std::max(3.0, 4.2 * m_scale);
		for (int k = 0; k < 4; k++) {
			int S = 0;
			const std::vector<uint32_t> px = key_symbol(k, R, S);
			m_key_sym[k] = std::make_shared<svg_art>();
			m_key_sym[k]->load_pixels(S, S, px);
			m_key_sym_px = S;
		}
	}

	// キートップの印刷。太字で、キーの幅に 6 文字（SELECT）が収まる大きさ
	if (m_font_key) DeleteObject(m_font_key);
	m_font_key_em = std::max(6, int(std::lround(9.0 * m_scale)));
	m_font_key = CreateFontA(-m_font_key_em, 0, 0, 0, FW_BOLD,
	                         FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
	                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Arial");

	// LCD の中の札の字は LCD の寸法に合わせる。MIC / LINE は字の高さが
	// 箱の 6 割強で、「LINE」が箱の幅の 3/4 ほど。パート番号は行の高さいっぱい
	{
		if (m_font_tag) DeleteObject(m_font_tag);
		if (m_font_num) DeleteObject(m_font_num);
		const lcd_geom g = lcd_grid();
		m_font_num = CreateFontA(-std::max(4, int(std::lround(g.line_h * 1.0))), 0, 0, 0,
		                         FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		                         VARIABLE_PITCH, "Segoe UI");
		RECT box[2];
		lcd_tag_boxes(g, box);
		const int bh = box[0].bottom - box[0].top, bw = box[0].right - box[0].left;
		// 写真では大文字の高さが箱の 75%、「MIC」の幅が箱の 71%
		const int em = std::max(4, int(std::min(1.07 * bh, 0.40 * bw)));
		m_font_tag_em = em;
		m_font_tag = CreateFontA(-em, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		                         DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
		                         CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
	}

	build_spots();
}

// 触れる場所は面ごとに違う。掴んでいる途中に作り直すと迷子になるので離す
void panel::build_spots()
{
	m_held = nullptr;
	m_spots.clear();
	if (m_lcd_only)
		return;

	// 面を選ぶつまみ。本体の外（下の帯）
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FRONT,
	                    scale(700, 386, 94, 13), UI_TEXT(tab_panel, "Panel"), "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_EDIT,
	                    scale(800, 386, 94, 13), UI_TEXT(tab_editor, "Editor"), "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FX,
	                    scale(898, 386, 94, 13), UI_TEXT(tab_effects, "Effects"), "" });

	if (m_page == page::editor) { build_editor_spots(); return; }
	if (m_page == page::effects) { build_effect_spots(); return; }

	for (int i = 0; i < 18; i++)
		m_spots.push_back({ spot_kind::button, CAT_B[i], CTL_NONE,
		                    scale(m_lay.cat_x[i % 6] - m_lay.cat_w / 2, m_lay.cat_y[i / 6],
		                          m_lay.cat_w, m_lay.cat_h),
		                    CAT_LABEL[i], "" });
	for (int i = 0; i < 6; i++)
		m_spots.push_back({ spot_kind::button, MODES[i].b, CTL_NONE,
		                    scale(m_lay.mode[i][0] - m_lay.mode_r,
		                          m_lay.mode[i][1] - m_lay.mode_r,
		                          m_lay.mode_r * 2, m_lay.mode_r * 2),
		                    MODES[i].label, "" });
	for (int i = 0; i < 9; i++)
		m_spots.push_back({ spot_kind::button, NAV[i].b, CTL_NONE,
		                    scale(m_lay.nav[i][0], m_lay.nav[i][1],
		                          m_lay.nav[i][2], m_lay.nav[i][3]),
		                    NAV[i].label, NAV[i].sub });
	for (int i = 0; i < 2; i++)
		m_spots.push_back({ spot_kind::button, ROUND[i].b, CTL_NONE,
		                    scale(m_lay.round_[i][0] - m_lay.round_[i][2] / 2,
		                          m_lay.round_[i][1] - m_lay.round_[i][3] / 2,
		                          m_lay.round_[i][2], m_lay.round_[i][3]),
		                    ROUND[i].label, ROUND[i].sub });

	m_spots.push_back({ spot_kind::wheel,  mu2000::button::count, CTL_NONE, m_wheel, "", "" });
	m_spots.push_back({ spot_kind::volume, mu2000::button::count, CTL_NONE, m_volume,
	                    "VOLUME", "" });
	if (m_lay.adgain[2] > 0)
		m_spots.push_back({ spot_kind::adgain, mu2000::button::count, CTL_NONE, m_adgain,
		                    "A/D INPUT", "" });
}

const spot *panel::hit(int x, int y) const
{
	for (const spot &s : m_spots) {
		if (x >= s.r.left && x < s.r.right && y >= s.r.top && y < s.r.bottom) {
			// 丸いものは丸の中だけ
			if (s.kind == spot_kind::wheel || s.kind == spot_kind::volume ||
			    s.kind == spot_kind::adgain) {
				const double cx = (s.r.left + s.r.right) * 0.5;
				const double cy = (s.r.top + s.r.bottom) * 0.5;
				const double rr = (s.r.right - s.r.left) * 0.5;
				if (std::hypot(x - cx, y - cy) > rr)
					continue;
			}
			return &s;
		}
	}
	return nullptr;
}


void panel::draw_tabs(HDC dc) const
{
	for (const spot &sp : m_spots) {
		if (sp.kind != spot_kind::tab)
			continue;
		const bool on = (sp.ctl == CTL_TAB_EDIT   && m_page == page::editor) ||
		                (sp.ctl == CTL_TAB_FX     && m_page == page::effects) ||
		                (sp.ctl == CTL_TAB_FRONT  && m_page == page::front);
		round_box(dc, sp.r, on ? RGB(70, 76, 84) : RGB(38, 41, 46),
		          on ? ACCENT : RGB(70, 74, 80), int(4 * m_scale));
		text_in(dc, sp.r, sp.label, on ? TEXT : TEXT_DIM, m_font_small,
		        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
}

panel::lcd_geom panel::lcd_grid() const
{
	lcd_geom g{};
	const int aw = m_lcd.right - m_lcd.left, ah = m_lcd.bottom - m_lcd.top;
	g.pad = std::max(1, int(1 * m_scale));

	// 点の大きさ。横は上の面 17 桁と左右の余白（LCD_SPAN 点ぶん）、
	// 縦は上の面 16 点・目盛りの帯 BAND 点・下の面 8 点が収まるように
	const double fit = std::min(aw / LCD_SPAN, double(ah - g.pad * 2) / (24 + BAND));
	const int d = std::max(1, int(fit));

	// 拡大して描く倍率（draw_lcd）。点の間隔が 12 画素ほどになるまで
	g.k = 1;
	double df = d;
#ifdef _WIN32
	g.k = std::min(6, (12 + d - 1) / d);
	// 拡大して描くので、点の大きさは端数（1/k 画素きざみ）にできる。
	// 整数に切り捨てると、3.8 画素入る窓で 3 画素になり、LCD の中に 2 割の
	// 余白ができる（起動直後の大きさがちょうどそうだった）
	if (g.k > 1)
		df = std::max(double(d), std::floor(fit * g.k) / g.k);
#endif
	g.df = df;
	g.d = int(std::lround(df));

	// 目盛りの帯。中の並びは band_y() の割合で決まる
	g.scale_h = std::max(8, int(std::lround(BAND * df)));
	g.tick_h  = std::max(1, int(std::lround(BAND_NUM[0] * g.scale_h)));
	g.line_h  = std::max(3, int(std::lround((BAND_NUM[1] - BAND_NUM[0]) * g.scale_h)));

	// 点は正方形のままにして、余った幅は左右に振り分ける。
	// 端数は 1/k 画素に丸める（拡大した絵の画素の境目に乗るように）
	auto q = [&](double v) { return std::round(v * g.k) / g.k; };
	g.fx0 = q(m_lcd.left + std::max(0.0, (aw - LCD_SPAN * df) / 2) + LCD_LEFT * df);
	g.fy0 = q(m_lcd.top + std::max(double(g.pad), (ah - (24 * df + g.scale_h)) / 2));
	g.fsy = g.fy0 + 16 * df + g.scale_h;
	g.x0 = int(std::lround(g.fx0));
	g.y0 = int(std::lround(g.fy0));
	g.sy = int(std::lround(g.fsy));
	return g;
}

int panel::band_y(const lcd_geom &g, double f) const
{
	return int(std::lround(g.fy0 + 16 * g.df + f * g.scale_h));
}

void panel::lcd_tag_boxes(const lcd_geom &g, RECT out[2]) const
{
	// 横は A1 A2 のマス（5.6 点ぶん）。縦は目盛りの帯の中の決まった割合
	const double d = g.df;
	const int l = int(std::lround(g.fx0 - 0.1 * d));
	const int r = int(std::lround(g.fx0 + 5.5 * d));
	out[0] = RECT{ l, band_y(g, BAND_MIC[0]),  r, band_y(g, BAND_MIC[1]) };
	out[1] = RECT{ l, band_y(g, BAND_LINE[0]), r, band_y(g, BAND_LINE[1]) };
}

void panel::draw_lcd(HDC dc, const snapshot &s) const
{
	if (!m_lcd_only && m_lay.lcd_frame) {
		RECT bez = m_lcd;
		InflateRect(&bez, int(5 * m_scale), int(5 * m_scale));
		round_box(dc, bez, RGB(60, 58, 52), RGB(110, 106, 96), int(5 * m_scale));
	}
	const lcd_geom g = lcd_grid();

	std::vector<lcd_shape> shapes;
	double sk = 1.0, ox = 0, oy = 0;             // 本体の座標から画面の座標へ
	bool done = false;
#ifdef _WIN32
	// GDI は図形の縁をぼかさない。点の格子を端数の大きさで描くために、
	// 点の間隔が 12 画素ほどになるまで拡大して描き、平均を取って縮める。
	// 文字（目盛りの番号や MIC）は縮めると読めなくなるので、後で等倍で重ねる
	const int k = g.k;
	if (k > 1 && supersample_lcd(dc, s, g, k, shapes)) {
		sk = 1.0 / k;
		ox = m_lcd.left;
		oy = m_lcd.top;
		done = true;
	}
#endif
	if (!done)
		draw_lcd_body(dc, s, g, m_lcd, 1.0, shapes);
	// セグメントは描き上がった LCD の上に、画面の画素で縁をぼかして塗る。
	// 窓の大きさや拡大の有無によらず、円弧や斜めの線がギザギザにならない
	// 全部を 1 枚の重ね絵にまとめて 1 回で貼る。形が前の画と同じなら
	// （演奏画面で置いているときなど）作り直さずに前の重ね絵を貼るだけ
	u64 hash = 1469598103934665603ull;
	auto mixin = [&](u64 v) { hash = (hash ^ v) * 1099511628211ull; };
	double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
	for (lcd_shape &sh : shapes) {
		for (size_t i = 0; i + 1 < sh.xy.size(); i += 2) {
			sh.xy[i] = ox + sh.xy[i] * sk;
			sh.xy[i + 1] = oy + sh.xy[i + 1] * sk;
			minx = std::min(minx, sh.xy[i]);     maxx = std::max(maxx, sh.xy[i]);
			miny = std::min(miny, sh.xy[i + 1]); maxy = std::max(maxy, sh.xy[i + 1]);
			mixin(u64(std::llround(sh.xy[i] * 64)));
			mixin(u64(std::llround(sh.xy[i + 1] * 64)));
		}
		mixin(u64(sh.ink) | (u64(sh.xy.size()) << 32));
	}
	if (!shapes.empty()) {
		const int x0 = int(std::floor(minx)), y0 = int(std::floor(miny));
		const int w = int(std::ceil(maxx)) - x0, h = int(std::ceil(maxy)) - y0;
		if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
			if (hash != m_seg_hash || x0 != m_seg_x || y0 != m_seg_y || w != m_seg_w || h != m_seg_h) {
				m_seg_px.assign(size_t(w) * h, 0);
				for (const lcd_shape &sh : shapes)
					fill_aa(m_seg_px, x0, y0, w, h, sh.xy, sh.ink);
				m_seg_hash = hash;
				m_seg_x = x0; m_seg_y = y0; m_seg_w = w; m_seg_h = h;
			}
			blit_premul(dc, x0, y0, w, h, m_seg_px.data(), false);
		}
	}
	draw_lcd_labels(dc, s, g);
	draw_lcd_message(dc, s);
}

void panel::draw_lcd_message(HDC dc, const snapshot &s) const
{
	if (!s.message[0])
		return;
	RECT r = m_lcd;
	fill(dc, r, RGB(24, 26, 22));
	text_in(dc, r, s.message, RGB(210, 220, 200), m_font_label,
	        DT_CENTER | DT_VCENTER | DT_WORDBREAK);
}

#ifdef _WIN32
bool panel::supersample_lcd(HDC dc, const snapshot &s, const lcd_geom &g, int k,
                            std::vector<lcd_shape> &shapes) const
{
	const int w = m_lcd.right - m_lcd.left, h = m_lcd.bottom - m_lcd.top;
	if (w <= 0 || h <= 0)
		return false;
	const int bw = w * k, bh = h * k;

	auto dib = [](HDC like, int dw, int dh, u32 **bits, HDC *mem) -> HBITMAP {
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = dw;
		bi.bmiHeader.biHeight = -dh;                   // 上から下へ
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		void *p = nullptr;
		HBITMAP bm = CreateDIBSection(like, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
		if (!bm)
			return nullptr;
		*mem = CreateCompatibleDC(like);
		SelectObject(*mem, bm);
		*bits = static_cast<u32 *>(p);
		return bm;
	};

	u32 *big = nullptr, *out = nullptr;
	HDC big_dc = nullptr, out_dc = nullptr;
	HBITMAP big_bm = dib(dc, bw, bh, &big, &big_dc);
	if (!big_bm)
		return false;
	HBITMAP out_bm = dib(dc, w, h, &out, &out_dc);
	if (!out_bm) {
		DeleteDC(big_dc);
		DeleteObject(big_bm);
		return false;
	}

	// 拡大した座標で描く。原点は LCD の左上
	lcd_geom gk = g;
	gk.d = int(std::lround(g.df * k));
	gk.pad = g.pad * k;
	gk.x0 = int(std::lround((g.fx0 - m_lcd.left) * k));
	gk.y0 = int(std::lround((g.fy0 - m_lcd.top) * k));
	gk.sy = gk.y0 + 16 * gk.d + g.scale_h * k;
	gk.tick_h = g.tick_h * k;
	gk.line_h = g.line_h * k;
	gk.scale_h = g.scale_h * k;
	draw_lcd_body(big_dc, s, gk, RECT{ 0, 0, bw, bh }, double(k), shapes);
	GdiFlush();

	// k × k の平均
	const int n = k * k;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			unsigned r = 0, gg = 0, b = 0;
			for (int yy = 0; yy < k; yy++) {
				const u32 *row = big + size_t(y * k + yy) * bw + size_t(x) * k;
				for (int xx = 0; xx < k; xx++) {
					b  += row[xx] & 0xff;
					gg += (row[xx] >> 8) & 0xff;
					r  += (row[xx] >> 16) & 0xff;
				}
			}
			out[size_t(y) * w + x] = ((r + n / 2) / n) << 16 | ((gg + n / 2) / n) << 8 | ((b + n / 2) / n);
		}
	BitBlt(dc, m_lcd.left, m_lcd.top, w, h, out_dc, 0, 0, SRCCOPY);

	DeleteDC(out_dc);
	DeleteObject(out_bm);
	DeleteDC(big_dc);
	DeleteObject(big_bm);
	return true;
}
#endif

void panel::draw_lcd_labels(HDC dc, const snapshot &s, const lcd_geom &g) const
{
	const lcd_ink pal = lcd_palette(s.contrast);
	const COLORREF LCD_DOT = pal.dot, LCD_GHOST = pal.ghost;
	// 札は等倍で描くので、端数つきの寸法から画素に丸める
	const double d = g.df, fx0 = g.fx0;
	const int x0 = g.x0;
	const int tick_h = g.tick_h, line_h = g.line_h;
	const int scale_y = int(std::lround(g.fy0 + 16 * d));
	auto px = [](double v) { return int(std::lround(v)); };
	auto ctl = [&](int col, int row) { return lcd_ctl(s, col, row); };

	// ---- 目盛りの帯。ここも**印刷ではなくセグメント**で、点いたり消えたりする
	{
		const bool on_scale = ctl(CD, 4);          // 「1」-「32」
		const bool on_a1a2  = ctl(CD, 3);          // 「A1」「A2」
		const COLORREF ink_scale = on_scale ? LCD_DOT : LCD_GHOST;
		const COLORREF ink_a1a2  = on_a1a2  ? LCD_DOT : LCD_GHOST;

		HPEN p = CreatePen(PS_SOLID, 1, on_scale ? LCD_DOT : LCD_GHOST);
		HGDIOBJ op = SelectObject(dc, p);
		for (int i = 0; i < TOP_COLS * 2; i++) {
			const int col = i / 2;
			// バーは 2 点ぶんの幅。番号と線はその真ん中（2 点目の右の隙間は除く）
			const int bx  = px(fx0 + col * (CELL_W + 1) * d + ((i & 1) ? 3 * d : 0)
			                   + d * (1.0 - DOT_GAP / 2));
			if (i >= 2 || on_a1a2) {
				MoveToEx(dc, bx, scale_y, nullptr);
				LineTo(dc, bx, scale_y + tick_h);
			}
			// 番号はパートの番号。**そのバーの真下**に置く
			const int part = i - 1;
			char n[8];
			std::snprintf(n, sizeof(n), i < 2 ? "A%d" : "%d", i < 2 ? i + 1 : part);
			RECT t{ bx - px(1.5 * d), scale_y + tick_h,
			        bx + px(1.5 * d), scale_y + tick_h + line_h };
			text_in(dc, t, n, i < 2 ? ink_a1a2 : ink_scale, m_font_num,
			        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
		}
		SelectObject(dc, op);
		DeleteObject(p);

		// MIC と LINE は左端に上下に並ぶ。実機は**黒い箱に白抜き**の字で、
		// 消えているときは箱がうっすら見え、字はそれより少し明るい
		{
			RECT box[2];
			lcd_tag_boxes(g, box);
			const char *name[2] = { "MIC", "LINE" };
			const bool on[2] = { ctl(CD, 1), ctl(CD, 2) };
			const int rad = std::max(2, px(0.5 * d));
			for (int k = 0; k < 2; k++) {
				const COLORREF face = on[k] ? LCD_DOT : LCD_GHOST;
				const COLORREF ink  = on[k] ? LCD_BACK : mix(LCD_GHOST, LCD_BACK, 0.6);
				round_box(dc, box[k], face, face, rad);
				// DT_VCENTER は行の高さ（下へはみ出す部分を含む）を真ん中に置くので、
				// 大文字だけの札は下に寄る。大文字の見える部分の真ん中を箱の真ん中に
				// 合わせる（Segoe UI の上の高さ 1.079 em、大文字の高さ 0.700 em）
				const double cy = (box[k].top + box[k].bottom) / 2.0;
				RECT t = box[k];
				t.top = int(std::lround(cy - (1.079 - 0.700 / 2) * m_font_tag_em));
				t.bottom = t.top + int(std::lround(1.33 * m_font_tag_em)) + 1;
				text_in(dc, t, name[k], ink, m_font_tag,
				        DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
			}
		}

		// BANK と PGM# は 2 つずつあり、パート番号の下に並んでいる。
		// 左側の組が D0、右側の組が D5 で点け消しされる
		struct { int part; const char *label; bool right; } marks[] = {
			{  3, "BANK", false }, { 11, "PGM#", false },
			{ 19, "BANK", true  }, { 27, "PGM#", true  },
		};
		for (const auto &mk : marks) {
			const int cx = px(fx0 + (part_x(mk.part) + part_x(mk.part + 1) + 2) * d / 2);
			const int w = int(26 * m_scale);
			RECT r{ cx - w / 2, band_y(g, BAND_MIC[0]), cx + w / 2, band_y(g, BAND_MIC[1]) };
			text_in(dc, r, mk.label,
			        ctl(CD, mk.right ? 5 : 0) ? LCD_DOT : LCD_GHOST, m_font_tag,
			        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
	}
}

void panel::draw_lcd_body(HDC dc, const snapshot &s, const lcd_geom &g,
                          const RECT &area, double px, std::vector<lcd_shape> &shapes) const
{
	fill(dc, area, LCD_BACK);
	// 色はコントラストしだい。ここから下の LCD_DOT / LCD_GHOST はこの色
	const lcd_ink pal = lcd_palette(s.contrast);
	const COLORREF LCD_DOT = pal.dot, LCD_GHOST = pal.ghost;

	// 実機の窓は、DDRAM の桁がそのまま横一列に並んでいるのではない。
	// ボタンを押して確かめた割り振りは（doc/gui.md）
	//
	//   上の面（点の並び。2 行、桁のあいだは 1 点、**行のあいだは空けない**）
	//     0-8   レベルメータ。1 マス 2 本で 18 本（A1 A2 と 1-16）
	//     9-16  文字 8 桁。1 行目が音色名、2 行目が ▶000◀001
	//   下の面
	//     行 0 の 17-18   部の番号「01」
	//     行 1 の 17-19   「A01」
	//     20-22（両行）   楽器のかたち。**点が細かく、正方形でもない**
	//     **23（両行）は絵ではない**。決まった形のセグメントを点けたり
	//     消したりする 64 個のビットが入っている（下の ctl）
	const int d = g.d, x0 = g.x0, y0 = g.y0;

	const COLORREF FAINT = pal.faint;                      // 絵の区画の消え点

	// 使う色ごとに筆を 1 本。点の縁を背景と混ぜるので、色の数は描くまで決まらない
	std::vector<std::pair<COLORREF, HBRUSH>> brushes;
	auto br = [&](COLORREF c) {
		for (const auto &b : brushes)
			if (b.first == c)
				return b.second;
		brushes.emplace_back(c, CreateSolidBrush(c));
		return brushes.back().second;
	};
	HBRUSH lit = br(LCD_DOT);

	// 点 1 つ。w × h は点の間隔で、右と下に DOT_GAP ぶんの隙間を空ける。
	// 隙間が 1 画素に満たないときは、その 1 画素を背景と混ぜた色で塗る。
	// 小さい窓で隙間が丸ごと 1 画素になり、文字が薄く見えていたのを防ぐ
	const double gap = DOT_GAP * d;
	auto dotbox = [&](int l, int t, int w, int h, COLORREF ink, double gp = -1) {
		if (gp < 0)
			gp = gap;
		int gi = int(gp);
		double fr = gp - gi;
		const bool part = fr > 0.05;
		int sw = w - gi - (part ? 1 : 0), sh = h - gi - (part ? 1 : 0);
		if (sw < 1 || sh < 1) {                  // 小さすぎる。隙間なしで塗る
			RECT r{ l, t, l + std::max(1, w), t + std::max(1, h) };
			FillRect(dc, &r, br(ink));
			return;
		}
		RECT r{ l, t, l + sw, t + sh };
		FillRect(dc, &r, br(ink));
		if (!part)
			return;
		const HBRUSH edge = br(mix(ink, LCD_BACK, fr));
		RECT rc{ l + sw, t, l + sw + 1, t + sh };
		FillRect(dc, &rc, edge);
		RECT rr{ l, t + sh, l + sw, t + sh + 1 };
		FillRect(dc, &rr, edge);
		RECT rx{ l + sw, t + sh, l + sw + 1, t + sh + 1 };
		FillRect(dc, &rx, br(mix(ink, LCD_BACK, 1.0 - (1.0 - fr) * (1.0 - fr))));
	};

	// 端数のある座標の多角形。ここでは描かずに溜め、draw_lcd が描き上がった
	// LCD の上に縁をぼかして塗る（lcd_shape）
	struct pt { double x, y; };
	auto poly = [&](std::initializer_list<pt> ps, COLORREF ink) {
		lcd_shape sh;
		sh.ink = ink;
		for (const pt &p : ps) {
			sh.xy.push_back(p.x);
			sh.xy.push_back(p.y);
		}
		shapes.push_back(std::move(sh));
	};
	// 角度は真上が 0 度で時計回り
	auto polar = [](double cx, double cy, double r, double deg) {
		const double a = deg * PI / 180.0;
		return pt{ cx + r * std::sin(a), cy - r * std::cos(a) };
	};
	// 帯状の弧（中心 cx cy、半径 r0-r1、a0 度から a1 度）。端は半径の向きに切る
	auto ring = [&](double cx, double cy, double r0, double r1, double a0, double a1,
	                COLORREF ink) {
		// 大きく描いても角が見えないよう、2 度きざみで折る
		const int n = std::max(8, std::min(180, int((a1 - a0) / 2)));
		lcd_shape sh;
		sh.ink = ink;
		for (int i = 0; i <= n; i++) {
			const pt p = polar(cx, cy, r1, a0 + (a1 - a0) * i / n);
			sh.xy.push_back(p.x);
			sh.xy.push_back(p.y);
		}
		for (int i = n; i >= 0; i--) {
			const pt p = polar(cx, cy, r0, a0 + (a1 - a0) * i / n);
			sh.xy.push_back(p.x);
			sh.xy.push_back(p.y);
		}
		shapes.push_back(std::move(sh));
	};

	auto ctl = [&](int col, int row) { return lcd_ctl(s, col, row); };

	// 1 マスぶんの点を描く。p は点の間隔（端数もよい。各点の端を丸めて並べる）
	auto cell = [&](int row, int col, double px, double py, double p) {
		const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
		auto at = [](double v) { return int(std::lround(v)); };
		for (int y = 0; y < CELL_H; y++)
			for (int x = 0; x < CELL_W; x++) {
				const int l = at(px + x * p), t = at(py + y * p);
				dotbox(l, t, at(px + (x + 1) * p) - l, at(py + (y + 1) * p) - t,
				       (s.lcd_on && BIT(c[y], 4 - x)) ? LCD_DOT : LCD_GHOST, DOT_GAP * p);
			}
	};

	// ---- 上の面。メータ 9 マス ＋ 文字 8 桁。行のあいだは空けない
	for (int row = 0; row < LCD_ROWS; row++)
		for (int col = 0; col < TOP_COLS; col++)
			cell(row, col, x0 + col * (CELL_W + 1) * d, y0 + row * CELL_H * d, d);

	// ---- 下の面
	const int sy = g.sy;
	const int seg_h = 8 * d;                       // 文字 1 行ぶんの高さ
	auto lx = [&](int which) { return x0 + int(std::lround(m_lay.low_x[which] * d)); };
	auto lw = [&](int which) { return int(std::lround(m_lay.low_w[which] * d)); };

	// 部の番号「01」と「A01」（「 A/D1」なども同じ 5 桁）。点は下の面の大きさ。
	// 実機の字間は 1 桁目と 2 桁目が 1 点、2 桁目と 3 桁目が 2 点、あとは 1 点。
	// 2 点あくところが「01」と「A01」の境目で、low.x の 11.96（13 × 0.92）はそこから来る
	const double ld = LOW_DOT * d;
	for (int i = 0; i < 2; i++)
		cell(0, TOP_COLS + i, lx(LOW_PART) + i * (CELL_W + 1) * ld, sy, ld);
	for (int i = 0; i < 3; i++)
		cell(1, TOP_COLS + i, lx(LOW_BANK) + i * (CELL_W + 1) * ld, sy, ld);

	// 楽器のかたち。20-22 桁の両行が 1 枚の絵。**23 桁目は絵ではない**ので入れない。
	// 点は横長で、文字の点と同じくほんのわずかに隙間がある
	{
		const int ix = lx(LOW_ICON), iw = lw(LOW_ICON);
		const int first = TOP_COLS + 3, last = LCD_COLS - 1;   // 20-22
		const int nx = (last - first) * CELL_W, ny = LCD_ROWS * CELL_H;
		for (int row = 0; row < LCD_ROWS; row++)
			for (int col = first; col < last; col++) {
				const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
				for (int y = 0; y < CELL_H; y++) {
					const int yy = row * CELL_H + y;
					const int top = sy + yy * seg_h / ny;
					const int bot = sy + (yy + 1) * seg_h / ny;
					for (int x = 0; x < CELL_W; x++) {
						const int xx = (col - first) * CELL_W + x;
						const int left  = ix + xx * iw / nx;
						const int right = ix + (xx + 1) * iw / nx;
						dotbox(left, top, std::max(1, right - left), std::max(1, bot - top),
						       (s.lcd_on && BIT(c[y], 4 - x)) ? LCD_DOT : FAINT);
					}
				}
			}
	}

	// ---- 決まった形のセグメント。**23 桁目のビットで点け消しする**。
	// 形と寸法は実機の写真から採った（単位は点の間隔 d）
	{
		auto ink = [&](bool on) { return on ? LCD_DOT : LCD_GHOST; };
		const double dd = d;
		const double dv = LOW_DOT * dd;   // 下の面の高さは下の面の点の間隔で測ってある
		// 細い線は、縮めたあとでも 1 画素（px）を下回らないようにする。
		// 下回ると色が薄まって、小さい窓では見えなくなる
		auto thick = [&](double t) { return std::max(t, px); };

		// 7 セグメント。seg は a b c d e f g の順のビット。
		// 実機のセグメントは端が斜めに切れた台形で、真ん中の g は両端がとがる
		auto seven = [&](double x, double y, double w, double h, double t, unsigned seg) {
			const double k  = std::max(0.5 * px, 0.1 * dd);     // セグメントのあいだの隙間
			const double ym = y + h / 2, ht = t / 2;
			poly({ { x + k, y }, { x + w - k, y }, { x + w - t - k, y + t }, { x + t + k, y + t } },
			     ink(BIT(seg, 0)));                                             // a
			poly({ { x + w, y + k }, { x + w, ym - ht - k }, { x + w - ht, ym - k },
			       { x + w - t, ym - ht - k }, { x + w - t, y + t + k } },
			     ink(BIT(seg, 1)));                                             // b
			poly({ { x + w, ym + ht + k }, { x + w, y + h - k }, { x + w - t, y + h - t - k },
			       { x + w - t, ym + ht + k }, { x + w - ht, ym + k } },
			     ink(BIT(seg, 2)));                                             // c
			poly({ { x + t + k, y + h - t }, { x + w - t - k, y + h - t }, { x + w - k, y + h },
			       { x + k, y + h } },
			     ink(BIT(seg, 3)));                                             // d
			poly({ { x, ym + ht + k }, { x + ht, ym + k }, { x + t, ym + ht + k },
			       { x + t, y + h - t - k }, { x, y + h - k } },
			     ink(BIT(seg, 4)));                                             // e
			poly({ { x, y + k }, { x + t, y + t + k }, { x + t, ym - ht - k },
			       { x + ht, ym - k }, { x, ym - ht - k } },
			     ink(BIT(seg, 5)));                                             // f
			poly({ { x + ht + k, ym }, { x + t + k, ym - ht }, { x + w - t - k, ym - ht },
			       { x + w - ht - k, ym }, { x + w - t - k, ym + ht }, { x + t + k, ym + ht } },
			     ink(BIT(seg, 6)));                                             // g
		};

		// 送り量の扇。中心角 45 度の細い弧を 8 本、同じ中心で重ねたもの。
		// 中心（扇の要）は下の面の下端より少し下にある。下から N 本を点ける
		auto fan = [&](int which, const bool *on8) {
			const double cx = lx(which) + lw(which) / 2.0;
			const double cy = sy + 8.4 * dv;
			for (int k = 0; k < 8; k++) {
				const double r = (1.45 + 1.0 * k) * dd;
				ring(cx, cy, r - thick(0.42 * dd) / 2, r + thick(0.42 * dd) / 2, -22.5, 22.5, ink(on8[k]));
			}
		};

		// VOL と EXP。**行 0 の 19 桁目**に、レベルメータと同じ形で
		// 入っている（左の 2 点が VOL、右の 2 点が EXP）。
		// 実機では離れた場所に、横に長い 8 本の棒で出る
		{
			const u8 *c = s.dots + (0 * LCD_COLS + TOP_COLS + 2) * CELL_H;
			// 棒の間隔は出来上がりの画素の整数に丸め、棒の上端も画素の境目に
			// 揃える。こうすると 8 本とも画素との位置関係が同じになり、太さが
			// 揃う（間隔に端数があると 1 画素の棒と 2 画素の棒が混ざる）。
			// 太さの端数は、どの棒も同じだけ下の縁がぼける
			const double want = 8.3 * dv / 8;
			const double pitch = std::max(1.0, std::round(want / px)) * px;
			const int bar = std::max(int(std::lround(px)), int(std::lround(thick(0.48 * pitch))));
			const double start = std::round((sy - 0.5 * dv + 4 * (want - pitch)) / px) * px;
			for (int y = 0; y < CELL_H; y++) {
				const int top = int(std::lround(start + y * pitch));
				const int bot = top + bar;
				const bool vol = s.lcd_on && (BIT(c[y], 4) || BIT(c[y], 3));
				const bool exp = s.lcd_on && (BIT(c[y], 1) || BIT(c[y], 0));
				RECT rv{ lx(LOW_VOL), top, lx(LOW_VOL) + lw(LOW_VOL), bot };
				FillRect(dc, &rv, br(ink(vol)));
				RECT re{ lx(LOW_EXP), top, lx(LOW_EXP) + lw(LOW_EXP), bot };
				FillRect(dc, &re, br(ink(exp)));
			}
		}

		// パン。下の開いた円弧の中で、針が 45 度おきの 7 か所に飛ぶ。
		// D15 が左下、D12 が真上、D9 が右下
		{
			const double cx = lx(LOW_PAN) + lw(LOW_PAN) / 2.0, cy = sy + 3.75 * dv;
			const double r = 3.6 * dd;
			// 円弧は点けたり消したりしない（実機はいつも点いている）
			ring(cx, cy, r - thick(0.25 * dd), r, -124.0, 124.0, ink(s.lcd_on));
			const double r0 = 0.29 * r, r1 = 0.72 * r, ht = thick(0.36 * dd) / 2;
			for (int k = 0; k < 7; k++) {
				const bool on = ctl(CD, 15 - k);
				const double a = -135.0 + 45.0 * k;
				const pt p0 = polar(cx, cy, r0, a), p1 = polar(cx, cy, r1, a);
				const double nx = std::cos(a * PI / 180.0) * ht, ny = std::sin(a * PI / 180.0) * ht;
				poly({ { p0.x - nx, p0.y - ny }, { p1.x - nx, p1.y - ny },
				       { p1.x + nx, p1.y + ny }, { p0.x + nx, p0.y + ny } }, ink(on));
			}
		}

		// リバーブ・コーラス・バリエーションの送り量
		{
			bool rev[8], cho[8], var[8];
			for (int k = 0; k < 8; k++) {
				rev[k] = ctl(CA, 15 - k);
				cho[k] = ctl(CB, 15 - k);
				var[k] = ctl(CC, 15 - k);
			}
			fan(LOW_REV, rev);
			fan(LOW_CHO, cho);
			fan(LOW_VAR, var);
		}

		// ノートシフト。符号（横棒は常時、縦棒が点くと ＋）と 2 桁。
		// 符号の縦棒は、横棒と交わるところで少し途切れている
		{
			const double t  = thick(0.4 * dd);
			const double dh = 5.4 * dv, dy = sy + 2.2 * dv, dw = 2.6 * dd;
			const double kx = lx(LOW_KEY);
			const double sw = 2.4 * dd, cy = dy + dh / 2, vh = 0.62 * dh;
			const double vx = kx + sw / 2, cut = std::max(px, 0.3 * dd);
			poly({ { kx, cy - t / 2 }, { kx + sw, cy - t / 2 }, { kx + sw, cy + t / 2 },
			       { kx, cy + t / 2 } }, ink(ctl(CB, 0)));
			const bool plus = ctl(CA, 0);
			poly({ { vx - t / 2, cy - vh / 2 }, { vx + t / 2, cy - vh / 2 },
			       { vx + t / 2, cy - t / 2 - cut }, { vx - t / 2, cy - t / 2 - cut } }, ink(plus));
			poly({ { vx - t / 2, cy + t / 2 + cut }, { vx + t / 2, cy + t / 2 + cut },
			       { vx + t / 2, cy + vh / 2 }, { vx - t / 2, cy + vh / 2 } }, ink(plus));

			// 十の位は a/d/e/g がひとまとめ。f は使われない
			const bool ten_adeg = ctl(CA, 1);
			unsigned ten = 0;
			if (ten_adeg) ten |= (1u << 0) | (1u << 3) | (1u << 4) | (1u << 6);
			if (ctl(CB, 1)) ten |= 1u << 1;
			if (ctl(CA, 7)) ten |= 1u << 2;
			if (ctl(CB, 4)) ten |= 1u << 5;
			seven(kx + sw + 0.2 * dd, dy, dw, dh, t, ten);

			unsigned one = 0;
			if (ctl(CB, 6)) one |= 1u << 0;   // a
			if (ctl(CA, 6)) one |= 1u << 1;   // b
			if (ctl(CA, 3)) one |= 1u << 2;   // c
			if (ctl(CA, 2)) one |= 1u << 3;   // d
			if (ctl(CB, 2)) one |= 1u << 4;   // e
			if (ctl(CB, 7)) one |= 1u << 5;   // f
			if (ctl(CB, 3)) one |= 1u << 6;   // g
			seven(kx + sw + 0.2 * dd + dw + 0.5 * dd, dy, dw, dh, t, one);
		}

		// いちばん右の ▶ は 4 つ。上の 1 つは札がなく、点く場面をまだ見ていない。
		// 残りの 3 つが XG / TG300B(GS) / PERFORM。
		// PLG のぶんは C2 か D8 のどちらかだが、まだ決められていない
		{
			const double mx = lx(LOW_MODE);
			const double th = 1.45 * dv, tw = th * 0.9;     // 正三角形に近い
			const bool mode[4] = { false, ctl(CB, 5), ctl(CA, 4), ctl(CA, 5) };
			for (int k = 0; k < 4; k++) {
				const double cy = sy + MODE_Y[k] * dv;
				poly({ { mx, cy - th / 2 }, { mx + tw, cy }, { mx, cy + th / 2 } }, ink(mode[k]));
			}
		}

		// 下の面の上に出る ▼ のカーソル。いま何を弄っているかを示す
		{
			// 先は下の面より少し上。VOL の棒や扇のいちばん上に掛からないように
			const int cur_y = sy - std::max(2, int(std::lround(1.6 * LOW_DOT * d)));
			const int hw = std::max(2, d);
			struct { int at; bool on; } cur[] = {
				{ LOW_VOL,  ctl(CC, 3) }, { LOW_EXP, ctl(CC, 4) },
				{ LOW_PAN,  ctl(CC, 5) }, { LOW_REV, ctl(CC, 6) },
				{ LOW_CHO,  ctl(CC, 7) }, { LOW_VAR, ctl(CD, 6) },
				{ LOW_KEY,  ctl(CD, 7) },
			};
			for (const auto &c : cur) {
				if (!c.on)
					continue;
				const int cx = lx(c.at) + lw(c.at) / 2;
				poly({ { double(cx - hw), double(cur_y - hw) }, { double(cx + hw), double(cur_y - hw) },
				       { double(cx), double(cur_y) } }, LCD_DOT);
			}
			// バンク番号とプログラム番号のカーソルは**楽器のかたちの上**。
			// バンクは 4-5 列目、プログラムは 12-13 列目の上（実機を見て教わった）
			const int ix = lx(LOW_ICON);
			const int tops[2] = { ix + (3 + 5) * d / 2, ix + (11 + 13) * d / 2 };
			const bool ton[2] = { ctl(CC, 1), ctl(CC, 0) };
			for (int k = 0; k < 2; k++) {
				if (!ton[k])
					continue;
				poly({ { double(tops[k] - hw), double(cur_y - hw) }, { double(tops[k] + hw), double(cur_y - hw) },
				       { double(tops[k]), double(cur_y) } }, LCD_DOT);
			}
		}
	}

	for (const auto &b : brushes)
		DeleteObject(b.second);
}

void panel::draw_button(HDC dc, const spot &sp, bool down) const
{
	round_box(dc, sp.r, down ? KEY_DOWN : KEY_FACE, KEY_EDGE, int(3 * m_scale));
}

// 大きなダイヤル。回した角度で窪みが回る
void panel::draw_wheel(HDC dc, int angle) const
{
	const POINT c = at(m_lay.dial[0], m_lay.dial[1]);
	const int r = int(m_lay.dial[2] * m_scale);

	// panel.txt で絵を渡されていれば、それを回して描く
	if (m_lay.dial_art) {
		m_lay.dial_art->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
		                     double(angle));
		return;
	}

	disc(dc, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(2 * m_scale)));
	const double a = angle * PI / 180.0;
	const int ox = c.x + int(std::sin(a) * r * 0.36);
	const int oy = c.y - int(std::cos(a) * r * 0.36);
	disc(dc, ox, oy, int(r * 0.45), RGB(186, 176, 140), RGB(146, 137, 106),
	     std::max(1, int(m_scale)));
}

// A/D INPUT のつまみ。panel.txt に adgain があるときだけ。回るだけで、まだ何にも効かない
void panel::draw_adgain(HDC dc) const
{
	if (m_lay.adgain[2] <= 0 || !m_lay.adgain_art)
		return;
	const POINT c = at(m_lay.adgain[0], m_lay.adgain[1]);
	const int r = int(m_lay.adgain[2] * m_scale);
	m_lay.adgain_art->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
	                       -135.0 + 270.0 * m_adgain_now);
}

// 音量つまみ
void panel::draw_volume(HDC dc, double v) const
{
	const POINT c = at(m_lay.volume[0], m_lay.volume[1]);
	const int r = int(m_lay.volume[2] * m_scale);
	const double deg = -135.0 + 270.0 * v;       // 左いっぱいから右いっぱいまで

	if (m_lay.volume_art) {
		m_lay.volume_art->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r }, deg);
	} else {
		disc(dc, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(m_scale)));
		const double a = deg * PI / 180.0;
		line(dc, c.x, c.y, c.x + int(std::sin(a) * r * 0.8),
		     c.y - int(std::cos(a) * r * 0.8), RGB(70, 64, 48),
		     std::max(2, int(2 * m_scale)));
	}
	if (!m_lay.labels_in_art)
		text_in(dc, scale(m_lay.volume[0] - 36, m_lay.volume[1] + m_lay.volume[2] + 4,
		                  72, 12), "VOLUME", PANEL_INK, m_font_small,
		        DT_CENTER | DT_TOP | DT_SINGLELINE);
}


// キートップの印刷（絵の組みを使うとき）。実機は太字の濃い灰色の名前と、
// 黒い丸に白抜きの記号（− ＋ ◀ ▶）。MUTE/SOLO だけ 2 行
void panel::draw_key_print(HDC dc, const RECT &key, const char *label, const char *sub,
                           mu2000::button b, bool down) const
{
	const int w = key.right - key.left, h = key.bottom - key.top;
	const int dy = down ? std::max(1, int(std::lround(m_scale))) : 0;
	const COLORREF ink = RGB(62, 60, 54);
	// f はキーの上端 0・下端 1 の割合で、**大文字の見える部分の真ん中**をそこへ置く。
	// DT_VCENTER は行の高さ（下へはみ出す分を含む）を真ん中にするので、大文字だけの
	// 名前は下に寄って見えていた（Arial の上の高さ 0.905 em、大文字の高さ 0.716 em）
	auto line_at = [&](const char *txt, double f) {
		const double cy = key.top + h * f + dy;
		RECT r{ key.left, int(std::lround(cy - (0.905 - 0.716 / 2) * m_font_key_em)), key.right, 0 };
		r.bottom = r.top + int(std::lround(1.2 * m_font_key_em)) + 1;
		text_in(dc, r, txt, ink, m_font_key, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
	};
	// 位置は実機の写真から（1 行のキーは真ん中、2 段のものは 0.33 と 0.61）
	const bool two = !std::strcmp(sub, "SOLO");
	if (two) {
		line_at("MUTE/", 0.33);
		line_at("SOLO", 0.61);
		return;
	}
	if (!sub[0]) {
		line_at(label, 0.49);
		return;
	}
	line_at(label, 0.33);

	// 記号。黒い丸に白で抜く（縁をぼかした絵があればそれを貼る）
	const int cx = key.left + w / 2, cy = key.top + int(std::lround(h * 0.61)) + dy;
	{
		const bool l = b == mu2000::button::select_left, rr = b == mu2000::button::select_right;
		const int kind = l ? 2 : rr ? 3 : (sub[0] == '+' ? 1 : 0);
		if (m_key_sym[kind] && m_key_sym[kind]->ok()) {
			const int S = m_key_sym_px;
			m_key_sym[kind]->draw(dc, RECT{ cx - S / 2, cy - S / 2, cx - S / 2 + S, cy - S / 2 + S });
			return;
		}
	}
	const int r = std::max(3, int(std::lround(4.2 * m_scale)));
	disc(dc, cx, cy, r, ink, ink, 1);
	HBRUSH white = CreateSolidBrush(RGB(236, 234, 226));
	const int a = std::max(1, int(std::lround(r * 0.6)));
	const int t = std::max(1, int(std::lround(r * 0.28)));
	const bool left = b == mu2000::button::select_left;
	const bool right = b == mu2000::button::select_right;
	if (left || right) {
		const int s2 = right ? 1 : -1;
		const POINT tri[3] = { { cx + s2 * a, cy }, { cx - s2 * a / 2, cy - a },
		                       { cx - s2 * a / 2, cy + a } };
		HGDIOBJ ob = SelectObject(dc, white);
		HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
		Polygon(dc, tri, 3);
		SelectObject(dc, op);
		SelectObject(dc, ob);
	} else {
		RECT hbar{ cx - a, cy - t / 2 - (t & 1), cx + a + 1, cy + t / 2 + 1 };
		FillRect(dc, &hbar, white);
		if (sub[0] == '+') {
			RECT vbar{ cx - t / 2 - (t & 1), cy - a, cx + t / 2 + 1, cy + a + 1 };
			FillRect(dc, &vbar, white);
		}
	}
	DeleteObject(white);
}

void panel::paint_front(HDC dc, const snapshot &s, u64 pressed, double volume,
                        const char *status) const
{
	RECT all{ 0, 0, m_w, m_h };
	fill(dc, all, RGB(24, 26, 30));
	fill(dc, scale(0, 0, LOGICAL_W, m_lay.body_h), PANEL_FACE);

	// ---- 飾り。位置も色も panel.txt から来る（doc/panel-editing.md）
	for (const deco &g : m_lay.decos) {
		if (g.k == deco::text)
			text_in(dc, scale(g.x, g.y, g.w, g.h), g.str.c_str(), g.a,
			        g.font ? m_font_label : m_font_small, g.align);
		else if (g.k == deco::disc) {
			const POINT c = at(g.x, g.y);
			disc(dc, c.x, c.y, int(g.w * m_scale), g.a, g.b,
			     std::max(1, int(g.h * m_scale)));
		} else if (g.k == deco::art) {
			if (g.pic)
				g.pic->draw(dc, scale(g.x, g.y, g.w, g.h));
		} else
			round_box(dc, scale(g.x, g.y, g.w, g.h), g.a, g.b,
			          std::max(1, int(g.radius * m_scale)));
	}

	// ---- 中

	draw_lcd(dc, s);

	// 窓の下の札は、下段の並びと同じ割合で置く。窓の中身とずれないように。
	// 札が絵に入っているときは書かない（以下の札も同じ）
	const bool labels = !m_lay.labels_in_art;
	if (labels) {
		const lcd_geom g = lcd_grid();
		const int y = at(0, m_lay.columns_y).y, h = int(12 * m_scale), w = int(64 * m_scale);
		for (const column &c : COLUMNS) {
			const int cx = int(std::lround(g.fx0 + (m_lay.low_x[c.at] + m_lay.low_w[c.at] / 2) * g.df));
			RECT r{ cx - w / 2, y, cx + w / 2, y + h };
			text_in(dc, r, c.label, PANEL_INK, m_font_small,
			        DT_CENTER | DT_TOP | DT_SINGLELINE);
		}

		// 窓の右の札。高さは液晶の中の ▶ に合わせる（横は panel.txt の modes.x）
		if (m_lay.modes_x >= 0) {
			const int x = at(m_lay.modes_x, 0).x;
			// ▶ の間隔より字が大きいと重なる（小さい窓で、字の下限が効くとき）
			const double step = (MODE_Y[2] - MODE_Y[1]) * LOW_DOT * g.df;
			const HFONT font = step < std::max(7.0, 8.5 * m_scale) ? m_font_tiny : m_font_small;
			for (int k = 0; k < 3; k++) {
				const int cy = int(std::lround(g.fsy + MODE_Y[k + 1] * LOW_DOT * g.df));
				RECT r{ x, cy - h, x + int(80 * m_scale), cy + h };
				text_in(dc, r, MODE_LABEL[k], PANEL_INK, font,
				        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
			}
		}
	}

	for (int i = 0; i < 18 && labels; i++)
		text_in(dc, scale(m_lay.cat_x[i % 6] - 34, m_lay.cat_y[i / 6] - 14, 68, 14),
		        CAT_LABEL[i], PANEL_INK, m_font_small,
		        DT_CENTER | DT_TOP | DT_SINGLELINE);

	// MU / PLG-1..3 の表示灯。LED は 6 番から
	{
		const char *plg[4] = { "MU", "PLG-1", "PLG-2", "PLG-3" };
		for (int i = 0; i < 4; i++) {
			const double px = m_lay.plg[0] + i * m_lay.plg[1];
			const POINT c = at(px, m_lay.plg[2]);
			const bool on = BIT(s.leds, 6 + i) != 0;
			if (const svg_art *pic = m_lay.plg_art.pick(on, false)) {
				const int rw = int(m_lay.plg_size[0] / 2 * m_scale);
				const int rh = int(m_lay.plg_size[1] / 2 * m_scale);
				pic->draw(dc, RECT{ c.x - rw, c.y - rh, c.x + rw, c.y + rh });
			} else {
				// 実機の表示灯は四角
				const int r = int(4 * m_scale);
				round_box(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
				          on ? LED_ON : RGB(64, 62, 52), RGB(110, 106, 92), std::max(1, int(m_scale)));
			}
			if (labels)
				text_in(dc, scale(px - 22, m_lay.plg[2] + 7, 44, 12), plg[i], PANEL_INK,
				        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
	}

	// ---- 右

	for (int i = 0; i < 6; i++) {
		const mode_button &m = MODES[i];
		const double mx = m_lay.mode[i][0], my = m_lay.mode[i][1];
		if (labels)
			text_in(dc, scale(mx - 34, my - m_lay.mode_r - 19, 68, 14), m.label, PANEL_INK,
			        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		const POINT c = at(mx, my);
		const bool down = ((pressed >> int(m.b)) & 1) != 0;
		const bool on = BIT(s.leds, m.led) != 0;
		// panel.txt で絵を渡されていれば、ようすに合う 1 枚を貼る
		const svg_art *pic = m_lay.mode_art.pick(on, down);
		if (on && !down && m_lay.mode_on[i])
			pic = m_lay.mode_on[i].get();
		if (pic) {
			const int r = int(m_lay.mode_r * m_scale);
			pic->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r });
		} else {
			disc(dc, c.x, c.y, int(m_lay.mode_r * m_scale),
			     down ? KEY_DOWN : RGB(198, 188, 152), KEY_EDGE, std::max(1, int(m_scale)));
			disc(dc, c.x, c.y, int(m_lay.mode_led_r * m_scale),
			     on ? LED_ON : RGB(74, 72, 60), RGB(110, 106, 92), 1);
		}
	}

	// 四角いボタン。名札は上に重ねる
	for (int i = 0; i < 9; i++) {
		const place &p = NAV[i];
		const double px = m_lay.nav[i][0], py = m_lay.nav[i][1];
		const double pw = m_lay.nav[i][2], ph = m_lay.nav[i][3];
		const spot *sp = nullptr;
		for (const spot &q : m_spots)
			if (q.kind == spot_kind::button && q.button == p.b) { sp = &q; break; }
		if (!sp)
			continue;
		const bool down = ((pressed >> int(p.b)) & 1) != 0;
		// 印刷まで入ったキーごとの絵があれば、それだけ貼る
		if (const svg_art *face = m_lay.nav_face[i].pick(down, down)) {
			face->draw(dc, sp->r);
			continue;
		}
		if (const svg_art *pic = m_lay.nav_art.pick(down, down))
			pic->draw(dc, sp->r);
		else
			draw_button(dc, *sp, down);
		if (m_lay.labels_in_art) {
			draw_key_print(dc, sp->r, p.label, p.sub, p.b, down);
			continue;
		}
		text_in(dc, scale(px, py + 4, pw, 12), p.label, RGB(58, 53, 38),
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		if (p.sub[0])
			text_in(dc, scale(px, py + ph - 14, pw, 12), p.sub, RGB(58, 53, 38),
			        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
	}
	for (int i = 0; i < 18; i++) {
		RECT r = scale(m_lay.cat_x[i % 6] - m_lay.cat_w / 2, m_lay.cat_y[i / 6],
		               m_lay.cat_w, m_lay.cat_h);
		const bool down = ((pressed >> int(CAT_B[i])) & 1) != 0;
		if (const svg_art *pic = m_lay.cat_art.pick(down, down))
			pic->draw(dc, r);
		else
			round_box(dc, r, down ? KEY_DOWN : KEY_FACE, KEY_EDGE, int(3 * m_scale));
	}
	for (int i = 0; i < 2; i++) {
		const place &p = ROUND[i];
		const double px = m_lay.round_[i][0], py = m_lay.round_[i][1];
		const POINT c = at(px, py);
		const bool down = ((pressed >> int(p.b)) & 1) != 0;
		const int rr = int(m_lay.round_[i][2] / 2 * m_scale);
		if (const svg_art *pic = m_lay.round_art.pick(down, down))
			pic->draw(dc, RECT{ c.x - rr, c.y - rr, c.x + rr, c.y + rr });
		else
			disc(dc, c.x, c.y, rr, down ? KEY_DOWN : KEY_FACE, KEY_EDGE,
			     std::max(1, int(m_scale)));
		if (labels)
			text_in(dc, scale(px - 40, py - 26, 80, 12), p.label, PANEL_INK,
			        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
	}

	draw_wheel(dc, m_wheel_angle);
	draw_volume(dc, volume);
	draw_adgain(dc);

	// 状態の行は本体の一番下（body_h の内側）に載るので、ボタンの名前と同じ濃い色で書く。
	// 前は暗い帯向けの薄い灰色で、本体の地の色に溶けて読めなかった
	if (status && status[0])
		text_in(dc, m_status, status, PANEL_INK, m_font_small,
		        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	text_in(dc, m_hint, UI_TEXT(hint_front, "Turn the big dial with the wheel / click buttons / "
                             "keys: A=PLAY E=EDIT U=UTIL F=EFFECT [ ]=PART"),
	        RGB(120, 124, 130), m_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	draw_tabs(dc);
}

// 論理座標の方眼。50 ごとに線、100 ごとに濃い線と数字を入れる。
// 絵の位置を直すときは、これを出して読み取ってから表を書き換える
void panel::draw_grid(HDC dc) const
{
	HPEN thin = CreatePen(PS_SOLID, 1, RGB(255, 80, 80));
	HPEN bold = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
	HGDIOBJ op = SelectObject(dc, thin);
	SetBkMode(dc, TRANSPARENT);

	for (int x = 0; x <= LOGICAL_W; x += 50) {
		SelectObject(dc, (x % 100) ? thin : bold);
		const POINT a = at(x, 0), b = at(x, LOGICAL_H);
		MoveToEx(dc, a.x, a.y, nullptr);
		LineTo(dc, b.x, b.y);
	}
	for (int y = 0; y <= LOGICAL_H; y += 50) {
		SelectObject(dc, (y % 100) ? thin : bold);
		const POINT a = at(0, y), b = at(LOGICAL_W, y);
		MoveToEx(dc, a.x, a.y, nullptr);
		LineTo(dc, b.x, b.y);
	}
	for (int x = 0; x <= LOGICAL_W; x += 100)
		for (int y = 0; y <= LOGICAL_H; y += 100) {
			char n[32];
			std::snprintf(n, sizeof(n), "%d,%d", x, y);
			RECT r{ at(x + 2, y + 1).x, at(x + 2, y + 1).y,
			        at(x + 60, y + 12).x, at(x + 60, y + 12).y };
			text_in(dc, r, n, RGB(200, 0, 0), m_font_small,
			        DT_LEFT | DT_TOP | DT_SINGLELINE);
		}

	SelectObject(dc, op);
	DeleteObject(thin);
	DeleteObject(bold);
}

void panel::paint(HDC dc, const snapshot &s, u64 pressed, const char *status) const
{
	if (m_lcd_only) {
		draw_lcd(dc, s);
		return;
	}
	if (m_page == page::editor)       paint_editor(dc, status);
	else if (m_page == page::effects) paint_effects(dc, status);
	else                              paint_front(dc, s, pressed, m_volume_now, status);
	if (m_grid)
		draw_grid(dc);
}

} // namespace ui

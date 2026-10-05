// license:BSD-3-Clause

#include "svg.h"
#include "png.h"

#include "ui/draw_imgui.h"
#include "ui/tex.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <mapbox/earcut.hpp>

namespace ui {

namespace {

// 2 × 3 の変換。点は (a x + c y + e, b x + d y + f) へ移る
struct mat {
	double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

	mat mul(const mat &m) const           // this を先、m をあと
	{
		mat r;
		r.a = a * m.a + b * m.c;
		r.b = a * m.b + b * m.d;
		r.c = c * m.a + d * m.c;
		r.d = c * m.b + d * m.d;
		r.e = e * m.a + f * m.c + m.e;
		r.f = e * m.b + f * m.d + m.f;
		return r;
	}
	void apply(double x, double y, double &ox, double &oy) const
	{
		ox = a * x + c * y + e;
		oy = b * x + d * y + f;
	}
};

// 数を 1 つ取り出す。SVG は区切りに空白でも , でも - でも来る
bool take_num(const char *&p, double &out)
{
	while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (!*p)
		return false;
	char *end = nullptr;
	out = std::strtod(p, &end);
	if (end == p)
		return false;
	p = end;
	return true;
}

bool is_cmd(char c)
{
	return std::strchr("MmLlHhVvCcZzAaQqSsTt", c) != nullptr;
}

// 属性を 1 つ取り出す。name="…" の中身。
// 名前の前が空白か < のものだけを見る（d を探して id="…" の中の d=" に当たらないように。
// width を探して stroke-width に当たらないように。Inkscape で保存した絵は id が d の前に来ることがある）
std::string attr(const std::string &tag, const char *name)
{
	const std::string key = std::string(name) + "=\"";
	size_t at = 0;
	while ((at = tag.find(key, at)) != std::string::npos) {
		const char before = at ? tag[at - 1] : ' ';
		if (before == ' ' || before == '\t' || before == '\n' || before == '\r' || before == '<')
			break;
		at += key.size();
	}
	if (at == std::string::npos)
		return {};
	const size_t start = at + key.size();
	const size_t end = tag.find('"', start);
	return tag.substr(start, end == std::string::npos ? end : end - start);
}

// style="fill:#404040;stroke:none;…" から 1 つ
std::string style_of(const std::string &style, const char *name)
{
	size_t at = 0;
	const std::string key(name);
	while ((at = style.find(key, at)) != std::string::npos) {
		// 前が区切りで、後ろが : であること（fill と fill-opacity を混ぜない）
		const bool head = (at == 0 || style[at - 1] == ';' || style[at - 1] == ' ');
		const size_t colon = at + key.size();
		if (head && colon < style.size() && style[colon] == ':') {
			const size_t end = style.find(';', colon);
			std::string v = style.substr(colon + 1,
			                             end == std::string::npos ? end : end - colon - 1);
			while (!v.empty() && v.front() == ' ')
				v.erase(0, 1);
			while (!v.empty() && v.back() == ' ')
				v.pop_back();
			return v;
		}
		at += key.size();
	}
	return {};
}

bool parse_color(const std::string &v, COLORREF &out)
{
	if (v.empty() || v == "none")
		return false;
	if (v[0] == '#') {
		unsigned n = 0;
		if (v.size() == 7 && std::sscanf(v.c_str() + 1, "%6x", &n) == 1) {
			out = RGB((n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff);
			return true;
		}
		if (v.size() == 4 && std::sscanf(v.c_str() + 1, "%3x", &n) == 1) {
			const int r = (n >> 8) & 0xf, g = (n >> 4) & 0xf, b = n & 0xf;
			out = RGB(r * 17, g * 17, b * 17);
			return true;
		}
		return false;
	}
	if (v == "black") { out = RGB(0, 0, 0); return true; }
	if (v == "white") { out = RGB(255, 255, 255); return true; }
	return false;
}

mat parse_transform(const std::string &t)
{
	mat m;
	if (t.empty())
		return m;
	const char *p = t.c_str();
	if (const char *q = std::strstr(p, "matrix(")) {
		q += 7;
		double v[6] = { 1, 0, 0, 1, 0, 0 };
		for (int i = 0; i < 6; i++)
			take_num(q, v[i]);
		m.a = v[0]; m.b = v[1]; m.c = v[2]; m.d = v[3]; m.e = v[4]; m.f = v[5];
	} else if (const char *q = std::strstr(p, "translate(")) {
		q += 10;
		double x = 0, y = 0;
		take_num(q, x);
		take_num(q, y);
		m.e = x; m.f = y;
	} else if (const char *q = std::strstr(p, "scale(")) {
		q += 6;
		double x = 1, y = 0;
		take_num(q, x);
		if (!take_num(q, y))
			y = x;
		m.a = x; m.d = y;
	}
	return m;
}

// 数の属性（"12.5" や "12.5px"）。無ければ def
double num_attr(const std::string &tag, const char *name, double def = 0.0)
{
	const std::string v = attr(tag, name);
	return v.empty() ? def : std::atof(v.c_str());
}

// 四角・丸・楕円・多角形を、同じ形のパスの d に直す（読み手は d だけを読む）。
// 丸みは 3 次ベジエで近づける（弧の命令 A は読まないので）
std::string shape_to_d(const std::string &name, const std::string &tag)
{
	char buf[512];
	constexpr double K = 0.5522847498;       // 円の 1/4 をベジエで描くときの係数
	auto ellipse = [&](double cx, double cy, double rx, double ry) {
		std::snprintf(buf, sizeof(buf),
		              "M %g,%g C %g,%g %g,%g %g,%g C %g,%g %g,%g %g,%g C %g,%g %g,%g %g,%g C %g,%g %g,%g %g,%g Z",
		              cx + rx, cy,
		              cx + rx, cy + ry * K, cx + rx * K, cy + ry, cx, cy + ry,
		              cx - rx * K, cy + ry, cx - rx, cy + ry * K, cx - rx, cy,
		              cx - rx, cy - ry * K, cx - rx * K, cy - ry, cx, cy - ry,
		              cx + rx * K, cy - ry, cx + rx, cy - ry * K, cx + rx, cy);
		return std::string(buf);
	};
	if (name == "circle") {
		const double r = num_attr(tag, "r");
		return r > 0 ? ellipse(num_attr(tag, "cx"), num_attr(tag, "cy"), r, r) : std::string();
	}
	if (name == "ellipse") {
		const double rx = num_attr(tag, "rx"), ry = num_attr(tag, "ry");
		return rx > 0 && ry > 0 ? ellipse(num_attr(tag, "cx"), num_attr(tag, "cy"), rx, ry) : std::string();
	}
	if (name == "rect") {
		const double x = num_attr(tag, "x"), y = num_attr(tag, "y");
		const double w = num_attr(tag, "width"), h = num_attr(tag, "height");
		if (w <= 0 || h <= 0)
			return {};
		// 片方だけ書いてあれば、もう片方も同じ（SVG の決まり）
		double rx = num_attr(tag, "rx", -1), ry = num_attr(tag, "ry", -1);
		if (rx < 0) rx = ry;
		if (ry < 0) ry = rx;
		rx = std::clamp(rx, 0.0, w / 2);
		ry = std::clamp(ry, 0.0, h / 2);
		if (rx <= 0 || ry <= 0) {
			std::snprintf(buf, sizeof(buf), "M %g,%g L %g,%g L %g,%g L %g,%g Z", x, y, x + w, y, x + w, y + h, x, y + h);
			return buf;
		}
		std::snprintf(buf, sizeof(buf),
		              "M %g,%g L %g,%g C %g,%g %g,%g %g,%g L %g,%g C %g,%g %g,%g %g,%g "
		              "L %g,%g C %g,%g %g,%g %g,%g L %g,%g C %g,%g %g,%g %g,%g Z",
		              x + rx, y, x + w - rx, y,
		              x + w - rx + rx * K, y, x + w, y + ry - ry * K, x + w, y + ry,
		              x + w, y + h - ry,
		              x + w, y + h - ry + ry * K, x + w - rx + rx * K, y + h, x + w - rx, y + h,
		              x + rx, y + h,
		              x + rx - rx * K, y + h, x, y + h - ry + ry * K, x, y + h - ry,
		              x, y + ry,
		              x, y + ry - ry * K, x + rx - rx * K, y, x + rx, y);
		return buf;
	}
	if (name == "polygon" || name == "polyline") {
		const std::string pts = attr(tag, "points");
		const char *q = pts.c_str();
		std::string d;
		double px, py;
		bool first = true;
		while (take_num(q, px) && take_num(q, py)) {
			std::snprintf(buf, sizeof(buf), "%s %g,%g ", first ? "M" : "L", px, py);
			d += buf;
			first = false;
		}
		if (!d.empty() && name == "polygon")
			d += "Z";
		return d;
	}
	return {};
}

} // namespace


bool svg_art::load_file(const std::string &path)
{
	if (path.size() > 4) {
		std::string ext = path.substr(path.size() - 4);
		for (char &c : ext)
			c = char(std::tolower(static_cast<unsigned char>(c)));
		if (ext == ".png") {
			clear();
			return load_png(path);
		}
	}
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::string all;
	char buf[8192];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		all.append(buf, n);
	std::fclose(f);
	return load_text(all);
}

bool svg_art::load_text(const std::string &text)
{
	clear();

	// コメントは読まない（<!-- --> の中に残した古い形を描かないように）
	std::string s;
	s.reserve(text.size());
	for (size_t at = 0; at < text.size();) {
		const size_t open = text.find("<!--", at);
		if (open == std::string::npos) {
			s.append(text, at, std::string::npos);
			break;
		}
		s.append(text, at, open - at);
		const size_t close = text.find("-->", open + 4);
		if (close == std::string::npos)
			break;
		at = close + 3;
	}

	// viewBox。無ければ width / height を使う
	{
		const std::string vb = attr(s, "viewBox");
		if (!vb.empty()) {
			const char *p = vb.c_str();
			for (int i = 0; i < 4; i++)
				take_num(p, m_vb[i]);
		} else {
			m_vb[0] = m_vb[1] = 0;
			m_vb[2] = std::atof(attr(s, "width").c_str());
			m_vb[3] = std::atof(attr(s, "height").c_str());
		}
		if (m_vb[2] <= 0 || m_vb[3] <= 0)
			return false;
	}

	// <g> の変換を 1 段だけ拾う。MAME の絵はこれで足りる
	mat gm;
	{
		const size_t at = s.find("<g ");
		if (at != std::string::npos) {
			const size_t end = s.find('>', at);
			gm = parse_transform(attr(s.substr(at, end - at), "transform"));
		}
	}

	// 形は書いてある順に描く。path のほか、rect・circle・ellipse・polygon・polyline も読む
	// （パスに直して同じように扱う）
	static const char *const ELEMENTS[] = { "path", "rect", "circle", "ellipse", "polygon", "polyline" };
	size_t at = 0;
	for (;;) {
		size_t found = std::string::npos;
		std::string name;
		for (const char *e : ELEMENTS) {
			const std::string open = std::string("<") + e;
			size_t f = at;
			while ((f = s.find(open, f)) != std::string::npos) {
				const char next = f + open.size() < s.size() ? s[f + open.size()] : '>';
				if (next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '/' || next == '>')
					break;
				f += open.size();            // <pathology> のような別の名前
			}
			if (f < found) {
				found = f;
				name = e;
			}
		}
		if (found == std::string::npos)
			break;
		const size_t end = s.find('>', found);
		if (end == std::string::npos)
			break;
		const std::string tag = s.substr(found, end - found);
		at = end + 1;

		const std::string d = name == "path" ? attr(tag, "d") : shape_to_d(name, tag);
		if (d.empty())
			continue;
		const std::string style = attr(tag, "style");
		const mat pm = parse_transform(attr(tag, "transform"));
		const mat m = pm.mul(gm);          // 自分の変換を先、g の変換をあと

		shape sh;
		COLORREF c = 0;
		std::string v = style_of(style, "fill");
		if (v.empty())
			v = attr(tag, "fill");
		if (parse_color(v, c)) {
			sh.fill = c;
			sh.has_fill = true;
		}
		v = style_of(style, "stroke");
		if (v.empty())
			v = attr(tag, "stroke");
		if (parse_color(v, c)) {
			sh.stroke = c;
			sh.has_stroke = true;
			const std::string w = style_of(style, "stroke-width");
			sh.stroke_w = w.empty() ? 1.0 : std::atof(w.c_str());
		}
		if (!sh.has_fill && !sh.has_stroke)
			continue;

		// d を読む。曲線はここで折れ線にしておく
		std::vector<pt> cur;
		double x = 0, y = 0, sx = 0, sy = 0;
		char cmd = 0;
		const char *p = d.c_str();
		auto push = [&](double px, double py) {
			double ox, oy;
			m.apply(px, py, ox, oy);
			cur.push_back({ ox, oy });
		};
		auto flush = [&](bool closed) {
			if (cur.size() >= 2) {
				sh.subs.push_back(cur);
				sh.closed.push_back(closed);
			}
			cur.clear();
		};

		while (*p) {
			while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')
				p++;
			if (!*p)
				break;
			if (is_cmd(*p)) {
				cmd = *p++;
			} else if (!cmd) {
				p++;
				continue;
			}

			const bool rel = (cmd >= 'a' && cmd <= 'z');
			const char c2 = char(std::toupper((unsigned char)cmd));

			if (c2 == 'Z') {
				flush(true);
				x = sx; y = sy;
				cmd = 0;
				continue;
			}

			double a1, a2, a3, a4, a5, a6;
			if (c2 == 'M') {
				if (!take_num(p, a1) || !take_num(p, a2)) break;
				if (rel) { a1 += x; a2 += y; }
				flush(false);
				x = a1; y = a2; sx = x; sy = y;
				push(x, y);
				cmd = rel ? 'l' : 'L';           // 続きは線として読む
			} else if (c2 == 'L') {
				if (!take_num(p, a1) || !take_num(p, a2)) break;
				if (rel) { a1 += x; a2 += y; }
				x = a1; y = a2;
				push(x, y);
			} else if (c2 == 'H') {
				if (!take_num(p, a1)) break;
				x = rel ? x + a1 : a1;
				push(x, y);
			} else if (c2 == 'V') {
				if (!take_num(p, a1)) break;
				y = rel ? y + a1 : a1;
				push(x, y);
			} else if (c2 == 'C') {
				if (!take_num(p, a1) || !take_num(p, a2) || !take_num(p, a3) ||
				    !take_num(p, a4) || !take_num(p, a5) || !take_num(p, a6))
					break;
				if (rel) { a1 += x; a2 += y; a3 += x; a4 += y; a5 += x; a6 += y; }
				const double x0 = x, y0 = y;
				const int steps = 12;
				for (int i = 1; i <= steps; i++) {
					const double t = double(i) / steps, u = 1 - t;
					const double bx = u * u * u * x0 + 3 * u * u * t * a1 +
					                  3 * u * t * t * a3 + t * t * t * a5;
					const double by = u * u * u * y0 + 3 * u * u * t * a2 +
					                  3 * u * t * t * a4 + t * t * t * a6;
					push(bx, by);
				}
				x = a5; y = a6;
			} else {
				// 読まない命令。数を食い潰して次へ。数でも命令でもない字なら 1 つ飛ばす（止まらないように）
				double junk;
				const char *before = p;
				while (*p && !is_cmd(*p) && take_num(p, junk))
					;
				if (p == before && *p && !is_cmd(*p)) {
					p++;
					cmd = 0;
				}
			}
		}
		flush(false);

		if (!sh.subs.empty())
			m_shapes.push_back(std::move(sh));
	}
	return ok();
}

// ---- filling one shape ------------------------------------------------------
//
// The GDI build filled every subpath of a shape in a single PolyPolygon call
// with ALTERNATE mode: a pixel is painted when an odd number of contours cover
// it. ImGui has no multi-contour fill, so each painted region is triangulated
// instead: every loop nested at an even depth paints (a root covers once),
// minus the loops it directly contains. Same rule, by construction.
//
// The triangulation is earcut (third_party/earcut.hpp): outline plus holes in,
// triangles out. Everything runs in SVG units, in doubles; the screen transform
// happens once, at emission.
//
// Hole-free loops go straight to AddConcavePolyFilled, which is exactly what
// that helper is good at. It must never see a loop with holes: with no ear to
// clip it emits a fan from vertex zero instead of failing.

struct fpt { double x, y; };

struct floop {
	std::vector<fpt> pts;
	double area = 0;             // signed; y grows down, so > 0 is clockwise
	int parent = -1;             // tightest containing loop, or -1
	int depth = 0;               // nesting depth; even depths paint
};

// SVG units to screen pixels, applied once, at emission.
struct fmap {
	double ox, oy, k, cx, cy, cs, sn;
	bool turn;
	ImVec2 operator()(const fpt &q) const
	{
		double px = ox + q.x * k, py = oy + q.y * k;
		if (turn) {
			const double dx = px - cx, dy = py - cy;
			px = cx + dx * cs - dy * sn;
			py = cy + dx * sn + dy * cs;
		}
		return ImVec2(float(px), float(py));
	}
};

static double floop_area(const std::vector<fpt> &p)
{
	double a = 0;
	for (size_t i = 0; i < p.size(); i++) {
		const fpt &u = p[i], &v = p[(i + 1) % p.size()];
		a += u.x * v.y - v.x * u.y;
	}
	return a / 2;
}

static bool floop_contains(const std::vector<fpt> &p, fpt q)
{
	bool in = false;
	for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
		const fpt &a = p[i], &b = p[j];
		if ((a.y > q.y) != (b.y > q.y) &&
		    q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x)
			in = !in;
	}
	return in;
}

// Emit one triangulated region with ImGui's own antialiased fringe. boundary
// holds the loops the region is bounded by -- the outline, then each hole --
// hole -- all wound so the paint sits on the same side, which is what makes
// one fringe routine serve both. tris indexes into verts; everything arrives
// in SVG units and lands on pixels here, for the first and only time.
static void floop_emit(ImDrawList *dl, const fmap &map,
                       const std::vector<fpt> &verts,
                       const std::vector<unsigned> &tris,
                       const std::vector<std::vector<fpt>> &boundary, ImU32 col)
{
	if (tris.empty() || (col & IM_COL32_A_MASK) == 0)
		return;
	std::vector<ImVec2> xy;
	xy.reserve(verts.size());
	for (const fpt &v : verts)
		xy.push_back(map(v));
	std::vector<std::vector<ImVec2>> edge;
	edge.reserve(boundary.size());
	for (const auto &loop : boundary) {
		edge.emplace_back();
		for (const fpt &v : loop)
			edge.back().push_back(map(v));
	}
	const ImVec2 uv = dl->_Data->TexUvWhitePixel;
	if ((dl->Flags & ImDrawListFlags_AntiAliasedFill) == 0) {
		const unsigned base = dl->_VtxCurrentIdx;
		dl->PrimReserve(int(tris.size()), int(xy.size()));
		for (const ImVec2 &v : xy) {
			dl->_VtxWritePtr[0].pos = v;
			dl->_VtxWritePtr[0].uv = uv;
			dl->_VtxWritePtr[0].col = col;
			dl->_VtxWritePtr++;
		}
		for (unsigned t : tris) {
			dl->_IdxWritePtr[0] = ImDrawIdx(base + t);
			dl->_IdxWritePtr++;
		}
		dl->_VtxCurrentIdx = ImDrawIdx(base + xy.size());
		return;
	}
	const float AA_SIZE = dl->_FringeScale;
	const ImU32 col_trans = col & ~IM_COL32_A_MASK;
	size_t fringe_pts = 0;
	for (const auto &loop : edge)
		fringe_pts += loop.size();
	const unsigned base = dl->_VtxCurrentIdx;
	dl->PrimReserve(int(tris.size()) + int(fringe_pts) * 6,
	                int(xy.size()) + int(fringe_pts) * 2);
	for (const ImVec2 &v : xy) {
		dl->_VtxWritePtr[0].pos = v;
		dl->_VtxWritePtr[0].uv = uv;
		dl->_VtxWritePtr[0].col = col;
		dl->_VtxWritePtr++;
	}
	for (unsigned t : tris) {
		dl->_IdxWritePtr[0] = ImDrawIdx(base + t);
		dl->_IdxWritePtr++;
	}
	unsigned fringe = base + unsigned(xy.size());
	for (const auto &loop : edge) {
		const size_t m = loop.size();
		if (m < 2)
			continue;
		std::vector<ImVec2> normals(m);
		for (size_t i = 0; i < m; i++) {
			const ImVec2 &p0 = loop[(i + m - 1) % m];
			const ImVec2 &p1 = loop[i];
			float dx = p1.x - p0.x, dy = p1.y - p0.y;
			const float len = std::hypot(dx, dy);
			if (len > 0) {
				dx /= len;
				dy /= len;
			}
			normals[i] = ImVec2(dy, -dx);
		}
		for (size_t i1 = 0; i1 < m; i1++) {
			const size_t i0 = (i1 + m - 1) % m;
			float dmx = (normals[i0].x + normals[i1].x) * 0.5f;
			float dmy = (normals[i0].y + normals[i1].y) * 0.5f;
			float d2 = dmx * dmx + dmy * dmy;
			if (d2 < 0.25f) {
				dmx = 0.0f;
				dmy = 1.0f;
			} else {
				const float d = std::sqrt(d2);
				dmx /= d;
				dmy /= d;
			}
			dmx *= AA_SIZE * 0.5f;
			dmy *= AA_SIZE * 0.5f;
			const ImVec2 &q = loop[i1];
			dl->_VtxWritePtr[0].pos = ImVec2(q.x - dmx, q.y - dmy);
			dl->_VtxWritePtr[0].uv = uv;
			dl->_VtxWritePtr[0].col = col;
			dl->_VtxWritePtr++;
			dl->_VtxWritePtr[0].pos = ImVec2(q.x + dmx, q.y + dmy);
			dl->_VtxWritePtr[0].uv = uv;
			dl->_VtxWritePtr[0].col = col_trans;
			dl->_VtxWritePtr++;
			const unsigned inner1 = fringe + unsigned(i1) * 2;
			const unsigned inner0 = fringe + unsigned(i0) * 2;
			dl->_IdxWritePtr[0] = ImDrawIdx(inner1);
			dl->_IdxWritePtr[1] = ImDrawIdx(inner0);
			dl->_IdxWritePtr[2] = ImDrawIdx(inner0 + 1);
			dl->_IdxWritePtr[3] = ImDrawIdx(inner0 + 1);
			dl->_IdxWritePtr[4] = ImDrawIdx(inner1 + 1);
			dl->_IdxWritePtr[5] = ImDrawIdx(inner1);
			dl->_IdxWritePtr += 6;
		}
		fringe += unsigned(m) * 2;
	}
	dl->_VtxCurrentIdx = ImDrawIdx(fringe);
}

// Paint one filled shape under the even-odd rule. tol is one square pixel in
// SVG units: the triangulation check below refuses anything further out.
static void floop_fill(ImDrawList *dl, const fmap &map, std::vector<floop> &loops,
                       ImU32 col, double tol)
{
	if (loops.empty() || (col & IM_COL32_A_MASK) == 0)
		return;
	if (loops.size() == 1) {                   // no nesting to work out
		const auto &p = loops[0].pts;
		if (p.size() < 3)
			return;
		std::vector<ImVec2> xy;
		for (const fpt &v : p)
			xy.push_back(map(v));
		dl->AddConcavePolyFilled(xy.data(), int(xy.size()), col);
		return;
	}
	// Nesting, tightest container first: bounding boxes reject nearly every
	// pair before the crossing test runs.
	std::vector<double> box;
	box.reserve(loops.size() * 4);
	for (const floop &l : loops) {
		double b[4] = { 1e30, 1e30, -1e30, -1e30 };
		for (const fpt &q : l.pts) {
			b[0] = std::min(b[0], q.x);
			b[1] = std::min(b[1], q.y);
			b[2] = std::max(b[2], q.x);
			b[3] = std::max(b[3], q.y);
		}
		box.insert(box.end(), b, b + 4);
	}
	for (size_t i = 0; i < loops.size(); i++) {
		const fpt probe = loops[i].pts[0];
		for (size_t j = 0; j < loops.size(); j++) {
			if (i == j)
				continue;
			if (probe.x < box[j * 4] || probe.x > box[j * 4 + 2] ||
			    probe.y < box[j * 4 + 1] || probe.y > box[j * 4 + 3])
				continue;
			if (!floop_contains(loops[j].pts, probe))
				continue;
			if (loops[i].parent < 0 ||
			    std::fabs(loops[j].area) < std::fabs(loops[loops[i].parent].area))
				loops[i].parent = int(j);
		}
	}
	for (size_t i = 0; i < loops.size(); i++) {
		loops[i].depth = 0;
		for (int p = loops[i].parent; p >= 0; p = loops[p].parent)
			loops[i].depth++;
	}
	for (size_t i = 0; i < loops.size(); i++) {
		// Even-odd: a root covers once, so it paints; each level of nesting
		// flips it. Depth counts containers, so roots sit at zero and paint.
		if (loops[i].depth % 2 != 0)
			continue;                           // odd depth stays unpainted
		// Clockwise on screen, the winding ImGui fringes for.
		if (loops[i].area < 0)
			std::reverse(loops[i].pts.begin(), loops[i].pts.end());
		// The outline and its holes, as earcut wants them: one outer ring
		// followed by the holes. Holes run the other way; the fringe below
		// needs the same convention, so the normalized copies serve both.
		using ring = std::vector<std::array<double, 2>>;
		std::vector<ring> poly;
		std::vector<std::vector<fpt>> boundary;
		poly.reserve(loops.size());
		boundary.push_back(loops[i].pts);
		{
			ring outer;
			for (const fpt &v : loops[i].pts)
				outer.push_back({ v.x, v.y });
			poly.push_back(std::move(outer));
		}
		std::vector<size_t> holes;
		for (size_t j = 0; j < loops.size(); j++)
			if (loops[j].parent == int(i))
				holes.push_back(j);
		for (size_t h : holes) {
			std::vector<fpt> hole = loops[h].pts;
			if (floop_area(hole) > 0)
				std::reverse(hole.begin(), hole.end());
			ring hr;
			for (const fpt &v : hole)
				hr.push_back({ v.x, v.y });
			poly.push_back(std::move(hr));
			boundary.push_back(std::move(hole));
		}
		std::vector<unsigned> tris = mapbox::earcut<unsigned>(poly);
		// Vertices in earcut's order: the outline, then each hole in turn.
		// Same normalized copies as above, so every index lands correctly.
		std::vector<fpt> verts;
		for (const auto &loop : boundary)
			for (const fpt &v : loop)
				verts.push_back(v);
		std::vector<ImVec2> flat0;
		flat0.reserve(loops[i].pts.size());
		for (const fpt &v : loops[i].pts)
			flat0.push_back(map(v));
		// The triangulation must account for the outline minus the holes, and
		// nothing else. Anything else is refused in favour of the outline
		// whole: visible where a hole fills in, but bounded -- never a spike.
		double want = std::fabs(floop_area(loops[i].pts));
		for (size_t h : holes)
			want -= std::fabs(floop_area(loops[h].pts));
		double got = 0;
		for (size_t t = 0; t + 2 < tris.size(); t += 3) {
			const unsigned a = tris[t], b = tris[t + 1], c = tris[t + 2];
			if (a >= verts.size() || b >= verts.size() || c >= verts.size())
				break;                          // corrupt output: fail the check below
			got += std::fabs(floop_area({ verts[a], verts[b], verts[c] }));
		}
		if (tris.empty() || (want > 0 && std::fabs(got - want) > 0.02 * want + tol))
			dl->AddConcavePolyFilled(flat0.data(), int(flat0.size()), col);
		else
			floop_emit(dl, map, verts, tris, boundary, col);
	}
}

void svg_art::draw(ImDrawList *dl, const RECT &dst, double deg) const
{
	if (!m_mips.empty()) {
		draw_image(dl, dst, deg);
		return;
	}
	if (m_shapes.empty())
		return;

	const double dw = double(dst.right - dst.left), dh = double(dst.bottom - dst.top);
	if (dw <= 0 || dh <= 0)
		return;
	const double k = std::min(dw / m_vb[2], dh / m_vb[3]);
	const double ox = dst.left + (dw - m_vb[2] * k) / 2 - m_vb[0] * k;
	const double oy = dst.top  + (dh - m_vb[3] * k) / 2 - m_vb[1] * k;

	const double cx = (dst.left + dst.right) / 2.0;
	const double cy = (dst.top + dst.bottom) / 2.0;
	const double rad = deg * 3.14159265358979 / 180.0;
	const double cs = std::cos(rad), sn = std::sin(rad);
	const bool turn = (deg != 0.0);

	std::vector<ImVec2> pts;

	for (const shape &sh : m_shapes) {
		if (sh.has_fill) {
			// The whole path at once, so the even-odd rule in floop_fill sees
			// every contour together: filling them one by one would lose the
			// holes, which is how the jacks came out as black discs. The loops
			// stay in SVG units here; the screen transform happens once, at
			// emission, so topology never sees a rounded pixel.
			const fmap map{ ox, oy, k, cx, cy, cs, sn, turn };
			const double step = 0.5 / k;         // half a screen pixel, in SVG units
			std::vector<floop> loops;
			for (const auto &sub : sh.subs) {
				floop l;
				for (const pt &q : sub) {
					if (l.pts.empty() || l.pts.back().x != q.x || l.pts.back().y != q.y)
						l.pts.push_back({ q.x, q.y });
				}
				// A flattened curve ends where it started, up to float dust: the
				// last point is the moveto start recomputed through four
				// Beziers. Leave it and the fringe normalizes a 1e-12 edge into
				// a nick at the seam -- three o'clock on every circle. GDI
				// never saw it: integer pixels collapse the edge to nothing.
				if (l.pts.size() > 1) {
					const fpt &a = l.pts.front(), &b = l.pts.back();
					const double dx = b.x - a.x, dy = b.y - a.y;
					if (dx * dx + dy * dy < 1e-18)
						l.pts.pop_back();
				}
				// Curves arrive flattened far past the pixel grid: a pin is a
				// hundred points for five pixels. The triangulator chokes on
				// vertices it cannot tell apart, so keep a vertex only when it
				// moves half a pixel. Anything this erases was invisible.
				if (l.pts.size() > 16) {
					std::vector<fpt> thin;
					thin.reserve(l.pts.size());
					thin.push_back(l.pts.front());
					for (size_t t = 1; t < l.pts.size(); t++) {
						const fpt &a = thin.back(), &b = l.pts[t];
						const double dx = b.x - a.x, dy = b.y - a.y;
						if (dx * dx + dy * dy >= step * step)
							thin.push_back(b);
					}
					// The seam too: a last point a hair from the first makes a
					// micro-edge whose fringe normal points anywhere -- the
					// nick at three o'clock, where every circle starts.
					while (thin.size() > 3) {
						const fpt &a = thin.front(), &b = thin.back();
						const double dx = b.x - a.x, dy = b.y - a.y;
						if (dx * dx + dy * dy >= step * step)
							break;
						thin.pop_back();
					}
					if (thin.size() >= 3)
						l.pts.swap(thin);
				}
				if (l.pts.size() >= 3) {
					l.area = floop_area(l.pts);
					if (l.area != 0)
						loops.push_back(std::move(l));
				}
			}
			floop_fill(dl, map, loops, im::col(sh.fill), 1.0 / (k * k));
		}
		if (sh.has_stroke) {
			const float w = float(std::max(1, int(sh.stroke_w * k + 0.5)));
			for (size_t i = 0; i < sh.subs.size(); i++) {
				pts.clear();
				for (const pt &q : sh.subs[i]) {
					double px = ox + q.x * k, py = oy + q.y * k;
					if (turn) {
						const double dx = px - cx, dy = py - cy;
						px = cx + dx * cs - dy * sn;
						py = cy + dx * sn + dy * cs;
					}
					pts.emplace_back(float(px), float(py));
				}
				if (pts.size() < 2)
					continue;
				const ImU32 c = im::col(sh.stroke);
				if (sh.closed[i] && pts.size() >= 2) {
					// Closed joins the last point back to the first itself; also
					// appending the first point makes a zero-length edge whose
					// join spikes -- the nick at three o'clock, where every
					// circle starts. GDI drew the closing segment by hand from
					// last to first, which has the same shape without one.
					while (pts.size() > 1) {
						const ImVec2 &a = pts.back(), &b = pts.front();
						const double dx = double(a.x) - b.x, dy = double(a.y) - b.y;
						if (dx * dx + dy * dy > 1e-12)
							break;
						pts.pop_back();
					}
					dl->AddPolyline(pts.data(), int(pts.size()), c, ImDrawFlags_Closed, w);
				} else {
					dl->AddPolyline(pts.data(), int(pts.size()), c, 0, w);
				}
			}
		}
	}
}


// ---- 画像のとき
//
// GDI のときは出来上がりの 1 枚を DIB にして AlphaBlend で貼っていた。
// ここでは同じ 1 枚を ImGui のテクスチャに置いてから AddImageQuad で置くだけ
// （im::tex が contexts ごと TextureData を持ってくれるので、
// DX11 / Metal / SDL_gpu のどれでも同じ 3 行で済む）

namespace {

uint32_t premul(uint32_t c)
{
	const uint32_t a = c >> 24;
	if (a == 255)
		return c;
	auto m = [&](int sh) { return (((c >> sh) & 0xff) * a + 127) / 255; };
	return (a << 24) | (m(16) << 16) | (m(8) << 8) | m(0);
}

// α を戻す。ミップと補間はかけたまま（错的でない）行い、テクスチャへ渡す
// 1 枚だけ戻す。DX11 / Metal / SDLRenderer はどれも SRC_ALPHA 合成
uint32_t unpremul(uint32_t c)
{
	const uint32_t a = c >> 24;
	if (a == 255 || a == 0)
		return a == 0 ? 0u : c;
	auto m = [&](int sh) {
		return std::min<uint32_t>(255, (((c >> sh) & 0xff) * 255 + a / 2) / a);
	};
	return (a << 24) | (m(16) << 16) | (m(8) << 8) | m(0);
}

} // namespace


bool svg_art::load_png(const std::string &path)
{
	int w = 0, h = 0;
	std::vector<u32> raw;
	if (!read_png(path, w, h, raw))
		return false;
	return load_pixels(w, h, raw);
}

// Set SMU_NATIVE_TEXTURES=1 to force native upload (A/B testing); unset or
// "0" follows the ratio below. Presence alone used to force it, which made
// =0 lie -- the value is what counts.
static bool svg_force_native()
{
	const char *v = std::getenv("SMU_NATIVE_TEXTURES");
	return v && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

bool svg_art::load_pixels(int w, int h, const std::vector<uint32_t> &raw)
{
	clear();
	if (w <= 0 || h <= 0 || raw.size() < size_t(w) * size_t(h))
		return false;

	level l0;
	l0.w = w;
	l0.h = h;
	l0.px.resize(raw.size());
	for (size_t i = 0; i < raw.size(); i++)
		l0.px[i] = premul(raw[i]);
	m_mips.push_back(std::move(l0));

	// 半分ずつ縮めた段。2 × 2 の平均（端の余りは端の画素を使い回す）。
	// 縮小側だけ要るので、SMU_NATIVE_TEXTURES=1 では作らない
	if (!svg_force_native()) {
		while (m_mips.back().w > 1 || m_mips.back().h > 1) {
			const level &a = m_mips.back();
			level b;
			b.w = std::max(1, (a.w + 1) / 2);
			b.h = std::max(1, (a.h + 1) / 2);
			b.px.resize(size_t(b.w) * size_t(b.h));
			for (int y = 0; y < b.h; y++)
				for (int x = 0; x < b.w; x++) {
					const int x0 = std::min(2 * x, a.w - 1), x1 = std::min(2 * x + 1, a.w - 1);
					const int y0 = std::min(2 * y, a.h - 1), y1 = std::min(2 * y + 1, a.h - 1);
					const uint32_t q[4] = { a.px[size_t(y0) * a.w + x0], a.px[size_t(y0) * a.w + x1],
					                        a.px[size_t(y1) * a.w + x0], a.px[size_t(y1) * a.w + x1] };
					uint32_t out = 0;
					for (int sh = 0; sh < 32; sh += 8) {
						uint32_t sum = 0;
						for (uint32_t v : q)
							sum += (v >> sh) & 0xff;
						out |= ((sum + 2) / 4) << sh;
					}
					b.px[size_t(y) * b.w + x] = out;
				}
			m_mips.push_back(std::move(b));
		}
	}
	m_vb[0] = m_vb[1] = 0;
	m_vb[2] = w;
	m_vb[3] = h;
	return true;
}

void svg_art::release_gpu() const
{
	delete static_cast<im::tex *>(m_cache.gpu);
	m_cache.gpu = nullptr;
}

// The texture is the picture resampled to about the size it gets drawn at, so
// the GPU does not have to minify it: ImGui textures have no mipmaps, and the
// panel background is 2000 px wide landing in about 700, which would shimmer.
// The downscaling itself is load_pixels' 2x2 box filter, one mip level at a
// time. Rounding the size *down* to a multiple of this keeps the texture just
// under its destination, so the GPU magnifies a little or lands 1:1 -- the
// harmless direction, and less memory than rounding up would take.
static constexpr int SIZE_GRAIN = 1;
static int grain(int px) { return std::max(SIZE_GRAIN, px / SIZE_GRAIN * SIZE_GRAIN); }

void svg_art::draw_image(ImDrawList *dl, const RECT &dst, double deg) const
{
	const int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
	if (dw <= 0 || dh <= 0 || m_mips.empty())
		return;
	const level &base = m_mips[0];

	// Where the picture sits inside dst: aspect kept, centered
	const double k = std::min(double(dw) / base.w, double(dh) / base.h);
	const double pw = base.w * k, ph = base.h * k;
	const double cx = (dst.left + dst.right) * 0.5, cy = (dst.top + dst.bottom) * 0.5;
	const double x0 = cx - pw / 2, y0 = cy - ph / 2;

	// Minify a lot and the GPU needs help (it has no mipmaps to fall back
	// on); otherwise the bytes go over unchanged. Sizes below are device
	// pixels, so a 2x display gets full-res bytes where a point-size target
	// would have halved them away. The line sits below what any test has
	// needed: resample won at 0.2, native at 0.435 and everywhere above, so
	// the line below keeps both with margin on each side.
	// SMU_NATIVE_TEXTURES=1 forces the native side, for A/B testing.
	static constexpr double NATIVE_MIN_RATIO = 0.3;
	ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
	if (fb.x <= 0 || fb.y <= 0)
		fb = ImVec2(1, 1);
	const int tw = grain(int(std::ceil(pw * fb.x)));
	const int th = grain(int(std::ceil(ph * fb.y)));
	const bool native = svg_force_native() ||
	                    std::min(double(tw) / base.w, double(th) / base.h) >= NATIVE_MIN_RATIO;

	im::tex *t = static_cast<im::tex *>(m_cache.gpu);
	if (!t) {
		t = new im::tex;
		m_cache.gpu = t;
	}
	if (native) {
		// Native bytes, GPU scales. Upload once; resizes never touch it.
		if (!t->valid() || m_cache.w != base.w || m_cache.h != base.h) {
			std::vector<uint32_t> px(size_t(base.w) * size_t(base.h));
			for (size_t i = 0; i < px.size(); i++)
				px[i] = unpremul(base.px[i]);
			m_cache.w = base.w;
			m_cache.h = base.h;
			t->upload(base.w, base.h, px);
		}
	} else if (!t->valid() || m_cache.w != tw || m_cache.h != th) {
		// 一番深い段（1/2, 1/4 …）から、足りる 段まで戻る
		size_t li = 0;
		while (li + 1 < m_mips.size() && m_mips[li + 1].w >= tw)
			li++;
		const level &L = m_mips[li];

		auto fetch = [&](int x, int y) -> uint32_t {
			x = std::max(0, std::min(x, L.w - 1));
			y = std::max(0, std::min(y, L.h - 1));
			return L.px[size_t(y) * L.w + x];
		};

		std::vector<uint32_t> px(size_t(tw) * size_t(th), 0);
		for (int y = 0; y < th; y++)
			for (int x = 0; x < tw; x++) {
				const double u = (x + 0.5) * L.w / tw - 0.5;
				const double v = (y + 0.5) * L.h / th - 0.5;
				const int ix = int(std::floor(u)), iy = int(std::floor(v));
				const double tx = u - ix, ty = v - iy;
				const uint32_t a = fetch(ix, iy), b = fetch(ix + 1, iy);
				const uint32_t c = fetch(ix, iy + 1), d = fetch(ix + 1, iy + 1);
				uint32_t out = 0;
				for (int sh = 0; sh < 32; sh += 8) {
					const double top = ((a >> sh) & 0xff) * (1 - tx) + ((b >> sh) & 0xff) * tx;
					const double bot = ((c >> sh) & 0xff) * (1 - tx) + ((d >> sh) & 0xff) * tx;
					out |= uint32_t(std::lround(top * (1 - ty) + bot * ty)) << sh;
				}
				// 補間はかけたまま（错的でない）して、ここだけ戻して texture へ
				px[size_t(y) * tw + x] = unpremul(out);
			}
		m_cache.w = tw;
		m_cache.h = th;
		t->upload(tw, th, px);
	}
	if (!t->valid())
		return;                        // context が無い（描く先が無い）

	// 角度を付けて 4 隅を置く。deg を 0 にするとただの AddImageQuad と同じ。
	// 回すのは絵ではなくこの四角の 4 隅なので、つまみを回してもテクスチャは
	// 一切触らない（GDI ではここに絵を焼き直していた）
	const double rad = deg * 3.14159265358979 / 180.0;
	const double cs = std::cos(rad), sn = std::sin(rad);
	auto corner = [&](double x, double y) {
		const double dx = x - cx, dy = y - cy;
		return ImVec2(float(cx + dx * cs - dy * sn), float(cy + dx * sn + dy * cs));
	};
	// The picture goes out as a grid of quads, each at most this big. A single
	// large textured quad does not survive the trip: through SDL_Renderer only
	// one of its two triangles reached the screen, so the panel art showed up
	// cut along the diagonal from the top-left to the bottom-right corner. The
	// command data is right -- the indices, the four corners and the texture
	// all check out when dumped from inside the backend -- and it is
	// size-dependent, so it is a rasterizer limit rather than something the
	// draw list got wrong: a 1335x514 quad loses a triangle, 1335x257 and
	// 664x514 do not. Same code, same texture, same frame; only the quad is
	// smaller. Tiles cost a handful of extra quads and they all share one
	// texture, so they still go out in a single command.
	static constexpr float TILE = 256.0f;
	const int nx = std::max(1, int(std::ceil(pw / TILE)));
	const int ny = std::max(1, int(std::ceil(ph / TILE)));
	for (int j = 0; j < ny; j++) {
		for (int i = 0; i < nx; i++) {
			const double xa = x0 + pw * i / nx, xb = x0 + pw * (i + 1) / nx;
			const double ya = y0 + ph * j / ny, yb = y0 + ph * (j + 1) / ny;
			const ImVec2 ua(float(i) / float(nx), float(j) / float(ny));
			const ImVec2 ub(float(i + 1) / float(nx), float(j + 1) / float(ny));
			dl->AddImageQuad(t->ref(),
			                 corner(xa, ya), corner(xb, ya), corner(xb, yb), corner(xa, yb),
			                 ua, ImVec2(ub.x, ua.y), ub, ImVec2(ua.x, ub.y));
		}
	}
}

} // namespace ui

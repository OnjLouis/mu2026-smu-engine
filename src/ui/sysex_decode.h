// license:BSD-3-Clause
//
// 貼り付けた MIDI（SysEx など）を 1 行ずつ読み解く（エディタの「SysEx の読み解き」のタブ）。
//
// 1 行から 16 進の数を拾う（「F0 43 10 4C」「f0h 43h」「0xF0,0x43」「Ex:f0h 43h …」（Domino の表）の
// どれでもよい）。拾ったバイトを MIDI の並びとして区切り（ランニングステータスは補う）、1 通ずつ
// **欄（field）**の並びにする。欄は字だけのものと、**値を持って書き換えられるもの**（パート・値・
// チャンネルなど）がある。書き換えられる欄は、その値が 1 通のどのバイトにあるかを知っているので、
// 画面で動かすとバイトを直して、行を書き直せる。
//   XG のパラメータチェンジ（43 1n 4C）  パラメータの表（xg/model.cpp）で番地を引き、パート・名前・値。
//                                       1 通に続けて何個も書いてあれば、番地を進めて全部
//     02 01 xx（リバーブ・コーラス・バリエーション）と 03 nn xx（インサーション）のパラメータは、
//     **今の音源の種類**の表で引く（種類が違えば同じ番地でも意味が違う）
//     3n rr pp  ドラムセットアップ（組・鍵・値）
//   XG System On・ドラムセットアップのリセット、GM・GS のリセット、マスターボリューム
//   チャンネルのメッセージ（ノート・CC・プログラムチェンジ・ベンドなど）
// 読めないものは「？」を付けてそのまま並べる

#ifndef S_MU2000_UI_SYSEX_DECODE_H
#define S_MU2000_UI_SYSEX_DECODE_H

#pragma once

#include "xg_ui.h"
#include "xg/fx_params.h"
#include "xg/fx_types.h"
#include "xg/sysfx.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ui {
namespace sxd {

inline std::string hex(u32 v, int digits = 2)
{
	char b[16];
	std::snprintf(b, sizeof(b), "%0*X", digits, v);
	return b;
}

// ---- 1 行から 16 進のバイトを拾う。「F0」「f0h」「0xF0」「F0,」など。1-2 桁の 16 進だけ
inline std::vector<u8> bytes_of(const std::string &line)
{
	std::vector<u8> out;
	size_t i = 0;
	while (i < line.size()) {
		if (!std::isalnum(u8(line[i]))) {
			i++;
			continue;
		}
		size_t j = i;
		while (j < line.size() && std::isalnum(u8(line[j])))
			j++;
		std::string t = line.substr(i, j - i);
		i = j;
		for (char &c : t)
			c = char(std::tolower(u8(c)));
		if (t.size() > 2 && t[0] == '0' && t[1] == 'x')
			t = t.substr(2);
		if (t.size() > 1 && t.back() == 'h')
			t.pop_back();
		if (t.empty() || t.size() > 2)
			continue;
		bool ishex = true;
		for (char c : t)
			ishex = ishex && std::isxdigit(u8(c));
		if (ishex)
			out.push_back(u8(std::strtoul(t.c_str(), nullptr, 16)));
	}
	return out;
}

// 書き直すときの書き方。元の行に合わせる
enum class style { plain, domino, c0x };
inline style style_of(const std::string &line)
{
	std::string l;
	for (char c : line)
		l += char(std::tolower(u8(c)));
	if (l.find("ex:") != std::string::npos || l.find("h ") != std::string::npos ||
	    (l.size() > 1 && l.back() == 'h'))
		return style::domino;
	if (l.find("0x") != std::string::npos)
		return style::c0x;
	return style::plain;
}

// 1 通ずつの並びを 1 行に書く
inline std::string write_line(const std::vector<std::vector<u8>> &msgs, style s)
{
	std::string out;
	for (const std::vector<u8> &m : msgs) {
		if (!out.empty())
			out += s == style::c0x ? ", " : "  ";
		if (s == style::domino && !m.empty() && m[0] == 0xf0)
			out += "Ex:";
		for (size_t i = 0; i < m.size(); i++) {
			char b[8];
			if (s == style::domino)
				std::snprintf(b, sizeof(b), "%02xh", m[i]);
			else if (s == style::c0x)
				std::snprintf(b, sizeof(b), "0x%02X", m[i]);
			else
				std::snprintf(b, sizeof(b), "%02X", m[i]);
			if (i)
				out += s == style::c0x ? "," : " ";
			out += b;
		}
	}
	return out;
}

// ---- バイトを MIDI の 1 通ずつに区切る（ランニングステータスは補って、1 通ごとに状態のバイトを持たせる）。
// 区切れないバイトは 1 つずつ別の「通」にする（読めないものとして出す）
inline std::vector<std::vector<u8>> split(const std::vector<u8> &b)
{
	std::vector<std::vector<u8>> out;
	u8 running = 0;
	size_t i = 0;
	while (i < b.size()) {
		if (b[i] == 0xf0) {
			size_t j = i;
			while (j < b.size() && b[j] != 0xf7)
				j++;
			out.emplace_back(b.begin() + std::ptrdiff_t(i), b.begin() + std::ptrdiff_t(std::min(j + 1, b.size())));
			i = std::min(j + 1, b.size());
			continue;
		}
		u8 st = b[i];
		if (st & 0x80) {
			i++;
			running = st >= 0xf0 ? 0 : st;
			if (st >= 0xf0) {
				out.push_back({ st });
				continue;
			}
		} else if (running) {
			st = running;
		} else {
			out.push_back({ b[i++] });
			continue;
		}
		const int want = ((st & 0xf0) == 0xc0 || (st & 0xf0) == 0xd0) ? 1 : 2;
		std::vector<u8> msg{ st };
		for (int k = 0; k < want && i < b.size() && !(b[i] & 0x80); k++)
			msg.push_back(b[i++]);
		out.push_back(std::move(msg));
	}
	return out;
}

// ---- 欄
enum class fk {
	text,       // 字だけ
	part,       // パート番号（1 バイト。0-63）
	value,      // 値（pos から size バイト。enc の書き方）。範囲 lo-hi、字は show
	dset,       // ドラムの組（3n の n）
	dkey,       // ドラムの鍵（13-91）
	ch,         // チャンネル（状態のバイトの下 4bit）
	bend,       // ベンド（2 バイト。LSB, MSB。真ん中からの離れ）
};
enum class shown { number, xg, fx, drum, fxtype, plus1, note };
struct field {
	fk kind = fk::text;
	std::string text;                  // 字（text のとき）、ほかは前に付ける名前（空でよい）
	int value = 0, lo = 0, hi = 127;
	int pos = 0, size = 1;
	xg::coding enc = xg::coding::byte7;
	shown how = shown::number;
	const xg::param *p = nullptr;
	const xg::fx_param *fp = nullptr;
	int drum_idx = -1;
	bool bad = false;                  // 読めないもの（色を変える）
};

inline int read_value(const u8 *d, int size, xg::coding enc)
{
	int v = 0;
	for (int k = 0; k < size; k++)
		v = enc == xg::coding::nibble ? (v << 4) | (d[k] & 0x0f) : (v << 7) | (d[k] & 0x7f);
	return v;
}

inline void write_value(u8 *d, int size, xg::coding enc, int v)
{
	for (int k = size - 1; k >= 0; k--) {
		d[k] = u8(enc == xg::coding::nibble ? (v & 0x0f) : (v & 0x7f));
		v >>= enc == xg::coding::nibble ? 4 : 7;
	}
}

inline std::string note_name(int n);

// 欄の値の字
inline std::string value_text(const field &f)
{
	switch (f.how) {
	case shown::xg:
		return f.p ? xg::format(*f.p, f.value) : std::to_string(f.value);
	case shown::fxtype:
		return xg::fx_name(f.value) + " (" + hex(u32(f.value >> 7)) + " " + hex(u32(f.value & 0x7f)) + ")";
	case shown::fx:
		if (f.fp) {
			if (f.fp->fmt == xg::fx_fmt::table && f.fp->texts && f.value >= f.fp->lo && f.value <= f.fp->hi)
				return f.fp->texts[f.value - f.fp->lo];
			if (f.fp->fmt == xg::fx_fmt::tenths) {
				char t[24];
				std::snprintf(t, sizeof(t), "%.1f", f.value / 10.0);
				return t;
			}
		}
		return std::to_string(f.value);
	case shown::drum:
		return xgui::drum_value_text(f.drum_idx, f.value);
	case shown::plus1:
		return std::to_string(f.value + 1);
	case shown::note:
		return note_name(f.value) + " (" + std::to_string(f.value) + ")";
	default:
		return std::to_string(f.value);
	}
}

// 欄の値をバイトへ戻す
inline void apply(std::vector<u8> &msg, const field &f, int v)
{
	v = std::clamp(v, f.lo, f.hi);
	if (f.pos < 0 || f.pos >= int(msg.size()))
		return;
	switch (f.kind) {
	case fk::part:
	case fk::dkey:
		msg[size_t(f.pos)] = u8(v);
		break;
	case fk::dset:
		msg[size_t(f.pos)] = u8(0x30 + v);
		break;
	case fk::ch:
		msg[0] = u8((msg[0] & 0xf0) | (v & 0x0f));
		break;
	case fk::bend: {
		if (f.pos + 1 >= int(msg.size()))
			return;
		const int raw = std::clamp(v + 8192, 0, 16383);
		msg[size_t(f.pos)] = u8(raw & 0x7f);
		msg[size_t(f.pos) + 1] = u8(raw >> 7);
		break;
	}
	case fk::value:
		if (f.pos + f.size <= int(msg.size()))
			write_value(msg.data() + f.pos, f.size, f.enc, v);
		break;
	default:
		break;
	}
}

inline const char *cc_name(int cc)
{
	switch (cc) {
	case 0: return "Bank Select MSB";  case 1: return "Modulation";      case 5: return "Portamento Time";
	case 6: return "Data Entry MSB";   case 7: return "Volume";          case 10: return "Pan";
	case 11: return "Expression";      case 32: return "Bank Select LSB"; case 38: return "Data Entry LSB";
	case 64: return "Hold 1";          case 65: return "Portamento";     case 66: return "Sostenuto";
	case 67: return "Soft Pedal";      case 71: return "Harmonic Content (Resonance)";
	case 72: return "Release Time";    case 73: return "Attack Time";    case 74: return "Brightness (Cutoff)";
	case 84: return "Portamento Control";
	case 91: return "Reverb Send";     case 93: return "Chorus Send";    case 94: return "Variation Send";
	case 96: return "Data Increment";  case 97: return "Data Decrement";
	case 98: return "NRPN LSB";        case 99: return "NRPN MSB";       case 100: return "RPN LSB";
	case 101: return "RPN MSB";        case 120: return "All Sound Off"; case 121: return "Reset All Controllers";
	case 123: return "All Notes Off";  case 124: return "Omni Off";      case 125: return "Omni On";
	case 126: return "Mono";           case 127: return "Poly";
	default: return nullptr;
	}
}

inline std::string note_name(int n)
{
	static const char *const N[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	return std::string(N[n % 12]) + std::to_string(n / 12 - 2);
}

inline const char *fx_key_prefix(int slot)
{
	static const char *const P4[4] = { "insertion1", "insertion2", "insertion3", "insertion4" };
	return slot >= 1 && slot <= 4 ? P4[slot - 1] : slot == 5 ? "reverb" : slot == 6 ? "chorus" : "variation";
}

// ---- 1 通を欄にする
inline std::vector<field> fields_of(const std::vector<u8> &b, xg::model &m)
{
	std::vector<field> out;
	auto text = [&](const std::string &s, bool bad = false) {
		field f;
		f.text = s;
		f.bad = bad;
		out.push_back(f);
	};
	if (b.empty())
		return out;
	const u8 st = b[0];
	if (st == 0xf0) {
		const size_t end = b.back() == 0xf7 ? b.size() - 1 : b.size();
		const size_t n = end >= 1 ? end - 1 : 0;
		const u8 *d = b.data() + 1;
		if (n >= 4 && d[0] == 0x7e && d[2] == 0x09) {
			text(d[3] == 0x01 ? "GM System On" : d[3] == 0x02 ? "GM System Off" : d[3] == 0x03 ? "GM2 System On" : "GM System ?");
			return out;
		}
		if (n >= 6 && d[0] == 0x7f && d[2] == 0x04 && d[3] == 0x01) {
			field f;
			f.kind = fk::value;
			f.text = "Master Volume";
			f.value = d[5];
			f.pos = 6;
			out.push_back(f);
			return out;
		}
		if (n >= 8 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12 && d[4] == 0x40 && d[5] == 0x00 && d[6] == 0x7f) {
			text("GS Reset");
			return out;
		}
		if (n >= 7 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12) {
			text("GS Parameter " + hex(d[4]) + " " + hex(d[5]) + " " + hex(d[6]) + " (not decoded)");
			return out;
		}
		if (n >= 6 && d[0] == 0x43 && (d[1] & 0xf0) == 0x30 && d[2] == 0x4c) {
			text("XG Parameter Request " + hex(d[3]) + " " + hex(d[4]) + " " + hex(d[5]));
			return out;
		}
		if (n >= 6 && d[0] == 0x43 && (d[1] & 0xf0) == 0x20 && d[2] == 0x4c) {
			text("XG Dump Request " + hex(d[3]) + " " + hex(d[4]) + " " + hex(d[5]));
			return out;
		}
		if (n >= 3 && d[0] == 0x43 && (d[1] & 0xf0) == 0x00 && d[2] == 0x4c) {
			text("XG Bulk Dump (" + std::to_string(n) + " bytes)");
			return out;
		}
		if (!(n >= 6 && d[0] == 0x43 && (d[1] & 0xf0) == 0x10 && d[2] == 0x4c)) {
			text("? SysEx (" + std::to_string(b.size()) + " bytes)", true);
			return out;
		}
		// ---- XG のパラメータチェンジ。b の中で hh は 4、mm は 5、ll は 6、データは 7 から
		const u8 hh = b[4], mm = b[5], ll = b[6];
		const int dn = int(end) - 7;
		if (hh == 0x00 && mm == 0x00 && ll == 0x7e) { text("XG System On"); return out; }
		if (hh == 0x00 && mm == 0x00 && ll == 0x7f) { text("XG All Parameter Reset"); return out; }
		if (hh == 0x00 && mm == 0x00 && ll == 0x7d) {
			text("Drum Setup Reset");
			field f;
			f.kind = fk::value;
			f.text = "DRUMS";
			f.value = dn > 0 ? b[7] : 0;
			f.hi = 3;
			f.pos = 7;
			f.how = shown::plus1;
			out.push_back(f);
			return out;
		}
		if (hh >= 0x30 && hh <= 0x33) {
			field fs;
			fs.kind = fk::dset;
			fs.text = "DRUMS";
			fs.value = hh - 0x30;
			fs.hi = 3;
			fs.pos = 4;
			fs.how = shown::plus1;
			out.push_back(fs);
			field fkey;
			fkey.kind = fk::dkey;
			fkey.value = mm;
			fkey.lo = XG_DRUM_KEY0;
			fkey.hi = XG_DRUM_KEY0 + XG_DRUM_KEYS - 1;
			fkey.pos = 5;
			out.push_back(fkey);
			for (int at = 0; at < dn; at++) {
				const int idx = xgui::drum_index(u8(ll + at));
				if (idx < 0) {
					text("? " + hex(u32(ll + at)) + " = " + hex(b[size_t(7 + at)]), true);
					continue;
				}
				const xgui::drum_param &dp = xgui::drum_params()[idx];
				field f;
				f.kind = fk::value;
				f.text = dp.head;
				f.value = b[size_t(7 + at)];
				f.lo = dp.lo;
				f.hi = dp.hi;
				f.pos = 7 + at;
				f.how = shown::drum;
				f.drum_idx = idx;
				out.push_back(f);
			}
			return out;
		}
		const int slot = hh == 0x03 ? mm + 1 : (hh == 0x02 && mm == 0x01) ? (ll < 0x20 ? 5 : ll < 0x40 ? 6 : 7) : 0;
		bool part_done = false;
		int at = 0;
		int lo = ll;
		while (at < dn) {
			const xg::param *hit = nullptr;
			for (const xg::param &p : xg::params()) {
				if (p.hi != hh || p.lo != lo)
					continue;
				if (p.where == xg::area::part ? mm < 64 : p.mid == mm) {
					hit = &p;
					break;
				}
			}
			if (hit && at + hit->size <= dn) {
				if (hit->where == xg::area::part && !part_done) {
					field fp;
					fp.kind = fk::part;
					fp.value = mm;
					fp.hi = 63;
					fp.pos = 5;
					out.push_back(fp);
					part_done = true;
				}
				field f;
				f.kind = fk::value;
				f.text = hit->label;
				f.pos = 7 + at;
				f.size = hit->size;
				f.enc = hit->enc;
				f.value = read_value(b.data() + f.pos, f.size, f.enc);
				f.p = hit;
				const bool is_type = std::string(hit->key).find(".type") != std::string::npos;
				f.how = is_type ? shown::fxtype : shown::xg;
				f.lo = is_type ? 0 : std::min(hit->min, hit->special >= 0 ? hit->special : hit->min);
				f.hi = is_type ? 0x3fff : std::max(hit->max, hit->special >= 0 ? hit->special : hit->max);
				out.push_back(f);
				at += hit->size;
				lo += hit->size;
				continue;
			}
			bool done = false;
			if (slot) {
				int type = -1;
				const xg::param *pt = xg::find(std::string(fx_key_prefix(slot)) + ".type");
				if (pt && m.get(*pt, 0, type)) {
					if (const xg::fx_def *def = xg::fx_find(type)) {
						for (int i = 0; i < def->count && !done; i++) {
							const xg::fx_param &fp = def->params[i];
							int size = fp.size;
							int a = fp.addr;
							if (slot >= 5) {
								const xg::sysfx w = slot == 5 ? xg::sysfx::reverb : slot == 6 ? xg::sysfx::chorus : xg::sysfx::variation;
								a = xg::sysfx_addr(w, fp, size);
							}
							if (a != lo || at + size > dn)
								continue;
							field f;
							f.kind = fk::value;
							f.text = std::string(fx_key_prefix(slot)) + " (" + xg::fx_name(type) + ") " + fp.label;
							f.pos = 7 + at;
							f.size = size;
							f.value = read_value(b.data() + f.pos, size, xg::coding::byte7);
							f.lo = fp.lo;
							f.hi = fp.hi;
							f.fp = &fp;
							f.how = shown::fx;
							out.push_back(f);
							at += size;
							lo += size;
							done = true;
						}
					}
				}
			}
			if (done)
				continue;
			text("? " + hex(hh) + " " + hex(mm) + " " + hex(u32(lo)) + " = " + hex(b[size_t(7 + at)]), true);
			at++;
			lo++;
		}
		return out;
	}
	// ---- チャンネルのメッセージ
	if (st < 0x80 || st >= 0xf0) {
		text("? " + hex(st), true);
		return out;
	}
	field fc;
	fc.kind = fk::ch;
	fc.value = st & 0x0f;
	fc.hi = 15;
	fc.pos = 0;
	fc.how = shown::plus1;
	fc.text = "Ch";
	out.push_back(fc);
	auto val = [&](const std::string &name, int pos, int lo = 0, int hi = 127, shown how = shown::number) {
		if (pos >= int(b.size()))
			return;
		field f;
		f.kind = fk::value;
		f.text = name;
		f.value = b[size_t(pos)];
		f.lo = lo;
		f.hi = hi;
		f.pos = pos;
		f.how = how;
		out.push_back(f);
	};
	switch (st & 0xf0) {
	case 0x80: text("Note Off"); val("Key", 1, 0, 127, shown::note); val("Vel", 2); break;
	case 0x90: text(b.size() > 2 && b[2] == 0 ? "Note Off" : "Note On"); val("Key", 1, 0, 127, shown::note); val("Vel", 2); break;
	case 0xa0: text("Poly Aftertouch"); val("Key", 1, 0, 127, shown::note); val("", 2); break;
	case 0xb0: {
		val("CC", 1);
		const char *nm = b.size() > 1 ? cc_name(b[1]) : nullptr;
		if (nm)
			text(nm);
		val("=", 2);
		break;
	}
	case 0xc0: val("Program Change", 1, 0, 127, shown::plus1); break;
	case 0xd0: val("Channel Aftertouch", 1); break;
	case 0xe0: {
		field f;
		f.kind = fk::bend;
		f.text = "Pitch Bend";
		f.value = b.size() > 2 ? (b[2] << 7 | b[1]) - 8192 : 0;
		f.lo = -8192;
		f.hi = 8191;
		f.pos = 1;
		out.push_back(f);
		break;
	}
	default: break;
	}
	return out;
}

} // namespace sxd
} // namespace ui

#endif // S_MU2000_UI_SYSEX_DECODE_H

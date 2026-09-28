// license:BSD-3-Clause
//
// 貼り付けた MIDI（SysEx など）を 1 行ずつ読み解く（エディタの「SysEx」のタブ）。
//
// 1 行から 16 進の数を拾う（「F0 43 10 4C」「f0h 43h」「0xF0,0x43」「Ex:f0h 43h …」（Domino の表）の
// どれでもよい）。拾ったバイトを MIDI の並びとして区切り、1 通ずつ意味の字にする。
//   XG のパラメータチェンジ（43 1n 4C）  パラメータの表（xg/model.cpp）で番地を引き、名前と値の字。
//                                       1 通に続けて何個も書いてあれば、番地を進めて全部
//     02 01 xx（リバーブ・コーラス・バリエーション）と 03 nn xx（インサーション）のパラメータは、
//     **今の音源の種類**の表で引く（種類が違えば同じ番地でも意味が違う）
//     3n rr pp  ドラムセットアップ
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

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {
namespace sxd {

// 1 行から 16 進のバイトを拾う。「F0」「f0h」「0xF0」「F0,」など。1-2 桁の 16 進だけ
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
		bool hex = true;
		for (char c : t)
			hex = hex && std::isxdigit(u8(c));
		if (hex)
			out.push_back(u8(std::strtoul(t.c_str(), nullptr, 16)));
	}
	return out;
}

inline std::string hex(u32 v, int digits = 2)
{
	char b[16];
	std::snprintf(b, sizeof(b), "%0*X", digits, v);
	return b;
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

// 今の音源の、そのエフェクトの種類（02 01 xx は lo で、03 nn xx は nn で）
inline int fx_slot_of(u8 hh, u8 mm, u8 ll)
{
	if (hh == 0x03)
		return mm + 1;                              // インサーション 1-4
	if (hh == 0x02 && mm == 0x01)
		return ll < 0x20 ? 5 : ll < 0x40 ? 6 : 7;   // リバーブ・コーラス・バリエーション
	return 0;
}

inline const char *fx_key_prefix(int slot)
{
	static const char *const P4[4] = { "insertion1", "insertion2", "insertion3", "insertion4" };
	return slot >= 1 && slot <= 4 ? P4[slot - 1] : slot == 5 ? "reverb" : slot == 6 ? "chorus" : "variation";
}

// 値のバイト（size 個）から値
inline int value_of(const u8 *d, int size, xg::coding enc)
{
	int v = 0;
	for (int k = 0; k < size; k++)
		v = enc == xg::coding::nibble ? (v << 4) | (d[k] & 0x0f) : (v << 7) | (d[k] & 0x7f);
	return v;
}

// XG のパラメータチェンジの中身（hh mm ll と、続くデータ）。1 つ以上の「名前 = 値」
inline std::string xg_change(u8 hh, u8 mm, u8 ll, const u8 *d, int n, xg::model &m)
{
	std::string out;
	auto add = [&](const std::string &s) { out += (out.empty() ? "" : " / ") + s; };
	if (hh == 0x00 && mm == 0x00 && ll == 0x7e)
		return "XG System On";
	if (hh == 0x00 && mm == 0x00 && ll == 0x7f)
		return "XG All Parameter Reset";
	if (hh == 0x00 && mm == 0x00 && ll == 0x7d)
		return "Drum Setup Reset  DRUMS" + std::to_string((n > 0 ? d[0] : 0) + 1);
	// ドラムセットアップ（3n rr pp）
	if (hh >= 0x30 && hh <= 0x33) {
		int at = 0;
		for (int a = ll; at < n; a++, at++) {
			const int idx = xgui::drum_index(u8(a));
			const std::string head = "DRUMS" + std::to_string(hh - 0x30 + 1) + " " + xgui::drum_key_text(mm);
			if (idx < 0)
				add(head + " ?" + hex(u32(a)) + " = " + std::to_string(d[at]));
			else
				add(head + " " + xgui::drum_params()[idx].head + " = " + xgui::drum_value_text(idx, d[at]));
		}
		return out;
	}
	// エフェクトのパラメータ（種類の表で引く）
	const int slot = fx_slot_of(hh, mm, ll);
	int at = 0;
	u32 lo = ll;
	while (at < n) {
		const u32 addr = xg::pack(hh, mm, u8(lo));
		// パラメータの表（パートは mm がパート番号）
		const xg::param *hit = nullptr;
		for (const xg::param &p : xg::params()) {
			if (p.hi != hh || p.lo != lo)
				continue;
			if (p.where == xg::area::part ? mm < 64 : p.mid == mm) {
				hit = &p;
				break;
			}
		}
		if (hit && at + hit->size <= n) {
			const int v = value_of(d + at, hit->size, hit->enc);
			std::string what = hit->where == xg::area::part ? xgui::part_name(mm) + " " : std::string();
			std::string val;
			if (std::string(hit->key).find(".type") != std::string::npos)
				val = xg::fx_name(v) + " (" + hex(u32(v >> 7)) + " " + hex(u32(v & 0x7f)) + ")";
			else
				val = xg::format(*hit, v);
			add(what + hit->label + " = " + val);
			at += hit->size;
			lo += u32(hit->size);
			continue;
		}
		// エフェクトの種類ごとのパラメータ
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
						if (a != int(lo) || at + size > n)
							continue;
						const int v = value_of(d + at, size, xg::coding::byte7);
						std::string val;
						if (fp.fmt == xg::fx_fmt::table && fp.texts && v >= fp.lo && v <= fp.hi) {
							val = fp.texts[v - fp.lo];
						} else if (fp.fmt == xg::fx_fmt::tenths) {
							char t[24];
							std::snprintf(t, sizeof(t), "%.1f", v / 10.0);
							val = t;
						} else
							val = std::to_string(v);
						add(std::string(fx_key_prefix(slot)) + " (" + xg::fx_name(type) + ") " + fp.label + " = " + val);
						at += size;
						lo += u32(size);
						done = true;
					}
				}
			}
		}
		if (done)
			continue;
		(void)addr;
		add("? " + hex(hh) + " " + hex(mm) + " " + hex(lo) + " = " + hex(d[at]));
		at++;
		lo++;
	}
	return out;
}

// 1 通（F0 … F7、またはチャンネルのメッセージ）の意味
inline std::string message(const std::vector<u8> &b, xg::model &m, bool &ok)
{
	ok = true;
	if (b.empty())
		return {};
	const u8 st = b[0];
	if (st == 0xf0) {
		// 中身（F0 と F7 を除く）
		const size_t end = b.back() == 0xf7 ? b.size() - 1 : b.size();
		const u8 *d = b.data() + 1;
		const size_t n = end - 1;
		if (n >= 4 && d[0] == 0x7e && d[2] == 0x09)
			return d[3] == 0x01 ? "GM System On" : d[3] == 0x02 ? "GM System Off" : d[3] == 0x03 ? "GM2 System On" : "GM System ?";
		if (n >= 6 && d[0] == 0x7f && d[2] == 0x04 && d[3] == 0x01)
			return "Master Volume = " + std::to_string(d[5]);
		if (n >= 8 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12 && d[4] == 0x40 && d[5] == 0x00 && d[6] == 0x7f)
			return "GS Reset";
		if (n >= 4 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12)
			return "GS Parameter " + hex(d[4]) + " " + hex(d[5]) + " " + hex(d[6]) + " (not decoded)";
		if (n >= 3 && d[0] == 0x43 && (d[1] & 0xf0) == 0x10 && d[2] == 0x4c && n >= 6) {
			const std::string s = xg_change(d[3], d[4], d[5], d + 6, int(n) - 6, m);
			ok = s.rfind("? ", 0) != 0 && s.find(" / ? ") == std::string::npos;   // 引けない番地があれば色を変える
			return s;
		}
		if (n >= 3 && d[0] == 0x43 && (d[1] & 0xf0) == 0x30 && d[2] == 0x4c && n >= 6)
			return "XG Parameter Request " + hex(d[3]) + " " + hex(d[4]) + " " + hex(d[5]);
		if (n >= 3 && d[0] == 0x43 && (d[1] & 0xf0) == 0x20 && d[2] == 0x4c && n >= 6)
			return "XG Dump Request " + hex(d[3]) + " " + hex(d[4]) + " " + hex(d[5]);
		if (n >= 3 && d[0] == 0x43 && (d[1] & 0xf0) == 0x00 && d[2] == 0x4c)
			return "XG Bulk Dump (" + std::to_string(n) + " bytes)";
		ok = false;
		return "? SysEx (" + std::to_string(b.size()) + " bytes)";
	}
	const int ch = (st & 0x0f) + 1;
	const std::string c = "Ch" + std::to_string(ch) + " ";
	switch (st & 0xf0) {
	case 0x80: return c + "Note Off " + note_name(b.size() > 1 ? b[1] : 0) + (b.size() > 2 ? " vel " + std::to_string(b[2]) : "");
	case 0x90:
		if (b.size() > 2 && b[2] == 0)
			return c + "Note Off " + note_name(b[1]);
		return c + "Note On " + note_name(b.size() > 1 ? b[1] : 0) + (b.size() > 2 ? " vel " + std::to_string(b[2]) : "");
	case 0xa0: return c + "Poly Aftertouch " + note_name(b.size() > 1 ? b[1] : 0) + " = " + std::to_string(b.size() > 2 ? b[2] : 0);
	case 0xb0: {
		const int cc = b.size() > 1 ? b[1] : 0, v = b.size() > 2 ? b[2] : 0;
		const char *nm = cc_name(cc);
		return c + "CC" + std::to_string(cc) + (nm ? std::string(" ") + nm : std::string()) + " = " + std::to_string(v);
	}
	case 0xc0: return c + "Program Change " + std::to_string((b.size() > 1 ? b[1] : 0) + 1);
	case 0xd0: return c + "Channel Aftertouch = " + std::to_string(b.size() > 1 ? b[1] : 0);
	case 0xe0: {
		const int raw = b.size() > 2 ? (b[2] << 7 | b[1]) : 8192;
		return c + "Pitch Bend " + (raw >= 8192 ? "+" : "") + std::to_string(raw - 8192);
	}
	default: break;
	}
	ok = false;
	return "? " + hex(st);
}

// 1 行の意味。バイトを MIDI の並びとして区切る（ランニングステータスも）。bad は読めないものがあったか
inline std::string line(const std::string &text, xg::model &m, bool &bad)
{
	bad = false;
	const std::vector<u8> b = bytes_of(text);
	if (b.empty())
		return {};
	std::string out;
	u8 running = 0;
	size_t i = 0;
	while (i < b.size()) {
		std::vector<u8> msg;
		if (b[i] == 0xf0) {
			size_t j = i;
			while (j < b.size() && b[j] != 0xf7)
				j++;
			msg.assign(b.begin() + std::ptrdiff_t(i), b.begin() + std::ptrdiff_t(std::min(j + 1, b.size())));
			i = std::min(j + 1, b.size());
		} else {
			u8 st = b[i];
			if (st & 0x80) {
				i++;
				running = st >= 0xf0 ? 0 : st;
			} else if (running) {
				st = running;
			} else {
				bad = true;
				out += (out.empty() ? "" : "  |  ") + std::string("? ") + hex(b[i]);
				i++;
				continue;
			}
			const int want = ((st & 0xf0) == 0xc0 || (st & 0xf0) == 0xd0) ? 1 : 2;
			msg.push_back(st);
			for (int k = 0; k < want && i < b.size() && !(b[i] & 0x80); k++)
				msg.push_back(b[i++]);
		}
		bool ok = true;
		const std::string s = message(msg, m, ok);
		bad = bad || !ok;
		out += (out.empty() ? "" : "  |  ") + s;
	}
	return out;
}

} // namespace sxd
} // namespace ui

#endif // S_MU2000_UI_SYSEX_DECODE_H

// license:BSD-3-Clause

#include "m2a.h"

#include <algorithm>
#include <cstring>

namespace smu2000::m2a {

namespace {

u16 rd16(const u8 *p) { return u16(p[0] | p[1] << 8); }
u32 rd32(const u8 *p) { return u32(p[0] | p[1] << 8 | p[2] << 16 | u32(p[3]) << 24); }

bool is(const u8 *p, const char *id) { return std::memcmp(p, id, 4) == 0; }

// LIST wave の中を読む
void read_wave(const std::vector<u8> &f, size_t at, size_t end, std::vector<wave> &out)
{
	wave w;
	bool have_data = false;
	while (at + 8 <= end) {
		const u8 *c = &f[at];
		const u32 len = rd32(c + 4);
		const size_t body = at + 8;
		if (body + len > end)
			break;
		if (is(c, "fmt ") && len >= 16) {
			w.channels = std::max<u16>(1, rd16(&f[body + 2]));
			w.rate = rd32(&f[body + 4]);
			w.bits = rd16(&f[body + 14]);
		} else if (is(c, "wsmp") && len >= 20) {
			const u32 cb = rd32(&f[body]);
			w.unity = rd16(&f[body + 4]);
			const u32 loops = rd32(&f[body + 16]);
			if (loops && cb + 16 <= len) {
				const size_t lp = body + cb;
				w.loop = true;
				w.loop_start = rd32(&f[lp + 8]);
				w.loop_length = rd32(&f[lp + 12]);
			}
		} else if (is(c, "data")) {
			w.data_offset = body;
			w.data_bytes = len;
			have_data = true;
		} else if (is(c, "LIST") && len >= 4 && is(&f[body], "INFO")) {
			size_t i = body + 4;
			while (i + 8 <= body + len) {
				const u32 l = rd32(&f[i + 4]);
				if (is(&f[i], "INAM") && i + 8 + l <= body + len) {
					std::string n(reinterpret_cast<const char *>(&f[i + 8]), l);
					while (!n.empty() && (n.back() == '\0' || n.back() == ' '))
						n.pop_back();
					w.name = n;
				}
				i += 8 + l + (l & 1);
			}
		}
		at = body + len + (len & 1);
	}
	if (!have_data)
		return;
	const u32 frame_bytes = u32(w.channels) * std::max<u32>(1, w.bits / 8);
	w.frames = u32(w.data_bytes / frame_bytes);
	out.push_back(w);
}

// 入れ子の LIST をたどって、LIST wave を見つける
void walk(const std::vector<u8> &f, size_t at, size_t end, std::vector<wave> &out, int depth)
{
	while (at + 8 <= end && depth < 16) {
		const u8 *c = &f[at];
		const u32 len = rd32(c + 4);
		const size_t body = at + 8;
		if (body + len > end)
			break;
		if ((is(c, "LIST") || is(c, "RIFF")) && len >= 4) {
			if (is(&f[body], "wave"))
				read_wave(f, body + 4, body + len, out);
			else
				walk(f, body + 4, body + len, out, depth + 1);
		}
		at = body + len + (len & 1);
	}
}

} // namespace

bool parse(const std::vector<u8> &file, std::vector<wave> &out, std::string &err)
{
	out.clear();
	if (file.size() < 12 || !is(file.data(), "RIFF")) {
		err = "not a RIFF file";
		return false;
	}
	walk(file, 0, file.size(), out, 0);
	return true;
}

std::vector<s16> pcm(const std::vector<u8> &file, const wave &w)
{
	std::vector<s16> out;
	if (w.data_offset + w.data_bytes > file.size())
		return out;
	const u8 *d = &file[w.data_offset];
	out.resize(w.frames);
	for (u32 i = 0; i < w.frames; i++) {
		int sum = 0;
		for (u32 ch = 0; ch < w.channels; ch++) {
			if (w.bits == 8)
				sum += (int(d[i * w.channels + ch]) - 128) << 8;
			else
				sum += s16(rd16(&d[(size_t(i) * w.channels + ch) * 2]));
		}
		out[i] = s16(sum / int(w.channels));
	}
	return out;
}

} // namespace smu2000::m2a

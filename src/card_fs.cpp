// license:BSD-3-Clause

#include "card_fs.h"

#include <map>

namespace smu2000::cardfs {

namespace {

constexpr u32 PAGE = 512, PB = 528, PPB = 32;

u16 rd16(const u8 *p) { return u16(p[0] | p[1] << 8); }
u32 rd32(const u8 *p) { return u32(p[0] | p[1] << 8 | p[2] << 16 | u32(p[3]) << 24); }

// 論理セクターを読む道具。物理ブロックと論理ブロックの対応を最初に作る
struct volume {
	const std::vector<u8> &raw;
	std::map<u32, u32> lmap;   // 論理ブロック → 物理ブロック
	u32 spc = 0, fat_start = 0, fat_sectors = 0, root_start = 0, root_entries = 0, data_start = 0;
	bool fat16 = false;
	std::vector<u8> fat;

	explicit volume(const std::vector<u8> &r) : raw(r) {}

	bool open(std::string &err)
	{
		const u32 blocks = u32(raw.size() / (PB * PPB));
		if (!blocks) {
			err = "not a SmartMedia image";
			return false;
		}
		for (u32 pb = 0; pb < blocks; pb++) {
			const u8 *sp = &raw[(size_t(pb) * PPB) * PB + PAGE];
			const u16 a = u16(sp[6] << 8 | sp[7]);
			if ((a & 0xf000) != 0x1000)
				continue;   // CIS（0000）・空き（ffff）・壊れたブロック
			const u32 lb = (a >> 1) & 0x3ff;
			lmap.emplace((pb / 1024) * 1000 + lb, pb);
		}
		std::vector<u8> s;
		sector(0, s);
		if (s[510] != 0x55 || s[511] != 0xaa) {
			err = "the card is not formatted";
			return false;
		}
		const u32 start = rd32(&s[454]);
		sector(start, s);
		if (rd16(&s[11]) != PAGE || !s[13] || s[16] == 0) {
			err = "the card is not formatted";
			return false;
		}
		spc = s[13];
		fat_start = start + rd16(&s[14]);
		fat_sectors = rd16(&s[22]);
		root_entries = rd16(&s[17]);
		root_start = fat_start + u32(s[16]) * fat_sectors;
		data_start = root_start + root_entries * 32 / PAGE;
		fat16 = std::string(reinterpret_cast<const char *>(&s[54]), 5) == "FAT16";
		fat.clear();
		for (u32 i = 0; i < fat_sectors; i++) {
			sector(fat_start + i, s);
			fat.insert(fat.end(), s.begin(), s.end());
		}
		return true;
	}

	void sector(u32 n, std::vector<u8> &out) const
	{
		out.assign(PAGE, 0xff);
		const auto it = lmap.find(n / PPB);
		if (it == lmap.end())
			return;
		const size_t o = (size_t(it->second) * PPB + n % PPB) * PB;
		if (o + PAGE <= raw.size())
			out.assign(raw.begin() + long(o), raw.begin() + long(o + PAGE));
	}

	u32 next(u32 c) const
	{
		if (fat16)
			return size_t(c) * 2 + 1 < fat.size() ? rd16(&fat[size_t(c) * 2]) : 0xffff;
		const size_t o = size_t(c) * 3 / 2;
		if (o + 1 >= fat.size())
			return 0xfff;
		const u16 v = rd16(&fat[o]);
		return (c & 1) ? (v >> 4) : (v & 0xfff);
	}

	bool chain_end(u32 c) const { return c < 2 || c >= (fat16 ? 0xfff8u : 0xff8u); }

	// クラスタの鎖を size バイトまで読む（size が 0 なら鎖の終わりまで。ディレクトリ用）
	void read_chain(u32 c, u32 size, std::vector<u8> &out) const
	{
		out.clear();
		std::vector<u8> s;
		for (u32 guard = 0; !chain_end(c) && guard < 65536 && (!size || out.size() < size); guard++) {
			for (u32 i = 0; i < spc; i++) {
				sector(data_start + (c - 2) * spc + i, s);
				out.insert(out.end(), s.begin(), s.end());
			}
			c = next(c);
		}
		if (size && out.size() > size)
			out.resize(size);
	}

	void walk(const std::vector<u8> &dir, const std::string &prefix, std::vector<entry> &out, int depth) const
	{
		for (size_t i = 0; i + 32 <= dir.size(); i += 32) {
			const u8 *e = &dir[i];
			if (e[0] == 0)
				break;
			if (e[0] == 0xe5 || e[11] == 0x0f || (e[11] & 0x08))
				continue;   // 消したもの・長い名前・ボリューム名
			std::string name(reinterpret_cast<const char *>(e), 8), ext(reinterpret_cast<const char *>(e + 8), 3);
			while (!name.empty() && name.back() == ' ')
				name.pop_back();
			while (!ext.empty() && ext.back() == ' ')
				ext.pop_back();
			if (name == "." || name == "..")
				continue;
			const std::string full = prefix + name + (ext.empty() ? "" : "." + ext);
			const u32 clus = rd16(e + 26);
			if (e[11] & 0x10) {
				if (depth < 8) {
					std::vector<u8> sub;
					read_chain(clus, 0, sub);
					walk(sub, full + "/", out, depth + 1);
				}
				continue;
			}
			out.push_back({ full, rd32(e + 28), clus });
		}
	}

	void root(std::vector<u8> &out) const
	{
		out.clear();
		std::vector<u8> s;
		for (u32 i = 0; i < root_entries * 32 / PAGE; i++) {
			sector(root_start + i, s);
			out.insert(out.end(), s.begin(), s.end());
		}
	}
};

} // namespace

bool list(const std::vector<u8> &raw, std::vector<entry> &out, std::string &err)
{
	out.clear();
	volume v(raw);
	if (!v.open(err))
		return false;
	std::vector<u8> r;
	v.root(r);
	v.walk(r, "", out, 0);
	return true;
}

bool read(const std::vector<u8> &raw, const std::string &path, std::vector<u8> &out, std::string &err)
{
	out.clear();
	volume v(raw);
	if (!v.open(err))
		return false;
	std::vector<u8> r;
	v.root(r);
	std::vector<entry> all;
	v.walk(r, "", all, 0);
	for (const entry &e : all)
		if (e.path == path) {
			v.read_chain(e.cluster, e.size, out);
			if (out.size() != e.size) {
				err = "the file is cut short on the card";
				return false;
			}
			return true;
		}
	err = "no such file on the card: " + path;
	return false;
}

} // namespace smu2000::cardfs

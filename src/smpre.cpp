// サンプリングの管理情報を探すための道具（解析用。make test には入れない）。
//
//   smpre <rom ディレクトリ> <手順ファイル> <出力ディレクトリ>
//
// 手順ファイルは 1 行に 1 つ:
//   press <ボタン名> [回数]   ボタンを押して離す（button_name() の名前）
//   hold <ボタン名> <ms>      押したまま ms 回して離す
//   pump <ms>                 時間を進める
//   sine <振幅> [周波数]       A/D INPUT（AD1・AD2 とも）に正弦を流す。0 で止める
//   sysex <16 進の並び>        MIDI IN A へ送る
//   snap <名前>               ワーク RAM・DRAM・サンプリング RAM を <名前>.ram/.dram/.smp に書き出す
//   lcd                       液晶の 2 行を出す
// # から後は無視する。押すたびに液晶を出す
#include "mu2000.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr u32 RATE = 44100;
constexpr double PI = 3.14159265358979323846;

struct rig {
	mu2000 mu;
	double amp = 0.0, freq = 440.0;
	u64 n = 0;
	bool collect = false;
	std::vector<double> out;

	void pump(u32 ms)
	{
		const u64 until = n + u64(ms) * RATE / 1000;
		for (; n < until; n++) {
			const s32 v = s32(std::lround(amp * std::sin(2 * PI * freq * double(n) / RATE)));
			mu.set_audio_input(v, v);
			s32 l, r;
			mu.run_sample(l, r);
			u8 b;
			while (mu.midi_out_take(b)) {}
			if (collect)
				out.push_back((double(l) + double(r)) * 0.5 / mu2000::DAC_FULL_SCALE);
		}
	}

	std::string lcd()
	{
		const u8 *dd = mu.lcd().ddram();
		std::string s;
		for (int row = 0; row < 2; row++) {
			for (int c = 0; c < 24; c++) {
				const u8 ch = dd[row * 0x40 + c];
				s += (ch >= 32 && ch < 127) ? char(ch) : '.';
			}
			if (!row)
				s += '|';
		}
		return s;
	}
};

// ボタンの名前は空白を抜いて小文字で比べる（"Select >" は select>、"Value +" は value+）
std::string squash(const char *s)
{
	std::string o;
	for (; *s; s++)
		if (*s != ' ')
			o += char(std::tolower(static_cast<unsigned char>(*s)));
	return o;
}

bool find_button(const std::string &name, mu2000::button &out)
{
	for (int i = 0; i < int(mu2000::button::count); i++) {
		const char *nm = mu2000::button_name(mu2000::button(i));
		if (nm && squash(name.c_str()) == squash(nm)) {
			out = mu2000::button(i);
			return true;
		}
	}
	return false;
}

// 周波数 f の成分の大きさ（Goertzel）
double tone(const std::vector<double> &x, double f)
{
	const double w = 2 * PI * f / RATE, c = 2 * std::cos(w);
	double s1 = 0, s2 = 0;
	for (double v : x) {
		const double s0 = v + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / double(x.size());
}

void write(const std::string &path, const std::vector<u8> &v)
{
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char *>(v.data()), std::streamsize(v.size()));
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "使い方: smpre <rom ディレクトリ> <手順ファイル> <出力ディレクトリ>\n");
		return 1;
	}
	const std::string dir = argv[1], out = argv[3];
	static rig g;
	if (!g.mu.load_program(dir + "/mu2000_flash.bin") || !g.mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", g.mu.error().c_str());
		return 1;
	}
	g.mu.load_sintab(dir + "/standin/sin-table.bin");
	g.mu.reset();
	for (u32 i = 0; i < 30 * RATE && !g.mu.midi_ready(); i += RATE / 100)
		g.pump(10);
	g.pump(1500);
	std::printf("起動した [%s]\n", g.lcd().c_str());

	std::ifstream script(argv[2]);
	std::string line;
	int lineno = 0;
	while (std::getline(script, line)) {
		lineno++;
		const size_t hash = line.find('#');
		if (hash != std::string::npos)
			line.resize(hash);
		std::istringstream ss(line);
		std::string cmd;
		if (!(ss >> cmd))
			continue;
		if (cmd == "press" || cmd == "hold") {
			std::string name;
			int count = 1;
			ss >> name;
			ss >> count;                  // press では回数、hold では ms
			mu2000::button b;
			if (!find_button(name, b)) {
				std::fprintf(stderr, "%d: ボタンが無い: %s\n", lineno, name.c_str());
				return 1;
			}
			const int times = cmd == "press" ? std::max(1, count) : 1;
			for (int i = 0; i < times; i++) {
				g.mu.set_button(b, true);
				g.pump(cmd == "hold" ? u32(count) : 80);
				g.mu.set_button(b, false);
				g.pump(300);
			}
			std::printf("%-6s %-14s [%s]\n", cmd.c_str(), name.c_str(), g.lcd().c_str());
		} else if (cmd == "pump") {
			u32 ms = 0;
			ss >> ms;
			g.pump(ms);
		} else if (cmd == "sine") {
			ss >> g.amp;
			double f;
			if (ss >> f)
				g.freq = f;
		} else if (cmd == "sysex") {
			std::string h;
			while (ss >> h)
				g.mu.midi_in(u8(std::strtoul(h.c_str(), nullptr, 16)), 0);
			g.pump(100);
		} else if (cmd == "snap") {
			std::string name;
			ss >> name;
			write(out + "/" + name + ".ram", g.mu.work_ram());
			write(out + "/" + name + ".dram", g.mu.dram());
			write(out + "/" + name + ".smp", g.mu.sample_ram());
			std::printf("snap   %s\n", name.c_str());
		} else if (cmd == "poke") {
			std::string a, h;
			ss >> a;
			u32 addr = u32(std::strtoul(a.c_str(), nullptr, 16));
			while (ss >> h)
				if (!g.mu.poke(addr++, u8(std::strtoul(h.c_str(), nullptr, 16)))) {
					std::fprintf(stderr, "%d: 書けない番地 %s\n", lineno, a.c_str());
					return 1;
				}
			std::printf("poke   %s\n", a.c_str());
		} else if (cmd == "copy") {
			// copy <ram|dram|smp> <スナップの名前> <開始 16 進> <長さ 16 進>
			// 前に書き出したスナップの同じ場所を、いまの機械へ写す（ram・dram は CPU の番地、smp はバイトの位置）
			std::string kind, name, a, l;
			ss >> kind >> name >> a >> l;
			const u32 start = u32(std::strtoul(a.c_str(), nullptr, 16)), len = u32(std::strtoul(l.c_str(), nullptr, 16));
			std::ifstream f(out + "/" + name + "." + kind, std::ios::binary);
			std::vector<u8> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			const u32 base = kind == "ram" ? 0x400000 : kind == "dram" ? 0x1000000 : 0;
			for (u32 i = 0; i < len; i++) {
				const u32 at = start + i;
				if (at - base >= v.size() ||
				    !(kind == "smp" ? g.mu.poke_sample(at, v[at]) : g.mu.poke(at, v[at - base]))) {
					std::fprintf(stderr, "%d: 写せない %s %x\n", lineno, kind.c_str(), at);
					return 1;
				}
			}
			std::printf("copy   %s %s %x +%x\n", kind.c_str(), name.c_str(), start, len);
		} else if (cmd == "measure") {
			// ms のあいだ出力を集めて、大きさと 330/440/660/880Hz の成分を出す
			u32 ms = 0;
			ss >> ms;
			g.out.clear();
			g.collect = true;
			g.pump(ms);
			g.collect = false;
			double sum = 0;
			for (double v : g.out)
				sum += v * v;
			// いちばん強い周波数（200-1800Hz を 1Hz おきに探してから 0.01Hz まで詰める）
			double best_f = 0, best = 0;
			for (double f = 200; f <= 1800; f += 1.0)
				if (const double t = tone(g.out, f); t > best) { best = t; best_f = f; }
			for (double f = best_f - 1.0; f <= best_f + 1.0; f += 0.01)
				if (const double t = tone(g.out, f); t > best) { best = t; best_f = f; }
			std::printf("measure rms %.4f  330 %.5f 440 %.5f 660 %.5f 880 %.5f  peak %.2f Hz\n",
			            std::sqrt(sum / std::max<size_t>(1, g.out.size())),
			            tone(g.out, 330), tone(g.out, 440), tone(g.out, 660), tone(g.out, 880), best_f);
		} else if (cmd == "add") {
			// add <名前> <周波数1> <ms1> [<周波数2> <ms2>]: 正弦をつないだサンプルを直に足す（src/sampling.cpp）
			std::string name;
			double f1 = 440, f2 = 0;
			u32 ms1 = 0, ms2 = 0;
			ss >> name >> f1 >> ms1 >> f2 >> ms2;
			std::vector<s16> pcm;
			for (u32 i = 0; i < ms1 * RATE / 1000; i++)
				pcm.push_back(s16(std::lround(12000 * std::sin(2 * PI * f1 * i / RATE))));
			for (u32 i = 0; i < ms2 * RATE / 1000; i++)
				pcm.push_back(s16(std::lround(12000 * std::sin(2 * PI * f2 * i / RATE))));
			std::string err;
			const int n = g.mu.sampling_add(pcm.data(), pcm.size(), name, err);
			std::printf("add    %s → %d %s (%zu)\n", name.c_str(), n, err.c_str(), pcm.size());
		} else if (cmd == "tones") {
			// tones <ms> <区切り ms>: ms のあいだ出力を集め、区切りごとに大きさと 440/660 の成分を出す
			u32 ms = 0, step = 100;
			ss >> ms >> step;
			g.out.clear();
			g.collect = true;
			g.pump(ms);
			g.collect = false;
			const size_t w = size_t(step) * RATE / 1000;
			for (size_t i = 0; i + w <= g.out.size(); i += w) {
				const std::vector<double> seg(g.out.begin() + long(i), g.out.begin() + long(i + w));
				double sum = 0;
				for (double v : seg)
					sum += v * v;
				std::printf("  %5zums rms %.4f  440 %.4f 660 %.4f\n", i * 1000 / RATE, std::sqrt(sum / double(w)),
				            tone(seg, 440), tone(seg, 660));
			}
		} else if (cmd == "loop") {
			// loop <番号> <0|1> [ループの頭]
			int n = 0, on = 0;
			u32 at = 0;
			ss >> n >> on >> at;
			std::printf("loop   %d %s\n", n, g.mu.sampling_loop(n, on != 0, at) ? "ok" : "無い");
		} else if (cmd == "scan") {
			// scan <16 進の番地> <16 進の長さ> <値…>: 1 バイトずつ値を書いて PGM001 の鍵 60 を押し、音量の移り変わりを出して戻す。
			// 押して 5・20・50・150・400・900ms、離して 30・150・400ms の大きさ（その前後 10ms の rms）
			std::string a, l, h;
			ss >> a >> l;
			const u32 start = u32(std::strtoul(a.c_str(), nullptr, 16)), len = u32(std::strtoul(l.c_str(), nullptr, 16));
			std::vector<u8> vals;
			while (ss >> h)
				vals.push_back(u8(std::strtoul(h.c_str(), nullptr, 16)));
			auto rms_at = [&](u32 ms) {
				const size_t c = size_t(ms) * RATE / 1000, w = RATE / 100;
				double sum = 0;
				size_t k = 0;
				for (size_t i = c >= w / 2 ? c - w / 2 : 0; i < c + w / 2 && i < g.out.size(); i++, k++)
					sum += g.out[i] * g.out[i];
				return k ? std::sqrt(sum / double(k)) : 0.0;
			};
			auto probe = [&]() {
				for (u8 b : { 0xb0, 0x78, 0x00, 0xc0, 0x01 })
					g.mu.midi_in(b, 0);
				g.pump(50);
				for (u8 b : { 0xc0, 0x00 })
					g.mu.midi_in(b, 0);
				g.pump(150);
				g.out.clear();
				g.collect = true;
				for (u8 b : { 0x90, 0x3c, 0x64 })
					g.mu.midi_in(b, 0);
				g.pump(1000);
				for (u8 b : { 0x80, 0x3c, 0x40 })
					g.mu.midi_in(b, 0);
				g.pump(500);
				g.collect = false;
				g.pump(1500);   // 長い余韻が次へ残らないように
				std::string s;
				char buf[16];
				for (u32 ms : { 5u, 20u, 50u, 100u, 200u, 400u, 700u, 990u, 1020u, 1060u, 1120u, 1250u, 1490u }) {
					std::snprintf(buf, sizeof(buf), " %5.2f", rms_at(ms) * 100);
					s += buf;
				}
				return s;
			};
			std::printf("scan   基準         %s\n", probe().c_str());
			for (u32 i = 0; i < len; i++) {
				const u32 at = start + i;
				const u8 orig = g.mu.dram()[at - 0x1000000];
				for (u8 v : vals) {
					if (v == orig)
						continue;
					g.mu.poke(at, v);
					std::printf("  %x %02x→%02x %s\n", at, orig, v, probe().c_str());
				}
				g.mu.poke(at, orig);
				std::fflush(stdout);
			}
		} else if (cmd == "card") {
			// card <MB>: 空のカードを作って差す
			u32 mb = 32;
			ss >> mb;
			std::printf("card   %u MB %s\n", mb, g.mu.card().create(mb) ? "ok" : "だめ");
		} else if (cmd == "cardload") {
			// cardload <ファイル>: カードの画像ファイルを差す
			std::string path, err;
			ss >> path;
			std::printf("cardload %s %s\n", path.c_str(), g.mu.card().load(path, err) ? "ok" : err.c_str());
		} else if (cmd == "cardfmt") {
			// cardfmt: 差したカードに smartmedia::format の論理の書式を書く
			std::printf("cardfmt %s\n", g.mu.card().format() ? "ok" : "だめ");
		} else if (cmd == "cardsave") {
			// cardsave <名前>: カードの生の並びを <名前>.sm に
			std::string name, err;
			ss >> name;
			std::printf("cardsave %s %s\n", name.c_str(), g.mu.card().save(out + "/" + name + ".sm", err) ? "ok" : err.c_str());
		} else if (cmd == "waitgone") {
			// waitgone <文字列> <最大 ms>: 液晶からその文字列が消えるまで回す
			std::string what;
			u32 ms = 10000;
			ss >> what >> ms;
			for (u32 t = 0; t < ms && g.lcd().find(what) != std::string::npos; t += 100)
				g.pump(100);
			std::printf("wait   [%s]\n", g.lcd().c_str());
		} else if (cmd == "lcd") {
			std::printf("lcd    [%s]\n", g.lcd().c_str());
		} else {
			std::fprintf(stderr, "%d: 知らない命令: %s\n", lineno, cmd.c_str());
			return 1;
		}
	}
	return 0;
}

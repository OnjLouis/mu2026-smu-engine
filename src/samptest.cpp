// サンプリングが一回りするかを確かめる。
//
//   samptest <rom ディレクトリ> [-v]
//
// パネルで SAMPLING → REC に入り、A/D INPUT に 440Hz の正弦を流しながら 1 秒ほど録音して止め、
// 残す。SAMPLE の画面で AUDITION を押し、出てきた音が 440Hz かを見る。
// firmware が録音に使う SWP30 の働き（サンプリング RAM と波形アクセス 0x7000）が
// 正しくないと、サンプルが出来ないか、試聴で別の音か無音になる。
// 続けて A/D パートの音量と、SmartMedia への書き出し・読み戻し（書式化 → SAVE → 別の機械で LOAD）、
// REC の InputSrc（AD2 / AD1+2）で録るものが変わるかを見る。
// 食い違えば 1 を返す。
#include "mu2000.h"
#include "ui/bridge.h"
#include "ui/driver.h"
#include "wav_in.h"
#include "card_fs.h"
#include "m2a.h"
#include "wavegen.h"
#include "ui/panel_macro.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr u32 RATE = 44100;
constexpr double PI = 3.14159265358979323846;

struct rig {
	mu2000 mu;
	bool verbose = false;
	double sine_amp = 0.0;          // A/D INPUT に流す正弦の振幅（0 なら無音）
	double sine2_amp = -1.0;        // AD2 だけ 660Hz にするときの振幅（負なら AD1 と同じもの）
	u64 n = 0;
	std::vector<double> out;        // 集めている間の出力（左右の平均）
	double sum_l = 0, sum_r = 0;    // 集めている間の左・右の二乗の和（パンを見る）
	bool collect = false;

	void pump(u32 ms)
	{
		const u64 until = n + u64(ms) * RATE / 1000;
		for (; n < until; n++) {
			const s32 v = s32(std::lround(sine_amp * std::sin(2 * PI * 440.0 * double(n) / RATE)));
			const s32 v2 = sine2_amp < 0 ? v : s32(std::lround(sine2_amp * std::sin(2 * PI * 660.0 * double(n) / RATE)));
			mu.set_audio_input(v, v2);
			s32 l, r;
			mu.run_sample(l, r);
			u8 b;
			while (mu.midi_out_take(b)) {}
			if (collect) {
				out.push_back((double(l) + double(r)) * 0.5 / mu2000::DAC_FULL_SCALE);
				sum_l += (double(l) / mu2000::DAC_FULL_SCALE) * (double(l) / mu2000::DAC_FULL_SCALE);
				sum_r += (double(r) / mu2000::DAC_FULL_SCALE) * (double(r) / mu2000::DAC_FULL_SCALE);
			}
		}
	}

	std::string lcd()
	{
		const u8 *dd = mu.lcd().ddram();
		std::string s;
		for (int row = 0; row < 2; row++) {
			for (int c = 0; c < 24; c++) {
				const u8 ch = dd[row * 0x40 + c];
				s += (ch >= 32 && ch < 127) ? char(ch) : ' ';
			}
			if (!row)
				s += '|';
		}
		return s;
	}

	void press(mu2000::button b, u32 hold_ms = 80)
	{
		mu.set_button(b, true);
		pump(hold_ms);
		mu.set_button(b, false);
		pump(300);
		if (verbose)
			std::printf("  %-14s [%s]\n", mu2000::button_name(b), lcd().c_str());
	}
};

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

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "使い方: samptest <rom ディレクトリ> [-v]\n");
		return 1;
	}
	const std::string dir = argv[1];
	static rig g;
	g.verbose = argc > 2 && !std::strcmp(argv[2], "-v");

	if (!g.mu.load_program(dir + "/mu2000_flash.bin") || !g.mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", g.mu.error().c_str());
		return 1;
	}
	g.mu.load_sintab(dir + "/standin/sin-table.bin");
	g.mu.reset();
	for (u32 i = 0; i < 30 * RATE && !g.mu.midi_ready(); i += RATE / 100)
		g.pump(10);
	g.pump(1500);

	int bad = 0;
	auto expect = [&](const char *what, const char *text) {
		const std::string s = g.lcd();
		const bool ok = s.find(text) != std::string::npos;
		std::printf("%s %-28s [%s]\n", ok ? "合" : "NG", what, s.c_str());
		if (!ok)
			bad++;
	};

	using B = mu2000::button;
	// SAMPLING の品書き: EDIT LOAD SAVE / REC UTIL RAM。REC は 4 つ目
	g.press(B::sampling_mode);
	expect("SAMPLING の品書き", "REC");
	for (int i = 0; i < 3; i++)
		g.press(B::select_right);
	g.press(B::enter);
	expect("REC の画面", "Sp=001");

	// 録音。始める前から正弦を流しておく
	g.sine_amp = 12000;
	g.pump(200);
	g.press(B::enter);
	expect("録音中", "Recording!");
	g.pump(1000);
	g.press(B::enter);                  // 止める
	g.sine_amp = 0;
	g.pump(300);
	g.press(B::exit);
	expect("残すか聞かれる", "Keep Sample 001?");
	g.press(B::enter);
	g.press(B::exit);

	// EDIT → SAMPLE → SMPL001 で試聴
	for (int i = 0; i < 3; i++)
		g.press(B::select_left);
	g.press(B::enter);
	g.press(B::enter);
	expect("出来たサンプル", "SMPL001");
	g.collect = true;
	g.mu.set_button(B::audition, true);
	g.pump(1000);
	g.mu.set_button(B::audition, false);
	g.collect = false;

	double rms = 0;
	for (double v : g.out)
		rms += v * v;
	rms = std::sqrt(rms / std::max<size_t>(1, g.out.size()));
	const double t440 = tone(g.out, 440), t330 = tone(g.out, 330), t587 = tone(g.out, 587);
	const bool loud = rms > 0.01;
	const bool pitch = t440 > 10 * std::max(t330, t587);
	std::printf("%s 試聴の音の大きさ              rms %.4f（全振幅 1）\n", loud ? "合" : "NG", rms);
	std::printf("%s 試聴の音が 440Hz              440Hz %.5f / 330Hz %.5f / 587Hz %.5f\n",
	            pitch ? "合" : "NG", t440, t330, t587);
	if (!loud) bad++;
	if (!pitch) bad++;

	// A/D パート。既定の音量は 0 で、入力は聞こえない。音量を上げると A/D INPUT がそのまま鳴る
	// （スレーブの MELI 6/7 を firmware がミキサに通す）
	g.press(B::exit);
	g.press(B::exit);
	g.press(B::exit);
	auto level_440 = [&](double &rms_out) {
		g.out.clear();
		g.sine_amp = 8000;
		g.pump(200);
		g.collect = true;
		g.pump(500);
		g.collect = false;
		g.sine_amp = 0;
		double sum = 0;
		for (double v : g.out)
			sum += v * v;
		rms_out = std::sqrt(sum / std::max<size_t>(1, g.out.size()));
		return tone(g.out, 440);
	};
	double rms_off = 0, rms_on = 0;
	const double ad_off = level_440(rms_off);
	for (int part = 0; part < 2; part++) {
		const u8 msg[] = { 0xf0, 0x43, 0x10, 0x4c, 0x10, u8(part), 0x0b, 100, 0xf7 };
		for (u8 b : msg)
			g.mu.midi_in(b, 0);
	}
	g.pump(300);
	const double ad_on = level_440(rms_on);
	const bool off_ok = rms_off < 0.001;
	const bool on_ok = rms_on > 0.05 && ad_on > 10 * std::max(tone(g.out, 330), tone(g.out, 587));
	std::printf("%s A/D パートの音量 0 では無音      rms %.5f\n", off_ok ? "合" : "NG", rms_off);
	std::printf("%s A/D パートの音量 100 で入力が鳴る rms %.4f / 440Hz %.5f\n", on_ok ? "合" : "NG", rms_on, ad_on);
	(void)ad_off;
	if (!off_ok) bad++;
	if (!on_ok) bad++;

	// SmartMedia。空のカードを差して UTIL → CARD → Format で書式化し、SAMPLING → SAVE で ALL+SEQ を書く。
	// 書いたカードを新しい機械に差し、SAMPLING → LOAD で読み戻して、サンプリング RAM が同じになるかを見る。
	// SmartMedia の NAND の命令・物理の書式・ECC と、SWP30 の続けて読む働き（波形アクセス 0x9000）を通る
	g.mu.card().create(32);
	g.pump(500);
	g.press(B::util);
	for (int i = 0; i < 4; i++)
		g.press(B::select_right);
	g.press(B::enter);
	for (int i = 0; i < 4; i++)
		g.press(B::select_right);
	expect("UTIL → CARD → Format", "Format");
	g.press(B::enter);
	g.press(B::enter);                  // 書式化してよいか
	for (int i = 0; i < 100 && g.lcd().find("Executing") != std::string::npos; i++)
		g.pump(100);
	expect("書式化を終えた", "Format");
	// 新しいカードを作るとき（smartmedia::format）に書く論理の書式は、firmware の書式化と同じ
	{
		for (int i = 0; i < 50; i++)
			g.pump(100);   // 書式化の後片付けが残っていれば待つ
		smu2000::smartmedia fresh;
		const bool made = fresh.create(32) && fresh.format();
		const bool same = made && fresh.raw() == g.mu.card().raw();
		std::printf("%s 新しいカードの書式が firmware の書式化と同じ\n", same ? "合" : "NG");
		if (!same)
			bad++;
	}
	g.press(B::exit);
	g.press(B::exit);
	g.press(B::exit);

	g.press(B::sampling_mode);
	g.press(B::select_right);
	g.press(B::select_right);
	g.press(B::enter);
	expect("SAVE の画面", "ALL+SEQ");
	g.press(B::enter);                  // 保存先のディレクトリ
	g.pump(1000);
	g.press(B::enter);                  // ファイルの名前
	g.pump(1000);
	expect("ファイルの名前", "ALL_SEQ");
	g.press(B::enter);
	expect("書き出し中", "SAVING");
	for (int i = 0; i < 100 && g.lcd().find("SAVING") != std::string::npos; i++)
		g.pump(100);
	expect("書き終えた", "<SAVE>");

	// カードの中身を本体を通さずに読む（サンプリングの窓の「カード」。card_fs.h・m2a.h）。
	// 書いた M2A の波形が、firmware の表のサンプル 1 と同じ長さ・同じ音で、そのまま試聴できる
	std::vector<u8> saved_m2a;
	{
		std::vector<smu2000::cardfs::entry> files;
		std::vector<u8> m2a;
		std::vector<smu2000::m2a::wave> waves;
		std::string err;
		bool listed = smu2000::cardfs::list(g.mu.card().raw(), files, err);
		bool found = false;
		for (const auto &e : files)
			found |= e.path == "ALL_SEQ.M2A";
		const bool read = found && smu2000::cardfs::read(g.mu.card().raw(), "ALL_SEQ.M2A", m2a, err);
		const bool parsed = read && smu2000::m2a::parse(m2a, waves, err);
		const auto list = g.mu.sampling_list();
		const bool same_len = parsed && waves.size() == 1 && !list.empty() &&
		                      waves[0].frames + 2 >= list[0].frames() && waves[0].frames <= list[0].frames();
		std::vector<s16> pcm = same_len ? smu2000::m2a::pcm(m2a, waves[0]) : std::vector<s16>();
		std::vector<double> x;
		for (size_t i = RATE / 10; i < pcm.size() && i < RATE / 2; i++)
			x.push_back(pcm[i] / 32768.0);
		const bool tone_ok = !x.empty() && tone(x, 440) > 10 * std::max(tone(x, 330), tone(x, 587));
		std::printf("%s カードを外から読む             %zu ファイル、波形 %zu 個、%u / %u サンプル %s\n",
		            listed && tone_ok ? "合" : "NG", files.size(), waves.size(), waves.empty() ? 0u : waves[0].frames,
		            list.empty() ? 0u : list[0].frames(), err.c_str());
		if (!(listed && tone_ok))
			bad++;
		saved_m2a = m2a;

		// 外の PCM の試聴（preview_pcm）。音源を通さずに鳴り、終われば止まる
		g.out.clear();
		const size_t n = pcm.size();
		g.mu.preview_pcm(std::move(pcm));
		g.collect = true;
		g.pump(200);
		g.collect = false;
		const bool playing = g.mu.preview_number() == -1;
		const double p440 = tone(g.out, 440), p330 = tone(g.out, 330);
		g.pump(u32(n * 1000 / RATE) + 100);
		const bool stopped = g.mu.preview_number() == 0;
		const bool prev_ok = n && playing && stopped && p440 > 0.01 && p440 > 10 * p330;
		std::printf("%s 外の PCM の試聴                440Hz %.4f / 330Hz %.5f\n", prev_ok ? "合" : "NG", p440, p330);
		if (!prev_ok)
			bad++;

		// 鳴らしたまま波形を差し替える（「波形を作る」で値を変えたとき）。keep_pos なら位置を続け、音は新しい波形になる。
		// keep_pos でなければ頭から
		std::vector<s16> wa(4000), wb(4000);
		for (size_t i = 0; i < wa.size(); i++) {
			wa[i] = s16(std::lround(12000 * std::sin(2 * PI * 441.0 * double(i) / RATE)));   // 441Hz と 882Hz はどちらもループで閉じる
			wb[i] = s16(std::lround(12000 * std::sin(2 * PI * 882.0 * double(i) / RATE)));
		}
		g.mu.preview_pcm(wa, 0);
		g.pump(50);
		const u32 pos_before = g.mu.preview_pos();
		g.mu.preview_pcm(wb, 0, true);
		const u32 pos_kept = g.mu.preview_pos();
		g.out.clear();
		g.collect = true;
		g.pump(200);
		g.collect = false;
		const double s882 = tone(g.out, 882), s441 = tone(g.out, 441);
		g.mu.preview_pcm(wa, 0);
		const u32 pos_reset = g.mu.preview_pos();
		g.mu.preview_stop();
		const bool swap_ok = pos_before > 1000 && pos_kept == pos_before && pos_reset == 0 && s882 > 0.01 && s882 > 10 * s441;
		std::printf("%s 試聴を鳴らしたまま差し替える        位置 %u → %u、差し替えた後 882Hz %.4f / 441Hz %.5f\n", swap_ok ? "合" : "NG",
		            pos_before, pos_kept, s882, s441);
		if (!swap_ok)
			bad++;
	}

	static rig h;
	if (!h.mu.load_program(dir + "/mu2000_flash.bin") || !h.mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", h.mu.error().c_str());
		return 1;
	}
	h.verbose = g.verbose;
	h.mu.load_sintab(dir + "/standin/sin-table.bin");
	h.mu.reset();
	for (u32 i = 0; i < 30 * RATE && !h.mu.midi_ready(); i += RATE / 100)
		h.pump(10);
	// 読み戻すカードは、取り出した M2A を PC の側で新しいカードに書き直したもの（smartmedia::format に
	// ファイルを渡す。サンプリングの窓で M2A のファイルを直に読み込むときと同じ）。firmware がそれを読めるか
	smu2000::smartmedia made;
	{
		const u32 mb = smu2000::smartmedia::megabytes_for(saved_m2a.size());
		std::vector<smu2000::smartmedia::root_file> put(1);
		put[0].name = "FROMPC.M2A";
		put[0].bytes = saved_m2a;
		const bool ok_made = mb && made.create(mb) && made.format(put);
		std::vector<smu2000::cardfs::entry> files;
		std::vector<u8> back;
		std::string err;
		const bool same = ok_made && smu2000::cardfs::list(made.raw(), files, err) && files.size() == 1 &&
		                  smu2000::cardfs::read(made.raw(), "FROMPC.M2A", back, err) && back == saved_m2a;
		std::printf("%s M2A を新しいカードに書く         %u MB、%zu バイト %s\n", same ? "合" : "NG", mb, saved_m2a.size(), err.c_str());
		if (!same)
			bad++;

		// 16MB を超えるファイル（論理ブロックがゾーン 1 へ入る）と、2 つ目のファイルも書いて読み戻せる
		std::vector<smu2000::smartmedia::root_file> big(2);
		big[0].name = "BIG.M2A";
		big[0].bytes.resize(20u << 20);
		for (size_t i = 0; i < big[0].bytes.size(); i++)
			big[0].bytes[i] = u8((i * 2654435761u) >> 13);
		big[1].name = "SMALL.TXT";
		big[1].bytes.assign(100, u8('x'));
		smu2000::smartmedia large;
		const u32 mb2 = smu2000::smartmedia::megabytes_for(big[0].bytes.size() + big[1].bytes.size());
		std::vector<u8> b0, b1;
		const bool big_ok = mb2 == 32 && large.create(mb2) && large.format(big) &&
		                    smu2000::cardfs::read(large.raw(), "BIG.M2A", b0, err) && b0 == big[0].bytes &&
		                    smu2000::cardfs::read(large.raw(), "SMALL.TXT", b1, err) && b1 == big[1].bytes;
		std::printf("%s 20MB のファイルを 32MB のカードに  %s\n", big_ok ? "合" : "NG", err.c_str());
		if (!big_ok)
			bad++;
	}
	// 読み戻しは、サンプリングの窓の「この M2A を読み込む」と同じボタンの押し方（ui::panel_macro）で。
	// まず firmware が書いたカードから読み、次にカードを PC で書いたものへ差し替えて（card_swapped）読む。
	// firmware は前のカードの FAT を覚えているので、差し替えに気づかないと新しいカードのファイルが見つからない
	auto load_by_macro = [&](const char *name, const char *what) {
		ui::panel_macro macro;
		macro.start(ui::panel_macro::load_m2a(name), "done");
		std::string msg;
		bool finished = false;
		size_t last = ~size_t(0);
		for (int i = 0; i < 200 * 100 && !finished; i++) {
			h.pump(10);
			finished = macro.tick(h.mu, msg);
			if (g.verbose && macro.at() != last) {
				last = macro.at();
				std::printf("    段 %zu [%s]\n", last, h.lcd().c_str());
			}
		}
		const bool ok = finished && msg == "done";
		std::printf("%s %-28s [%s] %s\n", ok ? "合" : "NG", what, h.lcd().c_str(), msg.c_str());
		if (!ok) bad++;
	};
	h.mu.card() = g.mu.card();
	h.pump(1500);
	load_by_macro("ALL_SEQ.M2A", "ボタンのマクロで LOAD");
	h.mu.card() = made;
	h.mu.card_swapped();
	h.pump(1000);
	load_by_macro("FROMPC.M2A", "差し替えたカードから LOAD");
	// 窓の道（bridge::request_macro → driver::sampling_tick）は早送りする。10ms のブロックを回して、
	// 何ブロックで終わるか（= 実時間で鳴らしていたら何秒か）を見る。サンプルがあるので Overwrite ALL? も通る
	{
		ui::bridge br;
		ui::driver drv;
		br.request_macro(ui::panel_macro::load_m2a("FROMPC.M2A"), "done");
		int blocks = 0;
		ui::bridge::sampling_view view;
		for (; blocks < 3000; blocks++) {
			drv.pump_midi(h.mu, br);
			h.pump(10);
			br.get_sampling(view);
			if (!view.message.empty())
				break;
		}
		const bool ok = view.message == "done" && blocks < 300;
		std::printf("%s 窓の道の LOAD（早送り）        %d ブロック（実時間で %.2f 秒） %s\n", ok ? "合" : "NG", blocks,
		            blocks / 100.0, view.message.c_str());
		if (!ok) bad++;
	}
	// 録音の最後の 1 語の後ろ半分（サンプルの長さの外）は書き出されないので、そこだけは違ってよい
	const auto &a = g.mu.sample_ram(), &b = h.mu.sample_ram();
	size_t differ = 0, used = 0;
	for (size_t i = 0; i < a.size(); i++) {
		differ += a[i] != b[i];
		used += a[i] != 0;
	}
	const bool same = used > 50000 && differ <= 2;
	std::printf("%s 読み戻したサンプリング RAM     使っている %zu バイト、違う %zu バイト\n", same ? "合" : "NG", used, differ);
	if (!same) bad++;

	// REC の InputSrc。AD1 に 440Hz、AD2 に 660Hz を入れ、AD2 と AD1+2 で録る。
	// firmware は MELI 6/7 からミキサの出力 8 への音量を切り替えるので、録ったものの周波数で分かる
	auto record_src = [&](int presses, double &f440, double &f660) {
		g.press(B::exit);
		g.press(B::select_right);                       // SAVE の隣が REC
		g.press(B::enter);
		for (int i = 0; i < 3; i++)
			g.press(B::select_right);
		for (int i = 0; i < presses; i++)
			g.press(B::value_plus);
		for (int i = 0; i < 3; i++)
			g.press(B::select_left);
		const std::vector<u8> before = g.mu.sample_ram();
		g.sine_amp = 8000;
		g.sine2_amp = 8000;
		g.pump(200);
		g.press(B::enter);
		g.pump(700);
		g.press(B::enter);
		g.sine_amp = 0;
		g.sine2_amp = -1;
		g.pump(300);
		std::vector<double> x;
		const auto &after = g.mu.sample_ram();
		for (size_t i = 0; i + 1 < after.size(); i += 2)
			if (after[i] != before[i] || after[i + 1] != before[i + 1])
				x.push_back(double(s16(after[i] | (after[i + 1] << 8))) / 32768.0);
		// 途中の 4000 サンプルで見る（変わらなかったバイトを飛ばしているので、全部を繋ぐと位相が飛ぶ）
		const std::vector<double> mid = x.size() > 8000 ? std::vector<double>(x.begin() + 4000, x.begin() + 8000) : std::vector<double>();
		f440 = mid.empty() ? 0 : tone(mid, 440);
		f660 = mid.empty() ? 0 : tone(mid, 660);
		g.press(B::exit);                               // Keep Sample? から抜ける（残すかどうかは見ない）
		g.press(B::exit);
		return x.size();
	};
	{
		double a440 = 0, a660 = 0, b440 = 0, b660 = 0;
		const size_t na = record_src(1, a440, a660);    // AD1 → AD2
		const bool ad2 = a660 > 0.05 && a440 < a660 / 20;
		std::printf("%s InputSrc=AD2 で AD2 だけ録る    %zu サンプル、440Hz %.4f / 660Hz %.4f\n", ad2 ? "合" : "NG", na, a440, a660);
		const size_t nb = record_src(1, b440, b660);    // AD2 → AD1+2
		const bool both = b440 > 0.05 && b660 > 0.05;
		std::printf("%s InputSrc=AD1+2 で両方を録る     %zu サンプル、440Hz %.4f / 660Hz %.4f\n", both ? "合" : "NG", nb, b440, b660);
		if (!ad2) bad++;
		if (!both) bad++;
	}

	// REC の TriggerLvl。レベルを上げて Enter を押すと「Waiting!」で待ち、入力が来ると録音が始まる。
	// firmware は CPU の A/D 変換器の AN0 / AN2（A/D INPUT の大きさ）を回し続けて読む
	g.press(B::exit);
	g.press(B::select_right);
	g.press(B::enter);
	g.press(B::select_right);
	for (int i = 0; i < 6; i++)
		g.press(B::value_plus);
	expect("TriggerLvl を上げた", "TriggerLvl=06");
	g.press(B::select_left);
	g.press(B::enter);
	g.pump(500);
	expect("入力が無いと待つ", "Waiting!");
	g.sine_amp = 12000;
	g.pump(500);
	expect("入力が来ると録音する", "Recording!");
	g.press(B::enter);
	g.sine_amp = 0;
	g.pump(300);

	// パネルを通さない道（src/sampling.cpp）。新しい機械で、A/D INPUT から直に録って firmware の表に足し、
	// 音色に割り当てる。firmware がそれを自分のサンプルとして扱う（一覧・試聴・REC の続き・音色として鳴る）かを見る
	{
		namespace sp = smu2000::sampling;
		static rig k;
		if (!k.mu.load_program(dir + "/mu2000_flash.bin") || !k.mu.load_wave(dir + "/dump")) {
			std::fprintf(stderr, "%s\n", k.mu.error().c_str());
			return 1;
		}
		k.verbose = g.verbose;
		k.mu.load_sintab(dir + "/standin/sin-table.bin");
		k.mu.reset();
		for (u32 i = 0; i < 30 * RATE && !k.mu.midi_ready(); i += RATE / 100)
			k.pump(10);
		k.pump(1500);
		auto check = [&](bool ok, const char *what, const std::string &detail) {
			std::printf("%s %-28s %s\n", ok ? "合" : "NG", what, detail.c_str());
			if (!ok)
				bad++;
		};

		// 引き金つきで録る。300ms は無音なので待ち、正弦が来てから録り始める
		k.mu.rec_start(sp::source::ad1, 4000, 10 * RATE);
		k.pump(300);
		const bool waited = k.mu.rec_state() == 1 && k.mu.rec_frames() == 0;
		k.sine_amp = 12000;
		k.pump(1000);
		k.sine_amp = 0;
		std::vector<s16> pcm = k.mu.rec_take();
		check(waited && pcm.size() > RATE * 9 / 10 && pcm.size() < RATE * 11 / 10 && std::abs(pcm[0]) >= 4000,
		      "直の録音: 引き金を待って録る", std::to_string(pcm.size()) + " サンプル、頭 " + std::to_string(pcm[0]));
		std::string err;
		const int n = k.mu.sampling_add(pcm.data(), pcm.size(), "", err);
		const auto list = k.mu.sampling_list();
		check(n == 1 && list.size() == 1 && list[0].name == "take001" && list[0].frames() >= pcm.size(),
		      "直の録音: firmware の表に足す", err.empty() ? (list.empty() ? std::string() : list[0].name) : err);

		// firmware の SAMPLE の画面に出て、試聴が 440Hz
		k.press(B::sampling_mode);
		k.press(B::enter);
		k.press(B::enter);
		check(k.lcd().find("SMPL001 take001") != std::string::npos, "直の録音: SAMPLE の画面に出る", k.lcd());
		k.out.clear();
		k.collect = true;
		k.mu.set_button(B::audition, true);
		k.pump(800);
		k.mu.set_button(B::audition, false);
		k.collect = false;
		check(tone(k.out, 440) > 0.005 && tone(k.out, 440) > 10 * tone(k.out, 660),
		      "直の録音: 試聴が 440Hz", std::to_string(tone(k.out, 440)));

		// 続けてパネルで録ると、空きの続き（直に足したものの後ろ）に 2 つ目ができる
		k.press(B::exit);
		k.press(B::exit);
		for (int i = 0; i < 3; i++)
			k.press(B::select_right);
		k.press(B::enter);
		check(k.lcd().find("Sp=002") != std::string::npos, "直の録音: REC は Sp=002 から", k.lcd());
		k.sine_amp = 12000;
		k.pump(200);
		k.press(B::enter);
		k.pump(600);
		k.press(B::enter);
		k.sine_amp = 0;
		k.pump(300);
		k.press(B::exit);
		k.press(B::enter);                  // Keep Sample 002?
		k.press(B::exit);
		k.press(B::exit);
		const auto list2 = k.mu.sampling_list();
		check(list2.size() == 2 && list2[1].start == list2[0].end,
		      "直の録音: パネルの録音がその後ろに続く",
		      list2.size() == 2 ? std::to_string(list2[0].end) + " / " + std::to_string(list2[1].start) : std::string());

		// 音色に割り当てて、バンク 16 の PGM001 で鳴らす
		sp::voice v;
		v.el[0].assigned = true;
		v.el[0].sample = 1;
		v.name = "Direct";
		v.el[0].level = 127;
		v.el[0].pan = 7;
		const bool set = k.mu.sampling_set_voice(0, v, err);
		sp::voice back;
		k.mu.sampling_voice(0, back);
		check(set && back.el[0].assigned && back.el[0].sample == 1 && back.name == "Direct", "直の割り当て: 読み戻せる", back.name);
		// 名前の余りは空白（0 だと LCD が CGRAM の字を出す）。名前の欄は 8 文字で、その後ろは触らない
		{
			const auto &d = k.mu.dram();
			const u32 o = sp::TAB_VOICE - 0x1000000;
			const std::string raw(reinterpret_cast<const char *>(&d[o + 2]), 10);
			check(raw == std::string("Direct  ") + std::string(2, '\0'), "直の割り当て: 名前は 8 文字・空白埋め", raw.substr(0, 8));
		}
		k.press(B::play);
		const u8 pc[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x00 };
		for (u8 b : pc)
			k.mu.midi_in(b, 0);
		k.pump(300);
		check(k.lcd().find("Direct") != std::string::npos, "直の割り当て: 音色の名前が出る", k.lcd());
		k.out.clear();
		k.collect = true;
		const u8 on[] = { 0x90, 0x3c, 0x64 };
		for (u8 b : on)
			k.mu.midi_in(b, 0);
		k.pump(600);
		k.collect = false;
		check(tone(k.out, 440) > 0.005 && tone(k.out, 440) > 10 * tone(k.out, 660),
		      "直の割り当て: ノート 60 が 440Hz", std::to_string(tone(k.out, 440)));
		const u8 off[] = { 0x80, 0x3c, 0x40 };
		for (u8 b : off)
			k.mu.midi_in(b, 0);
		k.pump(300);

		// 内蔵ウェーブ。要素の波形の欄に内蔵の波形の組（0x4000 を立てない）を書くと、サンプルでなく
		// ROM の波形が鳴る（s45e_mid さんの見つけたこと。doc/sampling-ram.md）。PGM010 に組 16（Syn Drum など）、
		// 比べに組無しを書いて、鍵 60 の大きさを見る
		{
			auto rms_of = [&](int wave) {
				sp::voice w;
				w.name = "RomWave";
				w.el[0].rom_wave = wave;
				std::string e;
				const bool ok = k.mu.sampling_set_voice(9, w, e);
				sp::voice back;
				k.mu.sampling_voice(9, back);
				const u8 sel[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x09 };
				for (u8 b : sel)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.out.clear();
				k.collect = true;
				for (u8 b : on)
					k.mu.midi_in(b, 0);
				k.pump(500);
				k.collect = false;
				for (u8 b : off)
					k.mu.midi_in(b, 0);
				k.pump(500);
				double sum = 0;
				for (double v : k.out)
					sum += v * v;
				return std::make_pair(ok && !back.el[0].assigned && back.el[0].rom_wave == wave, std::sqrt(sum / std::max<size_t>(1, k.out.size())));
			};
			const auto with = rms_of(16), without = rms_of(-1);
			check(with.first && without.first && with.second > 0.003 && without.second < 0.0001,
			      "内蔵ウェーブを割り当てると鳴る",
			      "組 16 で rms " + std::to_string(with.second) + "、組無しで " + std::to_string(without.second));

			// サンプル音色を書く SysEx（機種 0x68）。PGM010 に組 16 と音程・エンベロープを書き、その記録を
			// SysEx にして PGM021 へ送ると、firmware が同じ記録を作る（要素の [0] は firmware が付ける印なので除く）
			sp::voice w;
			w.name = "SxCopy";
			w.el[0].rom_wave = 16;
			w.el[0].coarse = 7;
			w.el[0].attack = 40;
			w.el[0].release = 20;
			w.el[0].pan = 3;
			std::string e;
			k.mu.sampling_set_voice(9, w, e);
			const u32 v9 = sp::TAB_VOICE + sp::VOICE_SIZE * 9 - 0x1000000, v20 = sp::TAB_VOICE + sp::VOICE_SIZE * 20 - 0x1000000;
			const std::vector<u8> rec9(k.mu.dram().begin() + v9, k.mu.dram().begin() + v9 + sp::VOICE_SIZE);
			const auto msgs = sp::voice_sysex(20, rec9.data());
			size_t bytes = 0;
			for (const auto &m : msgs) {
				for (u8 b : m)
					k.mu.midi_in(b, 0);
				bytes += m.size();
				k.pump(5);
			}
			k.pump(300);
			int differ = 0;
			for (u32 i = 0; i < sp::VOICE_SIZE; i++) {
				const bool elem0 = i >= 12 && i < 12 + 4 * 84 && (i - 12) % 84 == 0;
				if (!elem0 && k.mu.dram()[v20 + i] != rec9[i]) {
					if (!differ)
						std::printf("    最初の違い +%x: %02x → %02x\n", i, rec9[i], k.mu.dram()[v20 + i]);
					differ++;
				}
			}
			sp::voice back;
			k.mu.sampling_voice(20, back);
			check(differ == 0 && back.name == "SxCopy" && back.el[0].rom_wave == 16 && back.el[0].coarse == 7,
			      "サンプル音色の SysEx で写す",
			      std::to_string(msgs.size()) + " 通 " + std::to_string(bytes) + " バイト、違う " + std::to_string(differ) + " バイト");

			// 要素を重ねる。要素 1 = 組 16 を左いっぱい、要素 2 = 組 39 を右いっぱいにして、右の大きさを見る
			// （右にもリバーブで左の音が少し回るので、差は 20dB ほど）
			// （実機でも 2 要素が鳴ることを確かめた。doc/sampling-ram.md）
			auto lr_of = [&](const sp::voice &mv, int key) {
				std::string err;
				k.mu.sampling_set_voice(11, mv, err);
				const u8 sel[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x0b };
				for (u8 b : sel)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.sum_l = k.sum_r = 0;
				k.collect = true;
				const u8 non[] = { 0x90, u8(key), 0x64 }, noff[] = { 0x80, u8(key), 0x40 };
				for (u8 b : non)
					k.mu.midi_in(b, 0);
				k.pump(400);
				k.collect = false;
				for (u8 b : noff)
					k.mu.midi_in(b, 0);
				k.pump(600);
				return std::make_pair(k.sum_l, k.sum_r);
			};
			sp::voice mv;
			mv.name = "Layer";
			mv.el[0].rom_wave = 16;
			mv.el[0].pan = 0;
			mv.el[1] = mv.el[0];
			mv.el[1].rom_wave = 39;
			mv.el[1].pan = 14;
			const auto both = lr_of(mv, 60);
			sp::voice mb;
			k.mu.sampling_voice(11, mb);
			mv.el[1].on = false;
			const auto one = lr_of(mv, 60);
			mv.el[1].on = true;
			mv.el[1].key_lo = 72;
			const auto split60 = lr_of(mv, 60), split72 = lr_of(mv, 72);
			auto db = [](double a, double b) { return 10 * std::log10((a + 1e-30) / (b + 1e-30)); };
			check(mb.el[0].on && mb.el[1].on && !mb.el[2].on && mb.el[1].rom_wave == 39 &&
			      both.second > 0 && db(both.second, one.second) > 15 && db(split72.second, split60.second) > 15,
			      "要素を重ねる・鍵で分ける",
			      "右: 2 要素 " + std::to_string(db(both.second, one.second)) + " dB 上、鍵で分けて 72 は 60 より " +
			      std::to_string(db(split72.second, split60.second)) + " dB 上");
		}

		// 窓の道（bridge::post → driver::sampling_tick）と WAV の取り込み。48kHz・2ch の WAV
		// （左 660Hz、右 880Hz）を作り、AD2（右）を 44.1kHz に直して足し、PGM002 に割り当てて鳴らす
		std::vector<u8> wav;
		{
			const u32 rate = 48000, frames = rate / 2;
			auto put32 = [&](u32 v) { for (int i = 0; i < 4; i++) wav.push_back(u8(v >> (8 * i))); };
			auto put16 = [&](u32 v) { wav.push_back(u8(v)); wav.push_back(u8(v >> 8)); };
			wav.insert(wav.end(), { 'R', 'I', 'F', 'F' });
			put32(36 + frames * 4);
			wav.insert(wav.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
			put32(16); put16(1); put16(2); put32(rate); put32(rate * 4); put16(4); put16(16);
			wav.insert(wav.end(), { 'd', 'a', 't', 'a' });
			put32(frames * 4);
			for (u32 i = 0; i < frames; i++) {
				put16(u16(s16(std::lround(12000 * std::sin(2 * PI * 660.0 * i / rate)))));
				put16(u16(s16(std::lround(12000 * std::sin(2 * PI * 880.0 * i / rate)))));
			}
		}
		smu2000::wav_data w;
		const bool parsed = smu2000::parse_wav(wav, w, err);
		const std::vector<s16> right = smu2000::wav_for_sampling(w, sp::source::ad2, RATE * 10);
		std::vector<double> rd(right.begin(), right.end());
		check(parsed && right.size() > RATE / 2 - 10 && right.size() <= RATE / 2 &&
		      tone(rd, 880) > 10 * tone(rd, 660),
		      "WAV: 48kHz の右を 44.1kHz に", std::to_string(right.size()) + " サンプル");
		ui::bridge br;
		ui::driver drv;
		auto pcm2 = std::make_shared<std::vector<s16>>(right);
		br.post([pcm2](mu2000 &mu) {
			std::string e;
			const int num = mu.sampling_add(pcm2->data(), pcm2->size(), "wav880", e);
			return num ? "added " + std::to_string(num) : e;
		});
		sp::voice v2;
		v2.el[0].assigned = true;
		v2.el[0].sample = 3;
		v2.name = "Wav880";
		br.post([v2](mu2000 &mu) {
			std::string e;
			return mu.sampling_set_voice(1, v2, e) ? std::string("voice") : e;
		});
		drv.pump_midi(k.mu, br);
		ui::bridge::sampling_view view;
		br.get_sampling(view);
		check(view.samples.size() == 3 && view.samples[2].name == "wav880" && view.voices[1].name == "Wav880" &&
		      view.message == "voice",
		      "窓の道: 仕事を渡して表を読む", view.message);
		// 写しの入れ物は bridge と driver で入れ替えて使う。何度受け取っても、結果の一言は最後の仕事のまま
		// （入れ物に置いていたときは、新しい一言と古い一言が交互に届いた）
		bool steady = true;
		for (int i = 0; i < 40; i++) {
			drv.pump_midi(k.mu, br);
			br.get_sampling(view);
			steady = steady && view.message == "voice";
		}
		check(steady, "窓の道: 結果の一言が行き来しない", view.message);

		// 音量を変える（サンプル 1 は振幅 12000 の正弦）。2 倍で 24000 前後、もう 2 倍で 16bit の上限で止まる。
		// 見取り図（request_overview）も変えた後の波形になる
		const int before = k.mu.sampling_peak(k.mu.sampling_list()[0]);
		const int doubled = k.mu.sampling_gain(1, 2.0);
		const int clipped = k.mu.sampling_gain(1, 2.0);
		br.request_overview(1);
		drv.pump_midi(k.mu, br);
		br.get_sampling(view);
		s16 wmax = 0;
		for (s16 v : view.wave_hi)
			wmax = std::max(wmax, v);
		// 負の側は -32768 で止まるので、最大の絶対値は 32768 になりうる
		check(std::abs(doubled - 2 * before) <= 2 && clipped >= 32767 && view.wave_number == 1 &&
		      int(view.wave_hi.size()) == ui::bridge::WAVE_BUCKETS && wmax == 32767,
		      "音量を変える・見取り図",
		      std::to_string(before) + " → " + std::to_string(doubled) + " → " + std::to_string(clipped) +
		      "、見取り図の最大 " + std::to_string(wmax));

		// 拡大した部分（request_detail）。狭い範囲なら 1 サンプルに 1 つで、波形そのもの
		{
			br.request_detail(2, 1, 3000, 3100);
			drv.pump_midi(k.mu, br);
			br.get_sampling(view);
			const auto &d = view.details[2];
			const sp::sample s1 = k.mu.sampling_list()[0];
			bool same = d.number == 1 && d.from == 3000 && d.to == 3100 && d.lo.size() == 100;
			for (size_t i = 0; same && i < d.lo.size(); i++) {
				const size_t o = (size_t(s1.start) * 2 + 3000 + i) * 2;
				same = d.lo[i] == d.hi[i] && d.lo[i] == s16(k.mu.sample_ram()[o] | k.mu.sample_ram()[o + 1] << 8);
			}
			check(same, "窓の道: 拡大した部分の波形", std::to_string(d.lo.size()) + " 点");
			br.request_detail(2, 0, 0, 0);
		}

		// トリム。サンプル 1 の [1000, 1000 + 0.25 秒) だけを残す（鳴り終わりの後ろに余白 4 サンプルが付く）。後ろのサンプル 2・3 は前へ詰まり、空きが増える。
		// サンプル 3 を使う PGM002 は、この後の確認で 880Hz のまま鳴る
		{
			const auto l0 = k.mu.sampling_list();
			const u32 free0 = k.mu.sampling_free_frames();
			const u32 keep = RATE / 4;
			const bool ok = k.mu.sampling_trim(1, 1000, 1000 + keep, err);
			const auto l1 = k.mu.sampling_list();
			const u32 freed = l0[0].end - l1[0].end;
			check(ok && l1.size() == 3 && l1[0].frames() == (keep + sp::TAIL_PAD + 1) / 2 * 2 && l1[0].play_to == keep && l1[1].start == l1[0].end &&
			      l1[2].start == l1[1].end && l1[2].frames() == l0[2].frames() &&
			      k.mu.sampling_free_frames() == free0 + freed * 2,
			      "トリム: 残して後ろを詰める",
			      std::to_string(l0[0].frames()) + " → " + std::to_string(l1[0].frames()) + " フレーム、空き +" +
			      std::to_string(k.mu.sampling_free_frames() - free0));
		}
		// 前後の無音を除いた範囲と、範囲を決めた見取り図（拡大）。無音 2000・正弦 4410・無音 3000 のサンプルを足す
		{
			std::vector<s16> pad(2000 + 4410 + 3000, 0);
			for (int i = 0; i < 4410; i++)
				pad[size_t(2000 + i)] = s16(std::lround(10000 * std::sin(2 * PI * 440.0 * i / RATE + 0.3)));
			const int n4 = k.mu.sampling_add(pad.data(), pad.size(), "padded", err);
			u32 a = 0, b = 0;
			const bool found = n4 > 0 && k.mu.sampling_bounds(n4, 0.01, a, b);
			std::vector<s16> lo, hi;
			u32 fr = 0;
			k.mu.sampling_overview(n4, ui::bridge::WAVE_BUCKETS, lo, hi, fr, 2000, 2100);
			bool exact = lo.size() == 100;
			for (size_t i = 0; exact && i < lo.size(); i++)
				exact = lo[i] == hi[i] && lo[i] == pad[2000 + i];
			check(found && a >= 2000 && a < 2010 && b > 6400 && b <= 6410 && exact,
			      "無音を除いた範囲・拡大した見取り図",
			      std::to_string(a) + " - " + std::to_string(b) + "、見取り図 " + std::to_string(lo.size()) + " 点");
		}
		const u8 pc2[] = { 0xc0, 0x01 };
		for (u8 b : pc2)
			k.mu.midi_in(b, 0);
		k.pump(300);
		k.out.clear();
		k.collect = true;
		for (u8 b : on)
			k.mu.midi_in(b, 0);
		k.pump(400);
		k.collect = false;
		for (u8 b : off)
			k.mu.midi_in(b, 0);
		check(tone(k.out, 880) > 0.005 && tone(k.out, 880) > 10 * tone(k.out, 660),
		      "窓の道: PGM002 が 880Hz", std::to_string(tone(k.out, 880)));
		k.pump(300);

		// 音程。PGM002（880Hz のサンプル）を半音 -12 にすると 440Hz、微調 +31 で 448Hz ほど
		{
			sp::voice v3;
			k.mu.sampling_voice(1, v3);
			v3.el[0].coarse = -12;
			v3.el[0].fine = 31;
			k.mu.sampling_set_voice(1, v3, err);
			sp::voice back3;
			k.mu.sampling_voice(1, back3);
			for (u8 b : pc2)
				k.mu.midi_in(b, 0);
			k.pump(300);
			k.out.clear();
			k.collect = true;
			for (u8 b : on)
				k.mu.midi_in(b, 0);
			k.pump(600);
			k.collect = false;
			for (u8 b : off)
				k.mu.midi_in(b, 0);
			k.pump(300);
			double best_f = 0, best = 0;
			for (double f = 400; f <= 500; f += 0.1)
				if (const double t = tone(k.out, f); t > best) {
					best = t;
					best_f = f;
				}
			check(back3.el[0].coarse == -12 && back3.el[0].fine == 31 && best_f > 446.0 && best_f < 450.0,
			      "音程: 半音 -12・微調 +31", std::to_string(best_f) + " Hz");
		}

		// 試聴。サンプル 3（880Hz）の頭 0.1 秒を、音源を通さずにそのまま鳴らす。終わったら止まる
		{
			k.out.clear();
			k.collect = true;
			const bool started = k.mu.preview_start(3, 0, RATE / 10);
			k.pump(80);
			const bool during = k.mu.preview_number() == 3;
			k.pump(100);
			k.collect = false;
			check(started && during && k.mu.preview_number() == 0 && tone(k.out, 880) > 0.05,
			      "試聴: 選んだ範囲を鳴らして止まる", std::to_string(tone(k.out, 880)));
		}

		// ループ。0.2 秒の 440Hz と 0.2 秒の 660Hz をつないだサンプルを PGM003 に。ループなしなら 0.4 秒で消え、
		// ループの頭を 660Hz の頭にすると、押しているあいだ 660Hz だけが続く。トリムしてもループの頭は同じ音の所
		{
			std::vector<s16> two(RATE * 2 / 5);
			for (size_t i = 0; i < two.size(); i++)
				two[i] = s16(std::lround(12000 * std::sin(2 * PI * (i < RATE / 5 ? 440.0 : 660.0) * double(i) / RATE)));
			const int n5 = k.mu.sampling_add(two.data(), two.size(), "looped", err);
			sp::voice v5;
			v5.el[0].assigned = true;
			v5.el[0].sample = n5;
			v5.name = "Looped";
			k.mu.sampling_set_voice(2, v5, err);
			const u8 pc3[] = { 0xc0, 0x02 };
			// 押して 1.2 秒。0.8 秒から後の 0.4 秒を見る
			auto hold = [&]() {
				for (u8 b : pc3)
					k.mu.midi_in(b, 0);
				k.pump(300);
				for (u8 b : on)
					k.mu.midi_in(b, 0);
				k.pump(800);
				k.out.clear();
				k.collect = true;
				k.pump(400);
				k.collect = false;
				for (u8 b : off)
					k.mu.midi_in(b, 0);
				k.pump(300);
			};
			hold();
			const double once = tone(k.out, 660);
			const u32 at = RATE / 5;
			k.mu.sampling_loop(n5, true, at);
			hold();
			const double l440 = tone(k.out, 440), l660 = tone(k.out, 660);
			sp::sample s5;
			for (const sp::sample &x : k.mu.sampling_list())
				if (x.number == n5)
					s5 = x;
			check(once < 0.002 && l660 > 0.01 && l660 > 10 * l440 && s5.loop && s5.loop_from == at,
			      "ループ: 頭から終わりをくり返す",
			      "なし 660 " + std::to_string(once) + "、あり 440 " + std::to_string(l440) + " 660 " + std::to_string(l660));
			// 頭の 1000 を切る。ループの頭は 1000 前へ
			const bool trimmed = k.mu.sampling_trim(n5, 1000, u32(two.size()), err);
			for (const sp::sample &x : k.mu.sampling_list())
				if (x.number == n5)
					s5 = x;
			hold();
			check(trimmed && s5.loop && s5.loop_from == at - 1000 && tone(k.out, 660) > 0.01 &&
			      tone(k.out, 660) > 10 * tone(k.out, 440),
			      "ループ: トリムの後もループの頭は同じ所", std::to_string(s5.loop_from));
			// 試聴もループの頭へ戻って続く
			k.mu.preview_start(n5, 0, s5.frames(), s5.loop_from);
			k.pump(600);
			const bool still = k.mu.preview_number() == n5;
			k.mu.preview_stop();
			check(still, "ループ: 試聴も止めるまで続く", std::to_string(k.mu.preview_pos()));

			// 鳴り始め・鳴り終わり（波形は切らない）。440Hz の所は [0, at - 1000)、660Hz はその後
			const u32 mid = at - 1000;
			k.mu.sampling_points(n5, 0, mid, true, 0);   // 440Hz だけをくり返す
			hold();
			const double p440 = tone(k.out, 440), p660 = tone(k.out, 660);
			k.mu.sampling_points(n5, mid, 0, false, 0);  // 660Hz から 1 度だけ
			for (u8 b : pc3)
				k.mu.midi_in(b, 0);
			k.pump(300);
			k.out.clear();
			k.collect = true;
			for (u8 b : on)
				k.mu.midi_in(b, 0);
			k.pump(150);
			k.collect = false;
			for (u8 b : off)
				k.mu.midi_in(b, 0);
			k.pump(300);
			const double q440 = tone(k.out, 440), q660 = tone(k.out, 660);
			sp::sample s6;
			for (const sp::sample &x : k.mu.sampling_list())
				if (x.number == n5)
					s6 = x;
			check(p440 > 0.01 && p440 > 10 * p660 && q660 > 0.01 && q660 > 10 * q440 && s6.play_from == mid &&
			      s6.play_to == s6.frames() - sp::TAIL_PAD && !s6.loop && s6.loop_from == mid,
			      "鳴り始め・鳴り終わり: 終点までループ、始点から鳴る",
			      "E まで 440 " + std::to_string(p440) + " 660 " + std::to_string(p660) + "、S から 440 " +
			      std::to_string(q440) + " 660 " + std::to_string(q660));
			k.mu.sampling_points(n5, 0, 0, true, at - 1000);

			// つなぎ目の道具。だんだん小さくなる 440Hz（1 周期 100.227 サンプル）で
			{
				std::vector<s16> dec(RATE / 2);
				for (size_t i = 0; i < dec.size(); i++)
					dec[i] = s16(std::lround(16000.0 * (1.0 - 0.8 * double(i) / double(dec.size())) *
					                         std::sin(2 * PI * 440.0 * double(i) / RATE)));
				const int n7 = k.mu.sampling_add(dec.data(), dec.size(), "decay", err);
				sp::sample s7;
				for (const sp::sample &x : k.mu.sampling_list())
					if (x.number == n7)
						s7 = x;
				auto raw = [&](u32 i) {
					const size_t o = (size_t(s7.start) * 2 + i) * 2;
					return int(s16(k.mu.sample_ram()[o] | k.mu.sample_ram()[o + 1] << 8));
				};
				// ゼロクロス: 1000 の近くで下から上へ横切る所（周期の 10 倍 1002.3 のあたり）。偶数に限ると偶数
				u32 z = 0, ze = 0;
				const bool zok = k.mu.sampling_snap(n7, 1000, false, 441, z) && k.mu.sampling_snap(n7, 1000, true, 441, ze);
				check(zok && raw(z - 1) < 0 && raw(z) >= 0 && z > 990 && z < 1010 && !(ze & 1) && ze + 2 >= z && ze <= z,
				      "つなぎ目: ゼロクロスに吸い付ける", std::to_string(z) + "・偶数 " + std::to_string(ze));
				// 終点をループに合わせる: L = 2000 から、E - L が周期の整数倍に近い所
				u32 e = 0;
				const bool mok = k.mu.sampling_match_end(n7, 2000, 9000, RATE / 20, e);
				const double periods = double(e - 2000) * 440.0 / RATE;
				check(mok && std::fabs(periods - std::round(periods)) < 0.02,
				      "つなぎ目: 終点をループに合わせる", std::to_string(e) + "（" + std::to_string(periods) + " 周期）");
				// クロスフェード: 終点の手前が、ループの頭の手前と同じ形になる
				std::vector<int> before_l;
				for (u32 i = 0; i < 16; i++)
					before_l.push_back(raw(6000 - 16 + i));
				const int mid_before = raw(18000 - 1000);
				const bool xok = k.mu.sampling_crossfade(n7, 6000, 18000, 2000);
				int worst = 0;
				for (u32 i = 0; i < 16; i++)
					worst = std::max(worst, std::abs(raw(18000 - 16 + i) - before_l[i]));
				check(xok && worst <= 300 && raw(18000 - 1000) != mid_before,
				      "つなぎ目: クロスフェード", "終点の手前とループの頭の手前の差 " + std::to_string(worst));
				// 等パワーの曲線も、終わりではループの頭の手前と同じ形
				std::vector<int> before_l2;
				for (u32 i = 0; i < 16; i++)
					before_l2.push_back(raw(4000 - 16 + i));
				const bool pok = k.mu.sampling_crossfade(n7, 4000, 12000, 3000, true);
				int worst2 = 0;
				for (u32 i = 0; i < 16; i++)
					worst2 = std::max(worst2, std::abs(raw(12000 - 16 + i) - before_l2[i]));
				check(pok && worst2 <= 300, "つなぎ目: 等パワーのクロスフェード", "差 " + std::to_string(worst2));
			}

			// ループ区間を探す。音程がゆっくり揺れる（±1% のビブラート 5Hz）220Hz の中で、0.3 秒以上の組。
			// 見つかった組は、つなぎ目のまわりの形の差が、適当に選んだ組（周期の整数倍の長さ）より小さい
			{
				std::vector<s16> vib(RATE * 2);
				double ph = 0;
				for (size_t i = 0; i < vib.size(); i++) {
					const double f = 220.0 * (1.0 + 0.01 * std::sin(2 * PI * 5.0 * double(i) / RATE));
					ph += 2 * PI * f / RATE;
					vib[i] = s16(std::lround(12000 * std::sin(ph)));
				}
				u32 l = 0, e = 0;
				const bool fok = sp::find_loop(vib, 0, u32(vib.size()), RATE * 3 / 10, l, e);
				auto mismatch = [&](u32 a, u32 b) {
					double d = 0, s = 0;
					for (int k2 = -256; k2 < 256; k2++) {
						const double x = vib[size_t(long(a) + k2)], y = vib[size_t(long(b) + k2)];
						d += (x - y) * (x - y);
						s += x * x + y * y;
					}
					return d / s;
				};
				// 比べる組: 頭 10000、長さは 220Hz の 80 周期（ビブラートで周期がずれる）
				const double found = fok ? mismatch(l, e) : 1.0, naive = mismatch(10000, 10000 + u32(80 * RATE / 220));
				check(fok && !(l & 1) && e - l >= RATE * 3 / 10 && found < naive * 0.2 && found < 0.01,
				      "ループ区間を探す",
				      std::to_string(l) + " - " + std::to_string(e) + "、差 " + std::to_string(found) + "（適当な組 " +
				      std::to_string(naive) + "）");
			}

			// エンベロープ（ループの入った PGM003 で）。押して 0.8 秒あとと離して 0.15 秒あとの大きさを、既定と比べる
			auto env = [&](double &held, double &after, double &first) {
				for (u8 b : pc3)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.out.clear();
				k.collect = true;
				for (u8 b : on)
					k.mu.midi_in(b, 0);
				k.pump(1000);
				for (u8 b : off)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.collect = false;
				k.pump(1500);
				auto rms = [&](u32 ms) {
					const size_t c = size_t(ms) * RATE / 1000;
					double s = 0;
					for (size_t i = c - RATE / 200; i < c + RATE / 200; i++)
						s += k.out[i] * k.out[i];
					return std::sqrt(s / double(RATE / 100));
				};
				first = rms(20);
				held = rms(800);
				after = rms(1150);
			};
			double h0, a0, f0, h1, a1, f1;
			env(h0, a0, f0);
			sp::voice e;
			k.mu.sampling_voice(2, e);
			e.el[0].attack = 24;    // ゆっくり立ち上がる（0.4 秒ほど）
			e.el[0].decay1 = 63;
			e.el[0].level1 = 96;    // すぐ -24dB ほどへ
			e.el[0].release = 16;   // 離しても長く残る
			k.mu.sampling_set_voice(2, e, err);
			sp::voice eb;
			k.mu.sampling_voice(2, eb);
			env(h1, a1, f1);
			const double db = 20 * std::log10(h1 / h0);
			check(eb.el[0].attack == 24 && eb.el[0].decay1 == 63 && eb.el[0].level1 == 96 && eb.el[0].release == 16 && f1 < 0.2 * f0 &&
			      db < -18 && db > -30 && a1 > 0.5 * h1 && a0 < 0.1 * h0,
			      "エンベロープ: アタック・レベル・リリース",
			      "頭 " + std::to_string(f1 / f0) + "、押している間 " + std::to_string(db) + " dB、離した後 " +
			      std::to_string(a1 / h1) + "（既定 " + std::to_string(a0 / h0) + "）");
		}

		// 波形を作る（wavegen.h）。ノコギリを作ってサンプルに足し、ループを入れて PGM013 に割り当てる。
		// 4214 サンプルに 25 周期なので、音程を直さなくても鍵 60 が C3（261.63Hz）、鍵 72 がその倍。
		// サインには 2 倍音が無く、ノコギリにはある
		{
			namespace wg = smu2000::wavegen;
			auto play = [&](const std::vector<s16> &pcm, const char *name, int key) {
				std::string err;
				// 窓と同じに、終わりに頭の 4 サンプルを足して登録する（音源はその手前で折り返す）
				const std::vector<s16> padded = wg::with_loop_tail(pcm);
				const int n = k.mu.sampling_add(padded.data(), padded.size(), name, err);
				k.mu.sampling_loop(n, true, 0);
				sp::voice v;
				v.name = name;
				v.el[0].assigned = true;
				v.el[0].sample = n;
				k.mu.sampling_set_voice(12, v, err);
				const u8 sel[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x0c };
				for (u8 b : sel)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.out.clear();
				k.collect = true;
				const u8 non[] = { 0x90, u8(key), 0x64 }, noff[] = { 0x80, u8(key), 0x40 };
				for (u8 b : non)
					k.mu.midi_in(b, 0);
				k.pump(700);       // ループの長さ（0.1 秒）より長く鳴らす
				k.collect = false;
				for (u8 b : noff)
					k.mu.midi_in(b, 0);
				k.pump(400);
				return n;
			};
			const std::vector<s16> saw = wg::render(wg::basic(wg::shape::saw)), sine = wg::render(wg::basic(wg::shape::sine));
			int peak = 0;
			for (s16 s : saw)
				peak = std::max(peak, std::abs(int(s)));
			const int n1 = play(saw, "saw", 60);
			const double c3 = tone(k.out, 261.63), b2 = tone(k.out, 246.94), cs3 = tone(k.out, 277.18), saw2 = tone(k.out, 523.25);
			const int n_sine = play(sine, "sine", 60);
			const double sine1 = tone(k.out, 261.63), sine2 = tone(k.out, 523.25);
			// ループが波形の長さちょうどで回っているか。上向きのゼロ交差の間隔から高さを出す（4 サンプル短いと 261.88Hz になる）。
			// 100ms ごとの大きさも、ループが合っていればそろう
			auto pitch_of = [](const std::vector<double> &x, size_t from, size_t to) {
				double first = -1, last = -1;
				int cnt = 0;
				for (size_t i = from + 1; i < to && i < x.size(); i++)
					if (x[i - 1] < 0 && x[i] >= 0) {
						const double t = double(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
						if (first < 0)
							first = t;
						last = t;
						cnt++;
					}
				return cnt > 1 ? double(RATE) * (cnt - 1) / (last - first) : 0.0;
			};
			const double sine_hz = pitch_of(k.out, RATE / 10, RATE * 65 / 100);
			sp::sample sine_s;
			for (const sp::sample &x : k.mu.sampling_list())
				if (x.number == n_sine)
					sine_s = x;
			play(saw, "saw2", 72);
			const double c4 = tone(k.out, 523.25), c3at72 = tone(k.out, 261.63);
			check(n1 > 0 && saw.size() == wg::LOOP_FRAMES && peak > 29000 && peak <= 32767 * 9 / 10 + 1 &&
			      c3 > 0.003 && c3 > 20 * b2 && c3 > 20 * cs3 && saw2 > 0.2 * c3 && sine1 > 0.003 && sine2 < 0.02 * sine1 &&
			      c4 > 20 * c3at72 && std::fabs(sine_hz - 261.6276) < 0.03 && sine_s.frames() == wg::LOOP_FRAMES + 4 &&
			      sine_s.loop && sine_s.loop_from == 0 && sine_s.play_to == wg::LOOP_FRAMES,
			      "作った波形: 鍵 60 が C3、ノコギリに倍音",
			      "ノコギリ 261.6Hz " + std::to_string(c3) + "（隣の鍵 " + std::to_string(std::max(b2, cs3)) + "）、2 倍音 " +
			      std::to_string(saw2 / c3) + " 倍、サインの 2 倍音 " + std::to_string(sine2 / sine1) + " 倍、サインの高さ " +
			      std::to_string(sine_hz) + "Hz（ループ " + std::to_string(sine_s.play_to - sine_s.loop_from) + " サンプル）");

			// ファミコンと FM の形。矩形 25% は 2 倍音が基音の 0.707 倍・4 倍音が無い、三角（階段）は奇数の倍音だけで
			// 階段の角が 31・33 倍音に出る、ノイズは長い周期が 32767 段・短い周期が 93 段でくり返す。
			// FM は深さ 0 でサイン、比 1:1 で深くすると 2 倍音が出る、比 1:2 は奇数の倍音だけ
			const wg::spectrum p25 = wg::famicom(wg::famicom_wave::pulse25), tri = wg::famicom(wg::famicom_wave::triangle);
			const std::vector<s16> nl = wg::famicom_noise(false), ns = wg::famicom_noise(true, 0.9, 4);
			bool ns_periodic = ns.size() == 372, nl_once = nl.size() == 65534;
			for (size_t i = 0; i + 8 < ns.size() && ns_periodic; i += 4)
				ns_periodic = ns[i] == ns[i + 3];                    // 1 段は 4 サンプル
			int ups = 0;
			for (s16 v : nl)
				ups += v > 0;
			const wg::spectrum fm0 = wg::fm(1, 1, 0.0), fm1 = wg::fm(1, 1, 2.5), fm2 = wg::fm(1, 2, 1.5);
			check(std::fabs(p25.mag(2) / p25.mag(1) - 0.7071) < 0.01 && p25.mag(4) < 0.01 * p25.mag(1) &&
			      tri.mag(2) < 0.001 * tri.mag(1) && tri.mag(3) > 0.08 * tri.mag(1) && tri.mag(31) > 2.0 * tri.mag(29) &&
			      ns_periodic && nl_once && std::abs(ups - 32768) < 400 &&
			      fm0.mag(2) < 0.001 * fm0.mag(1) && fm1.mag(2) > 0.3 * fm1.mag(1) && fm2.mag(2) < 0.001 * fm2.mag(1) &&
			      fm2.mag(3) > 0.2 * fm2.mag(1),
			      "作った波形: ファミコンと FM",
			      "矩形 25% の 2 倍音 " + std::to_string(p25.mag(2) / p25.mag(1)) + "、三角の 3 倍音 " + std::to_string(tri.mag(3) / tri.mag(1)) +
			      "・31/29 倍音 " + std::to_string(tri.mag(31) / tri.mag(29)) + "、FM 1:1 の 2 倍音 " + std::to_string(fm1.mag(2) / fm1.mag(1)) +
			      "、1:2 の 3 倍音 " + std::to_string(fm2.mag(3) / fm2.mag(1)));

			// 倍音にならない成分を含む音。オルガンは 16' が 8' の 1 オクターブ下（ループは 2 倍の長さ）、ユニゾンは
			// 0.76 秒のループで、どちらも終わりから頭へ段差なしにつながる（隣り合うサンプルの差と同じくらい）
			auto seam = [](const std::vector<s16> &x) {
				int step = 0;
				for (size_t i = 1; i < x.size(); i++)
					step = std::max(step, std::abs(int(x[i]) - int(x[i - 1])));
				return std::make_pair(std::abs(int(x[0]) - int(x.back())), step);
			};
			auto as_double = [](const std::vector<s16> &x) {
				std::vector<double> d(x.size());
				for (size_t i = 0; i < x.size(); i++)
					d[i] = x[i] / 32768.0;
				return d;
			};
			const int bars16[9] = { 8, 0, 8, 0, 0, 0, 0, 0, 0 };
			const std::vector<s16> org = wg::render_partials(wg::organ(bars16), wg::ORGAN_MULT);
			const std::vector<s16> uni = wg::render_partials(wg::unison(wg::basic(wg::shape::saw), 5, 1, 24), wg::UNISON_MULT);
			const auto org_d = as_double(org), uni_d = as_double(uni);
			const auto so = seam(org), su = seam(uni);
			// 声の「あ」は 800Hz の山（3 倍音）が基音より大きい。シンク 1 倍はノコギリ、フォールドは量を増やすと倍音が増える
			const wg::spectrum va = wg::vowel(0), sy = wg::sync(1.0), f1 = wg::fold(0.5), f6 = wg::fold(6.0);
			// 段数 32・4bit にすると 31・33 倍音（階段の角）が立つ。ローファイは値の種類が 2^bit までになる
			float sine256[256];
			for (int i = 0; i < 256; i++)
				sine256[i] = float(std::sin(2 * PI * (i + 0.5) / 256));
			const wg::spectrum chip = wg::from_cycle_stepped(sine256, 256, 32, 4), smooth = wg::from_cycle(sine256, 256);
			std::vector<s16> lo = wg::render(wg::basic(wg::shape::sine));
			wg::lofi(lo, 4, 4);
			std::vector<int> kinds;
			bool held = true;
			for (size_t i = 0; i < lo.size(); i++) {
				if (std::find(kinds.begin(), kinds.end(), int(lo[i])) == kinds.end())
					kinds.push_back(lo[i]);
				held = held && lo[i] == lo[i - i % 4];
			}
			// ノイズの色: ブラウンは白より低い方に寄る（隣り合うサンプルの差が小さい）
			auto rough = [](const std::vector<s16> &x) {
				double d = 0, e = 0;
				for (size_t i = 1; i < x.size(); i++) {
					d += std::fabs(double(x[i]) - x[i - 1]);
					e += std::fabs(double(x[i]));
				}
				return d / e;
			};
			const double rw = rough(wg::noise(44100, 0.9, 1, 0)), rp = rough(wg::noise(44100, 0.9, 1, 1)), rb = rough(wg::noise(44100, 0.9, 1, 2));
			check(org.size() == wg::LOOP_FRAMES * 2 && tone(org_d, 130.81) > 0.5 * tone(org_d, 261.63) && tone(org_d, 261.63) > 0.1 &&
			      tone(org_d, 196.0) < 0.02 * tone(org_d, 261.63) && so.first <= so.second &&
			      uni.size() == wg::LOOP_FRAMES * 8 && su.first <= su.second && tone(uni_d, 261.63) > 0.01 &&
			      tone(uni_d, 261.63 * 201 / 200) > 0.3 * tone(uni_d, 261.63) && tone(uni_d, 261.63 * 203 / 200) < 0.1 * tone(uni_d, 261.63) &&
			      va.mag(3) > va.mag(1) && va.mag(3) > 3 * va.mag(8) && std::fabs(sy.mag(2) / sy.mag(1) - 0.5) < 0.01 &&
			      f1.mag(3) < 0.02 * f1.mag(1) && f6.mag(5) > 0.2 * f6.mag(1) &&
			      chip.mag(31) > 20 * smooth.mag(31) && chip.mag(31) > 0.02 * chip.mag(1) &&
			      kinds.size() <= 16 && held && rb < 0.2 * rw && rp < 0.8 * rw && rp > rb,
			      "作った波形: オルガン・ユニゾン・声ほか",
			      "オルガンのつなぎ目 " + std::to_string(so.first) + "（隣どうしの最大 " + std::to_string(so.second) + "）、ユニゾン " +
			      std::to_string(su.first) + "（" + std::to_string(su.second) + "）、あ の 3 倍音 " + std::to_string(va.mag(3) / va.mag(1)) +
			      " 倍、ローファイの値 " + std::to_string(kinds.size()) + " 種類、ノイズの荒さ 白 " + std::to_string(rw) + " ピンク " +
			      std::to_string(rp) + " ブラウン " + std::to_string(rb));

			// PWM（幅のうねりをループに焼き込む）と、1 度だけ鳴る音（プラック・ドラム）。
			// PWM は 0.76 秒のループでつなぎ目に段差が無く、頭（幅 50%）は 2 倍音が小さく、1/4 の所（幅 85%）は大きい。
			// プラックは C3 で、頭より終わりが小さい。キックは低い方へ落ちる（終わりの方が頭より低い）。どれも長さは偶数
			{
				const std::vector<s16> pw = wg::pwm(0.5, 0.35, 1);
				const auto pw_d = as_double(pw);
				const auto sp_ = seam(pw);
				const std::vector<double> pw_a(pw_d.begin(), pw_d.begin() + 2000), pw_b(pw_d.begin() + long(pw.size() / 4 - 1000), pw_d.begin() + long(pw.size() / 4 + 1000));
				const std::vector<s16> pl = wg::pluck(1.5, 0.7, 0.8);
				const auto pl_d = as_double(pl);
				const std::vector<double> pl_a(pl_d.begin() + 2000, pl_d.begin() + 8000), pl_z(pl_d.end() - 12000, pl_d.end() - 6000);
				const std::vector<s16> kick = wg::drum_hit(wg::drum::kick, 0.5, 0.4, 0.5);
				const auto kk = as_double(kick);
				auto crossings = [](const std::vector<double> &x, size_t from, size_t n) {
					int c = 0;
					for (size_t i = from + 1; i < from + n && i < x.size(); i++)
						c += x[i - 1] < 0 && x[i] >= 0;
					return c;
				};
				bool sizes = true, loud = true;
				for (int d = 0; d < 6; d++) {
					const std::vector<s16> h = wg::drum_hit(wg::drum(d), 0.5, 0.4, 0.5);
					int peak = 0;
					for (s16 v : h)
						peak = std::max(peak, std::abs(int(v)));
					sizes = sizes && h.size() >= 2000 && !(h.size() & 1) && std::abs(int(h.back())) < 400;
					loud = loud && peak > 29000;
				}
				check(pw.size() == wg::LOOP_FRAMES * 8 && sp_.first <= sp_.second &&
				      tone(pw_a, 523.25) < 0.35 * tone(pw_a, 261.63) && tone(pw_b, 523.25) > 0.5 * tone(pw_b, 261.63) &&
				      !(pl.size() & 1) && tone(pl_a, 261.63) > 5 * tone(pl_a, 246.94) && tone(pl_a, 261.63) > 5 * tone(pl_a, 277.18) &&
				      tone(pl_z, 261.63) < 0.5 * tone(pl_a, 261.63) && tone(pl_z, 261.63) > 0 &&
				      crossings(kk, 0, 2205) > crossings(kk, 6615, 2205) && crossings(kk, 6615, 2205) >= 2 && sizes && loud,
				      "作った波形: PWM・プラック・ドラム",
				      "PWM の 2 倍音 頭 " + std::to_string(tone(pw_a, 523.25) / tone(pw_a, 261.63)) + "・1/4 " +
				      std::to_string(tone(pw_b, 523.25) / tone(pw_b, 261.63)) + "、プラックの終わり/頭 " +
				      std::to_string(tone(pl_z, 261.63) / tone(pl_a, 261.63)) + "、キックの波の数 頭 50ms " +
				      std::to_string(crossings(kk, 0, 2205)) + "・150ms から " + std::to_string(crossings(kk, 6615, 2205)));
			}

			// 位相ひずみ・母音のうつり変わり・FM のベル。位相ひずみは量 0 でサイン、ノコギリは 2 倍音が出て、矩形は偶数の倍音が無い。
			// 母音のループはつなぎ目が合い、頭（あ）は 3 倍音が、まん中（い）は 1 倍音が大きい。ベルは C3 が鳴って減る
			{
				const wg::spectrum pd0 = wg::phase_distortion(wg::pd_wave::saw, 0.0), pd1 = wg::phase_distortion(wg::pd_wave::saw, 0.9),
				                   pq = wg::phase_distortion(wg::pd_wave::square, 0.9), pr = wg::phase_distortion(wg::pd_wave::reso, 1.0, 6.0);
				const std::vector<s16> vm = wg::vowel_morph(0.0, 1.0);
				const auto vm_d = as_double(vm);
				const auto sv = seam(vm);
				const std::vector<double> vm_a(vm_d.begin(), vm_d.begin() + 3000), vm_m(vm_d.begin() + long(vm.size() / 2 - 1500), vm_d.begin() + long(vm.size() / 2 + 1500));
				const std::vector<s16> bell = wg::fm_bell(2.0, 3.5, 5.0, 0.5);
				const auto bl = as_double(bell);
				const std::vector<double> bl_a(bl.begin() + 500, bl.begin() + 6500), bl_z(bl.begin() + 60000, bl.begin() + 66000);
				check(pd0.mag(2) < 0.001 * pd0.mag(1) && pd1.mag(2) > 0.3 * pd1.mag(1) && pq.mag(2) < 0.01 * pq.mag(1) &&
				      pq.mag(3) > 0.2 * pq.mag(1) && pr.mag(6) > pr.mag(2) &&
				      vm.size() == wg::LOOP_FRAMES * 8 && sv.first <= sv.second &&
				      tone(vm_a, 784.9) > tone(vm_a, 261.63) && tone(vm_m, 261.63) > tone(vm_m, 784.9) &&
				      !(bell.size() & 1) && tone(bl_a, 261.63) > 0.01 && tone(bl_z, 261.63) < 0.2 * tone(bl_a, 261.63),
				      "作った波形: 位相ひずみ・母音のうねり・ベル",
				      "PD ノコギリの 2 倍音 " + std::to_string(pd1.mag(2) / pd1.mag(1)) + "、矩形の 3 倍音 " + std::to_string(pq.mag(3) / pq.mag(1)) +
				      "、母音 頭の 3 倍音/基音 " + std::to_string(tone(vm_a, 784.9) / tone(vm_a, 261.63)) + "・まん中 " +
				      std::to_string(tone(vm_m, 784.9) / tone(vm_m, 261.63)) + "、ベルの終わり/頭 " + std::to_string(tone(bl_z, 261.63) / tone(bl_a, 261.63)));
			}

			// 音色のエディット（LFO・フィルター・ピッチ EG・フィルター EG）。PGM015 にノコギリを入れて欄を変え、鍵 60 を 1.5 秒鳴らす。
			// 50ms ごとの大きさと、頭と終わりの高さで効き目を見る
			{
				auto sound = [&](const sp::voice &vv) {
					std::string err;
					k.mu.sampling_set_voice(14, vv, err);
					const u8 sel[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x0e };
					for (u8 b : sel)
						k.mu.midi_in(b, 0);
					k.pump(300);
					k.out.clear();
					k.collect = true;
					for (u8 b : { u8(0x90), u8(60), u8(100) })
						k.mu.midi_in(b, 0);
					k.pump(1500);
					k.collect = false;
					for (u8 b : { u8(0x80), u8(60), u8(64) })
						k.mu.midi_in(b, 0);
					k.pump(500);
					return k.out;
				};
				auto rms = [](const std::vector<double> &x, size_t from, size_t n) {
					double s = 0;
					for (size_t i = from; i < from + n && i < x.size(); i++)
						s += x[i] * x[i];
					return std::sqrt(s / double(n));
				};
				auto part = [](const std::vector<double> &x, size_t from, size_t n) {
					return std::vector<double>(x.begin() + long(from), x.begin() + long(std::min(from + n, x.size())));
				};
				sp::voice base;
				base.name = "Edit";
				base.el[0].assigned = true;
				base.el[0].sample = n1;
				const std::vector<double> plain = sound(base);
				// 音量の LFO: 50ms ごとの大きさが大きく上下する
				sp::voice v_lfo = base;
				v_lfo.el[0].lfo_wave = 1;
				v_lfo.el[0].lfo_speed = 20;
				v_lfo.el[0].lfo_amp = 127;
				const std::vector<double> o_lfo = sound(v_lfo);
				double lo_p = 1e9, hi_p = 0, lo_l = 1e9, hi_l = 0;
				for (size_t i = RATE / 10; i + RATE / 20 <= plain.size(); i += RATE / 20) {
					lo_p = std::min(lo_p, rms(plain, i, RATE / 20));
					hi_p = std::max(hi_p, rms(plain, i, RATE / 20));
					lo_l = std::min(lo_l, rms(o_lfo, i, RATE / 20));
					hi_l = std::max(hi_l, rms(o_lfo, i, RATE / 20));
				}
				// ピッチ EG: 始めのレベル -64・デプス 64 で、頭は 1 オクターブ下（130.8Hz）、終わりは元の高さ
				sp::voice v_peg = base;
				v_peg.el[0].peg_depth = 64;
				v_peg.el[0].peg_level[0] = -64;
				v_peg.el[0].peg_rate[0] = 30;
				const std::vector<double> o_peg = sound(v_peg);
				const auto peg_head = part(o_peg, 0, RATE / 10), peg_tail = part(o_peg, RATE * 12 / 10, RATE / 4);
				const auto pl_head = part(plain, 0, RATE / 10);
				// フィルター: カットオフを下げると小さくなる。フィルター EG の始めを -64 にすると、頭は閉じていて後で開く
				sp::voice v_cut = base;
				v_cut.el[0].cutoff = 30;
				const std::vector<double> o_cut = sound(v_cut);
				sp::voice v_feg = base;
				v_feg.el[0].cutoff = 60;
				v_feg.el[0].feg_level[0] = -64;
				v_feg.el[0].feg_rate[0] = 25;
				const std::vector<double> o_feg = sound(v_feg);
				// レゾナンスを上げると大きくなる（切る高さの近くが持ち上がる）
				sp::voice v_res = base;
				v_res.el[0].cutoff = 60;
				const double r0 = rms(sound(v_res), RATE / 2, RATE / 2);
				v_res.el[0].resonance = 60;
				const double r1 = rms(sound(v_res), RATE / 2, RATE / 2);
				// HPF を上げると低い方が削れて小さくなる。強さの曲線 2 は、同じ強さ 100 でも普通（0）より大きい
				sp::voice v_hpf = base;
				v_hpf.el[0].hpf = 127;
				const double h1 = rms(sound(v_hpf), RATE / 2, RATE / 2);
				sp::voice v_vc = base;
				v_vc.el[0].vel_curve = 2;
				const double c2 = rms(sound(v_vc), RATE / 2, RATE / 2);
				// パートのミュート（mu2000::set_part_mute。MIDI を通さず、音源の中でそのパートの声を消す。イシュー #113）。
				// 鳴らしているのはパート 1。パート 1 を消すと無音、パート 2 だけ消すなら元どおり鳴る
				k.mu.set_part_mute(1);
				const double mute1 = rms(sound(base), RATE / 2, RATE / 2);
				k.mu.set_part_mute(2);
				const double mute2 = rms(sound(base), RATE / 2, RATE / 2);
				k.mu.set_part_mute(0);
				const double mute0 = rms(sound(base), RATE / 2, RATE / 2);
				const double plain_rms = rms(plain, RATE / 2, RATE / 2);
				check(mute1 < 0.001 * plain_rms && std::fabs(mute2 / plain_rms - 1.0) < 0.05 && std::fabs(mute0 / plain_rms - 1.0) < 0.05,
				      "パートのミュート（音源の中で消す）",
				      "パート 1 を消すと " + std::to_string(mute1 / plain_rms) + " 倍、パート 2 だけ消すと " + std::to_string(mute2 / plain_rms) +
				      " 倍、外すと " + std::to_string(mute0 / plain_rms) + " 倍");

				// 読み戻し
				sp::voice wr = base, rd;
				wr.el[0].lfo_wave = 2;
				wr.el[0].lfo_phase_init = false;
				wr.el[0].lfo_speed = 40;
				wr.el[0].lfo_delay = 55;
				wr.el[0].lfo_pitch = 11;
				wr.el[0].lfo_filter = 22;
				wr.el[0].lfo_amp = 33;
				wr.el[0].cutoff = 77;
				wr.el[0].resonance = 44;
				wr.el[0].hpf = 66;
				wr.el[0].vel_curve = 3;
				wr.el[0].peg_depth = 50;
				for (int i = 0; i < 4; i++) {
					wr.el[0].peg_rate[i] = 10 + i;
					wr.el[0].feg_rate[i] = 20 + i;
				}
				for (int i = 0; i < 5; i++) {
					wr.el[0].peg_level[i] = -20 + 10 * i;
					wr.el[0].feg_level[i] = 30 - 15 * i;
				}
				std::string err;
				k.mu.sampling_set_voice(14, wr, err);
				k.mu.sampling_voice(14, rd);
				const sp::element &a = wr.el[0], &b = rd.el[0];
				bool same = a.lfo_wave == b.lfo_wave && a.lfo_phase_init == b.lfo_phase_init && a.lfo_speed == b.lfo_speed &&
				            a.lfo_delay == b.lfo_delay && a.lfo_pitch == b.lfo_pitch && a.lfo_filter == b.lfo_filter &&
				            a.lfo_amp == b.lfo_amp && a.cutoff == b.cutoff && a.resonance == b.resonance && a.peg_depth == b.peg_depth && a.hpf == b.hpf &&
				            a.vel_curve == b.vel_curve;
				for (int i = 0; i < 4; i++)
					same = same && a.peg_rate[i] == b.peg_rate[i] && a.feg_rate[i] == b.feg_rate[i];
				for (int i = 0; i < 5; i++)
					same = same && a.peg_level[i] == b.peg_level[i] && a.feg_level[i] == b.feg_level[i];
				// 既定の音色は、firmware が作ったままの値で読める（LFO 三角・位相あり・速さ 31、カットオフ 127、レゾナンス 8、EG は動かない）
				sp::voice def;
				k.mu.sampling_voice(40, def);
				const sp::element &d = def.el[0];
				const bool defaults = d.lfo_wave == 1 && d.lfo_phase_init && d.lfo_speed == 31 && d.lfo_amp == 0 && d.cutoff == 127 &&
				                      d.resonance == 8 && d.peg_depth == 1 && d.peg_rate[0] == 63 && d.peg_level[0] == 0 && d.feg_level[4] == 0;
				check(hi_p < 1.3 * lo_p && hi_l > 8 * lo_l &&
				      tone(peg_head, 130.81) > 1.5 * tone(peg_head, 261.63) && tone(pl_head, 261.63) > 3 * tone(pl_head, 130.81) &&
				      tone(peg_tail, 261.63) > 10 * tone(peg_tail, 130.81) &&
				      rms(o_cut, RATE / 2, RATE / 2) < 0.2 * rms(plain, RATE / 2, RATE / 2) &&
				      rms(o_feg, RATE / 10, RATE / 5) < 0.1 * rms(o_feg, RATE * 12 / 10, RATE / 4) && r1 > 1.5 * r0 && same && defaults &&
				      h1 < 0.5 * rms(plain, RATE / 2, RATE / 2) && c2 > 1.1 * rms(plain, RATE / 2, RATE / 2) && d.hpf == 0 && d.vel_curve == 0,
				      "音色のエディット: LFO・フィルター・EG",
				      "LFO なし 最大/最小 " + std::to_string(hi_p / lo_p) + "、音量の LFO " + std::to_string(hi_l / lo_l) +
				      "、ピッチ EG の頭 130Hz/261Hz " + std::to_string(tone(peg_head, 130.81) / tone(peg_head, 261.63)) +
				      "、カットオフ 30 で " + std::to_string(rms(o_cut, RATE / 2, RATE / 2) / rms(plain, RATE / 2, RATE / 2)) +
				      " 倍、フィルター EG の頭/終わり " + std::to_string(rms(o_feg, RATE / 10, RATE / 5) / rms(o_feg, RATE * 12 / 10, RATE / 4)) +
				      "、レゾナンス " + std::to_string(r1 / r0) + " 倍、HPF " + std::to_string(h1 / rms(plain, RATE / 2, RATE / 2)) +
				      " 倍、強さの曲線 2 で " + std::to_string(c2 / rms(plain, RATE / 2, RATE / 2)) + " 倍");
			}
		}

		// サンプリングの中身まるごとを SysEx にする（sp::memory_sysex）。新しい機械にサンプルを 2 つ（片方はループと
		// 鳴らす範囲つき）と音色を入れ、表と波形を控えてから、できた SysEx を同じ機械に送る。1 通目が全部を消し、
		// 残りが元どおりに戻す。違ってよいのは鳴らすための表の +3（firmware が録るときは ff、SysEx では 7f になる）と、
		// 音色の要素の [0]（firmware が付ける印）だけ
		{
			namespace wg = smu2000::wavegen;
			static rig m;
			if (!m.mu.load_program(dir + "/mu2000_flash.bin") || !m.mu.load_wave(dir + "/dump"))
				return 1;
			m.mu.load_sintab(dir + "/standin/sin-table.bin");
			m.mu.reset();
			for (u32 i = 0; i < 30 * RATE && !m.mu.midi_ready(); i += RATE / 100)
				m.pump(10);
			m.pump(1500);
			std::vector<s16> ramp(3001);
			for (size_t i = 0; i < ramp.size(); i++)
				ramp[i] = s16(0x9000 + i * 7);
			const std::vector<s16> saw = wg::render(wg::basic(wg::shape::saw));
			std::string err;
			m.mu.sampling_add(ramp.data(), ramp.size(), "ramp", err);
			const int n = m.mu.sampling_add(saw.data(), saw.size(), "saw", err);
			m.mu.sampling_points(1, 100, 2500, true, 1000);
			m.mu.sampling_loop(n, true, 0);
			sp::voice v;
			v.name = "SxSaw";
			v.el[0].assigned = true;
			v.el[0].sample = n;
			v.el[0].release = 30;
			m.mu.sampling_set_voice(5, v, err);
			const std::vector<u8> dram0 = m.mu.dram(), pcm0 = m.mu.sample_ram();
			const auto msgs = sp::memory_sysex(dram0, pcm0, true);
			size_t bytes = 0;
			bool wiped = false;
			for (size_t i = 0; i < msgs.size(); i++) {
				for (u8 b : msgs[i])
					m.mu.midi_in(b, 0);
				bytes += msgs[i].size();
				// DIN は 1 秒に 3125 バイト。1 通目（全部を消す）の後は待つ
				m.pump(i ? u32(msgs[i].size() * 1000 / 3125 + 3) : sp::INIT_WAIT_MS);
				if (!i)
					wiped = m.mu.sampling_list().empty();
			}
			m.pump(300);
			const u32 used = (u32(dram0[sp::NEXT_FREE - 0x1000000 + 1]) << 16 | u32(dram0[sp::NEXT_FREE - 0x1000000 + 2]) << 8 |
			                  dram0[sp::NEXT_FREE - 0x1000000 + 3]) * 4;
			int d_pcm = 0, d_rec = 0, d_play = 0, d_voice = 0;
			for (u32 i = 0; i < used; i++)
				d_pcm += m.mu.sample_ram()[i] != pcm0[i];
			for (u32 i = 0; i < 36 * 2 + 4; i++) {
				const u32 at = i < 72 ? sp::TAB_SAMPLE - 0x1000000 + i : sp::NEXT_FREE - 0x1000000 + (i - 72);
				d_rec += m.mu.dram()[at] != dram0[at];
			}
			for (u32 i = 0; i < 16 * 2; i++)
				if (i % 16 != 3)
					d_play += m.mu.dram()[sp::TAB_PLAY - 0x1000000 + i] != dram0[sp::TAB_PLAY - 0x1000000 + i];
			const u32 v5 = sp::TAB_VOICE + sp::VOICE_SIZE * 5 - 0x1000000;
			for (u32 i = 0; i < sp::VOICE_SIZE; i++)
				if (!(i >= 12 && i < 12 + 4 * 84 && (i - 12) % 84 == 0))
					d_voice += m.mu.dram()[v5 + i] != dram0[v5 + i];
			const auto list = m.mu.sampling_list();
			check(wiped && msgs.size() > 100 && d_pcm == 0 && d_rec == 0 && d_play == 0 && d_voice == 0 && list.size() == 2 &&
			      list[0].name == "ramp" && list[0].loop && list[0].play_from == 100 && list[0].play_to == 2500 &&
			      list[0].loop_from == 1000 && list[1].name == "saw" && list[1].loop,
			      "まるごとの SysEx で消して戻す",
			      std::to_string(msgs.size()) + " 通 " + std::to_string(bytes) + " バイト、違い 波形 " + std::to_string(d_pcm) +
			      " 記録 " + std::to_string(d_rec) + " 表 " + std::to_string(d_play) + " 音色 " + std::to_string(d_voice));
			// 戻した音色が鳴る（鍵 60 が C3）
			const u8 sel[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x05 };
			for (u8 b : sel)
				m.mu.midi_in(b, 0);
			m.pump(300);
			m.out.clear();
			m.collect = true;
			for (u8 b : { 0x90, 60, 0x64 })
				m.mu.midi_in(u8(b), 0);
			m.pump(700);
			m.collect = false;
			for (u8 b : { 0x80, 60, 0x40 })
				m.mu.midi_in(u8(b), 0);
			m.pump(400);
			const double c3 = tone(m.out, 261.63), b2 = tone(m.out, 246.94);
			check(c3 > 0.003 && c3 > 20 * b2, "まるごとの SysEx: 戻した音色が鳴る",
			      "261.6Hz " + std::to_string(c3) + "、隣の鍵 " + std::to_string(b2));

			// 同じ SysEx を直に読み込む（sampling_load_sysex）。「全部を消す」だけ MIDI で送って firmware に消させ、
			// 残りは波形と表を直に書く（音色の通だけ firmware が受ける）。MIDI で全部送ったときと同じ結果になる
			for (u8 b : msgs[0])
				m.mu.midi_in(b, 0);
			m.pump(sp::INIT_WAIT_MS);
			const bool wiped2 = m.mu.sampling_list().empty();
			std::vector<u8> all;
			for (const auto &x : msgs)
				all.insert(all.end(), x.begin(), x.end());
			bool wipes = false;
			std::vector<u8> rest;
			const int direct = m.mu.sampling_load_sysex(all, wipes, rest);
			for (u8 b : rest)
				m.mu.midi_in(b, 0);
			const auto list_now = m.mu.sampling_list();      // 直に書いたぶんは、時間を進めなくても入っている
			m.pump(1500);
			int e_pcm = 0, e_tab = 0, e_voice = 0;
			for (u32 i = 0; i < used; i++)
				e_pcm += m.mu.sample_ram()[i] != pcm0[i];
			for (u32 i = 0; i < 36 * 2 + 4; i++) {
				const u32 at = i < 72 ? sp::TAB_SAMPLE - 0x1000000 + i : sp::NEXT_FREE - 0x1000000 + (i - 72);
				e_tab += m.mu.dram()[at] != dram0[at];
			}
			// 鳴らすための表の +3 は、firmware が受けたときと同じ 7f になる
			for (u32 i = 0; i < 16 * 2; i++)
				if (i % 16 != 3)
					e_tab += m.mu.dram()[sp::TAB_PLAY - 0x1000000 + i] != dram0[sp::TAB_PLAY - 0x1000000 + i];
				else
					e_tab += m.mu.dram()[sp::TAB_PLAY - 0x1000000 + i] != 0x7f;
			for (u32 i = 0; i < sp::VOICE_SIZE; i++)
				if (!(i >= 12 && i < 12 + 4 * 84 && (i - 12) % 84 == 0))
					e_voice += m.mu.dram()[v5 + i] != dram0[v5 + i];
			check(wiped2 && wipes && direct == int(msgs.size()) - 1 - 334 && list_now.size() == 2 && e_pcm == 0 && e_tab == 0 &&
			      e_voice == 0,
			      "SysEx を直に読み込む",
			      "直に書いた " + std::to_string(direct) + " 通、違い 波形 " + std::to_string(e_pcm) + " 表 " + std::to_string(e_tab) +
			      " 音色 " + std::to_string(e_voice));
		}
	}

	std::printf("サンプリング: 食い違い %d\n", bad);
	return bad ? 1 : 0;
}

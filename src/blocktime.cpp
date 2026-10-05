// 1 ブロックを作るのに何 ms かかるかを測る。音声デバイスは使わない。
//
//   blocktime <rom ディレクトリ> <MIDI> <ブロックのフレーム数> [秒数] [回数] [台数]
//
// **台数**を 2 以上にすると、同じ MIDI を流す MU2000 を何台も同時に回す（doc/todo.md の
// 「PFM のインスタンスを何枚も同時に」）。ROM は 1 つを分け合うので、DAW にプラグインを
// 何枚も挿したときに近い。台数を増やして「実時間に対する割合」が 100% を超える所が上限。
//
// 待ち時間の下限は「1 ブロックの最悪値 < ブロックの長さ」で決まるので、
// ここで出る最悪値が溜めをどこまで詰められるかの答えになる。
//
// **同じ区間を何回も測って中央値を出す。** 1 回だけだと、ほかのアプリや
// 周波数の上げ下げで数 % 揺れて、小さな改善が測れない。起動の直後の状態を
// 保存しておき、毎回そこへ戻してから流すので、どの回も中身は同じ仕事になる。
#include "compat/cli_text.h"
#include "compat/platform.h"
#include "compat/realtime.h"
#include "mu2000.h"
#include "smf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

double median(std::vector<double> v)
{
	std::sort(v.begin(), v.end());
	const size_t n = v.size();
	return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

struct run_result {
	double mean, mid, p95, p99, worst;   // ms
	int over;
	size_t blocks;
	double cpu_ns, swpm_ns, megm_ns, megs_ns;   // 1 サンプルあたり
	// CPU の枠の内訳（1 サンプルあたり）。sh2_share は SH-2 を回したサンプルの割合
	double sh2_ns, ndrv_ns, nemisc_ns, sh2_share;
	double loops;
};

} // namespace

int main(int argc, char **argv)
{
	smu2000::cli::init(argc, argv);       // -jp で日本語
	if (argc < 4) {
		std::fprintf(stderr, CLI_T("blocktime <rom> <midi> <frames> [seconds] [runs] [instances]\n", "blocktime <rom> <midi> <frames> [秒] [回数] [台数]\n"));
		return 1;
	}
	const std::string dir = argv[1];
	const int block = std::atoi(argv[3]);
	const double seconds = argc > 4 ? std::atof(argv[4]) : 20.0;
	const int repeats = argc > 5 ? std::max(1, std::atoi(argv[5])) : 5;
	const int copies = argc > 6 ? std::max(1, std::atoi(argv[6])) : 1;
	const u32 RATE = 44100;

	// Ask for performance cores (macOS; nothing elsewhere). Default-QoS
	// processes may land on efficiency cores and migrate; real-time audio
	// threads run elevated, so this matches production rather than
	// flattering the numbers. The slave threads inherit it;
	// mu2000::slave_loop raises itself the same way.
	smu2000::realtime_raise_self();

	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(argv[2], events, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) { std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1; }
	if (!mu.load_wave(dir + "/dump")) { std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1; }
	mu.load_sintab(dir + "/standin/sin-table.bin");
	mu.set_threaded(!std::getenv("SMU2000_SINGLE"));
	// 軽量モード（doc/native-dsp.md）でも測れるように
	if (const char *e = std::getenv("SMU2000_NATIVE_FX"))
		mu.set_native_fx(std::atoi(e));
	smu2000::pc_prof_start();
	mu.reset();

	// 起動を待つ（ここは測らない）
	for (u32 i = 0; i < 30 * RATE && !mu.midi_ready(); i++) { s32 l = 0, r = 0; mu.run_sample(l, r); }
	const std::vector<u8> booted = mu.save_state();
	mu.set_profile(true);

	// 2 台目から。ROM は 1 つを分け合う（36MB を人数分持たない）
	std::vector<std::unique_ptr<mu2000>> more;
	for (int i = 1; i < copies; i++) {
		auto m = std::make_unique<mu2000>();
		m->set_program_rom(mu.program_rom());
		m->set_wave_rom(mu.wave_rom());
		m->set_sintab_rom(mu.sintab_rom());
		m->set_threaded(!std::getenv("SMU2000_SINGLE"));
		if (const char *e = std::getenv("SMU2000_NATIVE_FX"))
			m->set_native_fx(std::atoi(e));
		m->reset();
		if (!m->load_state(booted.data(), booted.size(), err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		if (const char *e = std::getenv("SMU2000_NATIVE_ENGINE"))
			m->set_native_engine(std::atoi(e));
		more.push_back(std::move(m));
	}
	if (copies > 1)
		std::printf(CLI_T("Running %d MU2000s at once on the same MIDI\n", "MU2000 を %d 台、同じ MIDI で同時に回す\n"), copies);

	// perf_ticks() / perf_freq() are QueryPerformanceCounter and its frequency
	// on Windows, and a monotonic nanosecond clock on macOS, so the measurement
	// means the same thing on both
	const u64 freq = smu2000::perf_freq();
	const double tick = 1000.0 / double(freq);   // ms
	const double span = 1000.0 * block / RATE;
	const u64 total = u64(seconds * RATE);
	// 曲は繰り返す
	const double loop_at = events.empty() ? 0.0 : events.back().time + 0.5;

	std::printf(CLI_T("Blocks of %d frames (%.2f ms) x %.0f s, %d runs\n", "ブロック %d フレーム（%.2f ms ぶん）× %.0f 秒 を %d 回\n"), block, span, seconds, repeats);

	// **最初の十数秒は速く出る**（この機械では 13-25% 速く、約 14 秒で落ち着く）。
	// 周波数か温度の都合で、鳴らし続けたときの速さは落ち着いた後のほう。
	// だから一定の時間、測らずに回してから測る
	const double WARM_SECONDS = 20.0;
	const u64 w0 = smu2000::perf_ticks();
	std::vector<run_result> runs;
	int warm = 0;
	for (int rep = 0; rep < repeats; rep++) {
		const bool warming = double(smu2000::perf_ticks() - w0) * tick < WARM_SECONDS * 1000.0;
		if (!mu.load_state(booted.data(), booted.size(), err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
		for (auto &m : more)
			if (!m->load_state(booted.data(), booted.size(), err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
		// 状態を戻したら、native の口も入れ直す（写し取りは覚え直し）
		if (const char *e = std::getenv("SMU2000_NATIVE_ENGINE")) {
			mu.set_native_engine(std::atoi(e));
			for (auto &m : more)
				m->set_native_engine(std::atoi(e));
		}
		mu.clear_profile();

		// 音源として挿されたときと同じ状態で測る（compat/platform.h）
		const smu2000::denormals_off no_denormals;
		std::vector<double> ms;
		size_t next = 0;
		u64 done = 0;
		double base = 0.0;
		while (done < total) {
			const int n = int(std::min<u64>(u64(block), total - done));
			const u64 t0 = smu2000::perf_ticks();
			for (int i = 0; i < n; i++) {
				const double t = double(done + i) / RATE - base;
				while (next < events.size() && events[next].time <= t) {
					for (u8 b : events[next].bytes) {
						mu.midi_in(b, events[next].port ? 1 : 0);
						for (auto &m : more)
							m->midi_in(b, events[next].port ? 1 : 0);
					}
					next++;
				}
				if (loop_at > 0.0 && t >= loop_at) { next = 0; base = double(done + i) / RATE; }
				s32 l = 0, r = 0;
				mu.run_sample(l, r);
				for (auto &m : more) {
					s32 l2 = 0, r2 = 0;
					m->run_sample(l2, r2);
					l += l2; r += r2;
				}
			}
			const u64 t1 = smu2000::perf_ticks();
			ms.push_back(double(t1 - t0) * tick);
			done += n;
		}

		std::sort(ms.begin(), ms.end());
		auto pct = [&](double p) { return ms[size_t(p * (ms.size() - 1))]; };
		double sum = 0.0;
		int over = 0;
		for (double v : ms) { sum += v; if (v > span) over++; }

		run_result r{};
		r.mean = sum / ms.size();
		r.mid = pct(0.5); r.p95 = pct(0.95); r.p99 = pct(0.99); r.worst = ms.back();
		r.over = over;
		r.blocks = ms.size();
		if (mu.m_t_n) {
			const double n = double(mu.m_t_n);
			r.cpu_ns  = 1e9 * mu.m_t_cpu  / double(freq) / n;
			r.swpm_ns = 1e9 * mu.m_t_swpm / double(freq) / n;
			r.megm_ns = double(mu.swpm().m_t_meg) / n;
			r.megs_ns = double(mu.swps().m_t_meg) / n;
			r.sh2_ns    = 1e9 * mu.m_t_sh2    / double(freq) / n;
			r.ndrv_ns   = 1e9 * mu.m_t_ndrv   / double(freq) / n;
			r.nemisc_ns = 1e9 * mu.m_t_nemisc / double(freq) / n;
			r.sh2_share = double(mu.m_n_sh2) / n;
			r.loops   = double(mu.m_loops) / n;
		}
		std::printf(CLI_T("  %s  mean %.3f ms  worst %.2f  over %d  | CPU %.0f ns  SWP30 %.0f ns (MEG %.0f of it)  slave MEG %.0f ns\n", "  %s  平均 %.3f ms  最悪 %.2f  超過 %d  | CPU %.0f ns  SWP30 %.0f ns（うち MEG %.0f）  スレーブの MEG %.0f ns\n"),
		            warming ? CLI_T("warm-up ", "慣らし ") : (std::to_string(rep + 1) + CLI_T("", " 回目")).c_str(),
		            r.mean, r.worst, r.over, r.cpu_ns, r.swpm_ns, r.megm_ns, r.megs_ns);
		std::fflush(stdout);
		if (warming) {
			warm++;
			rep--;   // 数えない
			continue;
		}
		runs.push_back(r);
	}

	auto col = [&](double run_result::*m) {
		std::vector<double> v;
		for (const run_result &r : runs) v.push_back(r.*m);
		return v;
	};
	auto spread = [](const std::vector<double> &v) {
		const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
		const double m = median(v);
		return m > 0.0 ? 100.0 * (*hi - *lo) / m : 0.0;
	};

	const std::vector<double> means = col(&run_result::mean);
	const double mean = median(means);
	int over_max = 0;
	for (const run_result &r : runs) over_max = std::max(over_max, r.over);

	std::printf(CLI_T("Median (%d runs, after %d warm-up runs were dropped)\n", "中央値（%d 回。先に慣らしを %d 回捨てた）\n"), repeats, warm);
	std::printf(CLI_T("  mean %.3f ms (spread between runs %.1f%%)  median %.2f  95%% %.2f  99%% %.2f  worst %.2f ms\n", "  平均 %.3f ms（回ごとの幅 %.1f%%）  中央 %.2f  95%% %.2f  99%% %.2f  最悪 %.2f ms\n"),
	            mean, spread(means), median(col(&run_result::mid)), median(col(&run_result::p95)),
	            median(col(&run_result::p99)), median(col(&run_result::worst)));
	std::printf(CLI_T("  share of real time: mean %.1f%%  worst %.0f%%%s\n", "  実時間に対する割合: 平均 %.1f%%  最悪 %.0f%%%s\n"),
	            100.0 * mean / span, 100.0 * median(col(&run_result::worst)) / span,
	            copies > 1 ? CLI_T(" (all instances together)", "（全部の台を合わせて）") : "");
	if (copies > 1)
		std::printf(CLI_T("  per instance: mean %.1f%%  -> this machine fits %d in real time\n", "  1 台あたり: 平均 %.1f%%  → この機械で実時間に入るのは %d 台まで\n"),
		            100.0 * mean / span / copies, int(span * copies / mean));
	std::printf(CLI_T("  blocks that ran over their length: %d / %zu in the worst run\n", "  ブロックの長さを超えた回数: 多い回で %d / %zu\n"), over_max, runs[0].blocks);
	if (mu.m_t_n) {
		const std::vector<double> megm = col(&run_result::megm_ns);
		std::printf(CLI_T("  per sample: CPU %.0f ns / SWP30 master %.0f ns (MEG %.0f ns of it, spread %.1f%%)"
" / slave MEG %.0f ns (on its own thread)\n", "  1 サンプルあたり: CPU %.0f ns / SWP30 マスタ %.0f ns（うち MEG %.0f ns、幅 %.1f%%）"
		            " / スレーブの MEG %.0f ns（別糸）\n"),
		            median(col(&run_result::cpu_ns)), median(col(&run_result::swpm_ns)),
		            median(megm), spread(megm), median(col(&run_result::megs_ns)));
		std::printf(CLI_T("  run loop %.1f turns per sample\n", "  実行ループ %.1f 周 / サンプル\n"), median(col(&run_result::loops)));
		// CPU の枠の中身。測るために時計を 3 対よけいに読むので、
		// 足しても CPU の値とは一致しない（その差が時計の代金）
		const double sh2 = median(col(&run_result::sh2_ns));
		const double ndrv = median(col(&run_result::ndrv_ns));
		const double nemisc = median(col(&run_result::nemisc_ns));
		const double share = median(col(&run_result::sh2_share));
		std::printf(CLI_T("    CPU split: SH-2 %.0f ns (ran for %.1f%% of the samples)"
" / native tick %.0f ns / other upkeep %.0f ns / clock and the rest %.0f ns\n", "    CPU の内訳: SH-2 %.0f ns（回したのは %.1f%% のサンプル）"
		            " / native の tick %.0f ns / そのほかの面倒 %.0f ns / 時計と残り %.0f ns\n"),
		            sh2, 100.0 * share, ndrv, nemisc,
		            median(col(&run_result::cpu_ns)) - sh2 - ndrv - nemisc);
	}
	smu2000::pc_prof_report();
	return 0;
}

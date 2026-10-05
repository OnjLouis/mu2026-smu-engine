// license:BSD-3-Clause
//
// 起動の確認。ROM を読ませて CPU を走らせ、どこまで行くかを見る。
//
//   boot <rom ディレクトリ> [サイクル数] [--trace-swp <出力先>]
//
// rom ディレクトリには MU2000 リポジトリの roms/ をそのまま渡せる。

#include "compat/cli_text.h"
#include "mu2000.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>


int main(int argc, char **argv)
{
	smu2000::cli::init(argc, argv);       // -jp で日本語
	if (argc < 2) {
		std::fprintf(stderr,
			CLI_T("Usage: boot <rom directory> [cycles] [--trace-swp <output file>] [-v]\n", "使い方: boot <rom ディレクトリ> [サイクル数] [--trace-swp <出力先>] [-v]\n"));
		return 1;
	}

	const std::string dir = argv[1];
	u64 cycles = 28000000;               // 既定で 1 秒ぶん
	const char *trace = nullptr;
	bool with_reads = false;
	const char *pctrace = nullptr;
	const char *pchash = nullptr;
	const char *porttrace = nullptr;
	const char *updtrace = nullptr;
	u64 pcskip = 0;
	u64 pccount = 2000000;

	for (int i = 2; i < argc; i++) {
		if (!std::strcmp(argv[i], "--trace-swp") && i + 1 < argc)
			trace = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-pc") && i + 1 < argc)
			pctrace = argv[++i];
		else if (!std::strcmp(argv[i], "--hash-pc") && i + 1 < argc)
			pchash = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-upd") && i + 1 < argc)
			updtrace = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-port") && i + 1 < argc)
			porttrace = argv[++i];
		else if (!std::strcmp(argv[i], "--pc-skip") && i + 1 < argc)
			pcskip = std::strtoull(argv[++i], nullptr, 0);
		else if (!std::strcmp(argv[i], "--pc-count") && i + 1 < argc)
			pccount = std::strtoull(argv[++i], nullptr, 0);
		else if (!std::strcmp(argv[i], "--reads"))
			with_reads = true;
		else if (!std::strcmp(argv[i], "-v"))
			smu2000::g_verbose = true;
		else
			cycles = std::strtoull(argv[i], nullptr, 0);
	}

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str());
		return 1;
	}
	if (!mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str());
		return 1;
	}
	if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
		std::fprintf(stderr, CLI_T("Warning: %s\n", "警告: %s\n"), mu.error().c_str());

	std::FILE *tf = nullptr;
	if (trace) {
		tf = std::fopen(trace, "w");
		if (!tf) {
			std::fprintf(stderr, CLI_T("Cannot write: %s\n", "書けない: %s\n"), trace);
			return 1;
		}
		mu.set_swp_trace(tf, with_reads);
	}

	mu.reset();

	std::FILE *pf = nullptr, *hf = nullptr;
	if (pctrace) {
		pf = std::fopen(pctrace, "w");
		smu2000::g_pc_trace = pf;
		smu2000::g_pc_trace_left = pccount;
		smu2000::g_pc_skip = pcskip;
	}
	if (updtrace)
		smu2000::g_upd_trace = std::fopen(updtrace, "w");
	if (porttrace)
		smu2000::g_port_trace = std::fopen(porttrace, "w");
	if (pchash) {
		hf = std::fopen(pchash, "w");
		smu2000::g_pc_hash = hf;
	}
	std::printf(CLI_T("After reset  PC=%08x\n", "リセット後  PC=%08x\n"), mu.cpu().pc());

	// 少しずつ走らせて、進んでいるか見る
	const u64 step = cycles / 10 ? cycles / 10 : cycles;
	for (u64 done = 0; done < cycles; done += step) {
		mu.run_cycles(step);
		std::printf(CLI_T("%10llu cycles  PC=%08x\n", "%10llu サイクル  PC=%08x\n"),
		            (unsigned long long)mu.cpu().total_cycles(), mu.cpu().pc());
	}

	if (hf)
		std::fclose(hf);
	if (pf)
		std::fclose(pf);
	if (tf)
		std::fclose(tf);
	return 0;
}

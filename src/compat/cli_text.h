// license:BSD-3-Clause
//
// コマンドラインに出す言葉。既定は英語で、-jp（--jp でもよい）を付けるか、環境変数 SMU2000_LANG を
// ja / jp にすると日本語を出す（イシュー #119。日本語を読めない人には使いにくく、ASCII しか出せない端末では化ける）。
//
//   std::printf(CLI_T("Wrote: %s\n", "書き出した: %s\n"), path);
//
// どちらの文も同じ引数を同じ順に取ること（書式はコンパイラが両方見る）。
// 画面（ImGui）の言葉は ui/texts.h の別の仕組みで、こちらはコンソールとエラーの文だけ。
// テスト用の道具（samptest・xgtest・statetest など）は、集計が出力を読むので日本語のまま。
#ifndef S_MU2000_COMPAT_CLI_TEXT_H
#define S_MU2000_COMPAT_CLI_TEXT_H
#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace smu2000::cli {

inline std::atomic<bool> &japanese_flag()
{
	static std::atomic<bool> ja = [] {
		const char *e = std::getenv("SMU2000_LANG");
		return e && (!std::strncmp(e, "ja", 2) || !std::strncmp(e, "jp", 2));
	}();
	return ja;
}

inline bool japanese() { return japanese_flag().load(std::memory_order_relaxed); }
inline void set_japanese(bool on) { japanese_flag().store(on, std::memory_order_relaxed); }

// 引数の並びから -jp / --jp を抜く（あれば日本語にする）。main の頭で、ほかの引数を読む前に呼ぶ
inline void init(int &argc, char **argv)
{
	int w = 1;
	for (int r = 1; r < argc; r++) {
		if (!std::strcmp(argv[r], "-jp") || !std::strcmp(argv[r], "--jp")) {
			set_japanese(true);
			continue;
		}
		argv[w++] = argv[r];
	}
	argc = w;
}

} // namespace smu2000::cli

#define CLI_T(en, ja) (smu2000::cli::japanese() ? (ja) : (en))

#endif // S_MU2000_COMPAT_CLI_TEXT_H

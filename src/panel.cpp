// license:BSD-3-Clause
//
// フロントパネルを文字だけで動かしてみる道具。絵を描く前の確認用。
//
//   panel <rom ディレクトリ>                起動して LCD を出す
//   panel <rom ディレクトリ> --keys "..."   ボタンを順に押す
//   panel <rom ディレクトリ> --list         ボタンの名前を並べる
//   panel <rom ディレクトリ> --keys "..." --trace   押すたびに LCD を 1 行で出す
//                                           （品書きをたどるとき用）
//
// --keys には短い名前をカンマで並べる。例:
//   panel roms --keys "play,part+,part+,edit"
//
// LCD は実機と同じ HD44780。字の絵は roms の hd44780u_b04.bin から引く。
// firmware は 2 行 40 桁で使うが、実機の窓に出ているのは **2 行 24 桁**。
// 24 桁より先には何も書かれないことを起動画面で確かめた。

#include "compat/cli_text.h"
#include "mu2000.h"
#include "smf.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr u32 RATE = 44100;

// ボタンの短い名前。--keys で使う
struct alias { const char *key; mu2000::button b; };
const alias ALIASES[] = {
	{ "play",     mu2000::button::play },
	{ "edit",     mu2000::button::edit },
	{ "util",     mu2000::button::util },
	{ "effect",   mu2000::button::effect },
	{ "mute",     mu2000::button::mute_solo },
	{ "part+",    mu2000::button::part_plus },
	{ "part-",    mu2000::button::part_minus },
	{ "value+",   mu2000::button::value_plus },
	{ "value-",   mu2000::button::value_minus },
	{ "exit",     mu2000::button::exit },
	{ "enter",    mu2000::button::enter },
	{ "left",     mu2000::button::select_left },
	{ "right",    mu2000::button::select_right },
	{ "seq",      mu2000::button::seq },
	{ "select",   mu2000::button::select },
	{ "audition", mu2000::button::audition },
	{ "mode",     mu2000::button::sampling_mode },
	{ "piano",    mu2000::button::piano },
	{ "drum",     mu2000::button::drum },
};

// n サンプルぶん空回し
void idle(mu2000 &mu, double seconds)
{
	const size_t n = size_t(seconds * RATE);
	s32 l, r;
	for (size_t i = 0; i < n; i++)
		mu.run_sample(l, r);
}

// ボタンを押して離す。実機の指の速さに合わせて 80ms 押す
void tap(mu2000 &mu, mu2000::button b)
{
	mu.set_button(b, true);
	idle(mu, 0.08);
	mu.set_button(b, false);
	idle(mu, 0.12);
}

// 23 桁目の制御ビット（決まった形のセグメント）を 1 行にまとめて出す。
// 列 A-D は bit3-bit0、行は上の桁の 0-7 と下の桁の 0-7 をつないだ 0-15
void show_ctl(mu2000 &mu, const char *tag)
{
	const u8 *dd = mu.lcd().ddram();
	const u8 *cg = mu.lcd().cgram();
	std::printf("  %-22s ", tag);
	for (int row = 0; row < 16; row++) {
		const u8 code = dd[(row / 8) * 0x40 + 23];
		const u8 v = (code < 8) ? cg[code * 8 + (row % 8)] : 0;
		for (int col = 0; col < 4; col++)
			std::putchar(BIT(v, 3 - col) ? '#' : '.');
		std::putchar(' ');
	}
	std::printf("\n");
}

// 液晶の中身を 16 進で返す（2 行 × 24 桁）。**字形を起こすときに
// 「どのコードが実際に使われるか」を数える**のに使う（--lcd-hex）
std::string lcd_hex(mu2000 &mu)
{
	const u8 *dd = mu.lcd().ddram();
	std::string out;
	char buf[8];
	for (int line = 0; line < 2; line++)
		for (int pos = 0; pos < 24; pos++) {
			std::snprintf(buf, sizeof buf, "%02x ", dd[line * 0x40 + pos]);
			out += buf;
		}
	return out;
}

// LCD の 2 行を 1 行にして返す（窓に出ている 24 桁ぶん）。--trace 用
std::string lcd_line(mu2000 &mu)
{
	const u8 *dd = mu.lcd().ddram();
	std::string out;
	for (int line = 0; line < 2; line++) {
		for (int pos = 0; pos < 24; pos++) {
			const u8 c = dd[line * 0x40 + pos];
			out += (c >= 0x20 && c < 0x7f) ? char(c) : ' ';
		}
		if (!line)
			out += " | ";
	}
	return out;
}

void show_lcd(mu2000 &mu)
{
	hd44780_device &lcd = mu.lcd();
	const u8 *img = lcd.render();
	const int lines = lcd.lines(), cols = lcd.line_size(), rows = lcd.char_size();

	std::printf(CLI_T("LCD %d lines x %d columns (display %s)\n", "LCD %d 行 × %d 桁（表示 %s）\n"), lines, cols,
	            lcd.display_on() ? CLI_T("on", "オン") : CLI_T("off", "オフ"));

	// 1. 文字として
	const u8 *dd = lcd.ddram();
	for (int line = 0; line < lines; line++) {
		std::printf("  |");
		for (int pos = 0; pos < cols; pos++) {
			const u8 c = dd[line * 0x40 + pos];
			std::putchar((c >= 0x20 && c < 0x7f) ? char(c) : (c ? '?' : ' '));
		}
		std::printf("|\n");
	}

	// 2. 点として。字の絵が引けているかはこちらで分かる
	std::printf("\n");
	for (int line = 0; line < lines; line++) {
		for (int y = 0; y < rows; y++) {
			std::printf("  ");
			for (int pos = 0; pos < cols; pos++) {
				const u8 v = img[16 * (line * cols + pos) + y];
				for (int x = 4; x >= 0; x--)
					std::printf("%s", BIT(v, x) ? "##" : "  ");
				std::printf(" ");
			}
			std::printf("\n");
		}
		std::printf("\n");
	}

	// 生の DDRAM を 40 桁ぶん全部。窓の外に何が書かれているかを見る
	std::printf(CLI_T("\nDDRAM, 40 columns (. is a space, # is a custom character below 0x20)\n", "\nDDRAM 40 桁ぶん（. は空白、# は 0x20 未満の作り字）\n"));
	for (int line = 0; line < lines; line++) {
		std::printf("  %d |", line);
		for (int pos = 0; pos < 40; pos++) {
			const u8 c = dd[line * 0x40 + pos];
			std::putchar(c == 0x20 ? '.' : (c < 0x20 ? '#' : (c < 0x7f ? char(c) : '?')));
		}
		std::printf("|\n");
	}
	for (int line = 0; line < lines; line++) {
		std::printf(CLI_T("  %d hex:", "  %d 16進:"), line);
		for (int pos = 0; pos < 26; pos++)
			std::printf(" %02x", dd[line * 0x40 + pos]);
		std::printf("\n");
	}

	// firmware が作った字 8 個。セグメント部の絵はこれで描かれている
	std::printf(CLI_T("\nThe 8 CGRAM characters\n", "\nCGRAM の 8 文字\n"));
	const u8 *cg = lcd.cgram();
	for (int y = 0; y < 8; y++) {
		std::printf("   ");
		for (int ch = 0; ch < 8; ch++) {
			for (int x = 4; x >= 0; x--)
				std::printf("%s", BIT(cg[ch * 8 + y], x) ? "#" : ".");
			std::printf(" ");
		}
		std::printf("\n");
	}

	const u16 led = mu.leds();
	std::printf("LED:");
	for (int i = 0; i < 10; i++)
		std::printf(" %d", BIT(led, i));
	std::printf("\n");
}

} // namespace


int main(int argc, char **argv)
{
	smu2000::cli::init(argc, argv);       // -jp で日本語
	std::string dir, keys;
	bool list = false;
	int turn = 0;
	double turn_at = -1.0;
	double lcd_at = -1.0;
	int turn_at_n = 0;
	std::string wavfile;
	double hold = 0.0;
	std::string holdkey;
	std::string midfile;
	double play = 0.0;
	bool watch = false;
	bool trace = false;
	bool lcd_hex_on = false;
	std::string ramfile;           // 終わったときのワーク RAM（調べ用）
	double settle = 1.0;
	bool usb = false;
	int native = 0;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
		else if (!std::strcmp(argv[i], "--list")) list = true;
		else if (!std::strcmp(argv[i], "--turn") && i + 1 < argc) turn = std::atoi(argv[++i]);
		// **MIDI を流している最中にダイヤルを回す**（パネルで音色を替える道を見る）
		else if (!std::strcmp(argv[i], "--turn-at") && i + 2 < argc)
			{ turn_at = std::atof(argv[++i]); turn_at_n = std::atoi(argv[++i]); }
		// 書き出す wav（音を聞き比べる用）
		else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wavfile = argv[++i];
		// **その時刻の液晶の中身**を 16 進で出す（メーターの棒を突き合わせる用）
		else if (!std::strcmp(argv[i], "--lcd-at") && i + 1 < argc) lcd_at = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--hold") && i + 2 < argc) { holdkey = argv[++i]; hold = std::atof(argv[++i]); }
		else if (!std::strcmp(argv[i], "--settle") && i + 1 < argc) settle = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--mid") && i + 2 < argc) { midfile = argv[++i]; play = std::atof(argv[++i]); }
		else if (!std::strcmp(argv[i], "--watch")) watch = true;
		else if (!std::strcmp(argv[i], "--trace")) trace = true;
		// **液晶の中身を 16 進でも出す**（字形を起こすときのコードの棚卸し）
		else if (!std::strcmp(argv[i], "--lcd-hex")) { trace = true; lcd_hex_on = true; }
		// **終わったときのワーク RAM を書き出す**。ボタンを押す前と
		// 後で取って差を見ると、firmware がどこに状態を持っているかが分かる
		else if (!std::strcmp(argv[i], "--ram") && i + 1 < argc) ramfile = argv[++i];
		else if (!std::strcmp(argv[i], "--usb")) usb = true;
		// **native の口**（firmware を細く回す）でパネルを触ってみる。
		// 段の番号は set_native_engine と同じ（doc/native-engine.md）
		else if (!std::strcmp(argv[i], "--native")) native = (i + 1 < argc && argv[i + 1][0] != '-')
		                                                   ? std::atoi(argv[++i]) : 3;
		else if (dir.empty()) dir = argv[i];
	}

	if (list) {
		std::printf(CLI_T("Buttons (the name on the left is what --keys takes):\n", "ボタン（左が --keys で使う名前）:\n"));
		for (int i = 0; i < int(mu2000::button::count); i++) {
			const mu2000::button b = mu2000::button(i);
			const char *key = "";
			for (const alias &a : ALIASES)
				if (a.b == b) { key = a.key; break; }
			std::printf("  %-10s %s\n", key, mu2000::button_name(b));
		}
		return 0;
	}

	if (dir.empty()) {
		std::fprintf(stderr, CLI_T("Usage: panel <rom directory> [--keys \"play,edit\"] [--trace]\n", "使い方: panel <rom ディレクトリ> [--keys \"play,edit\"] [--trace]\n"));
		return 1;
	}

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
		std::fprintf(stderr, CLI_T("Warning: %s\n", "警告: %s\n"), mu.error().c_str());
	// 字の絵。roms の下か standin の下を見る
	if (!mu.load_lcd_font(dir + "/hd44780u_b04.bin") &&
	    !mu.load_lcd_font(dir + "/standin/hd44780u_b04.bin"))
		std::fprintf(stderr, CLI_T("Warning: %s\n", "警告: %s\n"), mu.error().c_str());

	mu.set_threaded(true);
	// HOST SELECT を USB にして起動する（gui・plugin の既定と同じ）
	mu.set_usb_host(usb);
	mu.reset();

	std::printf(CLI_T("Booting...", "起動中..."));
	std::fflush(stdout);
	{
		const size_t limit = size_t(30.0 * RATE);
		size_t i = 0;
		s32 l, r;
		for (; i < limit && !mu.midi_ready(); i++)
			mu.run_sample(l, r);
		std::printf(CLI_T(" %.2f s\n", " %.2f 秒\n"), double(i) / RATE);
	}
	// 起動直後は表示が動いている途中なので、少し落ち着かせる
	idle(mu, settle);
	// **native の口は起動しきってから入れる**。起動の途中で細く回すと
	// firmware が立ち上がりきらない
	if (native) {
		mu.set_native_engine(native);
		idle(mu, 0.5);
	}

	if (trace)
		std::printf("  %-10s %s\n", CLI_T("(boot)", "(起動)"),
		            (lcd_hex_on ? lcd_hex(mu) : lcd_line(mu)).c_str());

	if (!keys.empty()) {
		size_t at = 0;
		while (at <= keys.size()) {
			const size_t comma = keys.find(',', at);
			std::string k = keys.substr(at, comma == std::string::npos
			                                    ? std::string::npos : comma - at);
			at = (comma == std::string::npos) ? keys.size() + 1 : comma + 1;
			while (!k.empty() && k.front() == ' ') k.erase(0, 1);
			while (!k.empty() && k.back() == ' ') k.pop_back();
			if (k.empty())
				continue;

			bool found = false;
			for (const alias &a : ALIASES)
				if (k == a.key) {
					tap(mu, a.b);
					idle(mu, 0.3);
					if (trace)
						std::printf("  %-10s %s\n", a.key,
					            (lcd_hex_on ? lcd_hex(mu) : lcd_line(mu)).c_str());
					else
						show_ctl(mu, mu2000::button_name(a.b));
					found = true;
					break;
				}
			if (!found)
				std::fprintf(stderr, CLI_T("Unknown button: %s\n", "知らないボタン: %s\n"), k.c_str());
		}
	}

	if (!midfile.empty()) {
		static u32 seen[80][256] = {};
		std::vector<smf::event> evs;
		std::string err;
		if (!smf::load(midfile, evs, err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
		} else {
			std::printf(CLI_T("Feeding %zu MIDI events up to %.1f s\n", "MIDI %zu 件を %.1f 秒まで流す\n"), evs.size(), play);
			size_t at = 0;
			const size_t total = size_t(play * RATE);
			s32 l, r;
			std::vector<s16> pcm;
			if (!wavfile.empty())
				pcm.reserve(total * 2);
			bool turned = false;
			for (size_t i = 0; i < total; i++) {
				const double now = double(i) / RATE;
				while (at < evs.size() && evs[at].time <= now) {
					for (u8 b : evs[at].bytes) mu.midi_in(b);
					at++;
				}
				if (lcd_at >= 0.0 && now >= lcd_at) {
					lcd_at = -1.0;
					const u8 *dd = mu.lcd().ddram();
					std::printf("LCDHEX");
					for (int line = 0; line < 2; line++)
						for (int pos = 0; pos < 24; pos++)
							std::printf(" %02x", dd[line * 0x40 + pos]);
					std::printf("\n");
				}
				if (!turned && turn_at >= 0.0 && now >= turn_at) {
					turned = true;
					mu.turn_encoder(turn_at_n);
					std::printf(CLI_T("  at %.2f s, the dial by %+d clicks\n", "  %.2f 秒でダイヤルを %+d 目盛り\n"), now, turn_at_n);
				}
				mu.run_sample(l, r);
				if (!wavfile.empty()) {
					auto cl = [](s32 v) {
						const s32 m = 32767;
						return s16(v > m ? m : (v < -m - 1 ? -m - 1 : v));
					};
					pcm.push_back(cl(l >> 8));
					pcm.push_back(cl(r >> 8));
				}
				// どのマスにどの文字コードが出たかを数える
				if (watch && (i % 441) == 0) {
					const u8 *dd = mu.lcd().ddram();
					for (int ln = 0; ln < 2; ln++)
						for (int pos = 0; pos < 40; pos++)
							seen[ln * 40 + pos][dd[ln * 0x40 + pos]]++;
				}
			}
			if (!wavfile.empty()) {
				// 素の WAV（44.1kHz・16bit・2ch）
				FILE *f = std::fopen(wavfile.c_str(), "wb");
				if (f) {
					const u32 bytes = u32(pcm.size() * 2);
					const u8 hdr[] = { 'R','I','F','F',0,0,0,0,'W','A','V','E',
					                   'f','m','t',' ',16,0,0,0,1,0,2,0,
					                   0x44,0xAC,0,0, 0x10,0xB1,2,0, 4,0,16,0,
					                   'd','a','t','a',0,0,0,0 };
					u8 h[44];
					std::memcpy(h, hdr, 44);
					const u32 riff = bytes + 36;
					h[4] = u8(riff); h[5] = u8(riff >> 8); h[6] = u8(riff >> 16); h[7] = u8(riff >> 24);
					h[40] = u8(bytes); h[41] = u8(bytes >> 8);
					h[42] = u8(bytes >> 16); h[43] = u8(bytes >> 24);
					std::fwrite(h, 1, 44, f);
					std::fwrite(pcm.data(), 2, pcm.size(), f);
					std::fclose(f);
					std::printf(CLI_T("  wrote %s\n", "  %s に書き出した\n"), wavfile.c_str());
				}
			}
		}
		if (watch) {
			std::printf(CLI_T("\nCharacter codes seen (cell, code and count)\n", "\n出た文字コード（マス、コードと回数）\n"));
			for (int c = 0; c < 80; c++) {
				int kinds = 0;
				for (int v = 0; v < 256; v++) if (seen[c][v]) kinds++;
				if (kinds <= 1) continue;
				std::printf("  %d:%02d ", c / 40, c % 40);
				for (int v = 0; v < 256; v++)
					if (seen[c][v]) std::printf(" %02x*%u", v, seen[c][v]);
				std::printf("\n");
			}
		}
	}

	if (!holdkey.empty()) {
		for (const alias &a : ALIASES)
			if (holdkey == a.key) {
				std::printf(CLI_T("\n--- holding %s for %.1f s ---\n", "\n--- %s を %.1f 秒押しっぱなし ---\n"),
				            mu2000::button_name(a.b), hold);
				mu.set_button(a.b, true);
				idle(mu, hold);
				mu.set_button(a.b, false);
				idle(mu, 0.3);
				break;
			}
	}

	if (turn) {
		std::printf(CLI_T("\n--- the dial by %+d clicks ---\n", "\n--- ダイヤルを %+d 目盛り ---\n"), turn);
		mu.turn_encoder(turn);
		// 位相を送り切るまで回す
		for (int i = 0; i < 400 && mu.encoder_busy(); i++)
			idle(mu, 0.01);
		idle(mu, 0.3);
	}

	std::printf("\n");
	show_lcd(mu);
	if (!ramfile.empty()) {
		const std::vector<u8> &r = mu.nvram();
		if (std::FILE *f = std::fopen(ramfile.c_str(), "wb")) {
			std::fwrite(r.data(), 1, r.size(), f);
			std::fclose(f);
		}
	}

	return 0;
}

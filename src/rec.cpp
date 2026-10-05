// license:BSD-3-Clause
//
// 音声入力を WAV に録る。実機の音を取り込んで、こちらの音と突き合わせるための道具。
//
//   rec --list                            入力デバイスの一覧
//   rec <番号> <出力 wav> <秒数>            録る
//   rec <番号> <出力 wav> <秒数> --send <番号> <MIDI ファイル>
//                                         録りながら MIDI を実機へ流す
//
// WASAPI の共有モードで開く。デバイスの持つ形式（たいてい 32bit 浮動小数）で
// 受け取り、先頭 2ch だけを 16bit の WAV に落とす。
// 実機（MU2000）を S/PDIF で受けている口を選べば、実機の音がそのまま録れる。

#include "compat/cli_text.h"
#include "smf.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace {

// MinGW's libuuid does not always provide PKEY_Device_FriendlyName. Keep the
// value local, as the newer WASAPI input/output implementation does.
const PROPERTYKEY kFriendlyName = {
	{ 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

std::string wide_to_utf8(const wchar_t *w)
{
	if (!w) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

// 入力デバイスを集める。番号はこの並び順
bool collect(std::vector<IMMDevice *> &devs, std::vector<std::string> &names)
{
	IMMDeviceEnumerator *en = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                            __uuidof(IMMDeviceEnumerator), (void **)&en)))
		return false;
	IMMDeviceCollection *col = nullptr;
	if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
		en->Release();
		return false;
	}
	UINT count = 0;
	col->GetCount(&count);
	for (UINT i = 0; i < count; i++) {
		IMMDevice *d = nullptr;
		if (FAILED(col->Item(i, &d))) continue;
		IPropertyStore *props = nullptr;
		std::string name = CLI_T("(no name)", "(名前なし)");
		if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
			PROPVARIANT v;
			PropVariantInit(&v);
			if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR)
				name = wide_to_utf8(v.pwszVal);
			PropVariantClear(&v);
			props->Release();
		}
		devs.push_back(d);
		names.push_back(name);
	}
	col->Release();
	en->Release();
	return true;
}

void write_wav(const std::string &path, const std::vector<short> &pcm, unsigned rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	const unsigned bytes = unsigned(pcm.size() * 2);
	auto u32w = [&](unsigned v) { unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
	                                                     (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
	                              std::fwrite(b, 1, 4, f); };
	auto u16w = [&](unsigned v) { unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
	                              std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); u32w(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); u32w(16); u16w(1); u16w(2);
	u32w(rate); u32w(rate * 4); u16w(4); u16w(16);
	std::fwrite("data", 1, 4, f); u32w(bytes);
	std::fwrite(pcm.data(), 1, bytes, f);
	std::fclose(f);
}

// 32bit 浮動小数の WAV（--float）。全振幅を 1.0 とする
void write_wav_float(const std::string &path, const std::vector<float> &pcm, unsigned rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	const unsigned bytes = unsigned(pcm.size() * 4);
	auto u32w = [&](unsigned v) { unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
	                                                     (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
	                              std::fwrite(b, 1, 4, f); };
	auto u16w = [&](unsigned v) { unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
	                              std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); u32w(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); u32w(16); u16w(3); u16w(2);
	u32w(rate); u32w(rate * 8); u16w(8); u16w(32);
	std::fwrite("data", 1, 4, f); u32w(bytes);
	std::fwrite(pcm.data(), 1, bytes, f);
	std::fclose(f);
}

// MIDI ファイルを実時間で流す。midisend.exe と同じことを別スレッドで。
// SMF のポート指定（`FF 21`）で口 A〜D へ振り分ける。実機の MU2000 は
// USB で `Yamaha MU2000-1` `-2` … と口が並ぶので、A〜D をそこへ渡す
std::atomic<bool> g_stop{false};   // 録り終わったら送るのもやめる

void send_midi(const int want[4], const std::string &path, double delay)
{
	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(path, events, err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return;
	}
	HMIDIOUT outs[4] = { nullptr, nullptr, nullptr, nullptr };
	for (int i = 0; i < 4; i++) {
		if (want[i] < 0)
			continue;
		if (midiOutOpen(&outs[i], UINT(want[i]), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
			std::fprintf(stderr, CLI_T("Cannot open MIDI output %d\n", "MIDI 出力 %d を開けない\n"), want[i]);
	}
	HMIDIOUT out = outs[0];
	if (!out) {
		for (HMIDIOUT h : outs) if (h) midiOutClose(h);
		return;
	}
	// Windows の眠りの刻みは既定で 15.6ms ある。この中で Sleep(2) と書いても
	// 15ms 寝過ごすことがあり、そのぶん MIDI が遅れて届く。firmware は 1ms・2.5ms の
	// 刻みで音を組み立てるので、十数 ms もずれると層の遅れが毎回変わってしまう
	// （2026-09-17 に、同じ MIDI で 24 音のうち 1 音が 88ms ずれているのを見つけた）。
	// 刻みを 1ms に詰めてから送る
	timeBeginPeriod(1);
	LARGE_INTEGER freq, t0;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	auto now = [&] {
		LARGE_INTEGER t; QueryPerformanceCounter(&t);
		return double(t.QuadPart - t0.QuadPart) / double(freq.QuadPart);
	};
	for (const smf::event &e : events) {
		if (g_stop.load(std::memory_order_acquire))
			break;                       // 録音が終わった。曲の続きは要らない
		const double at = e.time + delay;
		while (now() < at) {
			if (g_stop.load(std::memory_order_acquire))
				break;
			const double left = at - now();
			if (left > 0.002) Sleep(DWORD((left - 0.001) * 1000));
		}
		if (g_stop.load(std::memory_order_acquire))
			break;
		if (e.bytes.empty()) continue;
		// その口を開いていなければ、前と同じく B（それも無ければ A）へ重ねる
		const int p = std::min<int>(e.port, 3);
		out = outs[p] ? outs[p] : (p && outs[1]) ? outs[1] : outs[0];
		if (e.bytes[0] == 0xf0) {
			std::vector<char> buf(e.bytes.begin(), e.bytes.end());
			MIDIHDR h{};
			h.lpData = buf.data();
			h.dwBufferLength = DWORD(buf.size());
			if (midiOutPrepareHeader(out, &h, sizeof(h)) == MMSYSERR_NOERROR) {
				midiOutLongMsg(out, &h, sizeof(h));
				while (!(h.dwFlags & MHDR_DONE)) Sleep(1);
				midiOutUnprepareHeader(out, &h, sizeof(h));
			}
		} else if (e.bytes[0] == 0xf5 && e.bytes.size() == 2) {
			// 口の切り替え（ケーブルメッセージ）。ファイルでは F7 02 F5 nn で入っている
			midiOutShortMsg(out, DWORD(0xf5) | (DWORD(e.bytes[1]) << 8));
		} else if (e.bytes[0] < 0xf0) {
			DWORD msg = e.bytes[0];
			if (e.bytes.size() > 1) msg |= DWORD(e.bytes[1]) << 8;
			if (e.bytes.size() > 2) msg |= DWORD(e.bytes[2]) << 16;
			midiOutShortMsg(out, msg);
		}
	}
	Sleep(100);
	for (HMIDIOUT h : outs)
		if (h) { midiOutReset(h); midiOutClose(h); }
	timeEndPeriod(1);
}

// 相手が生きているかを確かめる。MIDI の機器照会（Device Inquiry）を送って
// 返事が来るかを見る。音が録れないとき、機械が黙っているのか、
// 音の線（S/PDIF）が切れているのかを分けるため
// 32 ビットでは lambda を __stdcall の関数ポインタに cast できない（x64 は
// 呼び出し規約が 1 種類なので通っていた）。CALLBACK 付きの普通の関数にする
static void CALLBACK inquiry_cb(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR)
{
	std::vector<unsigned char> *got = (std::vector<unsigned char> *)inst;
	if (msg == MIM_DATA) {
		for (int i = 0; i < 3; i++)
			got->push_back((unsigned char)((p1 >> (8 * i)) & 0xff));
	} else if (msg == MIM_LONGDATA) {
		MIDIHDR *h = (MIDIHDR *)p1;
		for (DWORD i = 0; i < h->dwBytesRecorded; i++)
			got->push_back((unsigned char)h->lpData[i]);
	}
}

int inquiry(int out_port, int in_port)
{
	static std::vector<unsigned char> got;
	static std::vector<char> sysex_buf(1024);

	HMIDIOUT out = nullptr;
	if (midiOutOpen(&out, UINT(out_port), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
		std::fprintf(stderr, CLI_T("Cannot open MIDI output %d\n", "MIDI 出力 %d を開けない\n"), out_port);
		return 1;
	}
	HMIDIIN in = nullptr;
	if (midiInOpen(&in, UINT(in_port), (DWORD_PTR)inquiry_cb, (DWORD_PTR)&got,
	               CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
		std::fprintf(stderr, CLI_T("Cannot open MIDI input %d\n", "MIDI 入力 %d を開けない\n"), in_port);
		midiOutClose(out);
		return 1;
	}
	MIDIHDR hin{};
	hin.lpData = sysex_buf.data();
	hin.dwBufferLength = DWORD(sysex_buf.size());
	midiInPrepareHeader(in, &hin, sizeof(hin));
	midiInAddBuffer(in, &hin, sizeof(hin));
	midiInStart(in);

	unsigned char req[] = { 0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7 };
	MIDIHDR h{};
	h.lpData = (char *)req;
	h.dwBufferLength = sizeof(req);
	midiOutPrepareHeader(out, &h, sizeof(h));
	midiOutLongMsg(out, &h, sizeof(h));
	std::printf(CLI_T("Sent an identity request (output %d). Waiting 2 s for the reply\n", "機器照会を送った（出力 %d）。返事を 2 秒待つ\n"), out_port);
	Sleep(2000);

	midiInStop(in);
	midiInReset(in);
	midiInUnprepareHeader(in, &hin, sizeof(hin));
	midiOutUnprepareHeader(out, &h, sizeof(h));
	midiInClose(in);
	midiOutClose(out);

	if (got.empty()) {
		std::printf(CLI_T("No reply. The unit is not there, or MIDI is not reaching it\n", "返事なし。機械が居ないか、MIDI が届いていない\n"));
		return 1;
	}
	std::printf(CLI_T("Reply, %zu bytes:", "返事 %zu バイト:"), got.size());
	for (unsigned char b : got)
		std::printf(" %02X", b);
	std::printf("\n");
	return 0;
}

} // namespace


int main(int argc, char **argv)
{
	smu2000::cli::init(argc, argv);       // -jp で日本語
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
		std::fprintf(stderr, CLI_T("Cannot initialize COM\n", "COM を初期化できない\n"));
		return 1;
	}

	std::vector<IMMDevice *> devs;
	std::vector<std::string> names;
	if (!collect(devs, names)) {
		std::fprintf(stderr, CLI_T("Cannot enumerate the input devices\n", "入力デバイスを数えられない\n"));
		return 1;
	}

	if (argc >= 4 && !std::strcmp(argv[1], "--inquiry"))
		return inquiry(std::atoi(argv[2]), std::atoi(argv[3]));

	if (argc < 2 || !std::strcmp(argv[1], "--list")) {
		std::printf(CLI_T("Audio inputs:\n", "音声入力:\n"));
		for (size_t i = 0; i < names.size(); i++)
			std::printf("  %zu: %s\n", i, names[i].c_str());
		if (names.empty()) std::printf(CLI_T("  (none)\n", "  （なし）\n"));
		std::printf(CLI_T("\nUsage: rec <number> <output wav> <seconds>\n"
"        [--send <MIDI output number> <MIDI file>] [--send-b <number>]\n"
"        [--send-c <number>] [--send-d <number>]\n", "\n使い方: rec <番号> <出力 wav> <秒数>\n"
		            "        [--send <MIDI 出力番号> <MIDI ファイル>] [--send-b <番号>]\n"
		            "        [--send-c <番号>] [--send-d <番号>]\n"));
		return 0;
	}
	if (argc < 4) {
		std::fprintf(stderr, CLI_T("Usage: rec <number> <output wav> <seconds> "
"[--send <MIDI output number> <MIDI file>]\n", "使い方: rec <番号> <出力 wav> <秒数> "
		                     "[--send <MIDI 出力番号> <MIDI ファイル>]\n"));
		return 1;
	}

	const int index = std::atoi(argv[1]);
	const std::string wav = argv[2];
	const double seconds = std::atof(argv[3]);
	int midi_ports[4] = { -1, -1, -1, -1 };
	std::string midi_file;
	double midi_delay = 0.5;   // 録り始めてから流すまで
	bool want_float = false;   // --float: 32bit 浮動小数の WAV で書く（16bit に落とさない）
	for (int i = 4; i < argc; i++) {
		if (!std::strcmp(argv[i], "--float"))
			want_float = true;
		else if (!std::strcmp(argv[i], "--send") && i + 2 < argc) {
			midi_ports[0] = std::atoi(argv[++i]);
			midi_file = argv[++i];
		} else if (!std::strcmp(argv[i], "--send-b") && i + 1 < argc)
			midi_ports[1] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--send-c") && i + 1 < argc)
			midi_ports[2] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--send-d") && i + 1 < argc)
			midi_ports[3] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--delay") && i + 1 < argc)
			midi_delay = std::atof(argv[++i]);
	}
	if (index < 0 || size_t(index) >= devs.size()) {
		std::fprintf(stderr, CLI_T("There is no input with that number\n", "そんな番号の入力は無い\n"));
		return 1;
	}

	IAudioClient *client = nullptr;
	if (FAILED(devs[index]->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
	                                 (void **)&client))) {
		std::fprintf(stderr, CLI_T("Cannot open the device\n", "デバイスを開けない\n"));
		return 1;
	}
	WAVEFORMATEX *mix = nullptr;
	client->GetMixFormat(&mix);
	const unsigned rate = mix->nSamplesPerSec;
	const unsigned chans = mix->nChannels;
	const unsigned bits = mix->wBitsPerSample;
	bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
	if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT。mingw のライブラリには実体が無いので自前で
		static const GUID subtype_float =
			{ 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
		const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)mix;
		is_float = IsEqualGUID(ext->SubFormat, subtype_float) != 0;
	}
	std::printf("%s / %u Hz / %u ch / %u bit %s\n", names[index].c_str(),
	            rate, chans, bits, is_float ? CLI_T("float", "浮動小数") : CLI_T("integer", "整数"));

	HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	const REFERENCE_TIME dur = 10 * 1000 * 200;   // 200ms
	HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
	                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix, nullptr);
	if (FAILED(hr)) {
		std::fprintf(stderr, CLI_T("Initialization failed (0x%08lx)\n", "初期化に失敗 (0x%08lx)\n"), (unsigned long)hr);
		return 1;
	}
	client->SetEventHandle(ev);
	IAudioCaptureClient *cap = nullptr;
	if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void **)&cap))) {
		std::fprintf(stderr, CLI_T("Cannot create the capture client\n", "取り込み口を作れない\n"));
		return 1;
	}

	std::vector<short> pcm;
	std::vector<float> fpcm;       // --float のとき（受け取った値をそのまま）
	pcm.reserve(size_t(seconds * rate) * 2);
	const size_t want = size_t(seconds * rate);
	size_t got = 0;

	std::thread sender;
	client->Start();
	if (midi_ports[0] >= 0)
		sender = std::thread([&] { send_midi(midi_ports, midi_file, midi_delay); });

	while (got < want) {
		if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0)
			break;
		for (;;) {
			UINT32 avail = 0;
			if (FAILED(cap->GetNextPacketSize(&avail)) || !avail)
				break;
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
				break;
			const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
			for (UINT32 i = 0; i < frames; i++) {
				float l = 0.0f, r = 0.0f;
				if (!silent && data) {
					if (is_float) {
						const float *f = (const float *)(data + size_t(i) * chans * 4);
						l = f[0];
						r = chans > 1 ? f[1] : f[0];
					} else if (bits == 32) {
						const int *v = (const int *)(data + size_t(i) * chans * 4);
						l = float(v[0]) / 2147483648.0f;
						r = chans > 1 ? float(v[1]) / 2147483648.0f : l;
					} else if (bits == 16) {
						const short *v = (const short *)(data + size_t(i) * chans * 2);
						l = float(v[0]) / 32768.0f;
						r = chans > 1 ? float(v[1]) / 32768.0f : l;
					}
				}
				pcm.push_back(short(std::clamp(int(l * 32767.0f), -32768, 32767)));
				pcm.push_back(short(std::clamp(int(r * 32767.0f), -32768, 32767)));
				if (want_float) {
					fpcm.push_back(l);
					fpcm.push_back(r);
				}
			}
			got += frames;
			cap->ReleaseBuffer(frames);
			if (got >= want) break;
		}
	}
	client->Stop();
	g_stop.store(true, std::memory_order_release);
	if (sender.joinable())
		sender.join();

	if (want_float)
		write_wav_float(wav, fpcm, rate);
	else
		write_wav(wav, pcm, rate);
	std::printf(CLI_T("Wrote: %s (%.2f s)\n", "書き出した: %s（%.2f 秒）\n"), wav.c_str(), double(got) / rate);

	cap->Release();
	client->Release();
	CoTaskMemFree(mix);
	for (IMMDevice *d : devs) d->Release();
	CoUninitialize();
	return 0;
}

// license:BSD-3-Clause
//
// MIDI ファイルを Windows の MIDI 出力へ実時間で流す。
//
//   midisend --list                   MIDI 出力の一覧
//   midisend <MIDI ファイル> [--port 番号]
//
// live を端から端まで試すための道具。loopMIDI などを間に挟んで、
// これで流したものが live で鳴るかを確かめる。

#include "compat/cli_text.h"
#include "smf.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

namespace {

void list_outputs()
{
	const UINT n = midiOutGetNumDevs();
	std::printf(CLI_T("MIDI outputs:\n", "MIDI 出力:\n"));
	for (UINT i = 0; i < n; i++) {
		MIDIOUTCAPSA caps{};
		if (midiOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			std::printf("  %u: %s\n", i, caps.szPname);
	}
	if (!n)
		std::printf(CLI_T("  (none)\n", "  （なし）\n"));
}

} // namespace


int main(int argc, char **argv)
{
	smu2000::cli::init(argc, argv);       // -jp で日本語
	std::string path;
	int port = 0;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--list")) { list_outputs(); return 0; }
		else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
		else if (path.empty()) path = argv[i];
	}
	if (path.empty()) {
		std::fprintf(stderr, CLI_T("Usage: midisend <MIDI file> [--port number]\n"
"        midisend --list\n", "使い方: midisend <MIDI ファイル> [--port 番号]\n"
		                     "        midisend --list\n"));
		return 1;
	}

	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(path, events, err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	std::printf(CLI_T("%zu events, the last at %.2f s\n", "%zu イベント、最後は %.2f 秒\n"),
	            events.size(), events.empty() ? 0.0 : events.back().time);

	HMIDIOUT out = nullptr;
	if (midiOutOpen(&out, UINT(port), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
		std::fprintf(stderr, CLI_T("Cannot open MIDI output %d\n", "MIDI 出力 %d を開けない\n"), port);
		return 1;
	}
	MIDIOUTCAPSA caps{};
	midiOutGetDevCapsA(UINT(port), &caps, sizeof(caps));
	std::printf(CLI_T("Sending to: %d: %s\n", "送り先: %d: %s\n"), port, caps.szPname);

	timeBeginPeriod(1);
	const DWORD start = timeGetTime();

	std::vector<u8> sysex;
	MIDIHDR hdr{};

	for (const auto &e : events) {
		// 予定の時刻まで待つ
		for (;;) {
			const double now = (timeGetTime() - start) / 1000.0;
			if (now >= e.time)
				break;
			const double left = e.time - now;
			Sleep(left > 0.005 ? DWORD((left - 0.003) * 1000.0) : 0);
		}

		if (e.bytes.empty())
			continue;

		if (e.bytes[0] == 0xf0) {           // システムエクスクルーシブ
			sysex = e.bytes;
			hdr = {};
			hdr.lpData         = reinterpret_cast<LPSTR>(sysex.data());
			hdr.dwBufferLength = DWORD(sysex.size());
			midiOutPrepareHeader(out, &hdr, sizeof(hdr));
			midiOutLongMsg(out, &hdr, sizeof(hdr));
			while (!(hdr.dwFlags & MHDR_DONE))
				Sleep(0);
			midiOutUnprepareHeader(out, &hdr, sizeof(hdr));
		} else {
			DWORD msg = e.bytes[0];
			if (e.bytes.size() > 1) msg |= DWORD(e.bytes[1]) << 8;
			if (e.bytes.size() > 2) msg |= DWORD(e.bytes[2]) << 16;
			midiOutShortMsg(out, msg);
		}
	}

	Sleep(500);
	timeEndPeriod(1);
	midiOutReset(out);
	midiOutClose(out);
	std::printf(CLI_T("Finished sending\n", "送信終了\n"));
	return 0;
}

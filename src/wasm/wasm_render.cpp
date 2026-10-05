// license:BSD-3-Clause
//
// WebAssembly glue: offline MIDI-to-PCM render of the S-MU2000 core.
// Audio only; no GUI, no threads, no filesystem access inside the module.
// All data (ROMs, MIDI) comes in as memory buffers from the host (Node or
// browser); PCM samples come out as interleaved int16 stereo.
//

#include "mu2000.h"
#include "smf.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

constexpr uint32_t kRate = 44100;

mu2000 *g_mu = nullptr;
std::string g_err;

// Staged ROM images (copies owned here; handed to mu2000 as shared vectors).
std::vector<uint8_t> g_prog;
std::vector<uint8_t> g_wave_part[4];
bool g_have_wave[4] = {};
std::vector<uint8_t> g_sintab;
bool g_usb_host = false;

// Song state.
std::vector<smf::event> g_events;
// Integer fast-reject frame for each event: ceil(time*kRate). The render
// loop first compares the integer sample clock against this; only when it
// fires does it re-check the exact double condition (event.time <= N/kRate)
// so scheduling stays bit-identical to render.cpp while avoiding a double
// division for the ~99% of samples with nothing due.
std::vector<uint64_t> g_event_frames;
size_t g_next = 0;
int g_port = -1;                 // -1: follow SMF port; else override (F5)
uint64_t g_rendered = 0;         // samples rendered since boot finished
uint64_t g_scheduled_events = 0;

void set_err(const std::string &s) { g_err = s; }

} // namespace

extern "C" {

// ---- lifecycle -------------------------------------------------------------

EMSCRIPTEN_KEEPALIVE
int smu_sample_rate() { return (int)kRate; }

EMSCRIPTEN_KEEPALIVE
void smu_set_usb_host(int on) { g_usb_host = on != 0; }

EMSCRIPTEN_KEEPALIVE
int smu_init(int usb_host)
{
	set_err("");
	g_usb_host = usb_host != 0;
	delete g_mu;
	g_mu = nullptr;
	g_events.clear();
	g_event_frames.clear();
	g_next = 0;
	g_port = -1;
	g_rendered = 0;
	g_scheduled_events = 0;
#ifdef SMU_STANDALONE
	// No exceptions in the standalone (worklet) build: abort on OOM.
	g_mu = new mu2000();
#else
	try {
		g_mu = new mu2000();
	} catch (...) {
		set_err("mu2000 の生成に失敗");
		return -1;
	}
#endif
	g_mu->set_threaded(false);
	g_mu->set_usb_host(g_usb_host);
	return 0;
}

EMSCRIPTEN_KEEPALIVE
void smu_set_native_engine(int mode)
{
	if (g_mu)
		g_mu->set_native_engine(mode);
}

EMSCRIPTEN_KEEPALIVE
void smu_set_native_fx(int mode)
{
	if (g_mu)
		g_mu->set_native_fx(mode);
}

EMSCRIPTEN_KEEPALIVE
double smu_native_firmware_share()
{
	return g_mu ? g_mu->native_firmware_share() : 0.0;
}

EMSCRIPTEN_KEEPALIVE
void smu_shutdown()
{
	delete g_mu;
	g_mu = nullptr;
}

// ---- ROM input -------------------------------------------------------------
// kind: 0 = program (4MB), 1..4 = wave parts (8MB each), 5 = sintab (64KB).
// Returns 0 on accepted size, -1 with smu_error() set otherwise.
// Data is copied; the caller keeps ownership of (ptr,len).

EMSCRIPTEN_KEEPALIVE
int smu_set_rom(int kind, const uint8_t *ptr, size_t len)
{
	set_err("");
	if (!ptr && len) { set_err("null pointer"); return -1; }
	if (kind == 0) {
		if (len != 0x400000) { set_err("program ROM は 4MB 必要"); return -1; }
		g_prog.assign(ptr, ptr + len);
		return 0;
	}
	if (kind >= 1 && kind <= 4) {
		if (len != 0x800000) { set_err("wave ROM は 8MB × 4 必要"); return -1; }
		g_wave_part[kind - 1].assign(ptr, ptr + len);
		g_have_wave[kind - 1] = true;
		return 0;
	}
	if (kind == 5) {
		if (len != 0x10000) { set_err("sin 表は 64KB 必要"); return -1; }
		g_sintab.assign(ptr, ptr + len);
		return 0;
	}
	set_err("unknown rom kind");
	return -1;
}

// Assemble staged ROMs into the emulator and reset. Sintab is optional
// (warns like render.cpp); program + 4 wave parts are required.

EMSCRIPTEN_KEEPALIVE
int smu_reset()
{
	set_err("");
	if (!g_mu) { set_err("smu_init が呼ばれていない"); return -1; }
	if (g_prog.size() != 0x400000) { set_err("program ROM が無い"); return -1; }
	for (int i = 0; i < 4; i++)
		if (!g_have_wave[i]) { set_err("wave ROM が足りない"); return -1; }

	if (!g_mu->load_program_data(g_prog.data(), g_prog.size())) {
		set_err(g_mu->error());
		return -1;
	}

	// Wave interleave and sine-table rebuild live in mu2000
	// (load_wave_data / load_sintab_data); the file loaders use them too.
	const u8 *const parts[4] = {
		g_wave_part[0].data(), g_wave_part[1].data(),
		g_wave_part[2].data(), g_wave_part[3].data()
	};
	const size_t sizes[4] = {
		g_wave_part[0].size(), g_wave_part[1].size(),
		g_wave_part[2].size(), g_wave_part[3].size()
	};
	if (!g_mu->load_wave_data(parts, sizes)) {
		set_err(g_mu->error());
		return -1;
	}

	if (g_sintab.size() == 0x10000) {
		if (!g_mu->load_sintab_data(g_sintab.data(), g_sintab.size())) {
			set_err(g_mu->error());
			return -1;
		}
	}

	g_mu->reset();
	// Staged copies are now owned by the emulator; release them (~36 MB).
	g_prog.clear(); g_prog.shrink_to_fit();
	for (int i = 0; i < 4; i++) {
		g_wave_part[i].clear(); g_wave_part[i].shrink_to_fit();
		g_have_wave[i] = false;
	}
	g_sintab.clear(); g_sintab.shrink_to_fit();
	g_next = 0;
	g_port = -1;
	g_rendered = 0;
	g_scheduled_events = 0;
	return 0;
}

// ---- boot ------------------------------------------------------------------
// Run up to max_samples until the firmware enables MIDI reception.
// Returns samples consumed (boot length), or -1 on timeout.

EMSCRIPTEN_KEEPALIVE
int smu_boot(int max_samples)
{
	set_err("");
	if (!g_mu) { set_err("smu_init が呼ばれていない"); return -1; }
	if (max_samples <= 0)
		max_samples = (int)(30 * kRate);
	for (int i = 0; i < max_samples; i++) {
		if (g_mu->midi_ready())
			return i;
		s32 l = 0, r = 0;
		g_mu->run_sample(l, r);
	}
	set_err("起動を待ったが MIDI 受信が有効にならなかった");
	return -1;
}

EMSCRIPTEN_KEEPALIVE
int smu_midi_ready()
{
	return (g_mu && g_mu->midi_ready()) ? 1 : 0;
}

// Run n samples with no MIDI input, discarding the output (boot wait and
// blank gaps). Returns midi_ready after the run. The page chunks boot this
// way to keep a progress bar alive; the CLI uses smu_boot instead.
EMSCRIPTEN_KEEPALIVE
int smu_run_blank(int nframes)
{
	if (!g_mu || nframes <= 0)
		return -1;
	for (int f = 0; f < nframes; f++) {
		s32 l = 0, r = 0;
		g_mu->run_sample(l, r);
	}
	return smu_midi_ready();
}

// ---- song input ------------------------------------------------------------

EMSCRIPTEN_KEEPALIVE
int smu_load_midi(const uint8_t *ptr, size_t len)
{
	set_err("");
	if (!g_mu) { set_err("smu_init が呼ばれていない"); return -1; }
	g_events.clear();
	g_event_frames.clear();
	g_next = 0;
	g_port = -1;
	g_rendered = 0;
	g_scheduled_events = 0;
	std::string err;
	if (!smf::load_from_memory(ptr, len, g_events, err)) {
		set_err(err);
		return -1;
	}
	g_event_frames.reserve(g_events.size());
	for (const smf::event &e : g_events)
		g_event_frames.push_back(uint64_t(std::ceil(e.time * double(kRate) - 1e-9)));
	return (int)g_events.size();
}

EMSCRIPTEN_KEEPALIVE
double smu_song_length()
{
	return g_events.empty() ? 0.0 : g_events.back().time;
}

// Live single-message input (step 3 also uses this): feed raw bytes to a port.

EMSCRIPTEN_KEEPALIVE
int smu_midi_in(int port, const uint8_t *ptr, size_t len)
{
	if (!g_mu || !ptr)
		return -1;
	if (port < 0 || port >= mu2000::MIDI_PORTS)
		port = 0;
	for (size_t i = 0; i < len; i++)
		g_mu->midi_in(ptr[i], port);
	return 0;
}

// ---- render ----------------------------------------------------------------
// Render nframes of stereo int16 into out (out must hold nframes*2 samples).
// Returns frames written. Feeding of song events mirrors render.cpp.

EMSCRIPTEN_KEEPALIVE
int smu_render_frames(int16_t *out, int nframes)
{
	if (!g_mu || !out || nframes <= 0)
		return -1;
	const double inv_rate = 1.0 / double(kRate);
	for (int f = 0; f < nframes; f++) {
		const uint64_t now = g_rendered + uint64_t(f);
		// Fast path: integer reject avoids the double division when the
		// next event is still in the future (the common case). The double
		// comparison is the ground truth and keeps scheduling identical.
		while (g_next < g_events.size() && now >= g_event_frames[g_next]) {
			const double t = double(now) * inv_rate;
			if (g_events[g_next].time > t)
				break;
			const std::vector<u8> &ev = g_events[g_next].bytes;
			if (ev.size() == 2 && ev[0] == 0xf5) {
				g_port = std::clamp(int(ev[1]) - 1, 0, mu2000::MIDI_PORTS - 1);
			} else {
				const int to = g_port >= 0 ? g_port
				                           : smf::mu_port(g_events[g_next].port, true, g_usb_host);
				if (to >= 0) {
					for (u8 b : ev)
						g_mu->midi_in(b, to);
					g_scheduled_events++;
				}
			}
			g_next++;
		}
		s32 l = 0, r = 0;
		g_mu->run_sample(l, r);
		l = l * 32768 / mu2000::DAC_FULL_SCALE;
		r = r * 32768 / mu2000::DAC_FULL_SCALE;
		out[f * 2 + 0] = int16_t(std::clamp(l, -32768, 32767));
		out[f * 2 + 1] = int16_t(std::clamp(r, -32768, 32767));
	}
	g_rendered += uint64_t(nframes);
	return nframes;
}

EMSCRIPTEN_KEEPALIVE
int smu_song_done()
{
	if (!g_mu)
		return 1;
	return (g_next >= g_events.size() && g_mu->midi_idle()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
unsigned long long smu_scheduled_events() { return g_scheduled_events; }

EMSCRIPTEN_KEEPALIVE
unsigned long long smu_dropped() { return g_mu ? g_mu->midi_dropped() : 0; }

EMSCRIPTEN_KEEPALIVE
const char *smu_error() { return g_err.c_str(); }

// Copy last error as UTF-8 into (out, cap). Returns bytes written (no NUL).
EMSCRIPTEN_KEEPALIVE
int smu_error_copy(char *out, int cap)
{
	if (!out || cap <= 0)
		return 0;
	const size_t n = std::min(g_err.size(), size_t(cap));
	std::memcpy(out, g_err.data(), n);
	return (int)n;
}

} // extern "C"

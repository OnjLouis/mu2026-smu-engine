// license:BSD-3-Clause
// Small VST2 host used to verify the DLL ABI, MIDI path, state, and editor.

#include "vst2_abi.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace smu2000::vsti;

namespace {

using entry_fn = effect *(SMU_VSTCALLBACK *)(host_callback);

vintptr SMU_VSTCALLBACK host(effect *, vint32 opcode, vint32, vintptr, void *, float)
{
	return opcode == host_version ? 2400 : 0;
}

bool finite_audio(const std::vector<float> &v)
{
	for (float x : v)
		if (!std::isfinite(x))
			return false;
	return true;
}

bool probe_hybrid_fx(effect *fx, double sample_rate)
{
	if (fx->num_inputs != 12) {
		std::fprintf(stderr, "hybrid FX requires twelve input buses, got %d\n", fx->num_inputs);
		return false;
	}
	constexpr int block = 256;
	std::vector<float> silence(block), signal(block), left(block), right(block);
	float *inputs[12] = {};
	float *outputs[2] = { left.data(), right.data() };
	for (float *&input : inputs)
		input = silence.data();
	// Let the firmware initialise the MEG program before probing its buses.
	std::this_thread::sleep_for(std::chrono::seconds(2));
	for (int i = 0; i < 500; i++)
		fx->process_replacing(fx, inputs, outputs, block);
	for (int i = 0; i < block; i++)
		signal[i] = 0.2f * std::sin(2.0 * 3.141592653589793 * 440.0 * i / sample_rate);
	auto measure = [&](int bus) {
		inputs[bus] = signal.data();
		double early = 0.0, tail = 0.0;
		for (int i = 0; i < 8; i++) {
			fx->process_replacing(fx, inputs, outputs, block);
			for (float sample : left)
				early += double(sample) * sample;
		}
		inputs[bus] = silence.data();
		for (int i = 0; i < 100; i++) {
			fx->process_replacing(fx, inputs, outputs, block);
			for (float sample : left)
				tail += double(sample) * sample;
		}
		return std::pair<double, double>{early, tail};
	};
	const auto dry = measure(0);
	const auto reverb = measure(2);
	const auto chorus = measure(4);
	char insertion_type[] = { char(0xf0), 0x43, 0x10, 0x4c, 0x03,
	                          0x00, 0x00, 0x49, 0x00, char(0xf7) };
	char insertion_part[] = { char(0xf0), 0x43, 0x10, 0x4c, 0x03,
	                          0x00, 0x0c, 0x00, char(0xf7) };
	sysex_event ins_type_event{}, ins_part_event{};
	ins_type_event.type = ins_part_event.type = sysex_type;
	ins_type_event.byte_size = ins_part_event.byte_size = sysex_event_byte_size;
	ins_type_event.dump = insertion_type;
	ins_type_event.dump_bytes = sizeof(insertion_type);
	ins_part_event.dump = insertion_part;
	ins_part_event.dump_bytes = sizeof(insertion_part);
	struct { vint32 count; vintptr reserved; event *items[2]; } insertion_setup{};
	insertion_setup.count = 2;
	insertion_setup.items[0] = reinterpret_cast<event *>(&ins_type_event);
	insertion_setup.items[1] = reinterpret_cast<event *>(&ins_part_event);
	fx->dispatcher(fx, eff_process_events, 0, 0, &insertion_setup, 0);
	for (int i = 0; i < 100; i++)
		fx->process_replacing(fx, inputs, outputs, block);
	const auto insertion = measure(8);
	char insertion2_type[] = { char(0xf0), 0x43, 0x10, 0x4c, 0x03,
	                           0x01, 0x00, 0x49, 0x00, char(0xf7) };
	char insertion2_part[] = { char(0xf0), 0x43, 0x10, 0x4c, 0x03,
	                           0x01, 0x0c, 0x00, char(0xf7) };
	ins_type_event.dump = insertion2_type;
	ins_part_event.dump = insertion2_part;
	fx->dispatcher(fx, eff_process_events, 0, 0, &insertion_setup, 0);
	for (int i = 0; i < 100; i++)
		fx->process_replacing(fx, inputs, outputs, block);
	const auto insertion2 = measure(10);
	char insertion2_thru[] = { char(0xf0), 0x43, 0x10, 0x4c, 0x03,
	                           0x01, 0x00, 0x00, 0x00, char(0xf7) };
	ins_type_event.dump = insertion2_thru;
	struct { vint32 count; vintptr reserved; event *items[1]; } bypass_setup{};
	bypass_setup.count = 1;
	bypass_setup.items[0] = reinterpret_cast<event *>(&ins_type_event);
	fx->dispatcher(fx, eff_process_events, 0, 0, &bypass_setup, 0);
	for (int i = 0; i < 100; i++)
		fx->process_replacing(fx, inputs, outputs, block);
	const auto insertion2_through = measure(10);
	char variation_type[] = { char(0xf0), 0x43, 0x10, 0x4c, 0x02, 0x01,
	                          0x40, 0x05, 0x00, char(0xf7) };
	char variation_system[] = { char(0xf0), 0x43, 0x10, 0x4c, 0x02,
	                            0x01, 0x5a, 0x01, char(0xf7) };
	sysex_event type_event{}, system_event{};
	type_event.type = system_event.type = sysex_type;
	type_event.byte_size = system_event.byte_size = sysex_event_byte_size;
	type_event.dump = variation_type;
	type_event.dump_bytes = sizeof(variation_type);
	system_event.dump = variation_system;
	system_event.dump_bytes = sizeof(variation_system);
	struct { vint32 count; vintptr reserved; event *items[2]; } setup{};
	setup.count = 2;
	setup.items[0] = reinterpret_cast<event *>(&type_event);
	setup.items[1] = reinterpret_cast<event *>(&system_event);
	fx->dispatcher(fx, eff_process_events, 0, 0, &setup, 0);
	for (int i = 0; i < 100; i++)
		fx->process_replacing(fx, inputs, outputs, block);
	const auto variation = measure(6);
	std::printf("hybrid buses: dry %.8g / %.8g, reverb %.8g / %.8g, "
	            "chorus %.8g / %.8g, insertion %.8g / %.8g, "
	            "insertion2 %.8g / %.8g, bypass %.8g / %.8g, "
	            "variation %.8g / %.8g (early/tail)\n",
	            dry.first, dry.second, reverb.first, reverb.second,
	            chorus.first, chorus.second, insertion.first, insertion.second,
	            insertion2.first, insertion2.second,
	            insertion2_through.first, insertion2_through.second,
	            variation.first, variation.second);
	return dry.first > 1e-5 && reverb.second > 1e-4
		&& chorus.second > 1e-4 && insertion.first > 1e-3
		&& insertion2.first > 1e-3
		&& std::abs(insertion2.first - insertion2_through.first) > 1e-3
		&& variation.second > 1e-4;
}

// **冷えた起動の試験**（--coldstate。issue #51）。vstmididrv のように、起動が終わる前に
// 保存した状態（chunk）を戻し、すぐ曲頭（XG System On → 音色の指定 → ノートオン）を流す。
// 状態を戻さない台と出力が同じなら、曲頭の音色が消えていない
std::vector<float> cold_run(entry_fn entry, const std::vector<std::uint8_t> *chunk)
{
	effect *fx = entry(host);
	fx->dispatcher(fx, eff_open, 0, 0, nullptr, 0);
	fx->dispatcher(fx, eff_set_sample_rate, 0, 0, nullptr, 44100.0f);
	fx->dispatcher(fx, eff_set_block_size, 0, 512, nullptr, 0);
	if (chunk)
		fx->dispatcher(fx, eff_set_chunk, 0, vintptr(chunk->size()),
		               const_cast<std::uint8_t *>(chunk->data()), 0);
	fx->dispatcher(fx, eff_mains_changed, 0, 1, nullptr, 0);

	static char xg_on[] = { char(0xf0), 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, char(0xf7) };
	sysex_event sx{};
	sx.type = sysex_type;
	sx.byte_size = sysex_event_byte_size;
	sx.dump_bytes = vint32(sizeof(xg_on));
	sx.dump = xg_on;
	midi_event pc{}, on{};
	pc.type = on.type = midi_type;
	pc.byte_size = on.byte_size = midi_event_byte_size;
	pc.midi_data[0] = char(0xc0);
	pc.midi_data[1] = 48;                         // Strings
	on.midi_data[0] = char(0x90);
	on.midi_data[1] = 60;
	on.midi_data[2] = 100;
	struct { vint32 count; vintptr reserved; event *items[3]; } list{};
	list.count = 3;
	list.items[0] = reinterpret_cast<event *>(&sx);
	list.items[1] = reinterpret_cast<event *>(&pc);
	list.items[2] = reinterpret_cast<event *>(&on);
	fx->dispatcher(fx, eff_process_events, 0, 0, &list, 0);

	std::vector<float> out;
	std::vector<float> l(512), r(512);
	float *outputs[2] = { l.data(), r.data() };
	for (int blk = 0; blk < 44100 * 3 / 512; blk++) {
		fx->process_replacing(fx, nullptr, outputs, 512);
		out.insert(out.end(), l.begin(), l.end());
	}
	fx->dispatcher(fx, eff_mains_changed, 0, 0, nullptr, 0);
	fx->dispatcher(fx, eff_close, 0, 0, nullptr, 0);
	return out;
}

} // namespace

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "build/S-MU2000.dll";
	const bool require_audio = argc > 2 && !std::strcmp(argv[2], "--audio");
	const bool coldstate = argc > 2 && !std::strcmp(argv[2], "--coldstate");
	const bool hybrid_fx = argc > 2 && !std::strcmp(argv[2], "--hybrid-fx");
	const bool hybrid_fx_48 = argc > 2 && !std::strcmp(argv[2], "--hybrid-fx-48");
	HMODULE module = LoadLibraryA(path);
	if (!module) {
		std::fprintf(stderr, "cannot load %s (%lu)\n", path, GetLastError());
		return 1;
	}
	auto entry = reinterpret_cast<entry_fn>(GetProcAddress(module, "VSTPluginMain"));
	if (!entry) {
		std::fprintf(stderr, "VSTPluginMain is missing\n");
		FreeLibrary(module);
		return 1;
	}
	effect *fx = entry(host);
	if (!fx || fx->magic != effect_magic || !fx->dispatcher || !fx->process_replacing) {
		std::fprintf(stderr, "invalid VST2 effect\n");
		FreeLibrary(module);
		return 1;
	}
	if (fx->num_outputs != 2 || !(fx->flags & is_synth) ||
	    fx->dispatcher(fx, eff_get_plug_category, 0, 0, nullptr, 0) != category_synth) {
		std::fprintf(stderr, "plugin is not a stereo instrument\n");
		FreeLibrary(module);
		return 1;
	}

	fx->dispatcher(fx, eff_open, 0, 0, nullptr, 0);
	fx->dispatcher(fx, eff_set_sample_rate, 0, 0, nullptr,
	               hybrid_fx_48 ? 48000.0f : 44100.0f);
	fx->dispatcher(fx, eff_set_block_size, 0, 64, nullptr, 0);
	fx->dispatcher(fx, eff_mains_changed, 0, 1, nullptr, 0);
	if (hybrid_fx || hybrid_fx_48) {
		const bool passed = probe_hybrid_fx(fx, hybrid_fx_48 ? 48000.0 : 44100.0);
		fx->dispatcher(fx, eff_mains_changed, 0, 0, nullptr, 0);
		fx->dispatcher(fx, eff_close, 0, 0, nullptr, 0);
		FreeLibrary(module);
		return passed ? 0 : 4;
	}

	midi_event note{};
	note.type = midi_type;
	// The VST2 ABI reports the event payload size, not sizeof(VstMidiEvent).
	// Hosts such as FL Studio send the canonical value 24.
	note.byte_size = midi_event_byte_size;
	note.delta_frames = 7;
	note.midi_data[0] = 0x90;
	note.midi_data[1] = 60;
	note.midi_data[2] = 100;
	events incoming{};
	incoming.count = 1;
	incoming.items[0] = reinterpret_cast<event *>(&note);
	if (!fx->dispatcher(fx, eff_process_events, 0, 0, &incoming, 0)) {
		std::fprintf(stderr, "MIDI event was rejected\n");
		return 1;
	}

	std::vector<float> left(64), right(64);
	float *outputs[2] = { left.data(), right.data() };
	fx->process_replacing(fx, nullptr, outputs, 64);
	if (!finite_audio(left) || !finite_audio(right)) {
		std::fprintf(stderr, "non-finite audio\n");
		return 1;
	}
	if (require_audio) {
		bool audible = false;
		for (int block = 0; block < 30000 && !audible; block++) {
			fx->process_replacing(fx, nullptr, outputs, 64);
			for (int i = 0; i < 64; i++)
				if (std::fabs(left[i]) > 1.0e-7f || std::fabs(right[i]) > 1.0e-7f) {
					audible = true;
					break;
				}
		}
		if (!audible) {
			std::fprintf(stderr, "MIDI produced no audio after firmware boot\n");
			return 1;
		}
	}

	void *chunk = nullptr;
	const vintptr chunk_size = fx->dispatcher(fx, eff_get_chunk, 0, 0, &chunk, 0);
	if (chunk_size < 16 || !chunk) {
		std::fprintf(stderr, "state chunk is missing\n");
		return 1;
	}
	std::vector<std::uint8_t> saved(static_cast<std::uint8_t *>(chunk),
	                                static_cast<std::uint8_t *>(chunk) + chunk_size);
	if (coldstate) {
		// 起動済みの台の状態（ピアノのまま）を、起動中の台に戻してから曲頭を流す
		// 波形は内部の位相で揃わないので、100ms ごとの音量の動きで比べる
		// （Strings はゆっくり立ち上がって伸び、ピアノは一気に立って減衰する）
		const std::vector<float> with = cold_run(entry, &saved);
		const std::vector<float> ref = cold_run(entry, nullptr);
		auto env = [](const std::vector<float> &x) {
			std::vector<double> v;
			for (size_t at = 0; at + 4410 <= x.size(); at += 4410) {
				double e = 0;
				for (size_t i = at; i < at + 4410; i++)
					e += double(x[i]) * x[i];
				v.push_back(10.0 * std::log10(e / 4410.0 + 1e-20));
			}
			return v;
		};
		const std::vector<double> a = env(with), b = env(ref);
		double worst = 0;
		std::printf("冷えた起動（100ms ごとの音量 dB。上が状態を戻した台、下が戻さない台）\n");
		// 最初の 100ms は見ない（戻した状態で声が鳴っていると、止める MIDI のぶん
		// 曲頭が 10-20ms 遅れて、出だしだけずれる）。音色が違えば（ピアノの減衰）
		// 後ろの窓で数 dB ずつ離れていく
		for (size_t i = 0; i < a.size() && i < b.size(); i++) {
			std::printf("  %5.1f / %5.1f\n", a[i], b[i]);
			if (i >= 1 && b[i] > -80.0)
				worst = std::max(worst, std::fabs(a[i] - b[i]));
		}
		std::printf("いちばん違った所 %.1f dB（3 dB より小さければ同じ音色）\n", worst);
		fx->dispatcher(fx, eff_close, 0, 0, nullptr, 0);
		FreeLibrary(module);
		return worst < 3.0 ? 0 : 3;
	}
	if (!fx->dispatcher(fx, eff_set_chunk, 0, chunk_size, saved.data(), 0)) {
		std::fprintf(stderr, "state chunk was rejected\n");
		return 1;
	}

	rect *editor_rect = nullptr;
	if (!fx->dispatcher(fx, eff_edit_get_rect, 0, 0, &editor_rect, 0) || !editor_rect ||
	    editor_rect->right <= editor_rect->left || editor_rect->bottom <= editor_rect->top) {
		std::fprintf(stderr, "editor rectangle is invalid\n");
		return 1;
	}
	HWND parent = CreateWindowExA(0, "STATIC", "", WS_OVERLAPPED,
	                              0, 0, editor_rect->right, editor_rect->bottom,
	                              nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
	if (parent) {
		if (!fx->dispatcher(fx, eff_edit_open, 0, 0, parent, 0)) {
			std::fprintf(stderr, "editor could not attach\n");
			return 1;
		}
		fx->dispatcher(fx, eff_edit_close, 0, 0, nullptr, 0);
		DestroyWindow(parent);
	}

	char name[64] = {};
	fx->dispatcher(fx, eff_get_effect_name, 0, 0, name, 0);
	fx->dispatcher(fx, eff_mains_changed, 0, 0, nullptr, 0);
	fx->dispatcher(fx, eff_close, 0, 0, nullptr, 0);
	FreeLibrary(module);
	std::printf("VSTi probe passed: %s, stereo, MIDI%s, state, editor\n",
	            name, require_audio ? "/audio" : "");
	return 0;
}

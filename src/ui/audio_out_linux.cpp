// license:BSD-3-Clause
//
// ALSA の音声出力（Linux。issue #25）。口の形は audio_out.cpp（WASAPI）・
// audio_out_mac.cpp（CoreAudio）と同じ。
//
// doc/design.md の決まりはここでも同じで、**時計はこちらが持たない**。
// ALSA は「溜めに空きができるまで待つ」形なので、書き込みの呼び出しそのものが
// 拍子になる。CoreAudio と違って ALSA は呼び返してくれないので、
// WASAPI と同じように**自分のスレッドを 1 本**持つ。
//
// 44100Hz の 16bit ステレオのまま渡す。`default` のような plug の付いた口なら、
// 機械が別の形式でも ALSA の側で直してくれる。`hw:...` を名指しされたときは、
// その形式で開けなければ開かない（勝手に音を変えないため）。

#include "audio_out.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ui {

namespace {

// ALSA は口が無いだけで標準エラーへ書く（WSL のように /dev/snd が無い機械では
// 「open /dev/snd/seq failed」が出る）。困りごとはこちらが文字列で返すので黙らせる
void alsa_quiet(const char *, int, const char *, int, const char *, ...) {}

void hush_alsa()
{
	static bool done = false;
	if (!done) {
		snd_lib_error_set_handler(alsa_quiet);
		done = true;
	}
}

double now_sec()
{
	timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

std::string lowered(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
		out.push_back(char(std::tolower((unsigned char)c)));
	return out;
}

void put_u32(std::FILE *f, u32 v)
{
	const u8 b[4] = { u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24) };
	std::fwrite(b, 1, 4, f);
}

void put_u16(std::FILE *f, u16 v)
{
	const u8 b[2] = { u8(v), u8(v >> 8) };
	std::fwrite(b, 1, 2, f);
}

// 44100Hz・16bit・2ch の WAV の頭
void write_wav_header(std::FILE *f, u32 frames)
{
	const u32 data = frames * 4;
	std::fwrite("RIFF", 1, 4, f);
	put_u32(f, 36 + data);
	std::fwrite("WAVEfmt ", 1, 8, f);
	put_u32(f, 16);
	put_u16(f, 1);
	put_u16(f, 2);
	put_u32(f, AUDIO_RATE);
	put_u32(f, AUDIO_RATE * 4);
	put_u16(f, 4);
	put_u16(f, 16);
	std::fwrite("data", 1, 4, f);
	put_u32(f, data);
}

} // namespace

// 使える再生の口。ALSA の「名前の当て」から、出力に使えるものだけを拾う。
// 番号ではなく名前で選ぶのは、ほかの platform と同じ理由（挿し直すとずれる）
std::vector<std::string> audio_out::list()
{
	hush_alsa();
	std::vector<std::string> out;
	void **hints = nullptr;
	if (snd_device_name_hint(-1, "pcm", &hints) != 0 || !hints)
		return out;
	for (void **h = hints; *h; h++) {
		char *name = snd_device_name_get_hint(*h, "NAME");
		char *ioid = snd_device_name_get_hint(*h, "IOID");
		char *desc = snd_device_name_get_hint(*h, "DESC");
		// IOID が無ければ入出力どちらにも使える口
		const bool playable = !ioid || std::strcmp(ioid, "Output") == 0;
		if (name && playable && std::strcmp(name, "null") != 0) {
			std::string line = name;
			if (desc) {
				// 説明は複数行のことがある。1 行目だけを添える
				std::string d = desc;
				const size_t nl = d.find('\n');
				if (nl != std::string::npos)
					d.resize(nl);
				if (!d.empty())
					line += "  (" + d + ")";
			}
			out.push_back(line);
		}
		if (name) std::free(name);
		if (ioid) std::free(ioid);
		if (desc) std::free(desc);
	}
	snd_device_name_free_hint(hints);
	return out;
}

struct audio_out::impl
{
	snd_pcm_t *pcm = nullptr;
	fill_fn    fill;
	std::string name;

	// 書き出し（--dump-dev）。audio_out が持つものを指す。stop() より長生きする
	std::vector<s16> *cap = nullptr;

	std::thread       thread;
	std::atomic<bool> quit{false};
	std::atomic<bool> running{false};
	std::atomic<u32>  buffer_frames{0};
	std::atomic<u64>  produced{0}, starved{0};
	std::atomic<double> busy_sec{0.0}, worst_sec{0.0};
	cpu_meter         meter;        // 画面に出す直近の重さ（issue #80）
	std::atomic<bool> realtime{false};

	std::vector<s16> block;        // 1 回ぶんの作り置き（スレッドだけが触る）
	u32 period = 0;

	void run()
	{
		// 音声のスレッドは、ほかの仕事に何十ミリ秒も止められては困る。
		// Windows の MMCSS、macOS の HAL スレッドに当たるのがこれ。
		// 権限が無ければ黙って普通の優先度のまま（rtprio の設定が要る）
		sched_param sp{};
		sp.sched_priority = std::min(80, sched_get_priority_max(SCHED_FIFO));
		realtime.store(pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0);

		while (!quit.load(std::memory_order_acquire)) {
			const double t0 = now_sec();
			if (fill)
				fill(block.data(), period);
			if (cap)
				cap->insert(cap->end(), block.begin(), block.begin() + size_t(period) * 2);
			const double took = now_sec() - t0;
			busy_sec.store(busy_sec.load() + took);
			meter.add(took, double(period) / AUDIO_RATE);
			if (took > worst_sec.load())
				worst_sec.store(took);

			snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, block.data(), period);
			if (wrote == -EPIPE) {
				// 溜めが空になった（音が切れた）。作り直して続ける
				starved.store(starved.load() + 1);
				snd_pcm_prepare(pcm);
				continue;
			}
			if (wrote == -ESTRPIPE) {
				// 機械が眠っていた。起きるまで待って作り直す
				int r = 0;
				while ((r = snd_pcm_resume(pcm)) == -EAGAIN)
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				if (r < 0)
					snd_pcm_prepare(pcm);
				continue;
			}
			if (wrote < 0) {
				if (snd_pcm_recover(pcm, int(wrote), 1) < 0)
					break;
				continue;
			}
			produced.store(produced.load() + u64(wrote));
		}
		running.store(false);
	}
};

audio_out::audio_out() = default;

audio_out::~audio_out()
{
	stop();
}

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err, bool exclusive,
                      const std::string &device, bool raw, bool exact)
{
	(void)raw;          // ALSA には「エンジンを飛ばす」に当たるものが無い
	hush_alsa();
	stop();

	auto up = std::make_unique<impl>();
	up->fill = std::move(fill);
	up->cap  = m_capturing ? &m_cap : nullptr;

	// 名前の一部で選ぶ（--audio）。list() の行は「名前  (説明)」なので、
	// 名前の側と説明の側のどちらに当たってもよいように、行ごと見て名前を取る
	std::string want = device;
	if (!want.empty()) {
		const std::string w = lowered(want);
		std::string picked;
		const auto names = list();
		for (int pass = 0; pass < (exact ? 1 : 2) && picked.empty(); pass++)
		for (const std::string &line : names) {
			const std::string name = lowered(line);
			if (pass == 0 ? name != w && name.substr(0, name.find("  (")) != w
			              : name.find(w) == std::string::npos)
				continue;
			picked = line.substr(0, line.find("  ("));
			break;
		}
		if (!picked.empty())
			want = picked;
	}
	const std::string dev = want.empty() ? std::string("default") : want;

	// exclusive は ALSA には無い。hw: の口は元々ほかのアプリと混ざらないが、
	// こちらが取り上げるわけではないので、頼まれても黙って既定の開き方をする
	int rc = snd_pcm_open(&up->pcm, dev.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
	if (rc < 0) {
		err = std::string("音声の出口を開けない（") + dev + "）: " + snd_strerror(rc);
		return false;
	}

	unsigned rate = AUDIO_RATE;
	// 溜める目標。0 以下なら 40ms（ALSA の既定の溜めに近い）
	unsigned buffer_us = unsigned((latency_ms > 0 ? latency_ms : 40) * 1000);
	if (buffer_us < 8000)
		buffer_us = 8000;
	unsigned period_us = buffer_us / 4;

	snd_pcm_hw_params_t *hw = nullptr;
	snd_pcm_hw_params_alloca(&hw);
	int dir = 0;
	if (snd_pcm_hw_params_any(up->pcm, hw) < 0 ||
	    snd_pcm_hw_params_set_access(up->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0 ||
	    snd_pcm_hw_params_set_format(up->pcm, hw, SND_PCM_FORMAT_S16_LE) < 0 ||
	    snd_pcm_hw_params_set_channels(up->pcm, hw, 2) < 0 ||
	    snd_pcm_hw_params_set_rate_near(up->pcm, hw, &rate, &dir) < 0 ||
	    snd_pcm_hw_params_set_buffer_time_near(up->pcm, hw, &buffer_us, &dir) < 0 ||
	    snd_pcm_hw_params_set_period_time_near(up->pcm, hw, &period_us, &dir) < 0 ||
	    snd_pcm_hw_params(up->pcm, hw) < 0) {
		err = std::string("音声の形式を決められない（") + dev + "）";
		snd_pcm_close(up->pcm);
		return false;
	}
	if (rate != AUDIO_RATE) {
		// MU2000 は 44100Hz でしか動かない。plug の付いた口（default など）なら
		// ALSA が直してくれるので、ここへは来ない
		char buf[128];
		std::snprintf(buf, sizeof(buf), "この口は %u Hz でしか開けない（44100 が要る。default を使う）", rate);
		err = buf;
		snd_pcm_close(up->pcm);
		return false;
	}

	snd_pcm_uframes_t frames = 0;
	snd_pcm_hw_params_get_period_size(hw, &frames, &dir);
	if (!frames)
		frames = 512;
	up->period = u32(frames);
	up->block.assign(size_t(up->period) * 2, 0);
	up->buffer_frames.store(up->period);

	if ((rc = snd_pcm_prepare(up->pcm)) < 0) {
		err = std::string("音声を用意できない: ") + snd_strerror(rc);
		snd_pcm_close(up->pcm);
		return false;
	}

	up->name = dev;
	up->running.store(true);
	impl *raw_impl = up.get();
	up->thread = std::thread([raw_impl] { raw_impl->run(); });
	m_impl = std::move(up);
	(void)exclusive;
	return true;
}

void audio_out::stop()
{
	if (!m_impl)
		return;
	m_impl->quit.store(true, std::memory_order_release);
	if (m_impl->thread.joinable())
		m_impl->thread.join();
	if (m_impl->pcm) {
		snd_pcm_drop(m_impl->pcm);
		snd_pcm_close(m_impl->pcm);
		m_impl->pcm = nullptr;
	}
	m_impl.reset();
}

std::string audio_out::device_name() const
{
	return m_impl ? m_impl->name : std::string();
}

// ALSA に「独り占め」は無い。hw: の口は元々ほかのアプリと混ざらないが、
// こちらが取り上げたわけではないので false
bool audio_out::exclusive() const
{
	return false;
}

void audio_out::set_capture(const std::string &path)
{
	m_cap_path  = path;
	m_capturing = !path.empty();
	m_cap.clear();
}

u64 audio_out::capture_frames() const
{
	return u64(m_cap.size() / 2);
}

bool audio_out::write_capture(std::string &err)
{
	if (m_cap_path.empty()) {
		err = "書き出す先が決まっていない";
		return false;
	}
	std::FILE *f = std::fopen(m_cap_path.c_str(), "wb");
	if (!f) {
		err = "書けない: " + m_cap_path;
		return false;
	}
	write_wav_header(f, u32(m_cap.size() / 2));
	const std::size_t wrote = m_cap.empty()
	    ? 0 : std::fwrite(m_cap.data(), sizeof(s16), m_cap.size(), f);
	const bool ok = std::fclose(f) == 0 && wrote == m_cap.size();
	if (!ok)
		err = "書き込みが途中で終わった: " + m_cap_path;
	return ok;
}

u32 audio_out::buffer_frames() const { return m_impl ? m_impl->buffer_frames.load() : 0; }
u64 audio_out::produced() const      { return m_impl ? m_impl->produced.load() : 0; }
u64 audio_out::starved() const       { return m_impl ? m_impl->starved.load() : 0; }
bool audio_out::mmcss() const        { return m_impl && m_impl->realtime.load(); }

double audio_out::cpu_percent() const
{
	if (!m_impl)
		return 0.0;
	const u64 done = m_impl->produced.load();
	if (!done)
		return 0.0;
	const double audio = double(done) / AUDIO_RATE;
	return 100.0 * m_impl->busy_sec.load() / audio;
}

double audio_out::cpu_recent() const
{
	return m_impl ? m_impl->meter.value() : 0.0;
}

double audio_out::worst_ms() const
{
	return m_impl ? 1000.0 * m_impl->worst_sec.load() : 0.0;
}

} // namespace ui

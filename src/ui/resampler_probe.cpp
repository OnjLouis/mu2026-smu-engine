#include "resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

// Independent FIR oracle: silent spans must retain their phase and tails.
static bool check_silent_spans(double in_rate, double out_rate, bool clamp)
{
	constexpr int half = 32, steps = 256, frames = 8192;
	std::vector<float> table(half * steps + 2);
	for (size_t k = 0; k < table.size(); ++k) {
		const double d = double(k) / steps;
		const double x = 3.14159265358979323846 * d;
		const double sinc = k == 0 ? 1.0 : std::sin(x) / x;
		const double t = (d + half) / 64.0;
		const double window = 0.42 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * t)
		                    + 0.08 * std::cos(4.0 * 3.14159265358979323846 * t);
		table[k] = float(sinc * window);
	}
	std::vector<float> input(frames * 2, 0.0f);
	for (int at : {0, 511, 2200, 4096, 6100}) {
		input[at * 2] = 2.0f;
		input[(at + 3) * 2 + 1] = -1.75f;
	}
	ui::resampler rs;
	rs.configure(in_rate, out_rate);
	const double step = in_rate / out_rate;
	const double cutoff = std::min(1.0, out_rate / in_rate) * 0.955;
	const bool direct = std::abs(in_rate - out_rate) < 1e-6;
	for (int repeat = 0; repeat != 2; ++repeat) {
		rs.reset();
		double pos = 0.0;
		for (int at = 0; at < frames; at += 128) {
			rs.push_float(input.data() + at * 2, 128);
			const int available = rs.output_available();
			std::vector<float> output(size_t(available) * 2);
			if (clamp) rs.pull(output.data(), available);
			else rs.pull_unclamped(output.data(), available);
			for (int n = 0; n < available; ++n) {
				const auto centre = s64(std::floor(pos));
				double l = 0.0, r = 0.0, sum = 0.0;
				if (direct) {
					l = input[size_t(centre) * 2];
					r = input[size_t(centre) * 2 + 1];
				} else {
					for (int k = -half + 1; k <= half; ++k) {
						const auto idx = centre + k;
						if (idx < 0 || idx >= at + 128) continue;
						const double d = std::abs((pos - double(idx)) * cutoff);
						const double fx = d * steps;
						const size_t j = size_t(fx);
						if (j + 1 >= table.size()) continue;
						const double t = fx - double(j);
						const double h = table[j] + (table[j + 1] - table[j]) * t;
						l += h * input[size_t(idx) * 2];
						r += h * input[size_t(idx) * 2 + 1];
						sum += h;
					}
					if (sum > 1e-9) { l /= sum; r /= sum; }
					if (clamp) {
						l = std::clamp(l, -1.0, 1.0);
						r = std::clamp(r, -1.0, 1.0);
					}
				}
				if (output[size_t(n) * 2] != float(l)
				    || output[size_t(n) * 2 + 1] != float(r)) {
					std::fprintf(stderr, "silent-span mismatch %.0f -> %.0f at %.3f\n",
					             in_rate, out_rate, pos);
					return false;
				}
				pos += step;
			}
		}
	}
	return true;
}

int main()
{
	for (const auto rates : {std::array<double, 2>{48000, 44100},
	                         std::array<double, 2>{44100, 48000},
	                         std::array<double, 2>{96000, 44100},
	                         std::array<double, 2>{44100, 44100}})
		for (bool clamp : {false, true})
			if (!check_silent_spans(rates[0], rates[1], clamp)) return 1;
	ui::resampler rs;
	rs.configure(48000.0, 44100.0);
	std::array<float, 960 * 2> input {};
	std::array<float, 960 * 2> output {};
	input.fill(1.5f);
	rs.push_float(input.data(), 960);
	const int available = rs.output_available();
	rs.pull_unclamped(output.data(), available);
	float peak = 0.0f;
	for (int i = 64; i < available * 2; ++i)
		peak = std::max(peak, std::abs(output[i]));
	if (available <= 0 || std::abs(peak - 1.5f) > 0.05f) {
		std::fprintf(stderr, "external FX headroom lost: %.6f\n", peak);
		return 1;
	}

	rs.configure(48000.0, 44100.0);
	std::array<s16, 960 * 2> pcm {};
	pcm.fill(16384);
	rs.push(pcm.data(), 960);
	rs.pull(output.data(), rs.output_available());
	peak = 0.0f;
	for (int i = 64; i < available * 2; ++i)
		peak = std::max(peak, std::abs(output[i]));
	if (std::abs(peak - 0.5f) > 0.05f) {
		std::fprintf(stderr, "PCM resampling changed: %.6f\n", peak);
		return 1;
	}
	return 0;
}

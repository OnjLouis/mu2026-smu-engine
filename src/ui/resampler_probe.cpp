#include "resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

int main()
{
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

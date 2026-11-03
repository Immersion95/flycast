/* This file is part of Flycast, licensed under GPL-2.0-or-later. */
#pragma once

#include <cmath>
#include <mutex>

namespace libretro
{

struct VideoTiming
{
	double refreshHz = 59.940060;
	double sampleRate = 44100.0;
};

// Publish one coherent pair from the emulation thread. Frontend callbacks must
// run after taking a copy, with no timing lock held.
class VideoTimingState
{
public:
	void publish(double refreshHz, double sampleRate)
	{
		if (!std::isfinite(refreshHz) || refreshHz <= 0.0 || refreshHz >= 100.0
				|| !std::isfinite(sampleRate) || sampleRate <= 0.0)
			return;
		const std::lock_guard<std::mutex> lock(mutex);
		timing = { refreshHz, sampleRate };
	}

	VideoTiming get()
	{
		const std::lock_guard<std::mutex> lock(mutex);
		return timing;
	}

	void reset()
	{
		const std::lock_guard<std::mutex> lock(mutex);
		timing = {};
	}

private:
	std::mutex mutex;
	VideoTiming timing;
};

}

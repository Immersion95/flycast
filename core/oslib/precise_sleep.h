/*
 * This file is part of Flycast, licensed under GPL-2.0-or-later.
 */
#pragma once

#include <chrono>

class PreciseSleeper
{
public:
	using Clock = std::chrono::steady_clock;

	PreciseSleeper() = default;
	~PreciseSleeper();
	PreciseSleeper(const PreciseSleeper&) = delete;
	PreciseSleeper& operator=(const PreciseSleeper&) = delete;

	void sleepUntil(Clock::time_point deadline);

private:
	void sleepCoarse(Clock::time_point deadline);
#ifdef _WIN32
	void *timer = nullptr;
	bool timerCreated = false;
#endif
};

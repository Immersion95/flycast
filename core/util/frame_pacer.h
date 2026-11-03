/*
 * This file is part of Flycast, licensed under GPL-2.0-or-later.
 */
#pragma once

#include <chrono>
#include <cmath>

// Host time only: this schedule must never be serialized with the machine.
// All calls belong to the emulation thread (or to a stopped emulator).
class FramePacer
{
public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	void reset() { initialized = false; }

	TimePoint nextDeadline(TimePoint now, double refreshHz)
	{
		if (!std::isfinite(refreshHz) || refreshHz <= 0.0)
		{
			reset();
			return now;
		}

		const double ticks = std::chrono::duration<double, Clock::period>(
				std::chrono::duration<double>(1.0 / refreshHz)).count();
		// Reject unusable periods before converting floating point to an integer.
		if (ticks < 1.0 || ticks > std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(1)).count())
		{
			reset();
			return now;
		}

		if (!initialized)
		{
			initialized = true;
			deadline = now;
			fractionalTicks = 0.0;
			return now;
		}

		// Carry the sub-tick remainder instead of rounding every field's period.
		const double accumulatedTicks = ticks + fractionalTicks;
		const auto wholeTicks = static_cast<Clock::duration::rep>(accumulatedTicks);
		fractionalTicks = accumulatedTicks - wholeTicks;
		const Clock::duration period(wholeTicks);
		deadline += period;

		// Preserve small timing errors, but never repay a stall with a long burst
		// of unpaced fields. The next field starts a fresh schedule after a stall.
		if (now > deadline && now - deadline >= period)
		{
			deadline = now;
			fractionalTicks = 0.0;
		}
		return deadline;
	}

private:
	bool initialized = false;
	double fractionalTicks = 0.0;
	TimePoint deadline {};
};

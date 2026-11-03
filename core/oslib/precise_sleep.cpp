/*
 * This file is part of Flycast, licensed under GPL-2.0-or-later.
 */
#include "precise_sleep.h"

#include <atomic>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#elif defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64))
#include <intrin.h>
#endif

namespace {
void pauseCpu()
{
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
	_mm_pause();
#elif defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64))
	__yield();
#elif defined(__aarch64__) || (defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7)
	__asm__ __volatile__("yield");
#else
	std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
}

PreciseSleeper::~PreciseSleeper()
{
#ifdef _WIN32
	if (timer != nullptr)
		CloseHandle(timer);
#endif
}

void PreciseSleeper::sleepCoarse(Clock::time_point deadline)
{
#ifdef _WIN32
	if (!timerCreated)
	{
		timerCreated = true;
		// Windows 10 1803+. Older systems reject this flag and use the fallback.
		constexpr DWORD highResolution = 0x00000002;
		timer = CreateWaitableTimerExW(nullptr, nullptr, highResolution,
				TIMER_MODIFY_STATE | SYNCHRONIZE);
		if (timer == nullptr)
			timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
	}
	if (timer != nullptr)
	{
		const auto remaining = deadline - Clock::now();
		if (remaining <= Clock::duration::zero())
			return;
		const auto ticks = std::chrono::duration_cast<
				std::chrono::duration<long long, std::ratio<1, 10'000'000>>>(remaining).count();
		if (ticks <= 0)
			return;
		LARGE_INTEGER dueTime;
		dueTime.QuadPart = -ticks; // Relative delay in 100 ns units.
		if (SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE)
				&& WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
			return;
	}
#endif
	std::this_thread::sleep_until(deadline);
}

void PreciseSleeper::sleepUntil(Clock::time_point deadline)
{
	// Keep only the last half-millisecond active. Yielding the OS thread here
	// can instead give away the remainder of its time slice and miss the deadline.
	const auto coarseDeadline = deadline - std::chrono::microseconds(500);
	if (Clock::now() < coarseDeadline)
		sleepCoarse(coarseDeadline);
	while (Clock::now() < deadline)
		pauseCpu();
}

/*
    This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "types.h"
#include "cfg/option.h"
#include "audio/audiostream.h"
#include "emulator.h"

#include <libretro.h>

#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstring>
#include <limits>

/* Detect output refresh rate changes by monitoring
 * the last 'VSYNC_SWAP_INTERVAL_FRAMES' frames:
 * - Measure average (mean) audio samples per upload
 *   operation
 * - Determine vsync swap interval based on
 *   expected samples at 60 (or 50) Hz
 * - Check that vsync swap interval remains
 *   'stable' for at least 'VSYNC_SWAP_INTERVAL_FRAMES' */
#define VSYNC_SWAP_INTERVAL_FRAMES 6
/* Calculated swap interval is 'valid' if it is
 * within 'VSYNC_SWAP_INTERVAL_THRESHOLD' of an integer
 * value */
#define VSYNC_SWAP_INTERVAL_THRESHOLD 0.05f

extern void retro_request_av_info_update(void);

extern retro_environment_t        environ_cb;
extern retro_audio_sample_batch_t audio_batch_cb;

extern float libretro_expected_audio_samples_per_run;
extern unsigned libretro_vsync_swap_interval;
extern bool libretro_detect_vsync_swap_interval;

static float audio_samples_per_frame_avg;
static unsigned vsync_swap_interval_last;
static unsigned vsync_swap_interval_conter;

static std::mutex audio_buffer_mutex;
static std::vector<int16_t> audio_buffer;
static size_t audio_buffer_idx;
static size_t audio_batch_frames_max;
static bool drop_samples = true;

namespace {
class AudioSyncClock
{
public:
	void reset()
	{
		initialized = false;
	}

	void waitForFrames(size_t frames)
	{
		if (frames == 0)
			return;
		using namespace std::chrono;
		const auto interval = duration_cast<steady_clock::duration>(duration<double>((double)frames / 44100.0));
		auto now = steady_clock::now();
		if (!initialized || now + 250ms < nextTick || now > nextTick + 250ms)
		{
			nextTick = now;
			initialized = true;
		}
		nextTick += interval;
		const auto coarseTarget = nextTick - 1ms;
		if (coarseTarget > now)
			std::this_thread::sleep_until(coarseTarget);
		while (std::chrono::steady_clock::now() < nextTick)
			std::this_thread::yield();
	}

private:
	bool initialized = false;
	std::chrono::steady_clock::time_point nextTick {};
};

AudioSyncClock audioSyncClock;
}

static int16_t *audio_out_buffer = nullptr;

void retro_audio_init(void)
{
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	/* Worst case is 25 fps content with an audio sample rate
	 * of 44.1 kHz -> 1764 stereo samples
	 * But flycast can stop rendering for arbitrary lengths of
	 * time, leading to multiple 'frames' worth of audio being
	 * uploaded in retro_run(). We therefore require some leniency,
	 * but must limit the total number of samples that can be
	 * uploaded since the libretro frontend can 'hang' if too
	 * many samples are sent during a single call of retro_run().
	 * We therefore (arbitrarily) choose to allow up to 10 frames
	 * worth of 'worst case' stereo samples... */
	size_t audio_buffer_size = (44100 / 25) * 2 * 10;

	audio_buffer.resize(audio_buffer_size);
	audio_buffer_idx = 0;
	audio_batch_frames_max = std::numeric_limits<size_t>::max();
	audioSyncClock.reset();

	audio_out_buffer = (int16_t*)malloc(audio_buffer_size * sizeof(int16_t));

	drop_samples = false;

	audio_samples_per_frame_avg = 0.0f;
	vsync_swap_interval_last = 1;
	vsync_swap_interval_conter = 0;
	audioSyncClock.reset();
}

void retro_audio_deinit(void)
{
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	audio_buffer.clear();
	audio_buffer_idx = 0;

	if (audio_out_buffer != nullptr)
		free(audio_out_buffer);

	audio_out_buffer = nullptr;

	drop_samples = true;

	audio_samples_per_frame_avg = 0.0f;
	vsync_swap_interval_last = 1;
	vsync_swap_interval_conter = 0;
	audioSyncClock.reset();
}

void retro_audio_flush_buffer(void)
{
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);
	audio_buffer_idx = 0;

	/* We are manually 'resetting' the audio buffer
	 * -> any 'drop samples' lock can be released */
	drop_samples = false;
	audioSyncClock.reset();
}

void retro_audio_reset_timing(void)
{
	audioSyncClock.reset();
}

void retro_audio_upload(void)
{
	audio_buffer_mutex.lock();

	for (size_t i = 0; i < audio_buffer_idx; i++)
		audio_out_buffer[i] = audio_buffer[i];

	size_t num_frames = audio_buffer_idx >> 1;
	audio_buffer_idx = 0;

	/* Uploading audio 'resets' the audio buffer
	 * -> any 'drop samples' lock can be released */
	drop_samples = false;

	audio_buffer_mutex.unlock();

	/* Attempt to detect changes in output refresh rate */
	if (libretro_detect_vsync_swap_interval &&
	    (num_frames > 0))
	{
		/* Simple running average (leaky-integrator) */
		audio_samples_per_frame_avg = ((1.0f / (float)VSYNC_SWAP_INTERVAL_FRAMES) * (float)num_frames) +
				((1.0f - (1.0f / (float)VSYNC_SWAP_INTERVAL_FRAMES)) * audio_samples_per_frame_avg);

		float swap_ratio = audio_samples_per_frame_avg /
				libretro_expected_audio_samples_per_run;
		unsigned swap_integer;
		float swap_remainder;

		/* If internal frame rate is equal to (within threshold)
		 * or higher than the default 60 (or 50) Hz, fall back
		 * to a swap interval of 1 */
		if (swap_ratio < (1.0f + VSYNC_SWAP_INTERVAL_THRESHOLD))
		{
			swap_integer = 1;
			swap_remainder = 0.0f;
		}
		else
		{
			swap_integer = (unsigned)(swap_ratio + 0.5f);
			swap_remainder = swap_ratio - (float)swap_integer;
			swap_remainder = (swap_remainder < 0.0f) ?
					-swap_remainder : swap_remainder;
		}

		/* > Swap interval is considered 'valid' if it is
		 *   within VSYNC_SWAP_INTERVAL_THRESHOLD of an integer
		 *   value
		 * > If valid, check if new swap interval differs from
		 *   previously logged value */
		if ((swap_remainder <= VSYNC_SWAP_INTERVAL_THRESHOLD) &&
			 (swap_integer != libretro_vsync_swap_interval))
		{
			vsync_swap_interval_conter =
					(swap_integer == vsync_swap_interval_last) ?
							(vsync_swap_interval_conter + 1) : 0;

			/* Check whether swap interval is 'stable' */
			if (vsync_swap_interval_conter >= VSYNC_SWAP_INTERVAL_FRAMES)
			{
				libretro_vsync_swap_interval = swap_integer;
				vsync_swap_interval_conter = 0;
				retro_request_av_info_update();
			}

			vsync_swap_interval_last = swap_integer;
		}
		else
			vsync_swap_interval_conter = 0;
	}

	size_t batch_frames_max = audio_batch_frames_max;
	if (batch_frames_max == 0)
		batch_frames_max = std::numeric_limits<size_t>::max();

	size_t total_written_frames = 0;
	int16_t *audio_out_buffer_ptr = audio_out_buffer;
	while (num_frames > 0)
	{
		size_t frames_to_write = std::min(num_frames, batch_frames_max);
		size_t frames_written = audio_batch_cb(audio_out_buffer_ptr, frames_to_write);
		if (frames_written == 0)
			break;

		total_written_frames += frames_written;
		num_frames -= frames_written;
		audio_out_buffer_ptr += frames_written << 1;

		if (frames_written < frames_to_write)
		{
			batch_frames_max = frames_written;
			audio_batch_frames_max = std::max<size_t>(frames_written, 1);
		}
		else if (audio_batch_frames_max != std::numeric_limits<size_t>::max())
		{
			if (audio_batch_frames_max >= std::numeric_limits<size_t>::max() / 2)
				audio_batch_frames_max = std::numeric_limits<size_t>::max();
			else
				audio_batch_frames_max = std::max(audio_batch_frames_max * 2, batch_frames_max);
		}
	}

	if (num_frames > 0)
	{
		const size_t remaining_samples = num_frames << 1;
		const size_t written_samples_offset = total_written_frames << 1;
		const size_t existing_samples_capacity = audio_buffer.size();
		std::lock_guard<std::mutex> lock(audio_buffer_mutex);
		size_t existing_samples = audio_buffer_idx;
		if (remaining_samples + existing_samples > existing_samples_capacity)
		{
			if (remaining_samples >= existing_samples_capacity)
			{
				std::memcpy(audio_buffer.data(), audio_out_buffer + written_samples_offset,
						std::min(remaining_samples, existing_samples_capacity) * sizeof(int16_t));
				audio_buffer_idx = std::min(remaining_samples, existing_samples_capacity);
				drop_samples = true;
			}
			else
			{
				size_t keep_existing = existing_samples_capacity - remaining_samples;
				if (existing_samples > keep_existing)
					existing_samples = keep_existing;
				std::memmove(audio_buffer.data() + remaining_samples, audio_buffer.data(), existing_samples * sizeof(int16_t));
				std::memcpy(audio_buffer.data(), audio_out_buffer + written_samples_offset, remaining_samples * sizeof(int16_t));
				audio_buffer_idx = remaining_samples + existing_samples;
			}
		}
		else
		{
			std::memmove(audio_buffer.data() + remaining_samples, audio_buffer.data(), existing_samples * sizeof(int16_t));
			std::memcpy(audio_buffer.data(), audio_out_buffer + written_samples_offset, remaining_samples * sizeof(int16_t));
			audio_buffer_idx = remaining_samples + existing_samples;
		}
	}

	if (config::LimitFPS && config::TimingSource == 0 && !settings.input.fastForwardMode)
		audioSyncClock.waitForFrames(total_written_frames);
	else
		audioSyncClock.reset();
}

void WriteSample(s16 r, s16 l)
{
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	if (drop_samples)
		return;

	if (audio_buffer.size() < audio_buffer_idx + 2)
	{
		/* Audio buffer overflow...
		 * > Drop any existing samples
		 * > Drop any future samples until the next
		 *   call of retro_audio_upload() */
		audio_buffer_idx = 0;
		drop_samples = true;
		return;
	}

	audio_buffer[audio_buffer_idx++] = l;
	audio_buffer[audio_buffer_idx++] = r;
}

void InitAudio()
{
}

void TermAudio()
{
}

void StartAudioRecording(bool eight_khz)
{
}

u32 RecordAudio(void *buffer, u32 samples)
{
	return 0;
}

void StopAudioRecording()
{
}

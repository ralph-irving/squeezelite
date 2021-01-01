/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2021, ralph_irving@hotmail.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// sample processing - only included when building with PROCESS set

#include "squeezelite.h"

#if PROCESS

extern log_level loglevel;

extern struct buffer *outputbuf;
extern struct decodestate decode;
struct processstate process;
extern struct codec *codec;

#define LOCK_D   mutex_lock(decode.mutex);
#define UNLOCK_D mutex_unlock(decode.mutex);
#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)

// macros to map to processing functions - currently only resample.c
// this can be made more generic when multiple processing mechanisms get added
#if RESAMPLE
#define SAMPLES_FUNC resample_samples
#define DRAIN_FUNC   resample_drain
#define NEWSTREAM_FUNC resample_newstream
#define FLUSH_FUNC   resample_flush
#define INIT_FUNC    resample_init
#endif


// transfer all processed frames to the output buf
static void _write_samples(void) {
	frames_t frames = process.out_frames;
	u32_t *iptr   = (u32_t *)process.outbuf;
	unsigned cnt  = 10;

	LOCK_O;

	while (frames > 0) {

		frames_t f = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
		u32_t *optr = (u32_t *)outputbuf->writep;

		if (f > 0) {

			f = min(f, frames);
			
			memcpy(optr, iptr, f * BYTES_PER_FRAME);
			
			frames -= f;
			
			_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
			iptr += f * BYTES_PER_FRAME / sizeof(*iptr);

		} else if (cnt--) {

			// there should normally be space in the output buffer, but may need to wait during drain phase
			UNLOCK_O;
			usleep(10000);
			LOCK_O;

		} else {

			// bail out if no space found after 100ms to avoid locking
			LOG_ERROR("unable to get space in output buffer");
			UNLOCK_O;
			return;
		}
	}

	UNLOCK_O;
}

// process samples - called with decode mutex set
void process_samples(void) {

	SAMPLES_FUNC(&process);

	_write_samples();

	process.in_frames = 0;
}

// drain at end of track - called with decode mutex set
void process_drain(void) {
	bool done;

	do {

		done = DRAIN_FUNC(&process);

		_write_samples();

	} while (!done);

	LOG_DEBUG("processing track complete - frames in: %lu out: %lu", process.total_in, process.total_out);
}	

// new stream - called with decode mutex set
unsigned process_newstream(bool *direct, unsigned raw_sample_rate, unsigned supported_rates[]) {

	bool active = NEWSTREAM_FUNC(&process, raw_sample_rate, supported_rates);

	LOG_INFO("processing: %s", active ? "active" : "inactive");

	*direct = !active;

	if (active) {

		unsigned max_in_frames, max_out_frames;

		process.in_frames = process.out_frames = 0;
		process.total_in = process.total_out = 0;

		max_in_frames = codec->min_space / BYTES_PER_FRAME ;

		// increase size of output buffer by 10% as output rate is not an exact multiple of input rate
		if (process.out_sample_rate % process.in_sample_rate == 0) {
			max_out_frames = max_in_frames * (process.out_sample_rate / process.in_sample_rate);
		} else {
			max_out_frames = (int)(1.1 * (float)max_in_frames * (float)process.out_sample_rate / (float)process.in_sample_rate);
		}

		if (process.max_in_frames != max_in_frames) {
			LOG_DEBUG("creating process buf in frames: %u", max_in_frames);
			if (process.inbuf) free(process.inbuf);
			process.inbuf = malloc(max_in_frames * BYTES_PER_FRAME);
			process.max_in_frames = max_in_frames;
		}
		
		if (process.max_out_frames != max_out_frames) {
			LOG_DEBUG("creating process buf out frames: %u", max_out_frames);
			if (process.outbuf) free(process.outbuf);
			process.outbuf = malloc(max_out_frames * BYTES_PER_FRAME);
			process.max_out_frames = max_out_frames;
		}
		
		if (!process.inbuf || !process.outbuf) {
			LOG_ERROR("malloc fail creating process buffers");
			*direct = true;
			return raw_sample_rate;
		}
		
		return process.out_sample_rate;
	}

	return raw_sample_rate;
}

// process flush - called with decode mutex set
void process_flush(void) {

	LOG_INFO("process flush");

	FLUSH_FUNC();

	process.in_frames = 0;
}

// init - called with no mutex
void process_init(char *opt) {

	bool enabled = INIT_FUNC(opt);

	memset(&process, 0, sizeof(process));

	if (enabled) {
		LOCK_D;
		decode.process = true;
		UNLOCK_D;
	}
}

#endif // #if PROCESS

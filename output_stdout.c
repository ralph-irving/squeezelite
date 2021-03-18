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

// Stdout output

#include "squeezelite.h"

#define FRAME_BLOCK MAX_SILENCE_FRAMES

static log_level loglevel;

static bool running = true;

extern struct outputstate output;
extern struct buffer *outputbuf;

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

extern u8_t *silencebuf;
#if DSD
extern u8_t *silencebuf_dsd;
#endif

// buffer to hold output data so we can block on writing outside of output lock, allocated on init
static u8_t *buf;
static unsigned buffill;
static int bytes_per_frame;

static int _stdout_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR, u8_t flags,
								s32_t cross_gain_in, s32_t cross_gain_out, s32_t **cross_ptr) {

	u8_t *obuf;

	if (!silence) {
		
		if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}

		obuf = outputbuf->readp;

	} else {

		obuf = silencebuf;
	}

	IF_DSD(
		   if (output.outfmt != PCM) {
			   if (silence) {
				   obuf = silencebuf_dsd;
			   }
			   if (output.outfmt == DOP)
				   update_dop((u32_t *)obuf, out_frames, output.invert && !silence);
			   else if (output.invert && !silence)
				   dsd_invert((u32_t *)obuf, out_frames);
		   }
	)

	_scale_and_pack_frames(buf + buffill * bytes_per_frame, (s32_t *)(void *)obuf, out_frames, gainL, gainR, flags, output.format);

	buffill += out_frames;

	return (int)out_frames;
}

static void *output_thread() {

	LOCK;

	switch (output.format) {
	case S32_LE:
		bytes_per_frame = 4 * 2; break;
	case S24_3LE:
		bytes_per_frame = 3 * 2; break;
	case S16_LE:
		bytes_per_frame = 2 * 2; break;
	default:
		bytes_per_frame = 4 * 2; break;
		break;
	}

	UNLOCK;

	while (running) {

		LOCK;

		output.device_frames = 0;
		output.updated = gettime_ms();
		output.frames_played_dmp = output.frames_played;

		_output_frames(FRAME_BLOCK);

		UNLOCK;

		if (buffill) {
			fwrite(buf, bytes_per_frame, buffill, stdout);
			buffill = 0;
		}

	}

	return 0;
}

static thread_type thread;

void output_init_stdout(log_level level, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay) {
	loglevel = level;

	LOG_INFO("init output stdout");

	buf = malloc(FRAME_BLOCK * BYTES_PER_FRAME);
	if (!buf) {
		LOG_ERROR("unable to malloc buf");
		return;
	}
	buffill = 0;

	memset(&output, 0, sizeof(output));

	output.format = S32_LE;
	output.start_frames = FRAME_BLOCK * 2;
	output.write_cb = &_stdout_write_frames;
	output.rate_delay = rate_delay;

	if (params) {
		if (!strcmp(params, "32"))	output.format = S32_LE;
		if (!strcmp(params, "24")) output.format = S24_3LE;
		if (!strcmp(params, "16")) output.format = S16_LE;
	}

	// ensure output rate is specified to avoid test open
	if (!rates[0]) {
		rates[0] = 44100;
	}

	output_init_common(level, "-", output_buf_size, rates, 0);

#if LINUX || OSX || FREEBSD
	pthread_attr_t attr;
	pthread_attr_init(&attr);
#ifdef PTHREAD_STACK_MIN
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
#endif
	pthread_create(&thread, &attr, output_thread, NULL);
	pthread_attr_destroy(&attr);
#endif
#if WIN
	thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&output_thread, NULL, 0, NULL);
#endif
}

void output_close_stdout(void) {
	LOG_INFO("close output");

	LOCK;
	running = false;
	UNLOCK;

	free(buf);

	output_close_common();
}

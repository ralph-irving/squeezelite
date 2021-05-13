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

// Common output function

#include "squeezelite.h"

static log_level loglevel;

struct outputstate output;

static struct buffer buf;

struct buffer *outputbuf = &buf;

u8_t *silencebuf;
#if DSD
u8_t *silencebuf_dsd;
#endif

bool user_rates = false;

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

// functions starting _* are called with mutex locked

frames_t _output_frames(frames_t avail) {

	frames_t frames, size;
	bool silence;
	u8_t flags = output.channels;
	
	s32_t cross_gain_in = 0, cross_gain_out = 0; s32_t *cross_ptr = NULL;
	
	s32_t gainL = output.current_replay_gain ? gain(output.gainL, output.current_replay_gain) : output.gainL;
	s32_t gainR = output.current_replay_gain ? gain(output.gainR, output.current_replay_gain) : output.gainR;

	if (output.invert) { gainL = -gainL; gainR = -gainR; }

	frames = _buf_used(outputbuf) / BYTES_PER_FRAME;
	silence = false;

	// start when threshold met
	if (output.state == OUTPUT_BUFFER && (frames * BYTES_PER_FRAME) > output.threshold * output.next_sample_rate / 10 && frames > output.start_frames) {
		output.state = OUTPUT_RUNNING;
		LOG_INFO("start buffer frames: %u", frames);
		wake_controller();
	}
	
	// skip ahead - consume outputbuf but play nothing
	if (output.state == OUTPUT_SKIP_FRAMES) {
		if (frames > 0) {
			frames_t skip = min(frames, output.skip_frames);
			LOG_INFO("skip %u of %u frames", skip, output.skip_frames);
			frames -= skip;
			output.frames_played += skip;
			while (skip > 0) {
				frames_t cont_frames = min(skip, _buf_cont_read(outputbuf) / BYTES_PER_FRAME);
				skip -= cont_frames;
				_buf_inc_readp(outputbuf, cont_frames * BYTES_PER_FRAME);
			}
		}
		output.state = OUTPUT_RUNNING;
	}
	
	// pause frames - play silence for required frames
	if (output.state == OUTPUT_PAUSE_FRAMES) {
		LOG_INFO("pause %u frames", output.pause_frames);
		if (output.pause_frames == 0) {
			output.state = OUTPUT_RUNNING;
		} else {
			silence = true;
			frames = min(avail, output.pause_frames);
			frames = min(frames, MAX_SILENCE_FRAMES);
			output.pause_frames -= frames;
		}
	}
	
	// start at - play silence until jiffies reached
	if (output.state == OUTPUT_START_AT) {
		u32_t now = gettime_ms();
		if (now >= output.start_at || output.start_at > now + 10000) {
			output.state = OUTPUT_RUNNING;
		} else {
			u32_t delta_frames = (output.start_at - now) * output.current_sample_rate / 1000;
			silence = true;
			frames = min(avail, delta_frames);
			frames = min(frames, MAX_SILENCE_FRAMES);
		}
	}
	
	// play silence if buffering or no frames
	if (output.state <= OUTPUT_BUFFER || frames == 0) {
		silence = true;
		frames = min(avail, MAX_SILENCE_FRAMES);
	}

	LOG_SDEBUG("avail: %d frames: %d silence: %d", avail, frames, silence);
	frames = min(frames, avail);
	size = frames;
	
	while (size > 0) {
		frames_t out_frames;
		frames_t cont_frames = _buf_cont_read(outputbuf) / BYTES_PER_FRAME;
		int wrote;
		
		if (output.track_start && !silence) {
			if (output.track_start == outputbuf->readp) {
				unsigned delay = 0;
				if (output.current_sample_rate != output.next_sample_rate) {
					delay = output.rate_delay;
#if PULSEAUDIO
					set_sample_rate(output.next_sample_rate);
#endif
				}
				IF_DSD(
				   if (output.outfmt != output.next_fmt) {
					   delay = output.dsd_delay;
				   }
				)
				frames -= size;
				// add silence delay in two halves, before and after track start on rate or pcm-dop change
				if (delay) {
					output.state = OUTPUT_PAUSE_FRAMES;
					if (!output.delay_active) {
						output.pause_frames = output.current_sample_rate * delay / 2000;
						output.delay_active = true;  // first delay - don't process track start
						break;
					} else {
						output.pause_frames = output.next_sample_rate * delay / 2000;
						output.delay_active = false; // second delay - process track start
					}
				}
				LOG_INFO("track start sample rate: %u replay_gain: %u", output.next_sample_rate, output.next_replay_gain);
				output.frames_played = 0;
				output.track_started = true;
				output.track_start_time = gettime_ms();
				output.current_sample_rate = output.next_sample_rate;
				IF_DSD(
				   output.outfmt = output.next_fmt;
				)
				if (output.fade == FADE_INACTIVE || output.fade_mode != FADE_CROSSFADE) {
					output.current_replay_gain = output.next_replay_gain;
				}
				output.track_start = NULL;
				break;
			} else if (output.track_start > outputbuf->readp) {
				// reduce cont_frames so we find the next track start at beginning of next chunk
				cont_frames = min(cont_frames, (output.track_start - outputbuf->readp) / BYTES_PER_FRAME);
			}
		}

		IF_DSD(
			if (output.outfmt != PCM) {
				gainL = gainR = FIXED_ONE;
			}
		)
		
		if (output.fade && !silence) {
			if (output.fade == FADE_DUE) {
				if (output.fade_start == outputbuf->readp) {
					LOG_INFO("fade start reached");
					output.fade = FADE_ACTIVE;
				} else if (output.fade_start > outputbuf->readp) {
					cont_frames = min(cont_frames, (output.fade_start - outputbuf->readp) / BYTES_PER_FRAME);
				}
			}
			if (output.fade == FADE_ACTIVE) {
				// find position within fade
				frames_t cur_f = outputbuf->readp >= output.fade_start ? (outputbuf->readp - output.fade_start) / BYTES_PER_FRAME : 
					(outputbuf->readp + outputbuf->size - output.fade_start) / BYTES_PER_FRAME;
				frames_t dur_f = output.fade_end >= output.fade_start ? (output.fade_end - output.fade_start) / BYTES_PER_FRAME :
					(output.fade_end + outputbuf->size - output.fade_start) / BYTES_PER_FRAME;
				if (cur_f >= dur_f) {
					if (output.fade_mode == FADE_INOUT && output.fade_dir == FADE_DOWN) {
						LOG_INFO("fade down complete, starting fade up");
						output.fade_dir = FADE_UP;
						output.fade_start = outputbuf->readp;
						output.fade_end = outputbuf->readp + dur_f * BYTES_PER_FRAME;
						if (output.fade_end >= outputbuf->wrap) {
							output.fade_end -= outputbuf->size;
						}
						cur_f = 0;
					} else if (output.fade_mode == FADE_CROSSFADE) {
						LOG_INFO("crossfade complete");
						if (_buf_used(outputbuf) >= dur_f * BYTES_PER_FRAME) {
							_buf_inc_readp(outputbuf, dur_f * BYTES_PER_FRAME);
							LOG_INFO("skipped crossfaded start");
						} else {
							LOG_WARN("unable to skip crossfaded start");
						}
						output.fade = FADE_INACTIVE;
						output.current_replay_gain = output.next_replay_gain;
					} else {
						LOG_INFO("fade complete");
						output.fade = FADE_INACTIVE;
					}
				}
				// if fade in progress set fade gain, ensure cont_frames reduced so we get to end of fade at start of chunk
				if (output.fade) {
					if (output.fade_end > outputbuf->readp) {
						cont_frames = min(cont_frames, (output.fade_end - outputbuf->readp) / BYTES_PER_FRAME);
					}
					if (output.fade_dir == FADE_UP || output.fade_dir == FADE_DOWN) {
						// fade in, in-out, out handled via altering standard gain
						s32_t fade_gain;
						if (output.fade_dir == FADE_DOWN) {
							cur_f = dur_f - cur_f;
						}
						fade_gain = to_gain((float)cur_f / (float)dur_f);
						gainL = gain(gainL, fade_gain);
						gainR = gain(gainR, fade_gain);
						if (output.invert) { gainL = -gainL; gainR = -gainR; }
					}
					if (output.fade_dir == FADE_CROSS) {
						// cross fade requires special treatment - performed later based on these values
						// support different replay gain for old and new track by retaining old value until crossfade completes
						if (_buf_used(outputbuf) / BYTES_PER_FRAME > dur_f + size) { 
							cross_gain_in  = to_gain((float)cur_f / (float)dur_f);
							cross_gain_out = FIXED_ONE - cross_gain_in;
							if (output.current_replay_gain) {
								cross_gain_out = gain(cross_gain_out, output.current_replay_gain);
							}
							if (output.next_replay_gain) {
								cross_gain_in = gain(cross_gain_in, output.next_replay_gain);
							}
							gainL = output.gainL;
							gainR = output.gainR;
							if (output.invert) { gainL = -gainL; gainR = -gainR; }
							cross_ptr = (s32_t *)(output.fade_end + cur_f * BYTES_PER_FRAME);
						} else {
							LOG_INFO("unable to continue crossfade - too few samples");
							output.fade = FADE_INACTIVE;
						}
					}
				}
			}
		}
		
		out_frames = !silence ? min(size, cont_frames) : size;
		
		IF_DSD(
			if (output.outfmt != PCM) {
				flags = 0;
			}
		)

		wrote = output.write_cb(out_frames, silence, gainL, gainR, flags, cross_gain_in, cross_gain_out, &cross_ptr);

		if (wrote <= 0) {
			frames -= size;
			break;
		} else {
			out_frames = (frames_t)wrote;
		}

		size -= out_frames;

		_vis_export(outputbuf, &output, out_frames, silence);

		if (!silence) {
			_buf_inc_readp(outputbuf, out_frames * BYTES_PER_FRAME);
			output.frames_played += out_frames;
		}
	}
			
	LOG_SDEBUG("wrote %u frames", frames);

	return frames;
}

void _checkfade(bool start) {
	frames_t bytes;

	LOG_INFO("fade mode: %u duration: %u %s", output.fade_mode, output.fade_secs, start ? "track-start" : "track-end");

	bytes = output.next_sample_rate * BYTES_PER_FRAME * output.fade_secs;
	if (output.fade_mode == FADE_INOUT) {
		/* align on a frame boundary */
		bytes = ((bytes / 2) / BYTES_PER_FRAME) * BYTES_PER_FRAME;
	}

	if (start && (output.fade_mode == FADE_IN || (output.fade_mode == FADE_INOUT && _buf_used(outputbuf) == 0))) {
		bytes = min(bytes, outputbuf->size - BYTES_PER_FRAME); // shorter than full buffer otherwise start and end align
		LOG_INFO("fade IN: %u frames", bytes / BYTES_PER_FRAME);
		output.fade = FADE_DUE;
		output.fade_dir = FADE_UP;
		output.fade_start = outputbuf->writep;
		output.fade_end = output.fade_start + bytes;
		if (output.fade_end >= outputbuf->wrap) {
			output.fade_end -= outputbuf->size;
		}
	}

	if (!start && (output.fade_mode == FADE_OUT || output.fade_mode == FADE_INOUT)) {
		bytes = min(_buf_used(outputbuf), bytes);
		LOG_INFO("fade %s: %u frames", output.fade_mode == FADE_INOUT ? "IN-OUT" : "OUT", bytes / BYTES_PER_FRAME);
		output.fade = FADE_DUE;
		output.fade_dir = FADE_DOWN;
		output.fade_start = outputbuf->writep - bytes;
		if (output.fade_start < outputbuf->buf) {
			output.fade_start += outputbuf->size;
		}
		output.fade_end = outputbuf->writep;
	}

	if (start && output.fade_mode == FADE_CROSSFADE) {
		if (_buf_used(outputbuf) != 0) {
			if (output.next_sample_rate != output.current_sample_rate) {
				LOG_INFO("crossfade disabled as sample rates differ");
				return;
			}
			bytes = min(bytes, _buf_used(outputbuf));               // max of current remaining samples from previous track
			bytes = min(bytes, (frames_t)(outputbuf->size * 0.9));  // max of 90% of outputbuf as we consume additional buffer during crossfade
			LOG_INFO("CROSSFADE: %u frames", bytes / BYTES_PER_FRAME);
			output.fade = FADE_DUE;
			output.fade_dir = FADE_CROSS;
			output.fade_start = outputbuf->writep - bytes;
			if (output.fade_start < outputbuf->buf) {
				output.fade_start += outputbuf->size;
			}
			output.fade_end = outputbuf->writep;
			output.track_start = output.fade_start;
		} else if (outputbuf->size == OUTPUTBUF_SIZE && outputbuf->readp == outputbuf->buf) {
			// if default setting used and nothing in buffer attempt to resize to provide full crossfade support
			LOG_INFO("resize outputbuf for crossfade");
			_buf_resize(outputbuf, OUTPUTBUF_SIZE_CROSSFADE);
#if LINUX || FREEBSD
			touch_memory(outputbuf->buf, outputbuf->size);
#endif			
		}
	}
}

void output_init_common(log_level level, const char *device, unsigned output_buf_size, unsigned rates[], unsigned idle) {
	unsigned i;

	loglevel = level;

	output_buf_size = output_buf_size - (output_buf_size % BYTES_PER_FRAME);
	LOG_DEBUG("outputbuf size: %u", output_buf_size);

	buf_init(outputbuf, output_buf_size);
	if (!outputbuf->buf) {
		LOG_ERROR("unable to malloc output buffer");
		exit(0);
	}

	silencebuf = malloc(MAX_SILENCE_FRAMES * BYTES_PER_FRAME);
	if (!silencebuf) {
		LOG_ERROR("unable to malloc silence buffer");
		exit(0);
	}
	memset(silencebuf, 0, MAX_SILENCE_FRAMES * BYTES_PER_FRAME);

	IF_DSD(
		silencebuf_dsd = malloc(MAX_SILENCE_FRAMES * BYTES_PER_FRAME);
		if (!silencebuf_dsd) {
			LOG_ERROR("unable to malloc silence dsd buffer");
			exit(0);
		}
		dsd_silence_frames((u32_t *)silencebuf_dsd, MAX_SILENCE_FRAMES);
	)

	LOG_DEBUG("idle timeout: %u", idle);

	output.state = idle ? OUTPUT_OFF: OUTPUT_STOPPED;
	output.device = device;
	output.fade = FADE_INACTIVE;
	output.invert = false;
	output.error_opening = false;
	output.idle_to = (u32_t) idle;

	/* Skip test_open for stdout, set default sample rates */
	if ( output.device[0] == '-' ) {
		for (i = 0; i < MAX_SUPPORTED_SAMPLERATES; ++i) {
			output.supported_rates[i] = rates[i];
		}
	}
	else {
		if (!test_open(output.device, output.supported_rates, user_rates)) {
			LOG_ERROR("unable to open output device: %s", output.device);
			exit(0);
		}
	}

	if (user_rates) {
		for (i = 0; i < MAX_SUPPORTED_SAMPLERATES; ++i) {
			output.supported_rates[i] = rates[i];
		}
	}

	// set initial sample rate, preferring 44100
	for (i = 0; i < MAX_SUPPORTED_SAMPLERATES; ++i) {
		if (output.supported_rates[i] == 44100) {
			output.default_sample_rate = 44100;
			break;
		}
	}
	if (!output.default_sample_rate) {
		output.default_sample_rate = output.supported_rates[0];
	}
	
	output.current_sample_rate = output.default_sample_rate;

	if (loglevel >= lINFO) {
		char rates_buf[10 * MAX_SUPPORTED_SAMPLERATES] = "";
		for (i = 0; output.supported_rates[i]; ++i) {
			char s[10];
			sprintf(s, "%d ", output.supported_rates[i]);
			strcat(rates_buf, s);
		}
		LOG_INFO("supported rates: %s", rates_buf);
	}
}

void output_close_common(void) {
	buf_destroy(outputbuf);
	free(silencebuf);
	IF_DSD(
		free(silencebuf_dsd);
	)
}

void output_flush(void) {
	LOG_INFO("flush output buffer");
	buf_flush(outputbuf);
	LOCK;
	output.fade = FADE_INACTIVE;
	if (output.state != OUTPUT_OFF) {
		output.state = OUTPUT_STOPPED;
		output.stop_time = gettime_ms();
		if (output.error_opening) {
			output.current_sample_rate = output.default_sample_rate;
		}
		output.delay_active = false;
	}
	output.frames_played = 0;
	UNLOCK;
}

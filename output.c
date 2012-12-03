/* 
 *  Squeezelite - lightweight headless squeezeplay emulator for linux
 *
 *  (c) Adrian Smith 2012, triode1@btinternet.com
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

// alsa output

#include "squeezelite.h"

#include <alsa/asoundlib.h>

static log_level loglevel;

#define MAX_SILENCE_FRAMES 1024

// for mmap ouput we convert to LE formats on BE devices as it is likely hardware requires LE
static snd_pcm_format_t fmts_mmap[] = { SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S16_LE,
										SND_PCM_FORMAT_UNKNOWN };

// for non mmap output we rely on ALSA to do the conversion and just open the device in native 32bit native endian
static snd_pcm_format_t fmts_writei[] = {
#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	SND_PCM_FORMAT_S32_LE,
#else
	SND_PCM_FORMAT_S32_BE,
#endif
	SND_PCM_FORMAT_UNKNOWN };

typedef unsigned frames_t;

// ouput device
static struct {
	char *device;
	snd_pcm_format_t format;
	snd_pcm_uframes_t period_size;
	unsigned rate;
	bool mmap;
} alsa;

struct outputstate output;

static inline s32_t gain(s32_t gain, s32_t sample) {
	s64_t res = (s64_t)gain * (s64_t)sample;
	return (s32_t) (res >> 16);
}

void alsa_list_pcm(void) {
	void **hints, **n;
	if (snd_device_name_hint(-1, "pcm", &hints) >= 0) {
		n = hints;
		printf("Output devices:\n");
		while (*n) {
			char *name = snd_device_name_get_hint(*n, "NAME");
			char *desc = snd_device_name_get_hint(*n, "DESC");
			if (name) printf("  %-30s", name);
			if (desc) {
				char *s1 = strtok(desc, "\n");
				char *s2 = strtok(NULL, "\n");
				if (s1) printf(" - %s", s1);
				if (s2) printf(" - %s", s2);
			}
			printf("\n");
			if (name) free(name);
			if (desc) free(desc);
			n++;
		}
		snd_device_name_free_hint(hints);
	}
	printf("\n");
}

static void alsa_close(snd_pcm_t *pcmp) {
	int err;
	if ((err = snd_pcm_close(pcmp)) < 0) {
		LOG_ERROR("snd_pcm_close error: %s", snd_strerror(err));
	}
}

bool alsa_testopen(const char *device, u32_t *max_rate) {
	int err;
	snd_pcm_t *pcm;
	snd_pcm_hw_params_t *hw_params;
	hw_params = (snd_pcm_hw_params_t *) alloca(snd_pcm_hw_params_sizeof());
	memset(hw_params, 0, snd_pcm_hw_params_sizeof());

	// open device
	if ((err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		LOG_ERROR("playback open error: %s", snd_strerror(err));
		return false;
	}

	// get max params
	if ((err = snd_pcm_hw_params_any(pcm, hw_params)) < 0) {
		LOG_ERROR("hwparam init error: %s", snd_strerror(err));
		return false;
	}

	// get max rate
	if ((err = snd_pcm_hw_params_get_rate_max(hw_params, max_rate, 0)) < 0) {
		LOG_ERROR("unable to get max sample rate: %s", snd_strerror(err));
		return false;
	}

	if (*max_rate > 192000) {
		*max_rate = 192000;
	}

	if ((err = snd_pcm_close(pcm)) < 0) {
		LOG_ERROR("snd_pcm_close error: %s", snd_strerror(err));
		return false;
	}

	return true;
}

static bool pcm_probe(const char *device) {
	int err;
	snd_pcm_t *pcm;

	if ((err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		return false;
	}

	if ((err = snd_pcm_close(pcm)) < 0) {
		LOG_ERROR("snd_pcm_close error: %s", snd_strerror(err));
	}

	return true;
}

static int alsa_open(snd_pcm_t **pcmp, const char *device, unsigned sample_rate, unsigned buffer_time, unsigned period_count) {
	int err;
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_hw_params_alloca(&hw_params);

	// close if already open
	if (*pcmp) alsa_close(*pcmp);

	// reset params
	if (alsa.device) free(alsa.device);
	alsa.device = NULL;
	alsa.rate = 0;
	alsa.period_size = 0;
	alsa.format = SND_PCM_FORMAT_UNKNOWN;

	// open device
	if ((err = snd_pcm_open(pcmp, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		LOG_ERROR("playback open error: %s", snd_strerror(err));
		return err;
	}
	alsa.device = malloc(strlen(device) + 1);
	strcpy(alsa.device, device);

	// init params
	memset(hw_params, 0, snd_pcm_hw_params_sizeof());
	if ((err = snd_pcm_hw_params_any(*pcmp, hw_params)) < 0) {
		LOG_ERROR("hwparam init error: %s", snd_strerror(err));
		return err;
	}

	// set access 
	if ((err = snd_pcm_hw_params_set_access(*pcmp, hw_params, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0) {
		LOG_INFO("alsa mmap not available trying non mmap access");
		if ((err = snd_pcm_hw_params_set_access(*pcmp, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
			LOG_ERROR("access type not available: %s", snd_strerror(err));
			return err;
		}
		alsa.mmap = false;
	} else {
		alsa.mmap = true;
	}

	// set the sample format
	snd_pcm_format_t *fmt = alsa.mmap ? (snd_pcm_format_t *)fmts_mmap : (snd_pcm_format_t *)fmts_writei;
	while (*fmt != SND_PCM_FORMAT_UNKNOWN) {
		if (snd_pcm_hw_params_set_format(*pcmp, hw_params, *fmt) >= 0) {
			LOG_INFO("opened device %s using format: %s sample rate: %u", device, snd_pcm_format_name(*fmt), sample_rate);
			alsa.format = *fmt;
			break;
		}
		++fmt;
		if (*fmt == SND_PCM_FORMAT_UNKNOWN) {
			LOG_ERROR("unable to open audio device with any supported format");
			return -1;
		}
	}

	// set channels
	if ((err = snd_pcm_hw_params_set_channels (*pcmp, hw_params, 2)) < 0) {
		LOG_ERROR("channel count not available: %s", snd_strerror(err));
		return err;
	}

	// set sample rate
	if ((err = snd_pcm_hw_params_set_rate(*pcmp, hw_params, sample_rate, 0)) < 0) {
		LOG_ERROR("sample rate not available: %s", snd_strerror(err));
		return err;
	}
	alsa.rate = sample_rate;

	// set buffer time and period count
	unsigned count = period_count;
	if ((err = snd_pcm_hw_params_set_periods_near(*pcmp, hw_params, &count, 0)) < 0) {
		LOG_ERROR("unable to set period size %s", snd_strerror(err));
		return err;
	}

	unsigned time = buffer_time;
	int dir = 1;
	if ((err = snd_pcm_hw_params_set_buffer_time_near(*pcmp, hw_params, &time, &dir)) < 0) {
		LOG_ERROR("unable to set buffer time %s", snd_strerror(err));
		return err;
	}

	// get period_size
	if ((err = snd_pcm_hw_params_get_period_size(hw_params, &alsa.period_size, 0)) < 0) {
		LOG_ERROR("unable to get period size: %s", snd_strerror(err));
		return err;
	}

	// get buffer_size
	snd_pcm_uframes_t buffer_size;
	if ((err = snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size)) < 0) {
		LOG_ERROR("unable to get buffer size: %s", snd_strerror(err));
		return err;
	}

	LOG_INFO("buffer time: %u period count: %u buffer size: %u period size: %u", time, count, buffer_size, alsa.period_size);

	// set params
	if ((err = snd_pcm_hw_params(*pcmp, hw_params)) < 0) {
		LOG_ERROR("unable to set hw params: %s", snd_strerror(err));
		return err;
	}

	// dump info
	if (loglevel == SDEBUG) {
		static snd_output_t *debug_output;
		snd_output_stdio_attach(&debug_output, stderr, 0);
		snd_pcm_dump(*pcmp, debug_output);
	}

	return 0;
}


// output thread 

static struct buffer buf;

struct buffer *outputbuf = &buf;

static u8_t silencebuf[MAX_SILENCE_FRAMES * BYTES_PER_FRAME];

#define LOCK   pthread_mutex_lock(&outputbuf->mutex)
#define UNLOCK pthread_mutex_unlock(&outputbuf->mutex)

static bool running = true;

static void *output_thread() {
	snd_pcm_t *pcmp = NULL;
	bool start = true;
	bool output_off = false, probe_device = false;
	int err;

	while (running) {

		// disabled output - player is off
		while (output_off) {
			usleep(100000);
			LOCK;
			output_off = (output.state == OUTPUT_OFF);
			UNLOCK;
		}

		// wait until device returns - to allow usb audio devices to be turned off
		if (probe_device) {
			while (!pcm_probe(output.device)) {
				LOG_DEBUG("waiting for device %s to return", output.device);
				sleep(2);
			}
			probe_device = false;
		}

		if (!pcmp || alsa.rate != output.current_sample_rate) {
			LOG_INFO("open output device: %s", output.device);
			alsa_open(&pcmp, output.device, output.current_sample_rate, output.buffer_time, output.period_count);
			start = true;
		}

		snd_pcm_state_t state = snd_pcm_state(pcmp);

		if (state == SND_PCM_STATE_XRUN) {
			LOG_INFO("XRUN");
			if ((err = snd_pcm_recover(pcmp, -EPIPE, 1)) < 0) {
				LOG_WARN("XRUN recover failed: %s", snd_strerror(err));
			}
			start = true;
			continue;
		} else if (state == SND_PCM_STATE_SUSPENDED) {
			if ((err = snd_pcm_recover(pcmp, -ESTRPIPE, 1)) < 0) {
				LOG_WARN("SUSPEND recover failed: %s", snd_strerror(err));
			}
		} else if (state == SND_PCM_STATE_DISCONNECTED) {
			LOG_INFO("Device %s no longer available", output.device);
			alsa_close(pcmp);
			pcmp = NULL;
			probe_device = true;
			continue;
		}

		if (start) {
			if ((err = snd_pcm_start(pcmp)) < 0) {
				if ((err = snd_pcm_recover(pcmp, err, 1)) < 0) {
					LOG_WARN("start error: %s", snd_strerror(err));
				}
			} else {
				start = false;
			}
		}

		snd_pcm_sframes_t avail = snd_pcm_avail_update(pcmp);

		if (avail < 0) {
			if ((err = snd_pcm_recover(pcmp, avail, 1)) < 0) {
				if (err == -ENODEV) {
					LOG_INFO("Device %s no longer available", output.device);
					alsa_close(pcmp);
					pcmp = NULL;
					probe_device = true;
					continue;
				}
				LOG_WARN("recover failed: %s", snd_strerror(err));
			}
			start = true;
			continue;
		}

		if (avail < alsa.period_size) {
			if ((err = snd_pcm_wait(pcmp, 1000)) < 0) {
				if ((err = snd_pcm_recover(pcmp, err, 1)) < 0) {
					LOG_WARN("pcm wait error: %s", snd_strerror(err));
				}
				start = true;
				continue;
			}
			avail = snd_pcm_avail_update(pcmp);
		}

		// avoid spinning in cases where wait returns but no bytes available (seen with pulse audio)
		if (avail == 0) {
			LOG_SDEBUG("avail 0 - sleeping");
			usleep(10000);
			continue;
		}

		LOCK;

		snd_pcm_sframes_t frames = _buf_used(outputbuf) / BYTES_PER_FRAME;
		bool silence = false;

		// turn off if requested
		if (output.state == OUTPUT_OFF) {
			UNLOCK;
			alsa_close(pcmp);
			pcmp = NULL;
			output_off = true;
			LOG_INFO("disabling output");
			continue;
		}

		// start when threshold met, note: avail * 4 may need tuning
		if (output.state == OUTPUT_BUFFER && frames > avail * 4 && frames > output.threshold * output.next_sample_rate / 100) {
			output.state = OUTPUT_RUNNING;
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

		// start at - play slience until jiffies reached
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

		// play slience if buffering or no frames
		if (output.state <= OUTPUT_BUFFER || frames == 0) {
			silence = true;
			frames = min(avail, MAX_SILENCE_FRAMES);
		}

		LOG_SDEBUG("avail: %d frames: %d silence: %d", avail, frames, silence);
		frames = min(frames, avail);
		snd_pcm_sframes_t size = frames;

		snd_pcm_sframes_t delay;
		snd_pcm_delay(pcmp, &delay);
		output.alsa_frames = delay;
		output.updated = gettime_ms();

		while (size > 0) {
			snd_pcm_uframes_t alsa_frames;
			
			frames_t cont_frames = _buf_cont_read(outputbuf) / BYTES_PER_FRAME;
			
			if (output.track_start && !silence) {
				if (output.track_start == outputbuf->readp) {
					LOG_INFO("track start sample rate: %u replay_gain: %u", output.next_sample_rate, output.next_replay_gain);
					output.frames_played = 0;
					output.track_started = true;
					output.current_sample_rate = output.next_sample_rate;
					output.current_replay_gain = output.next_replay_gain;
					output.track_start = NULL;
					break;
				} else if (output.track_start > outputbuf->readp) {
					// reduce cont_frames so we find the next track start at beginning of next chunk
					cont_frames = min(cont_frames, (output.track_start - outputbuf->readp) / BYTES_PER_FRAME);
				}
			}

			alsa_frames = !silence ? min(size, cont_frames) : size;

			avail = snd_pcm_avail_update(pcmp);

			s32_t gainL = output.current_replay_gain ? gain(output.gainL, output.current_replay_gain) : output.gainL;
			s32_t gainR = output.current_replay_gain ? gain(output.gainR, output.current_replay_gain) : output.gainR;

			if (alsa.mmap) {

				const snd_pcm_channel_area_t *areas;
				snd_pcm_uframes_t offset;

				if ((err = snd_pcm_mmap_begin(pcmp, &areas, &offset, &alsa_frames)) < 0) {
					LOG_WARN("error from mmap_begin: %s", snd_strerror(err));
					break;
				}

				void  *outputptr = areas[0].addr + (areas[0].first + offset * areas[0].step) / 8;
				s32_t *inputptr  = (s32_t *) (silence ? silencebuf : outputbuf->readp);
				size_t cnt = alsa_frames;
				
				switch(alsa.format) {
				case SND_PCM_FORMAT_S16_LE:
					{
#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
						s16_t *optr = (s16_t *)(void *)outputptr;
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt--) {
								*(optr++) = *(inputptr++) >> 16;
								*(optr++) = *(inputptr++) >> 16;
							}
						} else {
							while (cnt--) {
								*(optr++) = gain(gainL, *(inputptr++)) >> 16;
								*(optr++) = gain(gainR, *(inputptr++)) >> 16;
							}
						}
#else
						u8_t *optr = (u8_t *)(void *)outputptr;
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt--) {
								*(optr++) = (*(inputptr)   & 0x00ff0000) >> 16;
								*(optr++) = (*(inputptr++) & 0xff000000) >> 24;
								*(optr++) = (*(inputptr)   & 0x00ff0000) >> 16;
								*(optr++) = (*(inputptr++) & 0xff000000) >> 24;
							}
						} else {
							while (cnt--) {
								s32_t lsample = gain(gainL, *(inputptr++));
								s32_t rsample = gain(gainR, *(inputptr++));
								*(optr++) = (lsample & 0x00ff0000) >> 16;
								*(optr++) = (lsample & 0xff000000) >> 24;
								*(optr++) = (rsample & 0x00ff0000) >> 16;
								*(optr++) = (rsample & 0xff000000) >> 24;
							}
						}
#endif
					}
					break;
				case SND_PCM_FORMAT_S24_LE: 
					{
#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
						s32_t *optr = (s32_t *)(void *)outputptr;
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt--) {
								*(optr++) = *(inputptr++) >> 8;
								*(optr++) = *(inputptr++) >> 8;
							}
						} else {
							while (cnt--) {
								*(optr++) = gain(gainL, *(inputptr++)) >> 8;
								*(optr++) = gain(gainR, *(inputptr++)) >> 8;
							}
						}
#else
						u8_t *optr = (u8_t *)(void *)outputptr;
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt--) {
								*(optr++) = (*(inputptr)   & 0x0000ff00) >>  8;
								*(optr++) = (*(inputptr)   & 0x00ff0000) >> 16;
								*(optr++) = (*(inputptr++) & 0xff000000) >> 24;
								*(optr++) = 0;
								*(optr++) = (*(inputptr)   & 0x0000ff00) >>  8;
								*(optr++) = (*(inputptr)   & 0x00ff0000) >> 16;
								*(optr++) = (*(inputptr++) & 0xff000000) >> 24;
								*(optr++) = 0;
							}
						} else {
							while (cnt--) {
								s32_t lsample = gain(gainL, *(inputptr++));
								s32_t rsample = gain(gainR, *(inputptr++));
								*(optr++) = (lsample & 0x0000ff00) >>  8;
								*(optr++) = (lsample & 0x00ff0000) >> 16;
								*(optr++) = (lsample & 0xff000000) >> 24;
								*(optr++) = 0;
								*(optr++) = (rsample & 0x0000ff00) >>  8;
								*(optr++) = (rsample & 0x00ff0000) >> 16;
								*(optr++) = (rsample & 0xff000000) >> 24;
								*(optr++) = 0;
							}
						}
#endif
					}
					break;
				case SND_PCM_FORMAT_S24_3LE:
					{
						u8_t *optr = (u8_t *)(void *)outputptr;
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt) {
								// attempt to do 32 bit memory accesses - move 2 frames at once: 16 bytes -> 12 bytes
								// falls through to exception case when not aligned or if less than 2 frames to move
								if (((uintptr_t)optr & 0x3) == 0 && cnt >= 2) {
									u32_t *o_ptr = (u32_t *)(void *)optr;
									while (cnt >= 2) {
										s32_t l1 = *(inputptr++); s32_t r1 = *(inputptr++);
										s32_t l2 = *(inputptr++); s32_t r2 = *(inputptr++);
#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
										*(o_ptr++) = (l1 & 0xffffff00) >>  8 | (r1 & 0x0000ff00) << 16;
										*(o_ptr++) = (r1 & 0xffff0000) >> 16 | (l2 & 0x00ffff00) <<  8;
										*(o_ptr++) = (l2 & 0xff000000) >> 24 | (r2 & 0xffffff00);
#else
										*(o_ptr++) = (l1 & 0x0000ff00) << 16 | (l1 & 0x00ff0000) | (l1 & 0xff000000) >> 16 |
											(r1 & 0x0000ff00) >> 8; 
										*(o_ptr++) = (r1 & 0x00ff0000) <<  8 | (r1 & 0xff000000) >> 8 | (l2 & 0x0000ff00) |
											(l2 & 0x00ff0000) >> 16;
										*(o_ptr++) = (l2 & 0xff000000) | (r2 & 0x0000ff00) << 8 | (r2 & 0x00ff0000) >> 8 |
											(r2 & 0xff000000) >> 24;
#endif
										optr += 12;
										cnt  -=  2;
									}
								} else {
									s32_t lsample = *(inputptr++);
									s32_t rsample = *(inputptr++);
									*(optr++) = (lsample & 0x0000ff00) >>  8;
									*(optr++) = (lsample & 0x00ff0000) >> 16;
									*(optr++) = (lsample & 0xff000000) >> 24;
									*(optr++) = (rsample & 0x0000ff00) >>  8;
									*(optr++) = (rsample & 0x00ff0000) >> 16;
									*(optr++) = (rsample & 0xff000000) >> 24;
									cnt--;
								}
							}
						} else {
							while (cnt) {
								// attempt to do 32 bit memory accesses - move 2 frames at once: 16 bytes -> 12 bytes
								// falls through to exception case when not aligned or if less than 2 frames to move
								if (((uintptr_t)optr & 0x3) == 0 && cnt >= 2) {
									u32_t *o_ptr = (u32_t *)(void *)optr;
									while (cnt >= 2) {
										s32_t l1 = gain(gainL, *(inputptr++)); s32_t r1 = gain(gainR, *(inputptr++));
										s32_t l2 = gain(gainL, *(inputptr++)); s32_t r2 = gain(gainR, *(inputptr++));
#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
										*(o_ptr++) = (l1 & 0xffffff00) >>  8 | (r1 & 0x0000ff00) << 16;
										*(o_ptr++) = (r1 & 0xffff0000) >> 16 | (l2 & 0x00ffff00) <<  8;
										*(o_ptr++) = (l2 & 0xff000000) >> 24 | (r2 & 0xffffff00);
#else
										*(o_ptr++) = (l1 & 0x0000ff00) << 16 | (l1 & 0x00ff0000) | (l1 & 0xff000000) >> 16 |
											(r1 & 0x0000ff00) >> 8; 
										*(o_ptr++) = (r1 & 0x00ff0000) <<  8 | (r1 & 0xff000000) >> 8 | (l2 & 0x0000ff00) |
											(l2 & 0x00ff0000) >> 16;
										*(o_ptr++) = (l2 & 0xff000000) | (r2 & 0x0000ff00) << 8 | (r2 & 0x00ff0000) >> 8 |
											(r2 & 0xff000000) >> 24;
#endif
										optr += 12;
										cnt  -=  2;
									}
								} else {
									s32_t lsample = gain(gainL, *(inputptr++));
									s32_t rsample = gain(gainR, *(inputptr++));
									*(optr++) = (lsample & 0x0000ff00) >>  8;
									*(optr++) = (lsample & 0x00ff0000) >> 16;
									*(optr++) = (lsample & 0xff000000) >> 24;
									*(optr++) = (rsample & 0x0000ff00) >>  8;
									*(optr++) = (rsample & 0x00ff0000) >> 16;
									*(optr++) = (rsample & 0xff000000) >> 24;
									cnt--;
								}
							}
						}
					}
					break;
				case SND_PCM_FORMAT_S32_LE:
					{
#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
						s32_t *optr = (s32_t *)(void *)outputptr;
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							memcpy(outputptr, inputptr, cnt * BYTES_PER_FRAME);
						} else {
							while (cnt--) {
								*(optr++) = gain(gainL, *(inputptr++));
								*(optr++) = gain(gainR, *(inputptr++));
							}
						}
#else
						u8_t *optr = (u8_t *)(void *)outputptr;
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt--) {
								*(optr++) = (*(inputptr)   & 0x000000ff);
								*(optr++) = (*(inputptr)   & 0x0000ff00) >>  8;
								*(optr++) = (*(inputptr)   & 0x00ff0000) >> 16;
								*(optr++) = (*(inputptr++) & 0xff000000) >> 24;
								*(optr++) = (*(inputptr)   & 0x000000ff);
								*(optr++) = (*(inputptr)   & 0x0000ff00) >>  8;
								*(optr++) = (*(inputptr)   & 0x00ff0000) >> 16;
								*(optr++) = (*(inputptr++) & 0xff000000) >> 24;
							}
						} else {
							while (cnt--) {
								s32_t lsample = gain(gainL, *(inputptr++));
								s32_t rsample = gain(gainR, *(inputptr++));
								*(optr++) = (lsample & 0x000000ff);
								*(optr++) = (lsample & 0x0000ff00) >>  8;
								*(optr++) = (lsample & 0x00ff0000) >> 16;
								*(optr++) = (lsample & 0xff000000) >> 24;
								*(optr++) = (rsample & 0x000000ff);
								*(optr++) = (rsample & 0x0000ff00) >>  8;
								*(optr++) = (rsample & 0x00ff0000) >> 16;
								*(optr++) = (rsample & 0xff000000) >> 24;
							}
						}
#endif
					}
					break;
				default:
					break;
				}
				
				snd_pcm_sframes_t w = snd_pcm_mmap_commit(pcmp, offset, alsa_frames);
				if (w < 0 || w != alsa_frames) {
					LOG_WARN("mmap_commit error");
					break;
				}

			} else {

				if (!silence && (gainL != FIXED_ONE || gainR!= FIXED_ONE)) {
					unsigned count = alsa_frames;
					s32_t *ptrL = (s32_t *)(void *)outputbuf->readp;
					s32_t *ptrR = (s32_t *)(void *)outputbuf->readp + 1;
					while (count--) {
						*ptrL = gain(output.gainL, *ptrL);
						*ptrR = gain(output.gainR, *ptrR);
						ptrL += 2;
						ptrR += 2;
					}
				}
				
				snd_pcm_sframes_t w = snd_pcm_writei(pcmp, silence ? silencebuf : outputbuf->readp, alsa_frames);
				if (w < 0) {
					LOG_WARN("writei error: %d", w);
					break;
				} else {
					if (w != alsa_frames) {
						LOG_WARN("writei only wrote %u of %u", w, alsa_frames);
					}						
					alsa_frames = w;
				}
			}

			size -= alsa_frames;
			
			if (!silence) {
				_buf_inc_readp(outputbuf, alsa_frames * BYTES_PER_FRAME);
				output.frames_played += alsa_frames;
			}
		}
		
		LOG_SDEBUG("wrote %u frames", frames);

		UNLOCK;
	}

	return 0;
}

static pthread_t thread;

void output_init(log_level level, const char *device, unsigned output_buf_size, unsigned buffer_time, unsigned period_count) {
	loglevel = level;

	LOG_INFO("init output");

	output_buf_size = output_buf_size - (output_buf_size % BYTES_PER_FRAME);
	LOG_DEBUG("outputbuf size: %u", output_buf_size);

	buf_init(outputbuf, output_buf_size);
	output.state = STOPPED;
	output.current_sample_rate = 44100;
	output.device = device;
	output.buffer_time = buffer_time;
	output.period_count = period_count;

	if (!alsa_testopen(output.device, &output.max_sample_rate)) {
		LOG_ERROR("unable to open output device");
		return;
	}

	LOG_INFO("output: %s maxrate: %u", output.device, output.max_sample_rate);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, STREAM_THREAD_STACK_SIZE);
	pthread_create(&thread, &attr, output_thread, NULL);
	pthread_attr_destroy(&attr);

	// try to set this thread to real-time scheduler class, likely only works as root
	struct sched_param param;
	param.sched_priority = 40;
	if (pthread_setschedparam(thread, SCHED_FIFO, &param) != 0) {
		LOG_DEBUG("unable to set output sched fifo: %s", strerror(errno));
	}
}

void output_flush(void) {
	LOG_INFO("flush output buffer");
	buf_flush(outputbuf);
}

void output_close(void) {
	LOG_INFO("close output");
	LOCK;
	running = false;
	UNLOCK;
	pthread_join(thread,NULL);
	buf_destroy(outputbuf);
}

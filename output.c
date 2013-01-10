/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012, 2013, triode1@btinternet.com
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

// Output using Alsa or Portaudio:
// - ALSA output is the preferred output for linux as it allows direct hardware access
// - PortAudio is the output output supported on other platforms and also builds on linux for test purposes

#include "squeezelite.h"

#if ALSA

#include <alsa/asoundlib.h>

#define MAX_SILENCE_FRAMES 1024

static snd_pcm_format_t fmts[] = { SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S16_LE,
								   SND_PCM_FORMAT_UNKNOWN };

// ouput device
static struct {
	char *device;
	snd_pcm_format_t format;
	snd_pcm_uframes_t period_size;
	unsigned rate;
	bool mmap;
	u8_t *write_buf;
} alsa;

static u8_t silencebuf[MAX_SILENCE_FRAMES * BYTES_PER_FRAME];

#endif // ALSA

#if PORTAUDIO

#include <portaudio.h>
#if OSX
#include <pa_mac_core.h>
#endif

#define MAX_SILENCE_FRAMES 102400 // silencebuf not used in pa case so set large

// ouput device
static struct {
	unsigned rate;
	PaStream *stream;
} pa;

#endif // PORTAUDIO

static log_level loglevel;

struct outputstate output;

static struct buffer buf;

struct buffer *outputbuf = &buf;

static bool running = true;

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#define MAX_SCALESAMPLE 0x7fffffffffffLL
#define MIN_SCALESAMPLE -MAX_SCALESAMPLE

static inline s32_t gain(s32_t gain, s32_t sample) {
	s64_t res = (s64_t)gain * (s64_t)sample;
	if (res > MAX_SCALESAMPLE) res = MAX_SCALESAMPLE;
	if (res < MIN_SCALESAMPLE) res = MIN_SCALESAMPLE;
	return (s32_t) (res >> 16);
}

static inline s32_t to_gain(float f) {
	return (s32_t)(f * 65536.0F);
}

#if ALSA

void list_devices(void) {
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

static bool test_open(const char *device, u32_t *max_rate) {
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
	alsa.device = malloc(strlen(device) + 1 + 4); // extra space for changing to plug
	strcpy(alsa.device, device);

	bool retry;
	do {
		// open device
		if ((err = snd_pcm_open(pcmp, alsa.device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
			LOG_ERROR("playback open error: %s", snd_strerror(err));
			return err;
		}

		// init params
		memset(hw_params, 0, snd_pcm_hw_params_sizeof());
		if ((err = snd_pcm_hw_params_any(*pcmp, hw_params)) < 0) {
			LOG_ERROR("hwparam init error: %s", snd_strerror(err));
			return err;
		}

		// open hw: devices without resampling, if sample rate fails try plughw: with resampling
		bool hw = !strncmp(alsa.device, "hw:", 3);
		retry = false;

		if ((err = snd_pcm_hw_params_set_rate_resample(*pcmp, hw_params, !hw)) < 0) {
			LOG_ERROR("resampling setup failed: %s", snd_strerror(err));
			return err;
		}

		if ((err = snd_pcm_hw_params_set_rate(*pcmp, hw_params, sample_rate, 0)) < 0) {
			if (hw) {
				strcpy(alsa.device + 4, device);
				memcpy(alsa.device, "plug", 4);
				LOG_INFO("reopening device %s in plug mode as %s for resampling", device, alsa.device);
				snd_pcm_close(*pcmp);
				retry = true;
			}
		}

	} while (retry);

	// set access 
	if (!alsa.mmap || snd_pcm_hw_params_set_access(*pcmp, hw_params, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0) {
		if ((err = snd_pcm_hw_params_set_access(*pcmp, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
			LOG_ERROR("access type not available: %s", snd_strerror(err));
			return err;
		}
		alsa.mmap = false;
	}

	// set the sample format
	snd_pcm_format_t *fmt = (snd_pcm_format_t *)fmts;
	while (*fmt != SND_PCM_FORMAT_UNKNOWN) {
		if (snd_pcm_hw_params_set_format(*pcmp, hw_params, *fmt) >= 0) {
			LOG_INFO("opened device %s using format: %s sample rate: %u mmap: %u", alsa.device, snd_pcm_format_name(*fmt), sample_rate, alsa.mmap);
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

	// create an intermediate buffer for non mmap case for all but S32_LE
	// this is used to pack samples into the output format before calling writei
	if (!alsa.mmap && !alsa.write_buf && alsa.format != SND_PCM_FORMAT_S32_LE) {
		alsa.write_buf = malloc(alsa.period_size * BYTES_PER_FRAME);
		if (!alsa.write_buf) {
			LOG_ERROR("unable to malloc write_buf");
			return -1;
		}
	}

	// set params
	if ((err = snd_pcm_hw_params(*pcmp, hw_params)) < 0) {
		LOG_ERROR("unable to set hw params: %s", snd_strerror(err));
		return err;
	}

	// dump info
	if (loglevel == lSDEBUG) {
		static snd_output_t *debug_output;
		snd_output_stdio_attach(&debug_output, stderr, 0);
		snd_pcm_dump(*pcmp, debug_output);
	}

	// this indicates we have opened the device ok
	alsa.rate = sample_rate;

	return 0;
}

#endif // ALSA

#if PORTAUDIO

void list_devices(void) {
 	PaError err;
	int i;

	if ((err = Pa_Initialize()) != paNoError) {
		LOG_WARN("error initialising port audio: %s", Pa_GetErrorText(err));
		return;
	}

	printf("Output devices:\n");
	for (i = 0; i < Pa_GetDeviceCount(); ++i) {
		printf("  %i - %s\n", i, Pa_GetDeviceInfo(i)->name);
	}
	printf("\n");

 	if ((err = Pa_Terminate()) != paNoError) {
		LOG_WARN("error closing port audio: %s", Pa_GetErrorText(err));
	}
}

static int pa_device_id(const char *device) {
	int len = strlen(device);
	int i;

	if (!strncmp(device, "default", 7)) {
		return Pa_GetDefaultOutputDevice();
	}
	if (len >= 1 && len <= 2 && device[0] > '0' && device[0] <= '9') {
		return atoi(device);
	}

	for (i = 0; i < Pa_GetDeviceCount(); ++i) {
		if (!strncmp(Pa_GetDeviceInfo(i)->name, device, len)) {
			return i;
		}
	}

	return -1;
}

static int pa_callback(const void *pa_input, void *pa_output, unsigned long pa_frames_wanted, 
					   const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags, void *userData);

static bool test_open(const char *device, u32_t *max_rate) {
	PaStreamParameters outputParameters;
	PaError err;
	u32_t rates[] = { 192000, 176400, 96000, 88200, 48000, 44100, 0 };
	int device_id, i;

	if ((device_id = pa_device_id(device)) == -1) {
		LOG_INFO("device %s not found", device);
		return false;
	}

	outputParameters.device = device_id;
	outputParameters.channelCount = 2;
	outputParameters.sampleFormat = paInt32;
	outputParameters.suggestedLatency =
		output.latency ? (double)output.latency/(double)1000 : Pa_GetDeviceInfo(outputParameters.device)->defaultHighOutputLatency;
	outputParameters.hostApiSpecificStreamInfo = NULL;

	// check supported sample rates
	// Note this does not appear to work on OSX - it always returns paNoError...
	for (i = 0; rates[i]; ++i) {
		if (Pa_IsFormatSupported(NULL, &outputParameters, (double)rates[i]) == paNoError) {
			*max_rate = rates[i];
			break;
		}
	}

	if ((err = Pa_OpenStream(&pa.stream, NULL, &outputParameters, (double)*max_rate, paFramesPerBufferUnspecified,
							 paNoFlag, pa_callback, NULL)) != paNoError) {
		LOG_WARN("error opening stream: %s", Pa_GetErrorText(err));
		return false;
	}

	if ((err = Pa_CloseStream(pa.stream)) != paNoError) {
		LOG_WARN("error closing stream: %s", Pa_GetErrorText(err));
		return false;
	}

	pa.stream = NULL;

	return true;
}

static void pa_stream_finished(void *userdata) {
	if (running) {
		LOG_INFO("stream finished");
		LOCK;
		output.pa_reopen = true;
		wake_controller();
		UNLOCK;
	}
}

static thread_type probe_thread;
bool probe_thread_running = false;

static void *pa_probe() {
	// this is a hack to partially support hot plugging of devices
	// we rely on terminating and reinitalising PA to get an updated list of devices and use name for output.device
	while (probe_thread_running) {
		LOG_INFO("probing device %s", output.device);
		LOCK;
		Pa_Terminate();
		Pa_Initialize();
		pa.stream = NULL;
		if (pa_device_id(output.device) != -1) {
			LOG_INFO("found");
			probe_thread_running = false;
			_pa_open();
		} else {
			sleep(5);
		}
		UNLOCK;
	}

	return 0;
}

void _pa_open(void) {
	PaStreamParameters outputParameters;
	PaError err = paNoError;
	int device_id;

	if (pa.stream) {
		if ((err = Pa_CloseStream(pa.stream)) != paNoError) {
			LOG_WARN("error closing stream: %s", Pa_GetErrorText(err));
		}
	}

	if ((device_id = pa_device_id(output.device)) == -1) {
		LOG_INFO("device %s not found", output.device);
		err = 1;
	} else {

		outputParameters.device = device_id;
		outputParameters.channelCount = 2;
		outputParameters.sampleFormat = paInt32;
		outputParameters.suggestedLatency =
			output.latency ? (double)output.latency/(double)1000 : Pa_GetDeviceInfo(outputParameters.device)->defaultHighOutputLatency;
		outputParameters.hostApiSpecificStreamInfo = NULL;
		
#if OSX
		// enable pro mode which aims to avoid resampling if possible
		PaMacCoreStreamInfo macInfo;
		PaMacCore_SetupStreamInfo(&macInfo, paMacCorePro);
		outputParameters.hostApiSpecificStreamInfo = &macInfo;
#endif
	}

	if (!err && 
		(err = Pa_OpenStream(&pa.stream, NULL, &outputParameters, (double)output.current_sample_rate, paFramesPerBufferUnspecified,
							 paPrimeOutputBuffersUsingStreamCallback, pa_callback, NULL)) != paNoError) {
		LOG_WARN("error opening device %i - %s : %s", outputParameters.device, Pa_GetDeviceInfo(outputParameters.device)->name, 
				 Pa_GetErrorText(err));
	}

	if (!err) {
		LOG_INFO("opened device %i - %s at %u latency %u ms", outputParameters.device, Pa_GetDeviceInfo(outputParameters.device)->name,
				 (unsigned int)Pa_GetStreamInfo(pa.stream)->sampleRate, (unsigned int)(Pa_GetStreamInfo(pa.stream)->outputLatency * 1000));

		pa.rate = output.current_sample_rate;

		if ((err = Pa_SetStreamFinishedCallback(pa.stream, pa_stream_finished)) != paNoError) {
			LOG_WARN("error setting finish callback: %s", Pa_GetErrorText(err));
		}
	
		if ((err = Pa_StartStream(pa.stream)) != paNoError) {
			LOG_WARN("error starting stream: %s", Pa_GetErrorText(err));
		}
	}

	if (err && !probe_thread_running) {
		// create a thread to probe for the device
#if LINUX || OSX
		pthread_create(&probe_thread, NULL, pa_probe, NULL);
#endif
#if WIN
		probe_thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&pa_probe, NULL, 0, NULL);
#endif
		probe_thread_running = true;
		UNLOCK;
		return;
	}
}

#endif // PORTAUDIO


#if ALSA

// output thread for Alsa

static void *output_thread(void *arg) {
	snd_pcm_t *pcmp = NULL;
	bool start = true;
	bool output_off = false, probe_device = (arg != NULL);
	int err;

	while (running) {

		// disabled output - player is off
		while (output_off) {
			usleep(100000);
			LOCK;
			output_off = (output.state == OUTPUT_OFF);
			UNLOCK;
			if (!running) return 0;
		}

		// wait until device returns - to allow usb audio devices to be turned off
		if (probe_device) {
			while (!pcm_probe(output.device)) {
				LOG_DEBUG("waiting for device %s to return", output.device);
				sleep(5);
			}
			probe_device = false;
		}

		if (!pcmp || alsa.rate != output.current_sample_rate) {
			LOG_INFO("open output device: %s", output.device);
			if (!!alsa_open(&pcmp, output.device, output.current_sample_rate, output.buffer_time, output.period_count)) {
				sleep(5);
				continue;
			}
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

		if (start && alsa.mmap) {
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

		// restrict avail in writei mode as write_buf is restricted to period_size
		if (!alsa.mmap) {
			avail = min(avail, alsa.period_size);
		}

		// avoid spinning in cases where wait returns but no bytes available (seen with pulse audio)
		if (avail == 0) {
			LOG_SDEBUG("avail 0 - sleeping");
			usleep(10000);
			continue;
		}

		// turn off if requested
		if (output.state == OUTPUT_OFF) {
			UNLOCK;
			alsa_close(pcmp);
			pcmp = NULL;
			output_off = true;
			LOG_INFO("disabling output");
			continue;
		}

#endif // ALSA

#if PORTAUDIO
	static int pa_callback(const void *pa_input, void *pa_output, unsigned long pa_frames_wanted, 
						   const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags statusFlags, void *userData) {

		u8_t *optr = (u8_t *)pa_output;
		frames_t avail = pa_frames_wanted;
#endif
		frames_t frames, size;
		bool silence;

		s32_t cross_gain_in = 0, cross_gain_out = 0; s32_t *cross_ptr = NULL;

		LOCK;

		frames = _buf_used(outputbuf) / BYTES_PER_FRAME;
		silence = false;

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
		size = frames;

#if ALSA
		snd_pcm_sframes_t delay;
		snd_pcm_delay(pcmp, &delay);
		output.device_frames = delay;
#endif
#if PORTAUDIO
		output.device_frames = (unsigned)((time_info->outputBufferDacTime - Pa_GetStreamTime(pa.stream)) * output.current_sample_rate);
#endif
		output.updated = gettime_ms();

		while (size > 0) {
			frames_t out_frames;
			
			frames_t cont_frames = _buf_cont_read(outputbuf) / BYTES_PER_FRAME;

			s32_t gainL = output.current_replay_gain ? gain(output.gainL, output.current_replay_gain) : output.gainL;
			s32_t gainR = output.current_replay_gain ? gain(output.gainR, output.current_replay_gain) : output.gainR;
			
			if (output.track_start && !silence) {
				if (output.track_start == outputbuf->readp) {
					LOG_INFO("track start sample rate: %u replay_gain: %u", output.next_sample_rate, output.next_replay_gain);
					output.frames_played = 0;
					output.track_started = true;
					output.current_sample_rate = output.next_sample_rate;
					if (!output.fade == FADE_ACTIVE || !output.fade_mode == FADE_CROSSFADE) {
						output.current_replay_gain = output.next_replay_gain;
					}
					output.track_start = NULL;
					if (output.current_sample_rate != output.next_sample_rate) {
						output.current_sample_rate = output.next_sample_rate;
						break;
					}
					continue;
				} else if (output.track_start > outputbuf->readp) {
					// reduce cont_frames so we find the next track start at beginning of next chunk
					cont_frames = min(cont_frames, (output.track_start - outputbuf->readp) / BYTES_PER_FRAME);
				}
			}

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
								cross_ptr = (s32_t *)(output.fade_end + cur_f * BYTES_PER_FRAME);
							} else {
								LOG_INFO("unable to continue crossfade - too few samples");
								output.fade = FADE_NONE;
							}
						}
					}
				}
			}

 			out_frames = !silence ? min(size, cont_frames) : size;

#if ALSA			
			if (alsa.mmap || alsa.format != SND_PCM_FORMAT_S32_LE) {

				// in all alsa cases except SND_PCM_FORMAT_S32_LE non mmap we take this path:
				// - mmap: scale and pack to output format, write direct into mmap region
				// - non mmap: scale and pack into alsa.write_buf, which is the used with writei to send to alsa
				const snd_pcm_channel_area_t *areas;
				snd_pcm_uframes_t offset;
				snd_pcm_uframes_t alsa_frames = (snd_pcm_uframes_t)out_frames;

				if (alsa.mmap) {

					if ((err = snd_pcm_mmap_begin(pcmp, &areas, &offset, &alsa_frames)) < 0) {
						LOG_WARN("error from mmap_begin: %s", snd_strerror(err));
						break;
					}

					out_frames = (frames_t)alsa_frames;
				}

				// perform crossfade buffer copying here as we do not know the actual out_frames value until here
				if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && cross_ptr) {
					s32_t *ptr = (s32_t *)(void *)outputbuf->readp;
					frames_t count = out_frames * 2;
					while (count--) {
						if (cross_ptr > (s32_t *)outputbuf->wrap) {
							cross_ptr -= outputbuf->size / BYTES_PER_FRAME * 2;
						}
						*ptr = gain(cross_gain_out, *ptr) + gain(cross_gain_in, *cross_ptr);
						ptr++; cross_ptr++;
					}
				}

				void  *outputptr = alsa.mmap ? (areas[0].addr + (areas[0].first + offset * areas[0].step) / 8) : alsa.write_buf;
				s32_t *inputptr  = (s32_t *) (silence ? silencebuf : outputbuf->readp);
				frames_t cnt = out_frames;
				
				switch(alsa.format) {
				case SND_PCM_FORMAT_S16_LE:
					{
						u32_t *optr = (u32_t *)(void *)outputptr;
#if LITTLE_ENDIAN
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt--) {
								*(optr++) = (*(inputptr) & 0xffff0000) | (*(inputptr+1) >> 16 & 0x0000ffff);
								inputptr += 2;
							}
						} else {
							while (cnt--) {
								*(optr++) = (gain(gainL, *(inputptr)) & 0xffff0000) | (gain(gainR, *(inputptr+1)) >> 16 & 0x0000ffff);
								inputptr += 2;
							}
						}
#else
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt--) {
								s32_t lsample = *(inputptr++);
								s32_t rsample = *(inputptr++);
								*(optr++) = 
									(lsample & 0x00ff0000) << 8 | (lsample & 0xff000000) >> 8 |
									(rsample & 0x00ff0000) >> 8 | (rsample & 0xff000000) >> 24;
							}
						} else {
							while (cnt--) {
								s32_t lsample = gain(gainL, *(inputptr++));
								s32_t rsample = gain(gainR, *(inputptr++));
								*(optr++) = 
									(lsample & 0x00ff0000) << 8 | (lsample & 0xff000000) >> 8 |
									(rsample & 0x00ff0000) >> 8 | (rsample & 0xff000000) >> 24;
							}
						}
#endif
					}
					break;
				case SND_PCM_FORMAT_S24_LE: 
					{
						u32_t *optr = (u32_t *)(void *)outputptr;
#if LITTLE_ENDIAN
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
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt--) {
								s32_t lsample = *(inputptr++);
								s32_t rsample = *(inputptr++);
								*(optr++) = 
									(lsample & 0xff000000) >> 16 | (lsample & 0x00ff0000) | (lsample & 0x0000ff00 << 16);
								*(optr++) = 
									(rsample & 0xff000000) >> 16 | (rsample & 0x00ff0000) | (rsample & 0x0000ff00 << 16);
							}
						} else {
							while (cnt--) {
								s32_t lsample = gain(gainL, *(inputptr++));
								s32_t rsample = gain(gainR, *(inputptr++));
								*(optr++) = 
									(lsample & 0xff000000) >> 16 | (lsample & 0x00ff0000) | (lsample & 0x0000ff00 << 16);
								*(optr++) = 
									(rsample & 0xff000000) >> 16 | (rsample & 0x00ff0000) | (rsample & 0x0000ff00 << 16);
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
#if LITTLE_ENDIAN
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
#if LITTLE_ENDIAN
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
						u32_t *optr = (u32_t *)(void *)outputptr;
#if LITTLE_ENDIAN
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							memcpy(outputptr, inputptr, cnt * BYTES_PER_FRAME);
						} else {
							while (cnt--) {
								*(optr++) = gain(gainL, *(inputptr++));
								*(optr++) = gain(gainR, *(inputptr++));
							}
						}
#else
						if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
							while (cnt--) {
								s32_t lsample = *(inputptr++);
								s32_t rsample = *(inputptr++);
								*(optr++) = 
 									(lsample & 0xff000000) >> 24 | (lsample & 0x00ff0000) >> 8 |
									(lsample & 0x0000ff00) << 8  | (lsample & 0x000000ff) << 24;
								*(optr++) = 
									(rsample & 0xff000000) >> 24 | (rsample & 0x00ff0000) >> 8 |
									(rsample & 0x0000ff00) << 8  | (rsample & 0x000000ff) << 24;
							}
						} else {
							while (cnt--) {
								s32_t lsample = gain(gainL, *(inputptr++));
								s32_t rsample = gain(gainR, *(inputptr++));
								*(optr++) = 
 									(lsample & 0xff000000) >> 24 | (lsample & 0x00ff0000) >> 8 |
									(lsample & 0x0000ff00) << 8  | (lsample & 0x000000ff) << 24;
								*(optr++) = 
									(rsample & 0xff000000) >> 24 | (rsample & 0x00ff0000) >> 8 |
									(rsample & 0x0000ff00) << 8  | (rsample & 0x000000ff) << 24;
							}
						}
#endif
					}
					break;
				default:
					break;
				}
				
				if (alsa.mmap) {
					snd_pcm_sframes_t w = snd_pcm_mmap_commit(pcmp, offset, out_frames);
					if (w < 0 || w != out_frames) {
						LOG_WARN("mmap_commit error");
						break;
					}
				} else {
					snd_pcm_sframes_t w = snd_pcm_writei(pcmp, alsa.write_buf, out_frames);
					if (w < 0) {
						LOG_WARN("writei error: %d", w);
						break;
					} else {
						if (w != out_frames) {
							LOG_WARN("writei only wrote %u of %u", w, out_frames);
						}						
						out_frames = w;
					}
				}

			} else {

#endif // ALSA

#if PORTAUDIO
			if (1) {
#endif
				if (!silence) {

					if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && cross_ptr) {
						s32_t *ptr = (s32_t *)(void *)outputbuf->readp;
						frames_t count = out_frames * 2;
						while (count--) {
							if (cross_ptr > (s32_t *)outputbuf->wrap) {
								cross_ptr -= outputbuf->size / BYTES_PER_FRAME * 2;
							}
							*ptr = gain(cross_gain_out, *ptr) + gain(cross_gain_in, *cross_ptr);
							ptr++; cross_ptr++;
						}
					}

					if (gainL != FIXED_ONE || gainR!= FIXED_ONE) {
						unsigned count = out_frames;
						s32_t *ptrL = (s32_t *)(void *)outputbuf->readp;
						s32_t *ptrR = (s32_t *)(void *)outputbuf->readp + 1;
						while (count--) {
							*ptrL = gain(gainL, *ptrL);
							*ptrR = gain(gainR, *ptrR);
							ptrL += 2;
							ptrR += 2;
						}
					}
				}
#if ALSA
				// only used in S32_LE non mmap case, write the 32 samples straight with writei, no need for intermediate buffer
				snd_pcm_sframes_t w = snd_pcm_writei(pcmp, silence ? silencebuf : outputbuf->readp, out_frames);
				if (w < 0) {
					LOG_WARN("writei error: %d", w);
					break;
				} else {
					if (w != out_frames) {
						LOG_WARN("writei only wrote %u of %u", w, out_frames);
					}						
					out_frames = w;
				}
#endif
#if PORTAUDIO
				if (!silence) {
					memcpy(optr, outputbuf->readp, out_frames * BYTES_PER_FRAME);
				} else {
					memset(optr, 0, out_frames * BYTES_PER_FRAME);
				}

				optr += out_frames * BYTES_PER_FRAME;
#endif
			}

			size -= out_frames;
			
			if (!silence) {
				_buf_inc_readp(outputbuf, out_frames * BYTES_PER_FRAME);
				output.frames_played += out_frames;
			}
		}
			
		LOG_SDEBUG("wrote %u frames", frames);

#if ALSA	
		UNLOCK;
	}

	return 0;
}
#endif

#if PORTAUDIO
		if (frames < pa_frames_wanted) {
			LOG_SDEBUG("pad with silience");
	   		memset(optr, 0, (pa_frames_wanted - frames) * BYTES_PER_FRAME);
		}

		if (pa.rate != output.current_sample_rate) {
			UNLOCK;
			return paComplete;
		} else {
			UNLOCK;
			return paContinue;
		}
	}
#endif

void _checkfade(bool start) {
	frames_t bytes;

	LOG_INFO("fade mode: %u duration: %u %s", output.fade_mode, output.fade_secs, start ? "track-start" : "track-end");

	bytes = output.next_sample_rate * BYTES_PER_FRAME * output.fade_secs;
	if (output.fade_mode == FADE_INOUT) {
		bytes /= 2;
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
		}
	}
}

#if ALSA
static pthread_t thread;
void output_init(log_level level, const char *device, unsigned output_buf_size, unsigned buffer_time, unsigned period_count, bool mmap, unsigned max_rate) {
#endif
#if PORTAUDIO
void output_init(log_level level, const char *device, unsigned output_buf_size, unsigned latency, unsigned max_rate) {
	PaError err;
#endif
	loglevel = level;

	LOG_INFO("init output");

	output_buf_size = output_buf_size - (output_buf_size % BYTES_PER_FRAME);
	LOG_DEBUG("outputbuf size: %u", output_buf_size);

	buf_init(outputbuf, output_buf_size);
	if (!outputbuf->buf) {
		LOG_ERROR("unable to malloc buffer");
		exit(0);
	}

	LOCK;

	output.state = STOPPED;
	output.current_sample_rate = 44100;
	output.device = device;
	output.fade = FADE_INACTIVE;

#if ALSA
	alsa.mmap = mmap;
	alsa.write_buf = NULL;
	output.buffer_time = buffer_time;
	output.period_count = period_count;

	LOG_INFO("requested buffer_time: %u period_count: %u mmap: %u", output.buffer_time, output.period_count, alsa.mmap);
#endif

#if PORTAUDIO
	output.latency = latency;
	pa.stream = NULL;

 	if ((err = Pa_Initialize()) != paNoError) {
		LOG_WARN("error initialising port audio: %s", Pa_GetErrorText(err));
		return;
	}
#endif

	if (!max_rate) {
		if (!test_open(output.device, &output.max_sample_rate)) {
			LOG_ERROR("unable to open output device");
			exit(0);
		}
	} else {
		output.max_sample_rate = max_rate;
	}

	LOG_INFO("output: %s maxrate: %u", output.device, output.max_sample_rate);

#if ALSA
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, STREAM_THREAD_STACK_SIZE);
	pthread_create(&thread, &attr, output_thread, max_rate ? "probe" : NULL);
	pthread_attr_destroy(&attr);

	// try to set this thread to real-time scheduler class, likely only works as root
	struct sched_param param;
	param.sched_priority = 40;
	if (pthread_setschedparam(thread, SCHED_FIFO, &param) != 0) {
		LOG_DEBUG("unable to set output sched fifo: %s", strerror(errno));
	}
#endif

#if PORTAUDIO
	_pa_open();
#endif

	UNLOCK;
}

void output_flush(void) {
	LOG_INFO("flush output buffer");
	buf_flush(outputbuf);
	LOCK;
	output.fade = FADE_INACTIVE;
	UNLOCK;
}

void output_close(void) {
#if PORTAUDIO
	 PaError err;
#endif

	LOG_INFO("close output");

	LOCK;

	running = false;

#if ALSA
	UNLOCK;
	pthread_join(thread,NULL);
	if (alsa.write_buf) free(alsa.write_buf);
#endif

#if PORTAUDIO
	probe_thread_running = false;

	if (pa.stream) {
		if ((err = Pa_AbortStream(pa.stream)) != paNoError) {
			LOG_WARN("error closing stream: %s", Pa_GetErrorText(err));
		}
	}
 	if ((err = Pa_Terminate()) != paNoError) {
		LOG_WARN("error closing port audio: %s", Pa_GetErrorText(err));
	}
	UNLOCK;
#endif

	buf_destroy(outputbuf);
}

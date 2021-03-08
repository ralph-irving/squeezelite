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

// upsampling using libsoxr - only included if RESAMPLE set

#include "squeezelite.h"

#if RESAMPLE

#include <math.h>
#include <soxr.h>

extern log_level loglevel;

struct soxr {
	soxr_t resampler;
	size_t old_clips;
	unsigned long q_recipe;
	unsigned long q_flags;
	double q_precision;         /* Conversion precision (in bits).           20    */
	double q_phase_response;    /* 0=minimum, ... 50=linear, ... 100=maximum 50    */
	double q_passband_end;      /* 0dB pt. bandwidth to preserve; nyquist=1  0.913 */
	double q_stopband_begin;    /* Aliasing/imaging control; > passband_end   1    */
	double scale;
	bool max_rate;
	bool exception;
#if !LINKALL
	// soxr symbols to be dynamically loaded
	soxr_io_spec_t (* soxr_io_spec)(soxr_datatype_t itype, soxr_datatype_t otype);
	soxr_quality_spec_t (* soxr_quality_spec)(unsigned long recipe, unsigned long flags);
	soxr_t (* soxr_create)(double, double, unsigned, soxr_error_t *, 
						   soxr_io_spec_t const *, soxr_quality_spec_t const *, soxr_runtime_spec_t const *);
	void (* soxr_delete)(soxr_t);
	soxr_error_t (* soxr_process)(soxr_t, soxr_in_t, size_t, size_t *, soxr_out_t, size_t olen, size_t *);
	size_t *(* soxr_num_clips)(soxr_t);
#if RESAMPLE_MP
	soxr_runtime_spec_t (* soxr_runtime_spec)(unsigned num_threads);
#endif
	// soxr_strerror is a macro so not included here
#endif
};

static struct soxr *r;

#if LINKALL
#define SOXR(h, fn, ...) (soxr_ ## fn)(__VA_ARGS__)
#else
#define SOXR(h, fn, ...) (h)->soxr_##fn(__VA_ARGS__)
#endif


void resample_samples(struct processstate *process) {
	size_t idone, odone;
	size_t clip_cnt;
	
	soxr_error_t error =
		SOXR(r, process, r->resampler, process->inbuf, process->in_frames, &idone, process->outbuf, process->max_out_frames, &odone);
	if (error) {
		LOG_INFO("soxr_process error: %s", soxr_strerror(error));
		return;
	}
	
	if (idone != process->in_frames) {
		// should not get here if buffers are big enough...
		LOG_ERROR("should not get here - partial sox process: %u of %u processed %u of %u out",
				  (unsigned)idone, process->in_frames, (unsigned)odone, process->max_out_frames);
	}
	
	process->out_frames = odone;
	process->total_in  += idone;
	process->total_out += odone;
	
	clip_cnt = *(SOXR(r, num_clips, r->resampler));
	if (clip_cnt - r->old_clips) {
		LOG_SDEBUG("resampling clips: %u", (unsigned)(clip_cnt - r->old_clips));
		r->old_clips = clip_cnt;
	}
}

bool resample_drain(struct processstate *process) {
	size_t odone;
	size_t clip_cnt;
		
	soxr_error_t error = SOXR(r, process, r->resampler, NULL, 0, NULL, process->outbuf, process->max_out_frames, &odone);
	if (error) {
		LOG_INFO("soxr_process error: %s", soxr_strerror(error));
		return true;
	}
	
	process->out_frames = odone;
	process->total_out += odone;
	
	clip_cnt = *(SOXR(r, num_clips, r->resampler));
	if (clip_cnt - r->old_clips) {
		LOG_DEBUG("resampling clips: %u", (unsigned)(clip_cnt - r->old_clips));
		r->old_clips = clip_cnt;
	}
	
	if (odone == 0) {

		LOG_INFO("resample track complete - total track clips: %u", r->old_clips);

		SOXR(r, delete, r->resampler);
		r->resampler = NULL;

		return true;

	} else {

		return false;
	}
}

bool resample_newstream(struct processstate *process, unsigned raw_sample_rate, unsigned supported_rates[]) {
	unsigned outrate = 0;
	int i;

	if (r->exception) {
		// find direct match - avoid resampling
		for (i = 0; supported_rates[i]; i++) {
			if (raw_sample_rate == supported_rates[i]) {
				outrate = raw_sample_rate;
				break;
			}
		}
		// else find next highest sync sample rate
		while (!outrate && i >= 0) {
			if (supported_rates[i] > raw_sample_rate && supported_rates[i] % raw_sample_rate == 0) {
				outrate = supported_rates[i];
				break;
			}
			i--;
		}
	}

	if (!outrate) {
		if (r->max_rate) {
			// resample to max rate for device
			outrate = supported_rates[0];
		} else {
			// resample to max sync sample rate
			for (i = 0; supported_rates[i]; i++) {
				if (supported_rates[i] % raw_sample_rate == 0 || raw_sample_rate % supported_rates[i] == 0) {
					outrate = supported_rates[i];
					break;
				}
			}
		}
		if (!outrate) {
			outrate = supported_rates[0];
		}
	}

	process->in_sample_rate = raw_sample_rate;
	process->out_sample_rate = outrate;

	if (r->resampler) {
		SOXR(r, delete, r->resampler);
		r->resampler = NULL;
	}

	if (raw_sample_rate != outrate) {

		soxr_io_spec_t io_spec;
		soxr_quality_spec_t q_spec;
		soxr_error_t error;
#if RESAMPLE_MP
		soxr_runtime_spec_t r_spec;
#endif

		LOG_INFO("resampling from %u -> %u", raw_sample_rate, outrate);

		io_spec = SOXR(r, io_spec, SOXR_INT32_I, SOXR_INT32_I);
		io_spec.scale = r->scale;

		q_spec = SOXR(r, quality_spec, r->q_recipe, r->q_flags);
		if (r->q_precision > 0) {
			q_spec.precision = r->q_precision;
		}
		if (r->q_passband_end > 0) {
			q_spec.passband_end = r->q_passband_end;
		}
		if (r->q_stopband_begin > 0) {
			q_spec.stopband_begin = r->q_stopband_begin;
		}
		if (r->q_phase_response > -1) {
			q_spec.phase_response = r->q_phase_response;
		}

#if RESAMPLE_MP
		r_spec = SOXR(r, runtime_spec, 0); // make use of libsoxr OpenMP support allowing parallel execution if multiple cores
#endif		   

		LOG_DEBUG("resampling with soxr_quality_spec_t[precision: %03.1f, passband_end: %03.6f, stopband_begin: %03.6f, "
				  "phase_response: %03.1f, flags: 0x%02x], soxr_io_spec_t[scale: %03.2f]", q_spec.precision,
				  q_spec.passband_end, q_spec.stopband_begin, q_spec.phase_response, q_spec.flags, io_spec.scale);

#if RESAMPLE_MP
		r->resampler = SOXR(r, create, raw_sample_rate, outrate, 2, &error, &io_spec, &q_spec, &r_spec);
#else
		r->resampler = SOXR(r, create, raw_sample_rate, outrate, 2, &error, &io_spec, &q_spec, NULL);
#endif

		if (error) {
			LOG_INFO("soxr_create error: %s", soxr_strerror(error));
			return false;
		}

		r->old_clips = 0;
		return true;

	} else {

		LOG_INFO("disable resampling - rates match");
		return false;
	}
}

void resample_flush(void) {
	if (r->resampler) {
		SOXR(r, delete, r->resampler);
		r->resampler = NULL;
	}
}

static bool load_soxr(void) {
#if !LINKALL
	void *handle = dlopen(LIBSOXR, RTLD_NOW);
	char *err;

	if (!handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	r->soxr_io_spec = dlsym(handle, "soxr_io_spec");
	r->soxr_quality_spec = dlsym(handle, "soxr_quality_spec");
	r->soxr_create = dlsym(handle, "soxr_create");
	r->soxr_delete = dlsym(handle, "soxr_delete");
	r->soxr_process = dlsym(handle, "soxr_process");
	r->soxr_num_clips = dlsym(handle, "soxr_num_clips");
#if RESAMPLE_MP
	r->soxr_runtime_spec = dlsym(handle, "soxr_runtime_spec");
#endif

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded "LIBSOXR);
#endif

	return true;
}

bool resample_init(char *opt) {
	char *recipe = NULL, *flags = NULL;
	char *atten = NULL;
	char *precision = NULL, *passband_end = NULL, *stopband_begin = NULL, *phase_response = NULL;

	r = malloc(sizeof(struct soxr));
	if (!r) {
		LOG_WARN("resampling disabled");
		return false;
	}

	r->resampler = NULL;
	r->old_clips = 0;
	r->max_rate = false;
	r->exception = false;

	if (!load_soxr()) {
		LOG_WARN("resampling disabled");
		return false;
	}

	if (opt) {
		recipe = next_param(opt, ':');
		flags = next_param(NULL, ':');
		atten = next_param(NULL, ':');
		precision = next_param(NULL, ':');
		passband_end = next_param(NULL, ':');
		stopband_begin = next_param(NULL, ':');
		phase_response = next_param(NULL, ':');
	}

	// default to HQ (20 bit) if not user specified
	r->q_recipe = SOXR_HQ;
	r->q_flags = 0;
	// default to 1db of attenuation if not user specified
	r->scale = pow(10, -1.0 / 20);
	// override recipe derived values with user specified values
	r->q_precision = 0;
	r->q_passband_end = 0;
	r->q_stopband_begin = 0;
	r->q_phase_response = -1;

	if (recipe && recipe[0] != '\0') {
		if (strchr(recipe, 'v')) r->q_recipe = SOXR_VHQ;
		if (strchr(recipe, 'h')) r->q_recipe = SOXR_HQ;
		if (strchr(recipe, 'm')) r->q_recipe = SOXR_MQ;
		if (strchr(recipe, 'l')) r->q_recipe = SOXR_LQ;
		if (strchr(recipe, 'q')) r->q_recipe = SOXR_QQ;
		if (strchr(recipe, 'L')) r->q_recipe |= SOXR_LINEAR_PHASE;
		if (strchr(recipe, 'I')) r->q_recipe |= SOXR_INTERMEDIATE_PHASE;
		if (strchr(recipe, 'M')) r->q_recipe |= SOXR_MINIMUM_PHASE;
		if (strchr(recipe, 's')) r->q_recipe |= SOXR_STEEP_FILTER;
		// X = async resampling to max_rate
		if (strchr(recipe, 'X')) r->max_rate = true;
		// E = exception, only resample if native rate is not supported
		if (strchr(recipe, 'E')) r->exception = true;
	}

	if (flags) {
		r->q_flags = strtoul(flags, 0, 16);
	}

	if (atten) {
		double scale = pow(10, -atof(atten) / 20);
		if (scale > 0 && scale <= 1.0) {
			r->scale = scale;
		}
	}

	if (precision) {
		r->q_precision = atof(precision);
	}

	if (passband_end) {
		r->q_passband_end = atof(passband_end) / 100;
	}

	if (stopband_begin) {
		r->q_stopband_begin = atof(stopband_begin) / 100;
	}

	if (phase_response) {
		r->q_phase_response = atof(phase_response);
	}

	LOG_INFO("resampling %s recipe: 0x%02x, flags: 0x%02x, scale: %03.2f, precision: %03.1f, passband_end: %03.5f, stopband_begin: %03.5f, phase_response: %03.1f",
			r->max_rate ? "async" : "sync",
			r->q_recipe, r->q_flags, r->scale, r->q_precision, r->q_passband_end, r->q_stopband_begin, r->q_phase_response);

	return true;
}

#endif // #if RESAMPLE

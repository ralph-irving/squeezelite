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

#include "squeezelite.h"

#include <FLAC/stream_decoder.h>

struct flac {
	FLAC__StreamDecoder *decoder;
#if !LINKALL
	// FLAC symbols to be dynamically loaded
	const char **FLAC__StreamDecoderErrorStatusString;
	const char **FLAC__StreamDecoderStateString;
	FLAC__StreamDecoder * (* FLAC__stream_decoder_new)(void);
	FLAC__bool (* FLAC__stream_decoder_reset)(FLAC__StreamDecoder *decoder);
	void (* FLAC__stream_decoder_delete)(FLAC__StreamDecoder *decoder);
	FLAC__StreamDecoderInitStatus (* FLAC__stream_decoder_init_stream)(
		FLAC__StreamDecoder *decoder,
		FLAC__StreamDecoderReadCallback read_callback,
		FLAC__StreamDecoderSeekCallback seek_callback,
		FLAC__StreamDecoderTellCallback tell_callback,
		FLAC__StreamDecoderLengthCallback length_callback,
		FLAC__StreamDecoderEofCallback eof_callback,
		FLAC__StreamDecoderWriteCallback write_callback,
		FLAC__StreamDecoderMetadataCallback metadata_callback,
		FLAC__StreamDecoderErrorCallback error_callback,
		void *client_data
	);
	FLAC__bool (* FLAC__stream_decoder_process_single)(FLAC__StreamDecoder *decoder);
	FLAC__StreamDecoderState (* FLAC__stream_decoder_get_state)(const FLAC__StreamDecoder *decoder);
#endif
};

static struct flac *f;

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;
extern struct processstate process;

#define LOCK_S   mutex_lock(streambuf->mutex)
#define UNLOCK_S mutex_unlock(streambuf->mutex)
#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)
#if PROCESS
#define LOCK_O_direct   if (decode.direct) mutex_lock(outputbuf->mutex)
#define UNLOCK_O_direct if (decode.direct) mutex_unlock(outputbuf->mutex)
#define IF_DIRECT(x)    if (decode.direct) { x }
#define IF_PROCESS(x)   if (!decode.direct) { x }
#else
#define LOCK_O_direct   mutex_lock(outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#endif

#if LINKALL
#define FLAC(h, fn, ...) (FLAC__ ## fn)(__VA_ARGS__)
#define FLAC_A(h, a)     (FLAC__ ## a)
#else
#define FLAC(h, fn, ...) (h)->FLAC__##fn(__VA_ARGS__)
#define FLAC_A(h, a)     (h)->FLAC__ ## a
#endif

static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *want, void *client_data) {
	size_t bytes;
	bool end;

	LOCK_S;
	bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	bytes = min(bytes, *want);
	end = (stream.state <= DISCONNECT && bytes == 0);

	memcpy(buffer, streambuf->readp, bytes);
	_buf_inc_readp(streambuf, bytes);
	UNLOCK_S;

	*want = bytes;

	return end ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
											   const FLAC__int32 *const buffer[], void *client_data) {

	size_t frames = frame->header.blocksize;
	unsigned bits_per_sample = frame->header.bits_per_sample;
	unsigned channels = frame->header.channels;

	FLAC__int32 *lptr = (FLAC__int32 *)buffer[0];
	FLAC__int32 *rptr = (FLAC__int32 *)buffer[channels > 1 ? 1 : 0];
	
	if (decode.new_stream) {
		LOCK_O;
		LOG_SQ_INFO("setting track_start");
		output.track_start = outputbuf->writep;
		decode.new_stream = false;

#if DSD
		if (output.has_dop && bits_per_sample == 24 && is_flac_dop((u32_t *)lptr, (u32_t *)rptr, frames)) {
			LOG_SQ_INFO("file contains DOP");
			output.next_dop = true;
			output.next_sample_rate = frame->header.sample_rate;
			output.fade = FADE_INACTIVE;
		} else {
			output.next_sample_rate = decode_newstream(frame->header.sample_rate, output.supported_rates);
			output.next_dop = false;
			if (output.fade_mode) _checkfade(true);
		}
#else
		output.next_sample_rate = decode_newstream(frame->header.sample_rate, output.supported_rates);
		if (output.fade_mode) _checkfade(true);
#endif

		UNLOCK_O;
	}

	LOCK_O_direct;

	while (frames > 0) {
		frames_t f;
		frames_t count;
		s32_t *optr;

		IF_DIRECT( 
			optr = (s32_t *)outputbuf->writep; 
			f = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME; 
		);
		IF_PROCESS(
			optr = (s32_t *)process.inbuf;
			f = process.max_in_frames;
		);

		f = min(f, frames);

		count = f;

		if (bits_per_sample == 8) {
			while (count--) {
				*optr++ = *lptr++ << 24;
				*optr++ = *rptr++ << 24;
			}
		} else if (bits_per_sample == 16) {
			while (count--) {
				*optr++ = *lptr++ << 16;
				*optr++ = *rptr++ << 16;
			}
		} else if (bits_per_sample == 24) {
			while (count--) {
				*optr++ = *lptr++ << 8;
				*optr++ = *rptr++ << 8;
			}
		} else if (bits_per_sample == 32) {
			while (count--) {
				*optr++ = *lptr++;
				*optr++ = *rptr++;
			}
		} else {
			LOG_SQ_ERROR("unsupported bits per sample: %u", bits_per_sample);
		}

		frames -= f;

		IF_DIRECT(
			_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
		);
		IF_PROCESS(
			process.in_frames = f;
			if (frames) LOG_SQ_ERROR("unhandled case");
		);
	}

	UNLOCK_O_direct;

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void error_cb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data) {
	LOG_SQ_INFO("flac error: %s", FLAC_A(f, StreamDecoderErrorStatusString)[status]);
}

static void flac_open(u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness) {
	if (f->decoder) {
		FLAC(f, stream_decoder_reset, f->decoder);
	} else {
		f->decoder = FLAC(f, stream_decoder_new);
	}
	FLAC(f, stream_decoder_init_stream, f->decoder, &read_cb, NULL, NULL, NULL, NULL, &write_cb, NULL, &error_cb, NULL);
}

static void flac_close(void) {
	FLAC(f, stream_decoder_delete, f->decoder);
	f->decoder = NULL;
}

static decode_state flac_decode(void) {
	bool ok = FLAC(f, stream_decoder_process_single, f->decoder);
	FLAC__StreamDecoderState state = FLAC(f, stream_decoder_get_state, f->decoder);
	
	if (!ok && state != FLAC__STREAM_DECODER_END_OF_STREAM) {
		LOG_SQ_INFO("flac error: %s", FLAC_A(f, StreamDecoderStateString)[state]);
	};
	
	if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
		return DECODE_COMPLETE;
	} else if (state > FLAC__STREAM_DECODER_END_OF_STREAM) {
		return DECODE_ERROR;
	} else {
		return DECODE_RUNNING;
	}
}

static bool load_flac() {
#if !LINKALL
	void *handle = dlopen(LIBFLAC, RTLD_NOW);
	char *err;

	if (!handle) {
		LOG_SQ_INFO("dlerror: %s", dlerror());
		return false;
	}

	f->FLAC__StreamDecoderErrorStatusString = dlsym(handle, "FLAC__StreamDecoderErrorStatusString");
	f->FLAC__StreamDecoderStateString = dlsym(handle, "FLAC__StreamDecoderStateString");
	f->FLAC__stream_decoder_new = dlsym(handle, "FLAC__stream_decoder_new");
	f->FLAC__stream_decoder_reset = dlsym(handle, "FLAC__stream_decoder_reset");
	f->FLAC__stream_decoder_delete = dlsym(handle, "FLAC__stream_decoder_delete");
	f->FLAC__stream_decoder_init_stream = dlsym(handle, "FLAC__stream_decoder_init_stream");
	f->FLAC__stream_decoder_process_single = dlsym(handle, "FLAC__stream_decoder_process_single");
	f->FLAC__stream_decoder_get_state = dlsym(handle, "FLAC__stream_decoder_get_state");

	if ((err = dlerror()) != NULL) {
		LOG_SQ_INFO("dlerror: %s", err);		
		return false;
	}

	LOG_SQ_INFO("loaded "LIBFLAC);
#endif

	return true;
}

struct codec *register_flac(void) {
	static struct codec ret = { 
		'f',          // id
		"flc",        // types
		8192,         // min read
		102400,       // min space
		flac_open,    // open
		flac_close,   // close
		flac_decode,  // decode
	};

	f = malloc(sizeof(struct flac));
	if (!f) {
		return NULL;
	}

	f->decoder = NULL;

	if (!load_flac()) {
		return NULL;
	}

	LOG_SQ_INFO("using flac to decode flc");
	return &ret;
}

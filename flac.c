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
#include <dlfcn.h>

// FLAC symbols to be dynamically loaded
FLAC_API const char **f__StreamDecoderErrorStatusString;
FLAC_API const char **f__StreamDecoderStateString;
FLAC_API FLAC__StreamDecoder * (* f__stream_decoder_new)(void);
FLAC_API FLAC__bool (* f__stream_decoder_reset)(FLAC__StreamDecoder *decoder);
FLAC_API void (* f__stream_decoder_delete)(FLAC__StreamDecoder *decoder);
FLAC_API FLAC__StreamDecoderInitStatus (* f__stream_decoder_init_stream)(
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
FLAC_API FLAC__bool (* f__stream_decoder_process_single)(FLAC__StreamDecoder *decoder);
FLAC_API FLAC__StreamDecoderState (* f__stream_decoder_get_state)(const FLAC__StreamDecoder *decoder);
// end of FLAC symbols

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;

struct decodestate decode;

#define LOCK_S   pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK_S pthread_mutex_unlock(&streambuf->mutex)
#define LOCK_O   pthread_mutex_lock(&outputbuf->mutex)
#define UNLOCK_O pthread_mutex_unlock(&outputbuf->mutex)

typedef u_int32_t frames_t;


static FLAC__StreamDecoder *decoder = NULL;

static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *want, void *client_data) {
	LOCK_S;
	size_t bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	bool end = (stream.state <= DISCONNECT && bytes == 0);
	bytes = min(bytes, *want);

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
	
	LOCK_O;

	if (decode.new_stream) {
		LOG_INFO("setting track_start");
		output.next_sample_rate = frame->header.sample_rate; 
		output.track_start = outputbuf->writep;
		decode.new_stream = false;
	}

	while (frames > 0) {
		frames_t f = _buf_cont_write(outputbuf) / BYTES_PER_FRAME;
		f = min(f, frames);

		frames_t count = f;
		u32_t *optr = (u32_t *)outputbuf->writep;

		if (bits_per_sample == 16) {
			while (count--) {
				*optr++ = *lptr++ << 16;
				*optr++ = *rptr++ << 16;
			}
		} else if (bits_per_sample == 24) {
			while (count--) {
				*optr++ = *lptr++ << 8;
				*optr++ = *rptr++ << 8;
			}
		} else {
			LOG_ERROR("unsupported bits per sample: %u", bits_per_sample);
		}

		frames -= f;
		_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
	}

	UNLOCK_O;

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void error_cb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data) {
	LOG_INFO("flac error: %s", f__StreamDecoderErrorStatusString[status]);
}

static void flac_open(u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness) {
	if (decoder) {
		f__stream_decoder_reset(decoder);
	} else {
		decoder = f__stream_decoder_new();
	}
	f__stream_decoder_init_stream(decoder, &read_cb, NULL, NULL, NULL, NULL, &write_cb, NULL, &error_cb, NULL);
}

static void flac_close(void) {
	f__stream_decoder_delete(decoder);
	decoder = NULL;
}

static void flac_decode(void) {
	if (!f__stream_decoder_process_single(decoder)) {
		FLAC__StreamDecoderState state = f__stream_decoder_get_state(decoder);
		LOG_ERROR("flac error: %s", f__StreamDecoderStateString[state]);
	};
}

static bool load_flac() {
	void *handle = dlopen("libFLAC.so", RTLD_NOW);
	if (!handle) {
		LOG_WARN("dlerror: %s", dlerror());
		return false;
	}
	
	f__StreamDecoderErrorStatusString = dlsym(handle, "FLAC__StreamDecoderErrorStatusString");
	f__StreamDecoderStateString = dlsym(handle, "FLAC__StreamDecoderStateString");

	f__stream_decoder_new = dlsym(handle, "FLAC__stream_decoder_new");
	f__stream_decoder_reset = dlsym(handle, "FLAC__stream_decoder_reset");
	f__stream_decoder_delete = dlsym(handle, "FLAC__stream_decoder_delete");

	f__stream_decoder_init_stream = dlsym(handle, "FLAC__stream_decoder_init_stream");
	f__stream_decoder_process_single = dlsym(handle, "FLAC__stream_decoder_process_single");
	f__stream_decoder_get_state = dlsym(handle, "FLAC__stream_decoder_get_state");

	char *err;
	if ((err = dlerror()) != NULL) {
		LOG_WARN("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded libFLAC");
	return true;
}

struct codec *register_flac(void) {
	static struct codec ret = { 
		.id    = 'f',
		.types = "flc",
		.open  = flac_open,
		.close = flac_close,
		.decode= flac_decode,
		.min_space = 102400,
		.min_read_bytes = 8192,
	};

	if (!load_flac()) {
		return NULL;
	}

	return &ret;
}

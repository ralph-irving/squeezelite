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

#include <neaacdec.h>
#include <dlfcn.h>

#define LIBFAAD "libfaad.so.2"

#define WRAPBUF_LEN 2048

struct faad {
	NeAACDecHandle hAac;
	// faad symbols to be dynamically loaded
	unsigned long NEAACDECAPI (* NeAACDecGetCapabilities)(void);
	NeAACDecConfigurationPtr NEAACDECAPI (* NeAACDecGetCurrentConfiguration)(NeAACDecHandle);
	unsigned char NEAACDECAPI (* NeAACDecSetConfiguration)(NeAACDecHandle, NeAACDecConfigurationPtr);
	NeAACDecHandle NEAACDECAPI (* NeAACDecOpen)(void);
	void NEAACDECAPI (* NeAACDecClose)(NeAACDecHandle);
	long NEAACDECAPI (* NeAACDecInit)(NeAACDecHandle, unsigned char *, unsigned long, unsigned long *, unsigned char *);
	void* NEAACDECAPI (* NeAACDecDecode)(NeAACDecHandle, NeAACDecFrameInfo *, unsigned char *, unsigned long);
	char* NEAACDECAPI (* NeAACDecGetErrorMessage)(unsigned char);
};

static struct faad *a;

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;

#define LOCK_S   pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK_S pthread_mutex_unlock(&streambuf->mutex)
#define LOCK_O   pthread_mutex_lock(&outputbuf->mutex)
#define UNLOCK_O pthread_mutex_unlock(&outputbuf->mutex)

typedef u_int32_t frames_t;

static void faad_decode(void) {
	LOCK_S;
	size_t bytes_total = _buf_used(streambuf);
	size_t bytes_wrap  = min(bytes_total, _buf_cont_read(streambuf));

	if (decode.new_stream) {

		// find adts sync at start of header
		while (bytes_wrap >= 2 && (*(streambuf->readp) != 0xFF || (*(streambuf->readp + 1) & 0xF6) != 0xF0)) {
			_buf_inc_readp(streambuf, 1);
			bytes_total--;
			bytes_wrap--;
		}

		unsigned char channels;
		unsigned long samplerate;

		long n = a->NeAACDecInit(a->hAac, streambuf->readp, bytes_wrap, &samplerate, &channels);
		if (n < 0) {
			LOG_WARN("error initialising - ending stream");
			UNLOCK_S;
			LOCK_O;
			decode.state = DECODE_ERROR;
			UNLOCK_O;
			return;
		}

		_buf_inc_readp(streambuf, n);
		bytes_total = _buf_used(streambuf);
		bytes_wrap  = min(bytes_total, _buf_cont_read(streambuf));

		LOG_INFO("setting track_start");
		output.next_sample_rate = samplerate; 
		output.track_start = outputbuf->writep;
		decode.new_stream = false;
	}

	NeAACDecFrameInfo info;

	s32_t *iptr;

	if (bytes_wrap < WRAPBUF_LEN && bytes_total > WRAPBUF_LEN) {

		// make a local copy of frames which may have wrapped round the end of streambuf
		u8_t buf[WRAPBUF_LEN];
		memcpy(buf, streambuf->readp, bytes_wrap);
		memcpy(buf + bytes_wrap, streambuf->buf, WRAPBUF_LEN - bytes_wrap);

		iptr = a->NeAACDecDecode(a->hAac, &info, buf, WRAPBUF_LEN);

	} else {

		iptr = a->NeAACDecDecode(a->hAac, &info, streambuf->readp, bytes_wrap);
	}

	_buf_inc_readp(streambuf, info.bytesconsumed);

	UNLOCK_S;

	if (info.error) {
		LOG_WARN("error: %u %s", info.error, a->NeAACDecGetErrorMessage(info.error));
	}

	if (!info.samples) {
		return;
	}

	LOCK_O;

	size_t frames = info.samples / info.channels;
	while (frames > 0) {
		frames_t f = _buf_cont_write(outputbuf) / BYTES_PER_FRAME;
		f = min(f, frames);

		frames_t count = f;
		s32_t *optr = (s32_t *)outputbuf->writep;

		if (info.channels == 2) {
			while (count--) {
				*optr++ = *iptr++;
				*optr++ = *iptr++;
			}
		} else if (info.channels == 1) {
			while (count--) {
				*optr++ = *iptr;
				*optr++ = *iptr++;
			}
		} else {
			LOG_WARN("unsupported number of channels");
		}

		frames -= f;
		_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
	}

	UNLOCK_O;

	LOG_SDEBUG("wrote %u frames", info.samples);
}

static void faad_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	if (size == '2') {
		LOG_INFO("opening adts stream");
	} else {
		LOG_ERROR("aac stream type %c not supported", size);
		LOCK_O;
		decode.state = DECODE_ERROR;
		UNLOCK_O;
		return;
	}

	if (a->hAac) {
		a->NeAACDecClose(a->hAac);
	}
	a->hAac = a->NeAACDecOpen();

	NeAACDecConfigurationPtr conf = a->NeAACDecGetCurrentConfiguration(a->hAac);

	conf->outputFormat = FAAD_FMT_32BIT;
	conf->downMatrix = 1;

	if (!a->NeAACDecSetConfiguration(a->hAac, conf)) {
		LOG_WARN("error setting config");
	};
}

static void faad_close(void) {
	a->NeAACDecClose(a->hAac);
	a->hAac = NULL;
}

static bool load_faad() {
	void *handle = dlopen(LIBFAAD, RTLD_NOW);
	if (!handle) {
		LOG_WARN("dlerror: %s", dlerror());
		return false;
	}

	a = malloc(sizeof(struct faad));

	a->hAac = NULL;
	a->NeAACDecGetCapabilities = dlsym(handle, "NeAACDecGetCapabilities");
	a->NeAACDecGetCurrentConfiguration = dlsym(handle, "NeAACDecGetCurrentConfiguration");
	a->NeAACDecSetConfiguration = dlsym(handle, "NeAACDecSetConfiguration");
	a->NeAACDecOpen = dlsym(handle, "NeAACDecOpen");
	a->NeAACDecClose = dlsym(handle, "NeAACDecClose");
	a->NeAACDecInit = dlsym(handle, "NeAACDecInit");
	a->NeAACDecDecode = dlsym(handle, "NeAACDecDecode");
	a->NeAACDecGetErrorMessage = dlsym(handle, "NeAACDecGetErrorMessage");

	char *err;
	if ((err = dlerror()) != NULL) {
		LOG_WARN("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded "LIBFAAD" cap: 0x%x", a->NeAACDecGetCapabilities());
	return true;
}

struct codec *register_faad(void) {
	static struct codec ret = { 
		.id    = 'a',
		.types = "aac",
		.open  = faad_open,
		.close = faad_close,
		.decode= faad_decode,
		.min_space = 20480,
		.min_read_bytes = WRAPBUF_LEN,
	};

	if (!load_faad()) {
		return NULL;
	}

	return &ret;
}

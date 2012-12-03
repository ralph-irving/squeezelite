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
	u8_t type;
	// faad symbols to be dynamically loaded
	unsigned long (* NeAACDecGetCapabilities)(void);
	NeAACDecConfigurationPtr (* NeAACDecGetCurrentConfiguration)(NeAACDecHandle);
	unsigned char (* NeAACDecSetConfiguration)(NeAACDecHandle, NeAACDecConfigurationPtr);
	NeAACDecHandle (* NeAACDecOpen)(void);
	void (* NeAACDecClose)(NeAACDecHandle);
	long (* NeAACDecInit)(NeAACDecHandle, unsigned char *, unsigned long, unsigned long *, unsigned char *);
	char (* NeAACDecInit2)(NeAACDecHandle, unsigned char *pBuffer, unsigned long, unsigned long *, unsigned char *);
	void *(* NeAACDecDecode)(NeAACDecHandle, NeAACDecFrameInfo *, unsigned char *, unsigned long);
	char *(* NeAACDecGetErrorMessage)(unsigned char);
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

// minimal code for mp4 file parsing to extract audio config and find media data

// adapted from faad2/common/mp4ff
u32_t mp4_desc_length(u8_t **buf) {
	u8_t b;
	u8_t num_bytes = 0;
	u32_t length = 0;

	do {
		b = **buf;
		*buf += 1;
		num_bytes++;
		length = (length << 7) | (b & 0x7f);
	} while ((b & 0x80) && num_bytes < 4);

	return length;
}

// read mp4 header to extract config data - assume this occurs at start of streambuf
static int read_mp4_header(unsigned long *samplerate_p, unsigned char *channels_p) {
	size_t bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	char type[5];
	u32_t len;

	while (bytes >= 8) {
		len = unpackN((u32_t *)streambuf->readp);
		memcpy(type, streambuf->readp + 4, 4);
		type[4] = '\0';

		// extract audio config from within esds and pass to DecInit2
		if (!strcmp(type, "esds") && bytes > len) {
			u8_t *ptr = streambuf->readp + 12;
			if (*ptr++ == 0x03) {
				mp4_desc_length(&ptr);
				ptr += 4;
			} else {
				ptr += 3;
			}
			mp4_desc_length(&ptr);
			ptr += 13;
			if (*ptr++ != 0x05) {
				LOG_WARN("error parsing esds");
				return -1;
			}
			unsigned config_len = mp4_desc_length(&ptr);
			if (a->NeAACDecInit2(a->hAac, ptr, config_len, samplerate_p, channels_p) != 0) {
				LOG_WARN("bad audio config");
				return -1;
			}
		}
		
		// found media data, advance past header and return
		// currently assume audio samples are packed with no gaps into mdat from this point - we don't use stsz, stsc to find them
		if (!strcmp(type, "mdat")) {
			LOG_DEBUG("type: mdat");
			_buf_inc_readp(streambuf, 8);
			if (len == 1) {
				_buf_inc_readp(streambuf, 8);
			}
			return 1;
		}

		// default to consuming entire box
		u32_t consume = len;

		// read into these boxes so reduce consume
		if (!strcmp(type, "moov") || !strcmp(type, "trak") || !strcmp(type, "mdia") || !strcmp(type, "minf") || !strcmp(type, "stbl")) {
			consume = 8;
		}
		if (!strcmp(type, "stsd")) consume = 16;
		if (!strcmp(type, "mp4a")) consume = 36;

		if (bytes > len) {
			LOG_DEBUG("type: %s len: %u consume: %u", type, len, consume);
			_buf_inc_readp(streambuf, consume);
			bytes -= consume;
		} else if (len > streambuf->size / 2) {
			LOG_WARN("type: %s len: %u - excessive length can't parse", type, len);
			return -1;
		} else {
			break;
		}
	}

	return 0;
}

static void faad_decode(void) {
	LOCK_S;
	size_t bytes_total = _buf_used(streambuf);
	size_t bytes_wrap  = min(bytes_total, _buf_cont_read(streambuf));

	if (decode.new_stream) {
		int found = 0;
		static unsigned char channels;
		static unsigned long samplerate;

		if (a->type == '2') {

			LOG_INFO("opening atds stream");

			while (bytes_wrap >= 2 && (*(streambuf->readp) != 0xFF || (*(streambuf->readp + 1) & 0xF6) != 0xF0)) {
				_buf_inc_readp(streambuf, 1);
				bytes_total--;
				bytes_wrap--;
			}

			long n = a->NeAACDecInit(a->hAac, streambuf->readp, bytes_wrap, &samplerate, &channels);
			if (n < 0) {
				found = -1;
			} else {
				_buf_inc_readp(streambuf, n);
				found = 1;
			}

		} else {

			LOG_INFO("opening mp4 stream");

			found = read_mp4_header(&samplerate, &channels);
		}

		if (found == 1) {

			LOG_INFO("samplerate: %u channels: %u", samplerate, channels);
			bytes_total = _buf_used(streambuf);
			bytes_wrap  = min(bytes_total, _buf_cont_read(streambuf));

			LOCK_O;
			LOG_INFO("setting track_start");
			output.next_sample_rate = samplerate; 
			output.track_start = outputbuf->writep;
			decode.new_stream = false;
			UNLOCK_O;
		}

		if (found == -1) {

			LOG_WARN("error reading stream header");
			UNLOCK_S;
			LOCK_O;
			decode.state = DECODE_ERROR;
			UNLOCK_O;
			return;
		}
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
				*optr++ = *iptr++ << 8;
				*optr++ = *iptr++ << 8;
			}
		} else if (info.channels == 1) {
			while (count--) {
				*optr++ = *iptr << 8;
				*optr++ = *iptr++ << 8;
			}
		} else {
			LOG_WARN("unsupported number of channels");
		}

		frames -= f;
		_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
	}

	UNLOCK_O;

	LOG_SDEBUG("wrote %u frames", info.samples / info.channels);
}

static void faad_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	a->type = size;

	if (a->hAac) {
		a->NeAACDecClose(a->hAac);
	}
	a->hAac = a->NeAACDecOpen();

	NeAACDecConfigurationPtr conf = a->NeAACDecGetCurrentConfiguration(a->hAac);

	conf->outputFormat = FAAD_FMT_24BIT;
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
		LOG_INFO("dlerror: %s", dlerror());
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
	a->NeAACDecInit2 = dlsym(handle, "NeAACDecInit2");
	a->NeAACDecDecode = dlsym(handle, "NeAACDecDecode");
	a->NeAACDecGetErrorMessage = dlsym(handle, "NeAACDecGetErrorMessage");

	char *err;
	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
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

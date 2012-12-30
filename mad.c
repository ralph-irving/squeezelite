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

#include <mad.h>
#include <dlfcn.h>

#define MAD_DELAY 529

#define READBUF_SIZE 2048 // local buffer used by decoder: FIXME merge with any other decoders needing one?

#define LIBMAD "libmad.so.0"

struct mad {
	u8_t *readbuf;
	unsigned readbuf_len;
	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;
	enum mad_error last_error;
	// for lame gapless processing
	bool checkgapless;
	u32_t skip;
	u64_t samples;
	// mad symbols to be dynamically loaded
	void (* mad_stream_init)(struct mad_stream *);
	void (* mad_frame_init)(struct mad_frame *);
	void (* mad_synth_init)(struct mad_synth *);
	void (* mad_frame_finish)(struct mad_frame *);
	void (* mad_stream_finish)(struct mad_stream *);
	void (* mad_stream_buffer)(struct mad_stream *, unsigned char const *, unsigned long);
	int  (* mad_frame_decode)(struct mad_frame *, struct mad_stream *);
	void (* mad_synth_frame)(struct mad_synth *, struct mad_frame const *);
	char const *(* mad_stream_errorstr)(struct mad_stream const *);
};

static struct mad *m;

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

// based on libmad minimad.c scale
static inline u32_t scale(mad_fixed_t sample) {
	sample += (1L << (MAD_F_FRACBITS - 24));
	
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;
	
	return (s32_t)(sample >> (MAD_F_FRACBITS + 1 - 24)) << 8;
}

// check for lame gapless params, don't advance streambuf
static void _check_lame_header(size_t bytes) {
	u8_t *ptr = streambuf->readp;

	if (*ptr == 0xff && (*(ptr+1) & 0xf0) == 0xf0 && bytes > 180) {

		// 2 channels
		if (!memcmp(ptr + 36, "Xing", 4) || !memcmp(ptr + 36, "Info", 4)) {
			ptr += 36 + 7;
		// mono	
		} else if (!memcmp(ptr + 21, "Xing", 4) || !memcmp(ptr + 21, "Info", 4)) {
			ptr += 21 + 7;
		}

		u32_t frame_count = 0, enc_delay = 0, enc_padding = 0;
		u8_t flags = *ptr;

		if (flags & 0x01) {
			frame_count = unpackN((u32_t *)(ptr + 1));
			ptr += 4;
		}
		if (flags & 0x02) ptr += 4;
		if (flags & 0x04) ptr += 100;
		if (flags & 0x08) ptr += 4;

		if (!!memcmp(ptr+1, "LAME", 4)) {
			return;
		}

		ptr += 22;

		enc_delay   = (*ptr << 4 | *(ptr + 1) >> 4) + MAD_DELAY;
		enc_padding = (*(ptr + 1) & 0xF) << 8 | *(ptr + 2);
		enc_padding = enc_padding > MAD_DELAY ? enc_padding - MAD_DELAY : 0;

		// add one frame to initial skip for this (empty) frame
		m->skip    = enc_delay + 1152;
		m->samples = frame_count * 1152 - enc_delay - enc_padding;
		
		LOG_INFO("gapless: skip: %u samples: " FMT_u64 " delay: %u padding: %u", m->skip, m->samples, enc_delay, enc_padding);
	}
}

static decode_state mad_decode(void) {
	LOCK_S;
	size_t bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	bool eos = false;

	if (m->checkgapless) {
		m->checkgapless = false;
		if (!stream.meta_interval) {
			_check_lame_header(bytes);
		}
	}

	if (m->stream.next_frame && m->readbuf_len) {
		m->readbuf_len -= m->stream.next_frame - m->readbuf;
		memmove(m->readbuf, m->stream.next_frame, m->readbuf_len);
	}

	bytes = min(bytes, READBUF_SIZE - m->readbuf_len);
	memcpy(m->readbuf + m->readbuf_len, streambuf->readp, bytes);
	m->readbuf_len += bytes;
	_buf_inc_readp(streambuf, bytes);

	if (stream.state <= DISCONNECT && _buf_used(streambuf) == 0) {
		eos = true;
		LOG_DEBUG("end of stream");
		memset(m->readbuf + m->readbuf_len, 0, MAD_BUFFER_GUARD);
		m->readbuf_len += MAD_BUFFER_GUARD;
	}

	UNLOCK_S;

	m->mad_stream_buffer(&m->stream, m->readbuf, m->readbuf_len);

	while (true) {

		if (m->mad_frame_decode(&m->frame, &m->stream) == -1) {
			decode_state ret;
			if (!eos && m->stream.error == MAD_ERROR_BUFLEN) {
				ret = DECODE_RUNNING;
			} else if (eos && (m->stream.error == MAD_ERROR_BUFLEN || m->stream.error == MAD_ERROR_LOSTSYNC)) {
				ret = DECODE_COMPLETE;
			} else if (!MAD_RECOVERABLE(m->stream.error)) {
				LOG_INFO("mad_frame_decode error: %s - stopping decoder", m->mad_stream_errorstr(&m->stream));
				ret = DECODE_COMPLETE;
			} else {
				if (m->stream.error != m->last_error) {
					// suppress repeat error messages
					LOG_DEBUG("mad_frame_decode error: %s", m->mad_stream_errorstr(&m->stream));
				}
				ret = DECODE_RUNNING;
			}
			m->last_error = m->stream.error;
			return ret;
		};

		m->mad_synth_frame(&m->synth, &m->frame);

		LOCK_O;
		
		if (decode.new_stream) {
			LOG_INFO("setting track_start");
			output.next_sample_rate = m->synth.pcm.samplerate;
			output.track_start = outputbuf->writep;
			if (output.fade_mode) _checkfade(true);
			decode.new_stream = false;
		}
		
		if (m->synth.pcm.length > _buf_space(outputbuf) / BYTES_PER_FRAME) {
			LOG_WARN("too many samples - dropping samples");
			m->synth.pcm.length = _buf_space(outputbuf) / BYTES_PER_FRAME;
		}
		
		size_t frames = m->synth.pcm.length;
		s32_t *iptrl = m->synth.pcm.samples[0];
		s32_t *iptrr = m->synth.pcm.samples[ m->synth.pcm.channels - 1 ];

		if (m->skip) {
			u32_t skip = min(m->skip, frames);
			LOG_DEBUG("gapless: skipping %u frames at start", skip);
			frames -= skip;
			m->skip -= skip;
			iptrl += skip;
			iptrr += skip;
		}

		if (m->samples) {
			if (m->samples < frames) {
				LOG_DEBUG("gapless: trimming %u frames from end", frames - m->samples);
				frames = m->samples;
			}
			m->samples -= frames;
		}

		LOG_SDEBUG("write %u frames", frames);

		while (frames > 0) {
			size_t f = min(frames, _buf_cont_write(outputbuf) / BYTES_PER_FRAME);
			s32_t *optr = (s32_t *)outputbuf->writep;
			size_t count = f;
			while (count--) {
				*optr++ = scale(*iptrl++);
				*optr++ = scale(*iptrr++);
			}
			frames -= f;
			_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
		}

		UNLOCK_O;
	}

	return eos ? DECODE_COMPLETE : DECODE_RUNNING;
}

static void mad_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	if (!m->readbuf) {
		m->readbuf = malloc(READBUF_SIZE + MAD_BUFFER_GUARD);
	}
	m->checkgapless = true;
	m->skip = MAD_DELAY;
	m->samples = 0;
	m->readbuf_len = 0;
	m->last_error = MAD_ERROR_NONE;
	m->mad_stream_init(&m->stream);
	m->mad_frame_init(&m->frame);
	m->mad_synth_init(&m->synth);
}

static void mad_close(void) {
	mad_synth_finish(&m->synth);
	m->mad_frame_finish(&m->frame);
	m->mad_stream_finish(&m->stream);
	free(m->readbuf);
	m->readbuf = NULL;
}

static bool load_mad() {
	void *handle = dlopen(LIBMAD, RTLD_NOW);
	if (!handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	m = malloc(sizeof(struct mad));

	m->readbuf = NULL;
	m->readbuf_len = 0;
	m->mad_stream_init = dlsym(handle, "mad_stream_init");
	m->mad_frame_init = dlsym(handle, "mad_frame_init");
	m->mad_synth_init = dlsym(handle, "mad_synth_init");
	m->mad_frame_finish = dlsym(handle, "mad_frame_finish");
	m->mad_stream_finish = dlsym(handle, "mad_stream_finish");
	m->mad_stream_buffer = dlsym(handle, "mad_stream_buffer");
	m->mad_frame_decode = dlsym(handle, "mad_frame_decode");
	m->mad_synth_frame = dlsym(handle, "mad_synth_frame");
	m->mad_stream_errorstr = dlsym(handle, "mad_stream_errorstr");

	char *err;
	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded "LIBMAD);
	return true;
}

struct codec *register_mad(void) {
	static struct codec ret = { 
		.id    = 'm',
		.types = "mp3",
		.open  = mad_open,
		.close = mad_close,
		.decode= mad_decode,
		.min_space = 204800,
		.min_read_bytes = READBUF_SIZE,
	};

	if (!load_mad()) {
		return NULL;
	}

	return &ret;
}

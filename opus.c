/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2017, ralph_irving@hotmail.com
 *		Philippe 2018-2019, philippe_44@outlook.com
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
#if OPUS

/* 
*  with some low-end CPU, the decode call takes a fair bit of time and if the outputbuf is locked during that
*  period, the output_thread (or equivalent) will be locked although there is plenty of samples available.
*  Normally, with PRIO_INHERIT, that thread should increase decoder priority and get the lock quickly but it
*  seems that when the streambuf has plenty of data, the decode thread grabs the CPU to much, even it the output
*  thread has a higher priority. Using an interim buffer where opus decoder writes the output is not great from
*  an efficiency (one extra memory copy) point of view, but it allows the lock to not be kept for too long
*/
#define FRAME_BUF 2048

#if BYTES_PER_FRAME == 4		
#define ALIGN(n) 	(n)
#else
#define ALIGN(n) 	(n << 16)		
#endif

#include <opusfile.h>

struct opus {
	struct OggOpusFile *of;
#if FRAME_BUF
	u8_t *write_buf;
#endif
#if !LINKALL
	// opus symbols to be dynamically loaded
	void (*op_free)(OggOpusFile *_of);
	int  (*op_read)(OggOpusFile *_of, opus_int16 *_pcm, int _buf_size, int *_li);
	const OpusHead* (*op_head)(OggOpusFile *_of, int _li);
	OggOpusFile*  (*op_open_callbacks) (void *_source, OpusFileCallbacks *_cb, unsigned char *_initial_data, size_t _initial_bytes, int *_error);
#endif
};

static struct opus *u;

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
#define LOCK_O_not_direct   if (!decode.direct) mutex_lock(outputbuf->mutex)
#define UNLOCK_O_not_direct if (!decode.direct) mutex_unlock(outputbuf->mutex)
#define IF_DIRECT(x)    if (decode.direct) { x }
#define IF_PROCESS(x)   if (!decode.direct) { x }
#else
#define LOCK_O_direct   mutex_lock(outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(outputbuf->mutex)
#define LOCK_O_not_direct
#define UNLOCK_O_not_direct
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#endif

#if LINKALL
#define OP(h, fn, ...) (op_ ## fn)(__VA_ARGS__)
#else
#define OP(h, fn, ...) (h)->op_ ## fn(__VA_ARGS__)
#endif

// called with mutex locked within vorbis_decode to avoid locking O before S
static int _read_cb(void *datasource, char *ptr, int size) {
	size_t bytes;

	LOCK_S;

	bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	bytes = min(bytes, size);

	memcpy(ptr, streambuf->readp, bytes);
	_buf_inc_readp(streambuf, bytes);

	UNLOCK_S;

	return bytes;
}

static decode_state opus_decompress(void) {
	frames_t frames;
	int n;
	static int channels;
	u8_t *write_buf;

	LOCK_S;

	if (stream.state <= DISCONNECT && !_buf_used(streambuf)) {
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	UNLOCK_S;

	if (decode.new_stream) {
		struct OpusFileCallbacks cbs;
		const struct OpusHead *info;
		int err;

		cbs.read = (op_read_func) _read_cb;
		cbs.seek = NULL; cbs.tell = NULL; cbs.close = NULL;

		if ((u->of = OP(u, open_callbacks, streambuf, &cbs, NULL, 0, &err)) == NULL) {
			LOG_WARN("open_callbacks error: %d", err);
			return DECODE_COMPLETE;
		}

		info = OP(u, head, u->of, -1);

		LOCK_O;
		output.next_sample_rate = decode_newstream(48000, output.supported_rates);
		IF_DSD(	output.next_fmt = PCM; )
		output.track_start = outputbuf->writep;
		if (output.fade_mode) _checkfade(true);
		decode.new_stream = false;
		UNLOCK_O;

		channels = info->channel_count;

		LOG_INFO("setting track_start");
	}

#if !FRAME_BUF
	LOCK_O_direct;
#endif

#if FRAME_BUF
	IF_DIRECT(
		frames = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
		write_buf = u->write_buf;
	);
#else
	IF_DIRECT(
		frames = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
		write_buf = outputbuf->writep;
	);
#endif
	IF_PROCESS(
		frames = process.max_in_frames;
		write_buf = process.inbuf;
	);

#if FRAME_BUF
	frames = min(frames, FRAME_BUF);
#endif
	
	// write the decoded frames into outputbuf then unpack them (they are 16 bits)
	n = OP(u, read, u->of, (opus_int16*) write_buf, frames * channels, NULL);
			
#if FRAME_BUF
	LOCK_O_direct;
#endif

	if (n > 0) {
		frames_t count;
		s16_t *iptr;
		ISAMPLE_T *optr;

		frames = n;
		count = frames * channels;

		iptr = (s16_t *)write_buf + count;
		optr = (ISAMPLE_T *) outputbuf->writep + frames * 2;

		if (channels == 2) {
#if BYTES_PER_FRAME == 4
			memcpy(outputbuf->writep, write_buf, frames * BYTES_PER_FRAME);
#else
			while (count--) {
				*--optr = *--iptr << 16;
			}
#endif
		} else if (channels == 1) {
			while (count--) {
				*--optr = ALIGN(*--iptr);
				*--optr = ALIGN(*iptr);
			}
		}

		IF_DIRECT(
			_buf_inc_writep(outputbuf, frames * BYTES_PER_FRAME);
		);
		IF_PROCESS(
			process.in_frames = frames;
		);

		LOG_SDEBUG("wrote %u frames", frames);

	} else if (n == 0) {

		if (stream.state <= DISCONNECT) {
			LOG_INFO("partial decode");
			UNLOCK_O_direct;
			UNLOCK_S;
			return DECODE_COMPLETE;
		} else {
			LOG_INFO("no frame decoded");
        }

	} else if (n == OP_HOLE) {

		// recoverable hole in stream, seen when skipping
		LOG_DEBUG("hole in stream");

	} else {

		LOG_INFO("op_read error: %d", n);
		UNLOCK_O_direct;
		return DECODE_COMPLETE;
	}

	UNLOCK_O_direct;

	return DECODE_RUNNING;
}


static void opus_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	if (!u->of) {
#if FRAME_BUF
		if (!u->write_buf) u->write_buf = malloc(FRAME_BUF * BYTES_PER_FRAME);
#endif
	} else {
		OP(u, free, u->of);
		u->of = NULL;
	}	
}

static void opus_close(void) {
	if (u->of) {
		OP(u, free, u->of);
		u->of = NULL;
	}
#if FRAME_BUF
	free(u->write_buf);
	u->write_buf = NULL;
#endif
}

static bool load_opus(void) {
#if !LINKALL
	void *handle = dlopen(LIBOPUS, RTLD_NOW);
	char *err;

	if (!handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	u->op_free = dlsym(handle, "op_free");
	u->op_read = dlsym(handle, "op_read");
	u->op_head = dlsym(handle, "op_head");
	u->op_open_callbacks = dlsym(handle, "op_open_callbacks");
	
	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded "LIBOPUS);
#endif

	return true;
}

struct codec *register_opus(void) {
	static struct codec ret = {
		'u',          // id
		"ops",        // types
		4096,         // min read
		20480,        // min space
		opus_open, 	  // open
		opus_close,   // close
		opus_decompress,  // decode
	};

	u = malloc(sizeof(struct opus));
	if (!u) {
		return NULL;
	}

	u->of = NULL;
	u->write_buf = NULL;

	if (!load_opus()) {
		return NULL;
	}

	LOG_INFO("using opus to decode ops");
	return &ret;
}
#endif /* OPUS */


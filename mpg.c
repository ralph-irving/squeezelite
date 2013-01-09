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

#include "squeezelite.h"

#include <mpg123.h>

#define READ_SIZE  512
#define WRITE_SIZE 32 * 1024

struct mpg {
	mpg123_handle *h;	
	// mpg symbols to be dynamically loaded
	int (* mpg123_init)(void);
	int (* mpg123_feature)(const enum mpg123_feature_set);
	void (* mpg123_rates)(const long **, size_t *);
	int (* mpg123_format_none)(mpg123_handle *);
	int (* mpg123_format)(mpg123_handle *, long, int, int);
	mpg123_handle *(* mpg123_new)(const char*, int *);
	void (* mpg123_delete)(mpg123_handle *);
	int (* mpg123_open_feed)(mpg123_handle *);
	int (* mpg123_decode)(mpg123_handle *, const unsigned char *, size_t, unsigned char *, size_t, size_t *);
	int (* mpg123_getformat)(mpg123_handle *, long *, int *, int *);
	const char* (* mpg123_plain_strerror)(int);
};

static struct mpg *m;

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;

#define LOCK_S   mutex_lock(streambuf->mutex)
#define UNLOCK_S mutex_unlock(streambuf->mutex)
#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)

static decode_state mpg_decode(void) {
	size_t bytes, space, size;
	int ret;

	LOCK_S;
	LOCK_O;
	bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	space = min(_buf_space(outputbuf), _buf_cont_write(outputbuf));
	bytes = min(bytes, READ_SIZE);
	space = min(space, WRITE_SIZE);

	if (stream.state <= DISCONNECT && bytes == 0) {
		UNLOCK_O;
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	ret = m->mpg123_decode(m->h, streambuf->readp, bytes, outputbuf->writep, space, &size);

	if (ret == MPG123_NEW_FORMAT) {

		if (decode.new_stream) {
			long rate;
			int channels, enc;
			
			m->mpg123_getformat(m->h, &rate, &channels, &enc);
			
			LOG_INFO("setting track_start");
			output.next_sample_rate = rate;
			output.track_start = outputbuf->writep;
			if (output.fade_mode) _checkfade(true);
			decode.new_stream = false;

		} else {
			LOG_WARN("format change mid stream - not supported");
		}
	}

	_buf_inc_readp(streambuf, bytes);
	_buf_inc_writep(outputbuf, size);

	UNLOCK_O;
	UNLOCK_S;

	LOG_SDEBUG("write %u frames", size / BYTES_PER_FRAME);

	if (ret == MPG123_DONE) {
		LOG_INFO("stream complete");
		return DECODE_COMPLETE;
	}

	if (ret == MPG123_ERR) {
		LOG_WARN("Error");
		return DECODE_COMPLETE;
	}

	// OK and NEED_MORE keep running
	return DECODE_RUNNING;
}

static void mpg_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	int err;
	const long *list;
	size_t count, i;

	if (m->h) {
		m->mpg123_delete(m->h);
	}

	m->h = m->mpg123_new(NULL, &err);

	if (m->h == NULL) {
		LOG_WARN("new error: %s", m->mpg123_plain_strerror(err));
	}

	// restrict output to 32bit signed 2 channel for all supported sample rates
	m->mpg123_rates(&list, &count);
	m->mpg123_format_none(m->h);
	for (i = 0; i < count; i++) {
		m->mpg123_format(m->h, list[i], 2, MPG123_ENC_SIGNED_32);
	}

	err = m->mpg123_open_feed(m->h);

	if (err) {
		LOG_WARN("open feed error: %s", m->mpg123_plain_strerror(err));
	}
}

static void mpg_close(void) {
	m->mpg123_delete(m->h);
	m->h = NULL;
}

static bool load_mpg() {
	void *handle = dlopen(LIBMPG, RTLD_NOW);
	char *err;

	if (!handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}
	
	m = malloc(sizeof(struct mpg));

	m->h = NULL;
	m->mpg123_init = dlsym(handle, "mpg123_init");
	m->mpg123_feature = dlsym(handle, "mpg123_feature");
	m->mpg123_rates = dlsym(handle, "mpg123_rates");
	m->mpg123_format_none = dlsym(handle, "mpg123_format_none");
	m->mpg123_format = dlsym(handle, "mpg123_format");
	m->mpg123_new = dlsym(handle, "mpg123_new");
	m->mpg123_delete = dlsym(handle, "mpg123_delete");
	m->mpg123_open_feed = dlsym(handle, "mpg123_open_feed");
	m->mpg123_decode = dlsym(handle, "mpg123_decode");
	m->mpg123_getformat = dlsym(handle, "mpg123_getformat");
	m->mpg123_plain_strerror = dlsym(handle, "mpg123_plain_strerror");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	m->mpg123_init();

	if (!m->mpg123_feature(MPG123_FEATURE_OUTPUT_32BIT)) {
		LOG_WARN("32 bit output not supported - disabled");
		return false;
	}

	LOG_INFO("loaded "LIBMPG);
	return true;
}

struct codec *register_mpg(void) {
	static struct codec ret = { 
		'm',          // id
		"mp3",        // types
		READ_SIZE,    // min read
		WRITE_SIZE,   // min space
		mpg_open,     // open
		mpg_close,    // close
		mpg_decode,   // decode
	};

	if (!load_mpg()) {
		return NULL;
	}

	return &ret;
}

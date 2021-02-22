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

// fifo bufffers 

#define _GNU_SOURCE

#include "squeezelite.h"

// _* called with muxtex locked

#if !WIN
inline
#endif
unsigned _buf_used(struct buffer *buf) {
	return buf->writep >= buf->readp ? buf->writep - buf->readp : buf->size - (buf->readp - buf->writep);
}

unsigned _buf_space(struct buffer *buf) {
	return buf->size - _buf_used(buf) - 1; // reduce by one as full same as empty otherwise
}

unsigned _buf_cont_read(struct buffer *buf) {
	return buf->writep >= buf->readp ? buf->writep - buf->readp : buf->wrap - buf->readp;
}

unsigned _buf_cont_write(struct buffer *buf) {
	return buf->writep >= buf->readp ? buf->wrap - buf->writep : buf->readp - buf->writep;
}

void _buf_inc_readp(struct buffer *buf, unsigned by) {
	buf->readp += by;
	if (buf->readp >= buf->wrap) {
		buf->readp -= buf->size;
	}
}

void _buf_inc_writep(struct buffer *buf, unsigned by) {
	buf->writep += by;
	if (buf->writep >= buf->wrap) {
		buf->writep -= buf->size;
	}
}

void buf_flush(struct buffer *buf) {
	mutex_lock(buf->mutex);
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	mutex_unlock(buf->mutex);
}

// adjust buffer to multiple of mod bytes so reading in multiple always wraps on frame boundary
void buf_adjust(struct buffer *buf, size_t mod) {
	size_t size;
	mutex_lock(buf->mutex);
	size = ((unsigned)(buf->base_size / mod)) * mod;
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	buf->wrap   = buf->buf + size;
	buf->size   = size;
	mutex_unlock(buf->mutex);
}

// called with mutex locked to resize, does not retain contents, reverts to original size if fails
void _buf_resize(struct buffer *buf, size_t size) {
	free(buf->buf);
	buf->buf = malloc(size);
	if (!buf->buf) {
		size    = buf->size;
		buf->buf= malloc(size);
		if (!buf->buf) {
			size = 0;
		}
	}
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	buf->wrap   = buf->buf + size;
	buf->size   = size;
	buf->base_size = size;
}

void _buf_unwrap(struct buffer *buf, size_t cont) {
	ssize_t len, by = cont - (buf->wrap - buf->readp);
	size_t size;
	u8_t *scratch;

	// do nothing if we have enough space
	if (by <= 0 || cont >= buf->size) return;

	// buffer already unwrapped, just move it up
	if (buf->writep >= buf->readp) {
		memmove(buf->readp - by, buf->readp, buf->writep - buf->readp);
		buf->readp -= by;
		buf->writep -= by;
		return;
	 }

	// how much is overlapping
	size = by - (buf->readp - buf->writep);
	len = buf->writep - buf->buf;

	// buffer is wrapped and enough free space to move data up directly
	if (size <= 0) {
		memmove(buf->readp - by, buf->readp, buf->wrap - buf->readp);
		buf->readp -= by;
		memcpy(buf->wrap - by, buf->buf, min(len, by));
		if (len > by) {
			memmove(buf->buf, buf->buf + by, len - by);
			buf->writep -= by;
		} else buf->writep += buf->size - by;
		return;
	}

	scratch = malloc(size);

	// buffer is wrapped but not enough free room => use scratch zone
	if (scratch) {
		memcpy(scratch, buf->writep - size, size);
		memmove(buf->readp - by, buf->readp, buf->wrap - buf->readp);
		buf->readp -= by;
		memcpy(buf->wrap - by, buf->buf, by);
		memmove(buf->buf, buf->buf + by, len - by - size);
		buf->writep -= by;
		memcpy(buf->writep - size, scratch, size);
		free(scratch);
	} else {
		_buf_unwrap(buf, cont / 2);
        _buf_unwrap(buf, cont - cont / 2);
	}
}

void buf_init(struct buffer *buf, size_t size) {
	buf->buf    = malloc(size);
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	buf->wrap   = buf->buf + size;
	buf->size   = size;
	buf->base_size = size;
	mutex_create_p(buf->mutex);
}

void buf_destroy(struct buffer *buf) {
	if (buf->buf) {
		free(buf->buf);
		buf->buf = NULL;
		buf->size = 0;
		buf->base_size = 0;
		mutex_destroy(buf->mutex);
	}
}

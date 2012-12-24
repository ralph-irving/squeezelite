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

// fifo bufffers 

#define _GNU_SOURCE

#include "squeezelite.h"

// _* called with muxtex locked

inline unsigned _buf_used(struct buffer *buf) {
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
	pthread_mutex_lock(&buf->mutex);
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	pthread_mutex_unlock(&buf->mutex);
}

// adjust buffer to multiple of mod bytes so reading in multiple always wraps on frame boundary
void buf_adjust(struct buffer *buf, size_t mod) {
	pthread_mutex_lock(&buf->mutex);
	size_t size = ((unsigned)(buf->base_size / mod)) * mod;
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	buf->wrap   = buf->buf + size;
	buf->size   = size;
	pthread_mutex_unlock(&buf->mutex);
}

// called with mutex locked to resize, does not retain contents
void _buf_resize(struct buffer *buf, size_t size) {
	// use realloc so we can continue if resize failed at expense of copying useless data
	u8_t *tmp = realloc(buf->buf, size);
	if (tmp) {
		buf->buf = tmp;
		buf->readp  = buf->buf;
		buf->writep = buf->buf;
		buf->wrap   = buf->buf + size;
		buf->size   = size;
		buf->base_size = size;
	}
}

void buf_init(struct buffer *buf, size_t size) {
	buf->buf    = malloc(size);
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	buf->wrap   = buf->buf + size;
	buf->size   = size;
	buf->base_size = size;

	pthread_mutexattr_t mutex_attr;
	pthread_mutexattr_init(&mutex_attr);
	pthread_mutexattr_setprotocol(&mutex_attr, PTHREAD_PRIO_INHERIT);

	pthread_mutex_init(&buf->mutex, &mutex_attr);

	pthread_mutexattr_destroy(&mutex_attr);
}

void buf_destroy(struct buffer *buf) {
	if (buf->buf) {
		free(buf->buf);
		buf->buf = NULL;
		buf->size = 0;
		buf->base_size = 0;
		pthread_mutex_destroy(&buf->mutex);
	}
}

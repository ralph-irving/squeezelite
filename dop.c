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

// DSP over PCM (DOP) specific functions

#include "squeezelite.h"

#if DSD

extern struct buffer *outputbuf;
extern struct outputstate output;

#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)

// check for 32 dop marker frames to see if this is a dop stream
// dop is always encoded in 24 bit samples with markers 0x05 or 0xFA in MSB
bool is_stream_dop(u8_t *lptr, u8_t *rptr, int step, frames_t frames) {
	int matched = 0;
	u32_t next = 0;

	while (frames--) {
		if ((*lptr == 0x05 && *rptr == 0x05) ||
			(*lptr == 0xFA && *rptr == 0xFA)) {
			if (*lptr == next) {
				matched++;
			} else {
				next = *lptr;
				matched = 1;
			}
			next = ( 0x05 + 0xFA ) - next;
		} else {
			return false;
		}
		if (matched == 32) {
			return true;
		}

		lptr+=step; rptr+=step;
	}
	return false;
}

// update the dop marker and potentially invert polarity for frames in the output buffer
// performaned on all output including silence to maintain marker phase consitency
void update_dop(u32_t *ptr, frames_t frames, bool invert) {
	static u32_t marker = 0x05;
	if (!invert) {
		while (frames--) {
			u32_t scaled_marker = marker << 24;
			*ptr = (*ptr & 0x00FFFF00) | scaled_marker;
			++ptr;
			*ptr = (*ptr & 0x00FFFF00) | scaled_marker;
			++ptr;
			marker = ( 0x05 + 0xFA ) - marker;
		}
	} else {
		while (frames--) {
			u32_t scaled_marker = marker << 24;
			*ptr = ((~(*ptr)) & 0x00FFFF00) | scaled_marker;
			++ptr;
			*ptr = ((~(*ptr)) & 0x00FFFF00) | scaled_marker;
			++ptr;
			marker = ( 0x05 + 0xFA ) - marker;
		}
	}
}

#endif // DSD

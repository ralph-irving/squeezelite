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

// check for 32 dop marker frames to see if this is dop in flac
// dop is always encoded in 24 bit samples with marker 0x0005xxxx or 0x00FAxxxx
bool is_flac_dop(u32_t *lptr, u32_t *rptr, frames_t frames) {
	int matched = 0;
	u32_t next = 0;

	while (frames--) {
		if (((*lptr & 0x00FF0000) == 0x00050000 && (*rptr & 0x00FF0000) == 0x00050000) ||
			((*lptr & 0x00FF0000) == 0x00FA0000 && (*rptr & 0x00FF0000) == 0x00FA0000)) {
			if (*lptr >> 24 == next) {
				matched++;
				next = ( 0x05 + 0xFA ) - next;
			} else {
				next = *lptr >> 24;
				matched = 1;
			}
		} else {
			return false;
		}
		if (matched == 32) {
			return true;
		}

		++lptr; ++rptr;
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
			*ptr = (*ptr & 0x00FFFFFF) | scaled_marker;
			++ptr;
			*ptr = (*ptr & 0x00FFFFFF) | scaled_marker;
			++ptr;
			marker = ( 0x05 + 0xFA ) - marker;
		}
	} else {
		while (frames--) {
			u32_t scaled_marker = marker << 24;
			*ptr = ((~(*ptr)) & 0x00FFFFFF) | scaled_marker;
			++ptr;
			*ptr = ((~(*ptr)) & 0x00FFFFFF) | scaled_marker;
			++ptr;
			marker = ( 0x05 + 0xFA ) - marker;
		}
	}
}

// fill silence buffer with 10101100 which represents dop silence
// leave marker zero it will be updated at output, leave lsb zero
void dop_silence_frames(u32_t *ptr, frames_t frames) {
	while (frames--) {
		*ptr++ = 0x00ACAC00;
		*ptr++ = 0x00ACAC00;
	}
}

void dop_init(bool enable, unsigned delay) {
	LOCK_O;
	output.has_dop = enable;
	output.dop_delay = delay;
	UNLOCK_O;
}

#endif // DSD

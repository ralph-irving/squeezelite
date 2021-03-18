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

// Scale and pack functions

#include "squeezelite.h"

#define MAX_SCALESAMPLE 0x7fffffffffffLL
#define MIN_SCALESAMPLE -MAX_SCALESAMPLE

// inlining these on windows prevents them being linkable...
#if !WIN
inline 
#endif
s32_t gain(s32_t gain, s32_t sample) {
	s64_t res = (s64_t)gain * (s64_t)sample;
	if (res > MAX_SCALESAMPLE) res = MAX_SCALESAMPLE;
	if (res < MIN_SCALESAMPLE) res = MIN_SCALESAMPLE;
	return (s32_t) (res >> 16);
}
#if !WIN
inline
#endif
s32_t to_gain(float f) {
	return (s32_t)(f * 65536.0F);
}

void _scale_and_pack_frames(void *outputptr, s32_t *inputptr, frames_t cnt, s32_t gainL, s32_t gainR, u8_t flags, output_format format) {
	// in-place copy input samples if mono/combined is used (never happens with DSD active)
	if ((flags & MONO_LEFT) && (flags & MONO_RIGHT)) {
		s32_t *ptr = inputptr;
		frames_t count = cnt;
		while (count--) {
			// use 64 bits integer for purists but should really not care
			*ptr = *(ptr + 1) = ((s64_t) *ptr + (s64_t) *(ptr + 1)) / 2;
			ptr += 2;
		}
	} else if (flags & MONO_RIGHT) {
		s32_t *ptr = inputptr + 1;
		frames_t count = cnt;
		while (count--) {
			*(ptr - 1) = *ptr;
			ptr += 2;
		}
	} else if (flags & MONO_LEFT) {	
		s32_t *ptr = inputptr;
		frames_t count = cnt;
		while (count--) {
			*(ptr + 1) = *ptr;
			ptr += 2;
		}
	}

	switch(format) {
#if DSD
	case U32_LE:
		{
#if SL_LITTLE_ENDIAN
			memcpy(outputptr, inputptr, cnt * BYTES_PER_FRAME);
#else
			u32_t *optr = (u32_t *)(void *)outputptr;
			while (cnt--) {
				s32_t lsample = *(inputptr++);
				s32_t rsample = *(inputptr++);
				*(optr++) = 
					(lsample & 0xff000000) >> 24 | (lsample & 0x00ff0000) >> 8 |
					(lsample & 0x0000ff00) << 8  | (lsample & 0x000000ff) << 24;
				*(optr++) = 
					(rsample & 0xff000000) >> 24 | (rsample & 0x00ff0000) >> 8 |
					(rsample & 0x0000ff00) << 8  | (rsample & 0x000000ff) << 24;
			}
#endif
		}
		break;
	case U32_BE:
		{
#if SL_LITTLE_ENDIAN
			u32_t *optr = (u32_t *)(void *)outputptr;
			while (cnt--) {
				s32_t lsample = *(inputptr++);
				s32_t rsample = *(inputptr++);
				*(optr++) = 
					(lsample & 0xff000000) >> 24 | (lsample & 0x00ff0000) >> 8 |
					(lsample & 0x0000ff00) << 8  | (lsample & 0x000000ff) << 24;
				*(optr++) = 
					(rsample & 0xff000000) >> 24 | (rsample & 0x00ff0000) >> 8 |
					(rsample & 0x0000ff00) << 8  | (rsample & 0x000000ff) << 24;
			}
#else
			memcpy(outputptr, inputptr, cnt * BYTES_PER_FRAME);
#endif
		}
		break;
	case U16_LE:
		{
			u32_t *optr = (u32_t *)(void *)outputptr;
#if SL_LITTLE_ENDIAN
			while (cnt--) {
				*(optr++) = (*(inputptr) >> 16 & 0x0000ffff) | (*(inputptr + 1) & 0xffff0000);
				inputptr += 2;
			}
#else
			while (cnt--) {
				s32_t lsample = *(inputptr++);
				s32_t rsample = *(inputptr++);
				*(optr++) = 
					(lsample & 0x00ff0000) << 8 | (lsample & 0xff000000) >> 8 |
					(rsample & 0x00ff0000) >> 8 | (rsample & 0xff000000) >> 24;
			}
#endif
		}
		break;
	case U16_BE:
		{
			u32_t *optr = (u32_t *)(void *)outputptr;
#if SL_LITTLE_ENDIAN
			while (cnt--) {
				s32_t lsample = *(inputptr++);
				s32_t rsample = *(inputptr++);
				*(optr++) = 
					(lsample & 0xff000000) >> 24 | (lsample & 0x00ff0000) >> 8 |
					(rsample & 0xff000000) >> 8 | (rsample & 0x00ff0000) << 8;
			}
#else
			while (cnt--) {
				*(optr++) = (*(inputptr) & 0xffff0000) | (*(inputptr + 1) >> 16 & 0x0000ffff);
				inputptr += 2;
			}
#endif
		}
		break;
	case U8:
		{
			u16_t *optr = (u16_t *)(void *)outputptr;
#if SL_LITTLE_ENDIAN
			while (cnt--) {
				*(optr++) = (u16_t)((*(inputptr) >> 24 & 0x000000ff) | (*(inputptr + 1) >> 16 & 0x0000ff00));
				inputptr += 2;
			}
#else
			while (cnt--) {
				*(optr++) = (u16_t)((*(inputptr) >> 16 & 0x0000ff00) | (*(inputptr + 1) >> 24 & 0x000000ff));
				inputptr += 2;
			}
#endif
		}
		break;
#endif
	case S16_LE:
		{
			u32_t *optr = (u32_t *)(void *)outputptr;
#if SL_LITTLE_ENDIAN
			if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
				while (cnt--) {
					*(optr++) = (*(inputptr) >> 16 & 0x0000ffff) | (*(inputptr + 1) & 0xffff0000);
					inputptr += 2;
				}
			} else {
				while (cnt--) {
					*(optr++) =  (gain(gainL, *(inputptr)) >> 16 & 0x0000ffff) | (gain(gainR, *(inputptr+1)) & 0xffff0000);
					inputptr += 2;
				}
			}
#else
			if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
				while (cnt--) {
					s32_t lsample = *(inputptr++);
					s32_t rsample = *(inputptr++);
					*(optr++) = 
						(lsample & 0x00ff0000) << 8 | (lsample & 0xff000000) >> 8 |
						(rsample & 0x00ff0000) >> 8 | (rsample & 0xff000000) >> 24;
				}
			} else {
				while (cnt--) {
					s32_t lsample = gain(gainL, *(inputptr++));
					s32_t rsample = gain(gainR, *(inputptr++));
					*(optr++) = 
						(lsample & 0x00ff0000) << 8 | (lsample & 0xff000000) >> 8 |
						(rsample & 0x00ff0000) >> 8 | (rsample & 0xff000000) >> 24;
				}
			}
#endif
		}
		break;
	case S24_LE: 
		{
			u32_t *optr = (u32_t *)(void *)outputptr;
#if SL_LITTLE_ENDIAN
			if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
				while (cnt--) {
					*(optr++) = *(inputptr++) >> 8;
					*(optr++) = *(inputptr++) >> 8;
				}
			} else {
				while (cnt--) {
					*(optr++) = gain(gainL, *(inputptr++)) >> 8;
					*(optr++) = gain(gainR, *(inputptr++)) >> 8;
				}
			}
#else
			if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
				while (cnt--) {
					s32_t lsample = *(inputptr++);
					s32_t rsample = *(inputptr++);
					*(optr++) = 
						(lsample & 0xff000000) >> 16 | (lsample & 0x00ff0000) | (lsample & 0x0000ff00 << 16);
					*(optr++) = 
						(rsample & 0xff000000) >> 16 | (rsample & 0x00ff0000) | (rsample & 0x0000ff00 << 16);
				}
			} else {
				while (cnt--) {
					s32_t lsample = gain(gainL, *(inputptr++));
					s32_t rsample = gain(gainR, *(inputptr++));
					*(optr++) = 
						(lsample & 0xff000000) >> 16 | (lsample & 0x00ff0000) | (lsample & 0x0000ff00 << 16);
					*(optr++) = 
						(rsample & 0xff000000) >> 16 | (rsample & 0x00ff0000) | (rsample & 0x0000ff00 << 16);
				}
			}
#endif
		}
		break;
	case S24_3LE:
		{
			u8_t *optr = (u8_t *)(void *)outputptr;
			if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
				while (cnt) {
					// attempt to do 32 bit memory accesses - move 2 frames at once: 16 bytes -> 12 bytes
					// falls through to exception case when not aligned or if less than 2 frames to move
					if (((uintptr_t)optr & 0x3) == 0 && cnt >= 2) {
						u32_t *o_ptr = (u32_t *)(void *)optr;
						while (cnt >= 2) {
							s32_t l1 = *(inputptr++); s32_t r1 = *(inputptr++);
							s32_t l2 = *(inputptr++); s32_t r2 = *(inputptr++);
#if SL_LITTLE_ENDIAN
							*(o_ptr++) = (l1 & 0xffffff00) >>  8 | (r1 & 0x0000ff00) << 16;
							*(o_ptr++) = (r1 & 0xffff0000) >> 16 | (l2 & 0x00ffff00) <<  8;
							*(o_ptr++) = (l2 & 0xff000000) >> 24 | (r2 & 0xffffff00);
#else
							*(o_ptr++) = (l1 & 0x0000ff00) << 16 | (l1 & 0x00ff0000) | (l1 & 0xff000000) >> 16 |
								(r1 & 0x0000ff00) >> 8; 
							*(o_ptr++) = (r1 & 0x00ff0000) <<  8 | (r1 & 0xff000000) >> 8 | (l2 & 0x0000ff00) |
								(l2 & 0x00ff0000) >> 16;
							*(o_ptr++) = (l2 & 0xff000000) | (r2 & 0x0000ff00) << 8 | (r2 & 0x00ff0000) >> 8 |
								(r2 & 0xff000000) >> 24;
#endif
							optr += 12;
							cnt  -=  2;
						}
					} else {
						s32_t lsample = *(inputptr++);
						s32_t rsample = *(inputptr++);
						*(optr++) = (lsample & 0x0000ff00) >>  8;
						*(optr++) = (lsample & 0x00ff0000) >> 16;
						*(optr++) = (lsample & 0xff000000) >> 24;
						*(optr++) = (rsample & 0x0000ff00) >>  8;
						*(optr++) = (rsample & 0x00ff0000) >> 16;
						*(optr++) = (rsample & 0xff000000) >> 24;
						cnt--;
					}
				}
			} else {
				while (cnt) {
					// attempt to do 32 bit memory accesses - move 2 frames at once: 16 bytes -> 12 bytes
					// falls through to exception case when not aligned or if less than 2 frames to move
					if (((uintptr_t)optr & 0x3) == 0 && cnt >= 2) {
						u32_t *o_ptr = (u32_t *)(void *)optr;
						while (cnt >= 2) {
							s32_t l1 = gain(gainL, *(inputptr++)); s32_t r1 = gain(gainR, *(inputptr++));
							s32_t l2 = gain(gainL, *(inputptr++)); s32_t r2 = gain(gainR, *(inputptr++));
#if SL_LITTLE_ENDIAN
							*(o_ptr++) = (l1 & 0xffffff00) >>  8 | (r1 & 0x0000ff00) << 16;
							*(o_ptr++) = (r1 & 0xffff0000) >> 16 | (l2 & 0x00ffff00) <<  8;
							*(o_ptr++) = (l2 & 0xff000000) >> 24 | (r2 & 0xffffff00);
#else
							*(o_ptr++) = (l1 & 0x0000ff00) << 16 | (l1 & 0x00ff0000) | (l1 & 0xff000000) >> 16 |
								(r1 & 0x0000ff00) >> 8; 
							*(o_ptr++) = (r1 & 0x00ff0000) <<  8 | (r1 & 0xff000000) >> 8 | (l2 & 0x0000ff00) |
								(l2 & 0x00ff0000) >> 16;
							*(o_ptr++) = (l2 & 0xff000000) | (r2 & 0x0000ff00) << 8 | (r2 & 0x00ff0000) >> 8 |
								(r2 & 0xff000000) >> 24;
#endif
							optr += 12;
							cnt  -=  2;
						}
					} else {
						s32_t lsample = gain(gainL, *(inputptr++));
						s32_t rsample = gain(gainR, *(inputptr++));
						*(optr++) = (lsample & 0x0000ff00) >>  8;
						*(optr++) = (lsample & 0x00ff0000) >> 16;
						*(optr++) = (lsample & 0xff000000) >> 24;
						*(optr++) = (rsample & 0x0000ff00) >>  8;
						*(optr++) = (rsample & 0x00ff0000) >> 16;
						*(optr++) = (rsample & 0xff000000) >> 24;
						cnt--;
					}
				}
			}
		}
		break;
	case S32_LE:
		{
			u32_t *optr = (u32_t *)(void *)outputptr;
#if SL_LITTLE_ENDIAN
			if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
				memcpy(outputptr, inputptr, cnt * BYTES_PER_FRAME);
			} else {
				while (cnt--) {
					*(optr++) = gain(gainL, *(inputptr++));
					*(optr++) = gain(gainR, *(inputptr++));
				}
			}
#else
			if (gainL == FIXED_ONE && gainR == FIXED_ONE) {
				while (cnt--) {
					s32_t lsample = *(inputptr++);
					s32_t rsample = *(inputptr++);
					*(optr++) = 
						(lsample & 0xff000000) >> 24 | (lsample & 0x00ff0000) >> 8 |
						(lsample & 0x0000ff00) << 8  | (lsample & 0x000000ff) << 24;
					*(optr++) = 
						(rsample & 0xff000000) >> 24 | (rsample & 0x00ff0000) >> 8 |
						(rsample & 0x0000ff00) << 8  | (rsample & 0x000000ff) << 24;
				}
			} else {
				while (cnt--) {
					s32_t lsample = gain(gainL, *(inputptr++));
					s32_t rsample = gain(gainR, *(inputptr++));
					*(optr++) = 
						(lsample & 0xff000000) >> 24 | (lsample & 0x00ff0000) >> 8 |
						(lsample & 0x0000ff00) << 8  | (lsample & 0x000000ff) << 24;
					*(optr++) = 
						(rsample & 0xff000000) >> 24 | (rsample & 0x00ff0000) >> 8 |
						(rsample & 0x0000ff00) << 8  | (rsample & 0x000000ff) << 24;
				}
			}
#endif
		}
		break;
	default:
		break;
	}
}

#if !WIN
inline 
#endif
void _apply_cross(struct buffer *outputbuf, frames_t out_frames, s32_t cross_gain_in, s32_t cross_gain_out, s32_t **cross_ptr) {
	s32_t *ptr = (s32_t *)(void *)outputbuf->readp;
	frames_t count = out_frames * 2;
	while (count--) {
		if (*cross_ptr > (s32_t *)outputbuf->wrap) {
			*cross_ptr -= outputbuf->size / BYTES_PER_FRAME * 2;
		}
		*ptr = gain(cross_gain_out, *ptr) + gain(cross_gain_in, **cross_ptr);
		ptr++; (*cross_ptr)++;
	}
}

#if !WIN
inline 
#endif
void _apply_gain(struct buffer *outputbuf, frames_t count, s32_t gainL, s32_t gainR, u8_t flags) {
	if (gainL == FIXED_ONE && gainR == FIXED_ONE && !(flags & (MONO_LEFT | MONO_RIGHT))) {
		return;
	} else if ((flags & MONO_LEFT) && (flags & MONO_RIGHT)) {
		ISAMPLE_T *ptrL = (ISAMPLE_T *)(void *)outputbuf->readp;
		ISAMPLE_T *ptrR = (ISAMPLE_T *)(void *)outputbuf->readp + 1;
		while (count--) {
			*ptrL = *ptrR = (gain(gainL, *ptrL) + gain(gainR, *ptrR)) / 2;
			ptrL += 2; ptrR += 2;
		}

	} else if (flags & MONO_RIGHT) {
		ISAMPLE_T *ptr = (ISAMPLE_T *)(void *)outputbuf->readp + 1;
		while (count--) {
			*(ptr - 1) = *ptr = gain(gainR, *ptr);
			ptr += 2;
		}
	} else if (flags & MONO_LEFT) {
		ISAMPLE_T *ptr = (ISAMPLE_T *)(void *)outputbuf->readp;
		while (count--) {
			*(ptr + 1) = *ptr = gain(gainL, *ptr);
			ptr += 2;
		}
	} else {
	   	ISAMPLE_T *ptrL = (ISAMPLE_T *)(void *)outputbuf->readp;
		ISAMPLE_T *ptrR = (ISAMPLE_T *)(void *)outputbuf->readp + 1;
		while (count--) {
			*ptrL = gain(gainL, *ptrL);
			*ptrR = gain(gainR, *ptrR);
			ptrL += 2; ptrR += 2;
		}
	}
}

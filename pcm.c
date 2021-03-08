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

#include "squeezelite.h"

#if BYTES_PER_FRAME == 4
#define SHIFT 16
#define OPTR_T	u16_t
#else
#define OPTR_T	u32_t	
#define SHIFT 0
#endif

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;
extern struct processstate process;

bool pcm_check_header = false;

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

#define MAX_DECODE_FRAMES 4096

static u32_t sample_rates[] = {
	11025, 22050, 32000, 44100, 48000, 8000, 12000, 16000, 24000, 96000, 88200, 176400, 192000, 352800, 384000, 705600, 768000
};

static u32_t sample_rate;
static u32_t sample_size;
static u32_t channels;
static bool  bigendian;
static bool  limit;
static u32_t audio_left;
static u32_t bytes_per_frame;

typedef enum { UNKNOWN = 0, WAVE, AIFF } header_format;

static void _check_header(void) {
	u8_t *ptr = streambuf->readp;
	unsigned bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	header_format format = UNKNOWN;

	// simple parsing of wav and aiff headers and get to samples

	if (bytes > 12) {
		if (!memcmp(ptr, "RIFF", 4) && !memcmp(ptr+8, "WAVE", 4)) {
			LOG_INFO("WAVE");
			format = WAVE;
		} else if (!memcmp(ptr, "FORM", 4) && (!memcmp(ptr+8, "AIFF", 4) || !memcmp(ptr+8, "AIFC", 4))) {
			LOG_INFO("AIFF");
			format = AIFF;
		}
	}

	if (format != UNKNOWN) {
		ptr   += 12;
		bytes -= 12;

		while (bytes >= 8) {
			char id[5];
			unsigned len;
			memcpy(id, ptr, 4);
			id[4] = '\0';
			
			if (format == WAVE) {
				len = *(ptr+4) | *(ptr+5) << 8 | *(ptr+6) << 16| *(ptr+7) << 24;
			} else {
				len = *(ptr+4) << 24 | *(ptr+5) << 16 | *(ptr+6) << 8 | *(ptr+7);
			}
				
			LOG_INFO("header: %s len: %d", id, len);

			if (format == WAVE && !memcmp(ptr, "data", 4)) {
				ptr += 8;
				_buf_inc_readp(streambuf, ptr - streambuf->readp);
				audio_left = len;

				if ((audio_left == 0xFFFFFFFF) || (audio_left == 0x7FFFEFFC)) {
					LOG_INFO("wav audio size unknown: %u", audio_left);
					limit = false;
				} else {
					LOG_INFO("wav audio size: %u", audio_left);
					limit = true;
				}
				return;
			}

			if (format == AIFF && !memcmp(ptr, "SSND", 4) && bytes >= 16) {
				unsigned offset = *(ptr+8) << 24 | *(ptr+9) << 16 | *(ptr+10) << 8 | *(ptr+11);
				// following 4 bytes is blocksize - ignored
				ptr += 8 + 8;
				_buf_inc_readp(streambuf, ptr + offset - streambuf->readp);
				
				// Reading from an upsampled stream, length could be wrong.
				// Only use length in header for files.
				if (stream.state == STREAMING_FILE) {
					audio_left = len - 8 - offset;
					LOG_INFO("aif audio size: %u", audio_left);
					limit = true;
				}
				return;
			}

			if (format == WAVE && !memcmp(ptr, "fmt ", 4) && bytes >= 24) {
				// override the server parsed values with our own
				channels    = *(ptr+10) | *(ptr+11) << 8;
				sample_rate = *(ptr+12) | *(ptr+13) << 8 | *(ptr+14) << 16 | *(ptr+15) << 24;
				sample_size = (*(ptr+22) | *(ptr+23) << 8) / 8;
				bigendian   = 0;
				LOG_INFO("pcm size: %u rate: %u chan: %u bigendian: %u", sample_size, sample_rate, channels, bigendian);
			}

			if (format == AIFF && !memcmp(ptr, "COMM", 4) && bytes >= 26) {
				int exponent;
				// override the server parsed values with our own
				channels    = *(ptr+8) << 8 | *(ptr+9);
				sample_size = (*(ptr+14) << 8 | *(ptr+15)) / 8;
				bigendian   = 1;
				// sample rate is encoded as IEEE 80 bit extended format
				// make some assumptions to simplify processing - only use first 32 bits of mantissa
				exponent = ((*(ptr+16) & 0x7f) << 8 | *(ptr+17)) - 16383 - 31;
				sample_rate  = *(ptr+18) << 24 | *(ptr+19) << 16 | *(ptr+20) << 8 | *(ptr+21);
				while (exponent < 0) { sample_rate >>= 1; ++exponent; }
				while (exponent > 0) { sample_rate <<= 1; --exponent; }
				LOG_INFO("pcm size: %u rate: %u chan: %u bigendian: %u", sample_size, sample_rate, channels, bigendian);
			}

			if (bytes >= len + 8) {
				ptr   += len + 8;
				bytes -= (len + 8);
			} else {
				LOG_WARN("run out of data");
				return;
			}
		}

	} else {
		LOG_WARN("unknown format - can't parse header");
	}
}

static decode_state pcm_decode(void) {
	unsigned bytes, in, out;
	frames_t frames, count;
	OPTR_T *optr;
	u8_t  *iptr;
	u8_t tmp[3*8];
	
	LOCK_S;

	if ( decode.new_stream && ( ( stream.state == STREAMING_FILE ) || pcm_check_header ) ) {
		_check_header();
	}

	LOCK_O_direct;

	bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));

	IF_DIRECT(
		out = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
	);
	IF_PROCESS(
		out = process.max_in_frames;
	);

	if ((stream.state <= DISCONNECT && bytes < bytes_per_frame) || (limit && audio_left == 0)) {
		UNLOCK_O_direct;
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	if (decode.new_stream) {
		LOG_INFO("setting track_start");
		LOCK_O_not_direct;
		output.track_start = outputbuf->writep;
		decode.new_stream = false;
#if DSD
		if (sample_size == 3 &&
			is_stream_dop(((u8_t *)streambuf->readp) + (bigendian?0:2),
						  ((u8_t *)streambuf->readp) + (bigendian?0:2) + sample_size,
						  sample_size * channels, bytes / (sample_size * channels))) {
			LOG_INFO("file contains DOP");
			if (output.dsdfmt == DOP_S24_LE || output.dsdfmt == DOP_S24_3LE)
				output.next_fmt = output.dsdfmt;
			else
				output.next_fmt = DOP;
			output.next_sample_rate = sample_rate;
			output.fade = FADE_INACTIVE;
		} else {
			output.next_sample_rate = decode_newstream(sample_rate, output.supported_rates);
			output.next_fmt = PCM;
			if (output.fade_mode) _checkfade(true);
		}
#else
		output.next_sample_rate = decode_newstream(sample_rate, output.supported_rates);
		if (output.fade_mode) _checkfade(true);
#endif
		UNLOCK_O_not_direct;
		IF_PROCESS(
			out = process.max_in_frames;
		);
		bytes_per_frame = channels * sample_size;
	}

	IF_DIRECT(
		optr = (OPTR_T *)outputbuf->writep;
	);
	IF_PROCESS(
		optr = (OPTR_T *)process.inbuf;
	);
	iptr = (u8_t *)streambuf->readp;

	in = bytes / bytes_per_frame;

	//  handle frame wrapping round end of streambuf
	//  - only need if resizing of streambuf does not avoid this, could occur in localfile case
	if (in == 0 && bytes > 0 && _buf_used(streambuf) >= bytes_per_frame) {
		memcpy(tmp, iptr, bytes);
		memcpy(tmp + bytes, streambuf->buf, bytes_per_frame - bytes);
		iptr = tmp;
		in = 1;
	}

	frames = min(in, out);
	frames = min(frames, MAX_DECODE_FRAMES);

	if (limit && frames * bytes_per_frame > audio_left) {
		LOG_INFO("reached end of audio");
		frames = audio_left / bytes_per_frame;
	}

	count = frames * channels;

	if (channels == 2) {
		if (sample_size == 1) {
			while (count--) {
				*optr++ = *iptr++ << (24-SHIFT);
			}
		} else if (sample_size == 2) {
			if (bigendian) {
#if BYTES_PER_FRAME == 4 && !SL_LITTLE_ENDIAN			
				// while loop below works as is, but memcpy is a win for that 16/16 typical case
				memcpy(optr, iptr, count * BYTES_PER_FRAME / 2);
#else				
				while (count--) {
					*optr++ = *(iptr) << (24-SHIFT) | *(iptr+1) << (16-SHIFT);
					iptr += 2;
				}
#endif				
			} else {
#if BYTES_PER_FRAME == 4 && SL_LITTLE_ENDIAN			
				// while loop below works as is, but memcpy is a win for that 16/16 typical case
				memcpy(optr, iptr, count * BYTES_PER_FRAME / 2);
#else
				while (count--) {
					*optr++ = *(iptr) << (16-SHIFT) | *(iptr+1) << (24-SHIFT);
					iptr += 2;
				}
#endif	
			}
		} else if (sample_size == 3) {
			if (bigendian) {
				while (count--) {
#if BYTES_PER_FRAME == 4				
					*optr++ = *(iptr) << 8 | *(iptr+1);
#else					
					*optr++ = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8;
#endif	
					iptr += 3;
				}
			} else {
				while (count--) {
#if BYTES_PER_FRAME == 4									
					*optr++ = *(iptr+1) | *(iptr+2) << 8;
#else
					*optr++ = *(iptr) << 8 | *(iptr+1) << 16 | *(iptr+2) << 24;
#endif	
					iptr += 3;
				}
			}
		} else if (sample_size == 4) {
			if (bigendian) {
				while (count--) {
#if BYTES_PER_FRAME == 4														
					*optr++ = *(iptr) << 8 | *(iptr+1);
#else
					*optr++ = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8 | *(iptr+3);
#endif	
					iptr += 4;
				}
			} else {
				while (count--) {
#if BYTES_PER_FRAME == 4																			
					*optr++ = *(iptr+2) | *(iptr+3) << 8;
#else
					*optr++ = *(iptr) | *(iptr+1) << 8 | *(iptr+2) << 16 | *(iptr+3) << 24;
#endif	
					iptr += 4;
				}
			}
		}
	} else if (channels == 1) {
		if (sample_size == 1) {
			while (count--) {
				*optr = *iptr++ << (24-SHIFT);
				*(optr+1) = *optr;
				optr += 2;
			}
		} else if (sample_size == 2) {
			if (bigendian) {
				while (count--) {
					*optr = *(iptr) << (24-SHIFT) | *(iptr+1) << (16-SHIFT);
					*(optr+1) = *optr;
					iptr += 2;
					optr += 2;
				}
			} else {
				while (count--) {
					*optr = *(iptr) << (16-SHIFT) | *(iptr+1) << (24-SHIFT);
					*(optr+1) = *optr;
					iptr += 2;
					optr += 2;
				}
			}
		} else if (sample_size == 3) {
			if (bigendian) {
				while (count--) {
#if BYTES_PER_FRAME == 4				
					*optr++ = *(iptr) << 8 | *(iptr+1);
#else					
					*optr = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8;
#endif				
					*(optr+1) = *optr;
					iptr += 3;
					optr += 2;
				}
			} else {
				while (count--) {
#if BYTES_PER_FRAME == 4														
					*optr++ = *(iptr+1) | *(iptr+2) << 8;
#else					
					*optr = *(iptr) << 8 | *(iptr+1) << 16 | *(iptr+2) << 24;
#endif				
					*(optr+1) = *optr;
					iptr += 3;
					optr += 2;
				}
			}
		} else if (sample_size == 4) {
			if (bigendian) {
				while (count--) {
#if BYTES_PER_FRAME == 4														
					*optr++ = *(iptr) << 8 | *(iptr+1);
#else					
					*optr++ = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8 | *(iptr+3);
#endif				
					*(optr+1) = *optr;
					iptr += 4;
					optr += 2;
				}
			} else {
				while (count--) {
#if BYTES_PER_FRAME == 4																			
					*optr++ = *(iptr+2) | *(iptr+3) << 8;
#else					
					*optr++ = *(iptr) | *(iptr+1) << 8 | *(iptr+2) << 16 | *(iptr+3) << 24;
#endif				
					*(optr+1) = *optr;
					iptr += 4;
					optr += 2;
				}
			}
		}
	} else {
		LOG_ERROR("unsupported channels");
	}

	LOG_SDEBUG("decoded %u frames", frames);

	_buf_inc_readp(streambuf, frames * bytes_per_frame);

	if (limit) {
		audio_left -= frames * bytes_per_frame;
	}

	IF_DIRECT(
		_buf_inc_writep(outputbuf, frames * BYTES_PER_FRAME);
	);
	IF_PROCESS(
		process.in_frames = frames;
	);

	UNLOCK_O_direct;
	UNLOCK_S;

	return DECODE_RUNNING;
}

static void pcm_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	sample_size = size - '0' + 1;
	sample_rate = sample_rates[rate - '0'];
	channels    = chan - '0';
	bigendian   = (endianness == '0');
	limit       = false;

	LOG_INFO("pcm size: %u rate: %u chan: %u bigendian: %u", sample_size, sample_rate, channels, bigendian);
	buf_adjust(streambuf, sample_size * channels);
}

static void pcm_close(void) {
	buf_adjust(streambuf, 1);
}

struct codec *register_pcm(void) {
	if ( pcm_check_header )
	{
		static struct codec ret = { 
			'p',         // id
			"wav,aif,pcm", // types
			4096,        // min read
			102400,      // min space
			pcm_open,    // open
			pcm_close,   // close
			pcm_decode,  // decode
		};

		LOG_INFO("using pcm to decode wav,aif,pcm");
		return &ret;
	}
	else
	{
		static struct codec ret = { 
			'p',         // id
			"aif,pcm", // types
			4096,        // min read
			102400,      // min space
			pcm_open,    // open
			pcm_close,   // close
			pcm_decode,  // decode
		};

		LOG_INFO("using pcm to decode aif,pcm");
		return &ret;
	}

	return NULL;
}

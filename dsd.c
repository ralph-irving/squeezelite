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

// dsd support

#include "squeezelite.h"

#if DSD

// use dsd2pcm from Sebastian Gesemann for conversion to pcm:
#include "./dsd2pcm/dsd2pcm.h"

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

#define BLOCK 4096 // expected size of dsd block
#define BLOCK_FRAMES BLOCK * BYTES_PER_FRAME
#define WRAP_BUF_SIZE 32 // max 4 bytes per frame and 8 channels

typedef enum { UNKNOWN=0, DSF, DSDIFF } dsd_type;

static dsd_format outfmt = PCM; // local copy of output.dsdfmt to avoid holding output lock

struct dsd {
	dsd_type type;
	u32_t consume;
	u32_t sample_rate;
	u32_t channels;
	u64_t sample_bytes;
	u32_t block_size;
	bool  lsb_first;
	dsd2pcm_ctx *dsd2pcm_ctx[2];
	float *transfer[2];
};

static struct dsd *d;

static u64_t unpack64be(const u8_t *p) {
	return 
		(u64_t)p[0] << 56 | (u64_t)p[1] << 48 | (u64_t)p[2] << 40 | (u64_t)p[3] << 32 |
		(u64_t)p[4] << 24 | (u64_t)p[5] << 16 |	(u64_t)p[6] << 8 | (u64_t)p[7];
}

static u64_t unpack64le(const u8_t *p) {
	return
		(u64_t)p[7] << 56 | (u64_t)p[6] << 48 | (u64_t)p[5] << 40 | (u64_t)p[4] << 32 |
		(u64_t)p[3] << 24 | (u64_t)p[2] << 16 |	(u64_t)p[1] << 8 | (u64_t)p[0];
}

static u32_t unpack32le(const u8_t *p) {
	return
		(u32_t)p[3] << 24 | (u32_t)p[2] << 16 |	(u32_t)p[1] << 8 | (u32_t)p[0];
}

static int _read_header(void) {
	unsigned bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	s32_t consume;

	if (!d->type && bytes >= 4) {
		if (!memcmp(streambuf->readp, "FRM8", 4)) {
			d->type = DSDIFF;
		} else if (!memcmp(streambuf->readp, "DSD ", 4)) {
			d->type = DSF;
		} else {
			LOG_WARN("bad type");
			return -1;
		}
	}

	while (bytes >= 16) {
		char id[5];
		u64_t len = d->type == DSDIFF ? unpack64be(streambuf->readp + 4) : unpack64le(streambuf->readp + 4);
		memcpy(id, streambuf->readp, 4);
		id[4] = '\0';
		consume = 0;

		if (d->type == DSDIFF) {
			if (!strcmp(id, "FRM8")) {
				if (!memcmp(streambuf->readp + 12, "DSD ", 4)) {
					consume = 16; // read into
				} else {
					LOG_WARN("bad dsdiff FRM8");
					return -1;
				}
			}
			if (!strcmp(id, "PROP") && !memcmp(streambuf->readp + 12, "SND ", 4)) {
				consume = 16; // read into
			}
			if (!strcmp(id, "FVER")) {
				LOG_INFO("DSDIFF version: %u.%u.%u.%u", *(streambuf->readp + 12), *(streambuf->readp + 13),
					 *(streambuf->readp + 14), *(streambuf->readp + 15));
			}
			if (!strcmp(id, "FS  ")) {
				d->sample_rate = unpackN((void *)(streambuf->readp + 12));
				LOG_INFO("sample rate: %u", d->sample_rate);
			}
			if (!strcmp(id, "CHNL")) {
				d->channels = unpackn((void *)(streambuf->readp + 12));
				LOG_INFO("channels: %u", d->channels);
			}
			if (!strcmp(id, "DSD ")) {
				LOG_INFO("found dsd len: " FMT_u64, len);
				d->sample_bytes = len;
				_buf_inc_readp(streambuf, 12);
				bytes  -= 12;
				return 1; // got to the audio
			}
		}

		if (d->type == DSF) {
			if (!strcmp(id, "fmt ")) {
				if (bytes >= len && bytes >= 52) {
					u32_t version = unpack32le((void *)(streambuf->readp + 12));
					u32_t format  = unpack32le((void *)(streambuf->readp + 16));
					LOG_INFO("DSF version: %u format: %u", version, format);
					if (format != 0) {
						LOG_WARN("only support DSD raw format");
						return -1;
					}
					d->channels = unpack32le((void *)(streambuf->readp + 24));
					d->sample_rate = unpack32le((void *)(streambuf->readp + 28));
					d->lsb_first = (unpack32le((void *)(streambuf->readp + 32)) == 1);
					d->sample_bytes = unpack64le((void *)(streambuf->readp + 36)) / 8;
					d->block_size = unpack32le((void *)(streambuf->readp + 44));
					LOG_INFO("channels: %u", d->channels);
					LOG_INFO("sample rate: %u", d->sample_rate);
					LOG_INFO("lsb first: %u", d->lsb_first);
					LOG_INFO("sample bytes: " FMT_u64, d->sample_bytes);
					LOG_INFO("block size: %u", d->block_size);
				} else {
					consume = -1; // come back later
				}
			}
			if (!strcmp(id, "data")) {
				LOG_INFO("found dsd len: " FMT_u64, len);
				_buf_inc_readp(streambuf, 12);
				bytes  -= 12;
				return 1; // got to the audio
			}
		}

		// default to consuming whole chunk
		if (!consume) {
			consume = (s32_t)((d->type == DSDIFF) ? len + 12 : len);
		}

		if (bytes >= consume) {
			LOG_DEBUG("id: %s len: " FMT_u64 " consume: %d", id, len, consume);
			_buf_inc_readp(streambuf, consume);
			bytes  -= consume;
		} else if (consume > 0) {
			LOG_DEBUG("id: %s len: " FMT_u64 " consume: %d - partial consume: %u", id, len, consume, bytes);
			_buf_inc_readp(streambuf, bytes);
			d->consume = consume - bytes;
			break;
		} else {
			break;
		}
	}

	return 0;
}

static decode_state _decode_dsf(void) {

	// samples in streambuf are interleaved on block basis
	// we transfer whole blocks for all channels in one call and so itterate the while loop below to handle wraps
	
	unsigned bytes = _buf_used(streambuf);
	unsigned block_left = d->block_size;
	unsigned padding = 0;
	
	unsigned bytes_per_frame;
	switch (outfmt) {
	case DSD_U32_LE:
	case DSD_U32_BE:
		bytes_per_frame = 4;
		break;
	case DSD_U16_LE:
	case DSD_U16_BE:
	case DOP:
	case DOP_S24_LE:
	case DOP_S24_3LE:
		bytes_per_frame = 2;
		break;
	default:
		bytes_per_frame = 1;
	}
	
	if (bytes < d->block_size * d->channels) {
		LOG_INFO("stream too short"); // this can occur when scanning the track
		return DECODE_COMPLETE;
	}
	
	IF_PROCESS(
		process.in_frames = 0;
	);

	while (block_left) {
		
		frames_t frames, out, count;
		unsigned bytes_read;
		
		u8_t *iptrl = (u8_t *)streambuf->readp;
		u8_t *iptrr = (u8_t *)streambuf->readp + d->block_size;
		u32_t *optr;
		
		if (iptrr >= streambuf->wrap) {
			iptrr -= streambuf->size;
		}
		
		// Remove zero padding from last block in case of inaccurate sample count 
		if ((_buf_used(streambuf) == d->block_size * d->channels)
			&& (d->sample_bytes > _buf_used(streambuf))) {
			int i;
			u8_t *ipl, *ipr;
			for (i = d->block_size - 1; i > 0; i--) {
				ipl = iptrl + i;
				if (ipl >= streambuf->wrap) ipl -= streambuf->size;
				ipr = iptrr + i;	
				if (ipr >= streambuf->wrap) ipr -= streambuf->size;
				if (*ipl || *ipr) break;
				padding++;
			}
			block_left -= padding;
		}

		bytes = min(block_left, min(streambuf->wrap - iptrl, streambuf->wrap - iptrr));

		IF_DIRECT(
			out = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
			optr = (u32_t *)outputbuf->writep;
		);
		IF_PROCESS(
			out = process.max_in_frames - process.in_frames;
			optr = (u32_t *)(process.inbuf + process.in_frames * BYTES_PER_FRAME);
		);

		frames = min(bytes, d->sample_bytes) / bytes_per_frame;
		if (frames == 0) {
			if (d->sample_bytes && bytes >= (2 * d->sample_bytes)) {
				// byte(s) left fill frame with silence byte(s) and play
				int i;
				for (i = d->sample_bytes; i < bytes_per_frame; i++)
					*(iptrl + i) = *(iptrr + i) = 0x69;
				frames = 1;
			} else {
				// should not get here due to wrapping m/2 for dsd should never result in 0 as header len is always even
				LOG_INFO("frames got to zero");
				return DECODE_COMPLETE;
			}
		}

		frames = min(frames, out);
		frames = min(frames, BLOCK);
		bytes_read = frames * bytes_per_frame;
		
		count = frames;
		
		switch (outfmt) {
			
		case DSD_U32_LE:
		case DSD_U32_BE:
			
			if (d->channels == 1) {
				if (d->lsb_first) {
					while (count--) {
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24 | dsd2pcm_bitreverse[*(iptrl+1)] << 16
							| dsd2pcm_bitreverse[*(iptrl+2)] << 8 | dsd2pcm_bitreverse[*(iptrl+3)];
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24 | dsd2pcm_bitreverse[*(iptrl+1)] << 16
							| dsd2pcm_bitreverse[*(iptrl+2)] << 8 | dsd2pcm_bitreverse[*(iptrl+3)];
						iptrl += 4;
					}
				} else {
					while (count--) {
						*(optr++) = *(iptrl) << 24 | *(iptrl+1) << 16 | *(iptrl+2) << 8 | *(iptrl+3);
						*(optr++) = *(iptrl) << 24 | *(iptrl+1) << 16 | *(iptrl+2) << 8 | *(iptrl+3);
						iptrl += 4;
					}
				}
			} else {
				if (d->lsb_first) {
					while (count--) {
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24 | dsd2pcm_bitreverse[*(iptrl+1)] << 16
							| dsd2pcm_bitreverse[*(iptrl+2)] << 8 | dsd2pcm_bitreverse[*(iptrl+3)];
						*(optr++) = dsd2pcm_bitreverse[*(iptrr)] << 24 | dsd2pcm_bitreverse[*(iptrr+1)] << 16
							| dsd2pcm_bitreverse[*(iptrr+2)] << 8 | dsd2pcm_bitreverse[*(iptrr+3)];
						iptrl += 4;
						iptrr += 4;
					}
				} else {
					while (count--) {
						*(optr++) = *(iptrl) << 24 | *(iptrl+1) << 16 | *(iptrl+2) << 8 | *(iptrl+3);
						*(optr++) = *(iptrr) << 24 | *(iptrr+1) << 16 | *(iptrr+2) << 8 | *(iptrr+3);
						iptrl += 4;
						iptrr += 4;
					}
				}
			}
			
			break;

		case DSD_U16_LE:
		case DSD_U16_BE:
			
			if (d->channels == 1) {
				if (d->lsb_first) {
					while (count--) {
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24 | dsd2pcm_bitreverse[*(iptrl+1)] << 16;
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24 | dsd2pcm_bitreverse[*(iptrl+1)] << 16;
						iptrl += 2;
					}
				} else {
					while (count--) {
						*(optr++) = *(iptrl) << 24 | *(iptrl+1) << 16;
						*(optr++) = *(iptrl) << 24 | *(iptrl+1) << 16;
						iptrl += 2;
					}
				}
			} else {
				if (d->lsb_first) {
					while (count--) {
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24 | dsd2pcm_bitreverse[*(iptrl+1)] << 16;
						*(optr++) = dsd2pcm_bitreverse[*(iptrr)] << 24 | dsd2pcm_bitreverse[*(iptrr+1)] << 16;
						iptrl += 2;
						iptrr += 2;
					}
				} else {
					while (count--) {
						*(optr++) = *(iptrl) << 24 | *(iptrl+1) << 16;
						*(optr++) = *(iptrr) << 24 | *(iptrr+1) << 16;
						iptrl += 2;
						iptrr += 2;
					}
				}
			}
			
			break;

		case DSD_U8:
			
			if (d->channels == 1) {
				if (d->lsb_first) {
					while (count--) {
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24;
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24;
						iptrl += 1;
					}
				} else {
					while (count--) {
						*(optr++) = *(iptrl) << 24;
						*(optr++) = *(iptrl) << 24;
						iptrl += 1;
					}
				}
			} else {
				if (d->lsb_first) {
					while (count--) {
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 24;
						*(optr++) = dsd2pcm_bitreverse[*(iptrr)] << 24;
						iptrl += 1;
						iptrr += 1;
					}
				} else {
					while (count--) {
						*(optr++) = *(iptrl) << 24;
						*(optr++) = *(iptrr) << 24;
						iptrl += 1;
						iptrr += 1;
					}
				}
			}
			
			break;

		case DOP:
		case DOP_S24_LE:
		case DOP_S24_3LE:
			
			if (d->channels == 1) {
				if (d->lsb_first) {
					while (count--) {
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 16 | dsd2pcm_bitreverse[*(iptrl+1)] << 8;
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 16 | dsd2pcm_bitreverse[*(iptrl+1)] << 8;
						iptrl += 2;
					}
				} else {
					while (count--) {
						*(optr++) = *(iptrl) << 16 | *(iptrl+1) << 8;
						*(optr++) = *(iptrl) << 16 | *(iptrl+1) << 8;
						iptrl += 2;
					}
				}
			} else {
				if (d->lsb_first) {
					while (count--) {
						*(optr++) = dsd2pcm_bitreverse[*(iptrl)] << 16 | dsd2pcm_bitreverse[*(iptrl+1)] << 8;
						*(optr++) = dsd2pcm_bitreverse[*(iptrr)] << 16 | dsd2pcm_bitreverse[*(iptrr+1)] << 8;
						iptrl += 2;
						iptrr += 2;
					}
				} else {
					while (count--) {
						*(optr++) = *(iptrl) << 16 | *(iptrl+1) << 8;
						*(optr++) = *(iptrr) << 16 | *(iptrr+1) << 8;
						iptrl += 2;
						iptrr += 2;
					}
				}
			}
			
			break;

		case PCM:
			
			if (d->channels == 1) {
				float *iptrf = d->transfer[0];
				dsd2pcm_translate(d->dsd2pcm_ctx[0], frames, iptrl, 1, d->lsb_first, iptrf, 1);
				while (count--) {
					double scaled = *iptrf++ * 0x7fffffff;
					if (scaled >  2147483647.0) scaled =  2147483647.0;
					if (scaled < -2147483648.0) scaled = -2147483648.0;
					*optr++ = (s32_t)scaled;
					*optr++ = (s32_t)scaled;
				}
			} else {
				float *iptrfl = d->transfer[0];
				float *iptrfr = d->transfer[1];
				dsd2pcm_translate(d->dsd2pcm_ctx[0], frames, iptrl, 1, d->lsb_first, iptrfl, 1);
				dsd2pcm_translate(d->dsd2pcm_ctx[1], frames, iptrr, 1, d->lsb_first, iptrfr, 1);
				while (count--) {
					double scaledl = *iptrfl++ * 0x7fffffff;
					double scaledr = *iptrfr++ * 0x7fffffff;
					if (scaledl >  2147483647.0) scaledl =  2147483647.0;
					if (scaledl < -2147483648.0) scaledl = -2147483648.0;
					if (scaledr >  2147483647.0) scaledr =  2147483647.0;
					if (scaledr < -2147483648.0) scaledr = -2147483648.0;
					*optr++ = (s32_t)scaledl;
					*optr++ = (s32_t)scaledr;
				}
			}
			
			break;
			
		}
		
		_buf_inc_readp(streambuf, bytes_read);
		
		block_left -= bytes_read;
		
		if (d->sample_bytes > bytes_read) {
			d->sample_bytes -= bytes_read;
		} else {
			LOG_INFO("end of track samples");
			block_left = 0;
			d->sample_bytes = 0;
		}
		
		IF_DIRECT(
			_buf_inc_writep(outputbuf, frames * BYTES_PER_FRAME);
		);
		IF_PROCESS(
			process.in_frames += frames;
		);

		LOG_SDEBUG("write %u frames", frames);
	}
	
	if (padding) {
		_buf_inc_readp(streambuf, padding);
		LOG_INFO("Zero padding removed: %u bytes", padding);
	}
	
	// skip the other channel blocks
	// the right channel has already been read and is guarenteed to be in streambuf so can be skipped immediately
	if (d->channels > 1) {
		_buf_inc_readp(streambuf, d->block_size);
	}
	if (d->channels > 2) {
		d->consume = d->block_size * (d->channels - 2);
	}

	return DECODE_RUNNING;
}

static decode_state _decode_dsdiff(void) {

	// samples in streambuf are interleaved on byte per channel
	// we process as little as necessary per call and only need to handle frames wrapping round streambuf

	unsigned bytes_per_frame, bytes_read;
	frames_t out, frames, count;
	u8_t *iptr;
	u32_t *optr;
	u8_t tmp[WRAP_BUF_SIZE];
	
	unsigned bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	
	IF_DIRECT(
		out = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
	);
	IF_PROCESS(
		out = process.max_in_frames;
	);
	
	switch (outfmt) {
	case DSD_U32_LE:
	case DSD_U32_BE:
		bytes_per_frame = d->channels * 4;
		break;
	case DSD_U16_LE:
	case DSD_U16_BE:
	case DOP:
	case DOP_S24_LE:
	case DOP_S24_3LE:
		bytes_per_frame = d->channels * 2;
		break;
	default:
		bytes_per_frame = d->channels;
		out = min(out, BLOCK);
	}
	
	frames = min(min(bytes, d->sample_bytes) / bytes_per_frame, out);
	bytes_read = frames * bytes_per_frame;
	
	iptr = (u8_t *)streambuf->readp;
	
	IF_DIRECT(
		optr = (u32_t *)outputbuf->writep;
	);
	IF_PROCESS(
		optr = (u32_t *)process.inbuf;
	);
	
	// handle wrap around end of streambuf and partial dsd frame at end of stream
	if (!frames && bytes < bytes_per_frame) {
		memset(tmp, 0x69, WRAP_BUF_SIZE); // 0x69 = dsd silence
		memcpy(tmp, streambuf->readp, bytes);
		if (_buf_used(streambuf) > bytes_per_frame) {
			memcpy(tmp + bytes, streambuf->buf, bytes_per_frame - bytes);
			bytes_read = bytes_per_frame;
		} else {
			bytes_read = bytes;
		}
		iptr = tmp;
		frames = 1;
	}
	
	count = frames;
	
	switch (outfmt) {

	case DSD_U32_LE:
	case DSD_U32_BE:
		
		if (d->channels == 1) {
			while (count--) {
				*(optr++) = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8 | *(iptr+3);
				*(optr++) = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8 | *(iptr+3);
				iptr += bytes_per_frame;
			}
		} else {
			while (count--) {
				*(optr++) = *(iptr  ) << 24 | *(iptr + d->channels)     << 16
					| *(iptr + 2 * d->channels)     << 8 | *(iptr + 3 * d->channels);
				*(optr++) = *(iptr+1) << 24 | *(iptr + d->channels + 1) << 16
					| *(iptr + 2 * d->channels + 1) << 8 | *(iptr + 3 * d->channels + 1);
				iptr += bytes_per_frame;
			}
		}
		
		break;
		
	case DSD_U16_LE:
	case DSD_U16_BE:
		
		if (d->channels == 1) {
			while (count--) {
				*(optr++) = *(iptr) << 24 | *(iptr+1) << 16;
				*(optr++) = *(iptr) << 24 | *(iptr+1) << 16;
				iptr += bytes_per_frame;
			}
		} else {
			while (count--) {
				*(optr++) = *(iptr  ) << 24 | *(iptr + d->channels)     << 16;
				*(optr++) = *(iptr+1) << 24 | *(iptr + d->channels + 1) << 16;
				iptr += bytes_per_frame;
			}
		}
		
		break;
		
	case DSD_U8:
		
		if (d->channels == 1) {
			while (count--) {
				*(optr++) = *(iptr) << 24;
				*(optr++) = *(iptr) << 24;
				iptr += bytes_per_frame;
			}
		} else {
			while (count--) {
				*(optr++) = *(iptr  ) << 24;
				*(optr++) = *(iptr+1) << 24;
				iptr += bytes_per_frame;
			}
		}
		
		break;
		
	case DOP:
	case DOP_S24_LE:
	case DOP_S24_3LE:
		
		if (d->channels == 1) {
			while (count--) {
				*(optr++) = *(iptr) << 16 | *(iptr+1) << 8;
				*(optr++) = *(iptr) << 16 | *(iptr+1) << 8;
				iptr += bytes_per_frame;
			}
		} else {
			while (count--) {
				*(optr++) = *(iptr  ) << 16 | *(iptr + d->channels)     << 8;
				*(optr++) = *(iptr+1) << 16 | *(iptr + d->channels + 1) << 8;
				iptr += bytes_per_frame;
			}
		}
		
		break;
		
	case PCM:
		
		if (d->channels == 1) {
			float *iptrf = d->transfer[0];
			dsd2pcm_translate(d->dsd2pcm_ctx[0], frames, iptr, 1, 0, iptrf, 1);
			while (count--) {
				double scaled = *iptrf++ * 0x7fffffff;
				if (scaled >  2147483647.0) scaled =  2147483647.0;
				if (scaled < -2147483648.0) scaled = -2147483648.0;
				*optr++ = (s32_t)scaled;
				*optr++ = (s32_t)scaled;
			}
		} else {
			float *iptrfl = d->transfer[0];
			float *iptrfr = d->transfer[1];
			dsd2pcm_translate(d->dsd2pcm_ctx[0], frames, iptr,     d->channels, 0, iptrfl, 1);
			dsd2pcm_translate(d->dsd2pcm_ctx[1], frames, iptr + 1, d->channels, 0, iptrfr, 1);
			while (count--) {
				double scaledl = *iptrfl++ * 0x7fffffff;
				double scaledr = *iptrfr++ * 0x7fffffff;
				if (scaledl >  2147483647.0) scaledl =  2147483647.0;
				if (scaledl < -2147483648.0) scaledl = -2147483648.0;
				if (scaledr >  2147483647.0) scaledr =  2147483647.0;
				if (scaledr < -2147483648.0) scaledr = -2147483648.0;
				*optr++ = (s32_t)scaledl;
				*optr++ = (s32_t)scaledr;
			}
		}

		break;
		
	}
	
	_buf_inc_readp(streambuf, bytes_read);
	
	if (d->sample_bytes > bytes_read) {
		d->sample_bytes -= bytes_read;
	} else {
		LOG_INFO("end of track samples");
		d->sample_bytes = 0;
	}
	
	IF_DIRECT(
		_buf_inc_writep(outputbuf, frames * BYTES_PER_FRAME);
			  );
	IF_PROCESS(
		process.in_frames = frames;
	);
	
	LOG_SDEBUG("write %u frames", frames);

	return DECODE_RUNNING;
}


static decode_state dsd_decode(void) {
	decode_state ret;
	char *fmtstr;

	fmtstr = "None";

	LOCK_S;

	if ((stream.state <= DISCONNECT && !_buf_used(streambuf)) || (!decode.new_stream && d->sample_bytes == 0)) {
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	if (d->consume) {
		unsigned consume = min(d->consume, min(_buf_used(streambuf), _buf_cont_read(streambuf)));
		LOG_DEBUG("consume: %u of %u", consume, d->consume);
		_buf_inc_readp(streambuf, consume);
		d->consume -= consume;
		if (d->consume) {
			UNLOCK_S;
			return DECODE_RUNNING;
		}
	}

	if (decode.new_stream) {
		int r = _read_header();
		if (r < 1) {
			UNLOCK_S;
			return DECODE_ERROR;
		}
		if (r == 0) {
			UNLOCK_S;
			return DECODE_RUNNING;
		}
		// otherwise got to start of audio

		LOCK_O;

		LOG_INFO("setting track_start");
		output.track_start = outputbuf->writep;

		outfmt = output.dsdfmt;

		switch (outfmt) {
		case DSD_U32_LE:
			fmtstr = "DSD_U32_LE";
			output.next_sample_rate = d->sample_rate / 32;
			break;
		case DSD_U32_BE:
			fmtstr = "DSD_U32_BE";
			output.next_sample_rate = d->sample_rate / 32;
			break;
		case DSD_U16_LE:
			fmtstr = "DSD_U16_LE";
			output.next_sample_rate = d->sample_rate / 16;
			break;
		case DSD_U16_BE:
			fmtstr = "DSD_U16_BE";
			output.next_sample_rate = d->sample_rate / 16;
			break;
		case DSD_U8:
			fmtstr = "DSD_U8";
			output.next_sample_rate = d->sample_rate / 8;
			break;
		case DOP:
			fmtstr = "DOP";
			output.next_sample_rate = d->sample_rate / 16;
			break;
		case DOP_S24_LE:
			fmtstr = "DOP_S24_LE";
			output.next_sample_rate = d->sample_rate / 16;
			break;
		case DOP_S24_3LE:
			fmtstr = "DOP_S24_3LE";
			output.next_sample_rate = d->sample_rate / 16;
			break;
		case PCM:
			// PCM case after DSD rate check and possible fallback to PCM conversion 
			break;
		}

		if (outfmt != PCM && output.next_sample_rate > output.supported_rates[0]) {
			LOG_INFO("DSD sample rate too high for device - converting to PCM");
			outfmt = PCM;
		}
		
		if (outfmt == PCM) {
			LOG_INFO("DSD to PCM output");
			output.next_sample_rate = decode_newstream(d->sample_rate / 8, output.supported_rates);
			if (output.fade_mode) _checkfade(true);
		} else {
			LOG_INFO("DSD%u stream, format: %s, rate: %uHz\n", d->sample_rate / 44100, fmtstr, output.next_sample_rate);
			output.fade = FADE_INACTIVE;
		}

		output.next_fmt = outfmt;
		decode.new_stream = false;

		UNLOCK_O;
	}

	LOCK_O_direct;

	switch (d->type) {
	case DSF:
		ret = _decode_dsf();
		break;
	case DSDIFF:
		ret = _decode_dsdiff();
		break;
	default:
		ret = DECODE_ERROR;
	}

	UNLOCK_O_direct;
	UNLOCK_S;

	return ret;
}

void dsd_init(dsd_format format, unsigned delay) {
	LOCK_O;
	output.dsdfmt = format;
	output.dsd_delay = delay;
	UNLOCK_O;
}

static void dsd_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	d->type = UNKNOWN;

	if (!d->dsd2pcm_ctx[0]) {
		d->dsd2pcm_ctx[0] = dsd2pcm_init();
		d->dsd2pcm_ctx[1] = dsd2pcm_init();
	} else {
		dsd2pcm_reset(d->dsd2pcm_ctx[0]);
		dsd2pcm_reset(d->dsd2pcm_ctx[1]);
	}
	if (!d->transfer[1]) {
		d->transfer[0] = malloc(sizeof(float) * BLOCK);
		d->transfer[1] = malloc(sizeof(float) * BLOCK);
	}
}

static void dsd_close(void) {
	if (d->dsd2pcm_ctx[0]) {
		dsd2pcm_destroy(d->dsd2pcm_ctx[0]);
		dsd2pcm_destroy(d->dsd2pcm_ctx[1]);
		d->dsd2pcm_ctx[0] = NULL;
		d->dsd2pcm_ctx[1] = NULL;
	}
	if (d->transfer[0]) {
		free(d->transfer[0]);
		free(d->transfer[1]);
		d->transfer[0] = NULL;
		d->transfer[1] = NULL;
	}
}

struct codec *register_dsd(void) {
	static struct codec ret = { 
		'd',         // id
		"dsf,dff",   // types
		BLOCK * 2,   // min read
		BLOCK_FRAMES,// min space
		dsd_open,    // open
		dsd_close,   // close
		dsd_decode,  // decode
	};

	d = malloc(sizeof(struct dsd));
	if (!d) {
		return NULL;
	}

	memset(d, 0, sizeof(struct dsd));

	dsd2pcm_precalc();

	LOG_INFO("using dsd to decode dsf,dff");
	return &ret;
}

// invert polarity for frames in the output buffer
void dsd_invert(u32_t *ptr, frames_t frames) {
	while (frames--) {
		*ptr = ~(*ptr);
		++ptr;
		*ptr = ~(*ptr);
		++ptr;
	}
}

// fill silence buffer with 10101100 which represents dsd silence
void dsd_silence_frames(u32_t *ptr, frames_t frames) {
	while (frames--) {
		*ptr++ = 0x69696969;
		*ptr++ = 0x69696969;
	}
}

#endif // DSD

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

#if FFMPEG

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#define READ_SIZE  4096 * 4   // this is large enough to ensure ffmpeg always gets new data when decode is called
#define WRITE_SIZE 256 * 1024 // FIXME - make smaller, but still to absorb max wma output

// FIXME - do we need to align these params as per ffmpeg on i386? 
#define attribute_align_arg

struct ff_s {
	// state for ffmpeg decoder
	bool wma;
	u8_t wma_mmsh;
	u8_t wma_playstream;
	u8_t wma_metadatastream;
	u8_t *readbuf;
	bool end_of_stream;
	AVInputFormat *input_format;
	AVFormatContext *formatC;
	AVCodecContext *codecC;
	AVCodecParameters *codecP;
	AVFrame *frame;
	AVPacket *avpkt;
	unsigned mmsh_bytes_left;
	unsigned mmsh_bytes_pad;
	unsigned mmsh_packet_len;
#if !LINKALL
	// ffmpeg symbols to be dynamically loaded from libavcodec
	unsigned (* avcodec_version)(void);
	AVCodec * (* avcodec_find_decoder)(int);
	int attribute_align_arg (* avcodec_open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55,28,1)
	AVFrame * (* av_frame_alloc)(void);
	void (* av_frame_free)(AVFrame **);
#else
	AVFrame * (* avcodec_alloc_frame)(void);
	void (* avcodec_free_frame)(AVFrame **);
#endif
	int attribute_align_arg (* avcodec_decode_audio4)(AVCodecContext *, AVFrame *, int *, const AVPacket *);
	AVCodecContext * (* avcodec_alloc_context3)(const AVCodec *);
	void (* avcodec_free_context)(AVCodecContext **);
	int (* avcodec_parameters_to_context)(AVCodecContext *, const AVCodecParameters *);
	// ffmpeg symbols to be dynamically loaded from libavformat
	unsigned (* avformat_version)(void);
	AVFormatContext * (* avformat_alloc_context)(void);
	void (* avformat_free_context)(AVFormatContext *);
	int (* avformat_open_input)(AVFormatContext **, const char *, AVInputFormat *, AVDictionary **);
	int (* avformat_find_stream_info)(AVFormatContext *, AVDictionary **);
	AVIOContext * (* avio_alloc_context)(unsigned char *, int, int,	void *,
		int (*read_packet)(void *, uint8_t *, int), int (*write_packet)(void *, uint8_t *, int), int64_t (*seek)(void *, int64_t, int));
	void (* av_init_packet)(AVPacket *);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57,24,102)
	void (* av_packet_unref)(AVPacket *);
#else
	void (* av_free_packet)(AVPacket *);
#endif
	int (* av_read_frame)(AVFormatContext *, AVPacket *);
	AVInputFormat * (* av_find_input_format)(const char *);
	void (* av_register_all)(void);
	// ffmpeg symbols to be dynamically loaded from libavutil
	unsigned (* avutil_version)(void);
	void (* av_log_set_callback)(void (*)(void*, int, const char*, va_list));
	void (* av_log_set_level)(int);
	int  (* av_strerror)(int, char *, size_t);
	void * (* av_malloc)(size_t);
	void (* av_freep)(void *);
#endif
};

static struct ff_s *ff;

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
#define IF_DIRECT(x)    if (decode.direct) { x }
#define IF_PROCESS(x)   if (!decode.direct) { x }
#else
#define LOCK_O_direct   mutex_lock(outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#endif

#if LINKALL
#define AV(h, fn, ...)       (av_ ## fn)(__VA_ARGS__)
#define AVIO(h, fn, ...)     (avio_ ## fn)(__VA_ARGS__)
#define AVCODEC(h, fn, ...)  (avcodec_ ## fn)(__VA_ARGS__)
#define AVFORMAT(h, fn, ...) (avformat_ ## fn)(__VA_ARGS__)
#else
#define AV(h, fn, ...)       (h)->av_##fn(__VA_ARGS__)
#define AVIO(h, fn, ...)     (h)->avio_##fn(__VA_ARGS__)
#define AVCODEC(h, fn, ...)  (h)->avcodec_##fn(__VA_ARGS__)
#define AVFORMAT(h, fn, ...) (h)->avformat_##fn(__VA_ARGS__)
#endif


// our own version of useful error function not included in earlier ffmpeg versions
static char *av__err2str(int errnum) {
	static char buf[64];
	AV(ff, strerror, errnum, buf, 64); 
	return buf;
}

// parser to extract asf data packet length from asf header
const u8_t header_guid[16] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
const u8_t file_props_guid[16] = { 0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 };

static int _parse_packlen(void) {
	int bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	u8_t *ptr = streambuf->readp;
	int remain = 1;

	while (bytes >= 24 && remain > 0) {
		u32_t len = *(ptr+16) | *(ptr+17) << 8 | *(ptr+18) << 16 | *(ptr+19) << 24; // assume msb 32 bits are 0
		if (!memcmp(ptr, header_guid, 16) && bytes >= 30) {
			ptr    += 30;
			bytes  -= 30;
			remain = len - 30;
			continue;
		}
		if (!memcmp(ptr, file_props_guid, 16) && len == 104) {
			u32_t packlen = *(ptr+92) | *(ptr+93) << 8 | *(ptr+94) << 16 | *(ptr+95) << 24;
			LOG_INFO("asf packet len: %u", packlen);
			return packlen;
		}
		ptr    += len;
		bytes  -= len;
		remain -= len;
	}

	LOG_WARN("could not parse packet length");
	return 0;
}

static int _read_data(void *opaque, u8_t *buffer, int buf_size) {
	unsigned int bytes;

	LOCK_S;

	bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	ff->end_of_stream = (stream.state <= DISCONNECT && bytes == 0);
	bytes = min(bytes, buf_size);

	// for chunked wma extract asf header and data frames from framing structure
	// pad asf data frames to size of packet extracted from asf header
	if (ff->wma_mmsh) {
		unsigned chunk_type = 0, chunk_len = 0;
		
		if (ff->mmsh_bytes_left) {
			// bytes remaining from previous frame
			if (bytes >= ff->mmsh_bytes_left) {
				bytes = ff->mmsh_bytes_left;
				ff->mmsh_bytes_left = 0;
			} else {
				ff->mmsh_bytes_left -= bytes;
			}
		} else if (ff->mmsh_bytes_pad) {
			// add padding for previous frame
			bytes = min(ff->mmsh_bytes_pad, buf_size);
			memset(buffer, 0, bytes);
			ff->mmsh_bytes_pad -= bytes;
			UNLOCK_S;
			return bytes;
		} else if (bytes >= 12) {
			// new chunk header
			chunk_type = (*(streambuf->readp) & 0x7f) | *(streambuf->readp + 1) << 8;
			chunk_len = *(streambuf->readp + 2) | *(streambuf->readp + 3) << 8;
			_buf_inc_readp(streambuf, 12);
			bytes -= 12;
		} else if (_buf_used(streambuf) >= 12) {
			// new chunk header split over end of streambuf, read in two
			u8_t header[12];
			memcpy(header, streambuf->readp, bytes);
			_buf_inc_readp(streambuf, bytes);
			memcpy(header + bytes, streambuf->readp, 12 - bytes);
			_buf_inc_readp(streambuf, 12 - bytes);
			chunk_type = (header[0] & 0x7f) | header[1] << 8;
			chunk_len  = header[2] | header[3] << 8;
			bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
			bytes = min(bytes, buf_size);
		} else {
			// should not get here...
			LOG_ERROR("chunk parser stalled bytes: %u %u", bytes, _buf_used(streambuf));
			UNLOCK_S;
			return 0;
		}
		
		if (chunk_type && chunk_len) {
			if (chunk_type == 0x4824) {
				// asf header - parse packet length
				ff->mmsh_packet_len = _parse_packlen();
				ff->mmsh_bytes_pad = 0;
			} else if (chunk_type == 0x4424 && ff->mmsh_packet_len) {
				// asf data packet - add padding
				ff->mmsh_bytes_pad = ff->mmsh_packet_len - chunk_len + 8;
			} else {
				LOG_INFO("unknown chunk: %04x", chunk_type);
				// other packet - no padding
				ff->mmsh_bytes_pad = 0;
			}
	
			if (chunk_len - 8 <= bytes) {
				bytes = chunk_len - 8;
				ff->mmsh_bytes_left = 0;
			} else {
				ff->mmsh_bytes_left = chunk_len - 8 - bytes;
			}
		}

	}

	memcpy(buffer, streambuf->readp, bytes);

	_buf_inc_readp(streambuf, bytes);

	if (ff->mmsh_bytes_pad && bytes + ff->mmsh_bytes_pad < buf_size) {
		memset(buffer + bytes, 0, ff->mmsh_bytes_pad);
		bytes += ff->mmsh_bytes_pad;
		ff->mmsh_bytes_pad = 0;
	}

	UNLOCK_S;

	return bytes;
}

static decode_state ff_decode(void) {
	int r, len, got_frame;
	AVPacket pkt_c;
	s32_t *optr = NULL;

	if (decode.new_stream) {

		AVIOContext *avio;
		AVStream *av_stream;
		AVCodec *codec;
		int o;
		int audio_stream = -1;

		ff->mmsh_bytes_left = ff->mmsh_bytes_pad = ff->mmsh_packet_len = 0;

		if (!ff->readbuf) {
			ff->readbuf = AV(ff, malloc, READ_SIZE +  AV_INPUT_BUFFER_PADDING_SIZE);
		}

		avio = AVIO(ff, alloc_context, ff->readbuf, READ_SIZE, 0, NULL, _read_data, NULL, NULL);
		avio->seekable = 0;

		ff->formatC = AVFORMAT(ff, alloc_context);
		if (ff->formatC == NULL) {
			LOG_ERROR("null context");
			return DECODE_ERROR;
		}

		ff->formatC->pb = avio;
		ff->formatC->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOPARSE;

		o = AVFORMAT(ff, open_input, &ff->formatC, "", ff->input_format, NULL);
		if (o < 0) {
			LOG_WARN("avformat_open_input: %d %s", o, av__err2str(o));
			return DECODE_ERROR;
		}

		LOG_INFO("format: name:%s lname:%s", ff->formatC->iformat->name, ff->formatC->iformat->long_name);
	
		o = AVFORMAT(ff, find_stream_info, ff->formatC, NULL);
		if (o < 0) {
			LOG_WARN("avformat_find_stream_info: %d %s", o, av__err2str(o));
			return DECODE_ERROR;
		}
		
		if (ff->wma && ff->wma_playstream < ff->formatC->nb_streams) {
			if (ff->formatC->streams[ff->wma_playstream]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				LOG_INFO("using wma stream sent from server: %i", ff->wma_playstream);
				audio_stream = ff->wma_playstream;
			}
		}

		if (audio_stream == -1) {
			unsigned int i;
			for (i = 0; i < ff->formatC->nb_streams; ++i) {
				if (ff->formatC->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
					audio_stream = i;
					LOG_INFO("found stream: %i", i);
					break;
				}
			}
		}

		if (audio_stream == -1) {
			LOG_WARN("no audio stream found");
			return DECODE_ERROR;
		}

		av_stream = ff->formatC->streams[audio_stream];

		ff->codecC = AVCODEC (ff, alloc_context3, NULL);
		if ( ff->codecC == NULL ) {
			LOG_ERROR("can't allocate avctx");
			return DECODE_ERROR;
		}

		ff->codecP = av_stream->codecpar;

		if ( (AVCODEC(ff, parameters_to_context, ff->codecC, ff->codecP) ) < 0) {
			AVCODEC(ff, free_context, &ff->codecC);

			LOG_ERROR("can't initialize avctx");
			return DECODE_ERROR;
		}

		codec = AVCODEC(ff, find_decoder, ff->codecP->codec_id);

		AVCODEC(ff, open2, ff->codecC, codec, NULL);

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55,28,1)
		ff->frame = AV(ff, frame_alloc);
#else
		ff->frame = AVCODEC(ff, alloc_frame);
#endif

		ff->avpkt = AV(ff, malloc, sizeof(AVPacket));
		if (ff->avpkt == NULL) {
			LOG_ERROR("can't allocate avpkt");
			return DECODE_ERROR;
		}

		AV(ff, init_packet, ff->avpkt);
		ff->avpkt->data = NULL;
		ff->avpkt->size = 0;

		LOCK_O;
		LOG_INFO("setting track_start");
		output.next_sample_rate = decode_newstream(ff->codecP->sample_rate, output.supported_rates);
		IF_DSD(	output.next_fmt = PCM; )
		output.track_start = outputbuf->writep;
		if (output.fade_mode) _checkfade(true);
		decode.new_stream = false;
		UNLOCK_O;
	}

	got_frame = 0;

	if ((r = AV(ff, read_frame, ff->formatC, ff->avpkt)) < 0) {
		if (r == AVERROR_EOF) {
			if (ff->end_of_stream) {
				LOG_INFO("decode complete");
				return DECODE_COMPLETE;
			} else {
				LOG_INFO("codec end of file");
			}
		} else {
			LOG_ERROR("av_read_frame error: %i %s", r, av__err2str(r));
		}
		return DECODE_RUNNING;
	}

	// clone packet as we are adjusting it
	pkt_c = *ff->avpkt;

	IF_PROCESS(
		optr = (s32_t *)process.inbuf;
		process.in_frames = 0;
	);

	while (pkt_c.size > 0 || got_frame) {

		len = AVCODEC(ff, decode_audio4, ff->codecC, ff->frame, &got_frame, &pkt_c);
		if (len < 0) {
			LOG_ERROR("avcodec_decode_audio4 error: %i %s", len, av__err2str(len));
			break; // exit loop, free the packet, and continue decoding
		}

		pkt_c.data += len;
		pkt_c.size -= len;
		
		if (got_frame) {
			
			s16_t *iptr16 = (s16_t *)ff->frame->data[0];
			s32_t *iptr32 = (s32_t *)ff->frame->data[0];
			s16_t *iptr16l = (s16_t *)ff->frame->data[0];
			s16_t *iptr16r = (s16_t *)ff->frame->data[1];
			s32_t *iptr32l = (s32_t *)ff->frame->data[0];
			s32_t *iptr32r = (s32_t *)ff->frame->data[1];
			float *iptrfl = (float *)ff->frame->data[0];
			float *iptrfr = (float *)ff->frame->data[1];

			frames_t frames = ff->frame->nb_samples;

			LOG_SDEBUG("got audio channels: %u samples: %u format: %u", ff->codecC->channels, ff->frame->nb_samples,
					   ff->codecC->sample_fmt);
			
			LOCK_O_direct;

			while (frames > 0) {
				frames_t count;
				frames_t f;
				
				IF_DIRECT(
					optr = (s32_t *)outputbuf->writep;
					f = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
					f = min(f, frames);
				);

				IF_PROCESS(
					if (process.in_frames + frames > process.max_in_frames) {
						LOG_WARN("exceeded process buffer size - dropping frames");
						break;
					}
					f = frames;	   
				);

				count = f;
				
				if (ff->codecC->channels == 2) {
					if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16) {
						while (count--) {
							*optr++ = *iptr16++ << 16;
							*optr++ = *iptr16++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32) {
						while (count--) {
							*optr++ = *iptr32++;
							*optr++ = *iptr32++;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16P) {
						while (count--) {
							*optr++ = *iptr16l++ << 16;
							*optr++ = *iptr16r++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32P) {
						while (count--) {
							*optr++ = *iptr32l++;
							*optr++ = *iptr32r++;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_FLTP) {
						while (count--) {
							double scaledl = *iptrfl++ * 0x7fffffff;
							double scaledr = *iptrfr++ * 0x7fffffff;
							if (scaledl > 2147483647.0) scaledl = 2147483647.0;
							if (scaledl < -2147483648.0) scaledl = -2147483648.0;
							if (scaledr > 2147483647.0) scaledr = 2147483647.0;
							if (scaledr < -2147483648.0) scaledr = -2147483648.0;
							*optr++ = (s32_t)scaledl;
							*optr++ = (s32_t)scaledr;
						}
					} else {
						LOG_WARN("unsupported sample format: %u", ff->codecC->sample_fmt);
					}
				} else if (ff->codecC->channels == 1) {
					if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16) {
						while (count--) {
							*optr++ = *iptr16   << 16;
							*optr++ = *iptr16++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32) {
						while (count--) {
							*optr++ = *iptr32;
							*optr++ = *iptr32++;						
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16P) {
						while (count--) {
							*optr++ = *iptr16l << 16;
							*optr++ = *iptr16l++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32P) {
						while (count--) {
							*optr++ = *iptr32l;
							*optr++ = *iptr32l++;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_FLTP) {
						while (count--) {
							double scaled = *iptrfl++ * 0x7fffffff;
							if (scaled > 2147483647.0) scaled = 2147483647.0;
							if (scaled < -2147483648.0) scaled = -2147483648.0;
							*optr++ = (s32_t)scaled;
							*optr++ = (s32_t)scaled;
						}
					} else {
						LOG_WARN("unsupported sample format: %u", ff->codecC->sample_fmt);
					}
				} else {
					LOG_WARN("unsupported number of channels");
				}
				
				frames -= f;
				
				IF_DIRECT(
					_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
				);

				IF_PROCESS(
					process.in_frames += f;
				);
			}
			
			UNLOCK_O_direct;
		}
	}

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57,24,102)
	AV(ff, packet_unref, ff->avpkt);
#else
	AV(ff, free_packet, ff->avpkt);
#endif

	return DECODE_RUNNING;
}

static void _free_ff_data(void) {
	if (ff->codecC) {
		AVCODEC(ff, free_context, &ff->codecC);
		ff->codecC = NULL;
	}

	if (ff->formatC) {
		if (ff->formatC->pb) {
			// per ffmpeg docs, the buffer originally pointed to by ff->readbuf may be dynamically freed and reallocated behind the scenes, so this is the one that must be freed
			// otherwise, a double free can occur (seen by valgrind), resulting in e.g. SIGILL
			AV(ff, freep, &ff->formatC->pb->buffer);
			ff->readbuf = NULL;
			AV(ff, freep, &ff->formatC->pb);
		}

		AVFORMAT(ff, free_context, ff->formatC);
		ff->formatC = NULL;
	}

	if (ff->frame) {
		// ffmpeg version dependant free function
#if !LINKALL
    #if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55,28,1)
		ff->av_frame_free ? AV(ff, frame_free, &ff->frame) : AV(ff, freep, &ff->frame);
    #else
		ff->avcodec_free_frame ? AVCODEC(ff, free_frame, &ff->frame) : AV(ff, freep, &ff->frame);
    #endif
#elif LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(54,28,0)
    #if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55,28,1)
		AV(ff, frame_free, &ff->frame);
    #else
		AVCODEC(ff, free_frame, &ff->frame);
    #endif
#else
		AV(ff, freep, &ff->frame);
#endif
		ff->frame = NULL;
	}

	if (ff->avpkt) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57,24,102)
		AV(ff, packet_unref, ff->avpkt);
#else
		AV(ff, free_packet, ff->avpkt);
#endif
		AV(ff, freep, &ff->avpkt);
		ff->avpkt = NULL;
	}
}

static void ff_open_wma(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	_free_ff_data();

	ff->input_format = AV(ff, find_input_format, "asf");
	if (ff->input_format == NULL) {
		LOG_ERROR("asf format not supported by ffmpeg library");
	}

	ff->wma = true;
	ff->wma_mmsh = size - '0';
	ff->wma_playstream = rate - 1;
	ff->wma_metadatastream = chan != '?' ? chan : 0;

	LOG_INFO("open wma chunking: %u playstream: %u metadatastream: %u", ff->wma_mmsh, ff->wma_playstream, ff->wma_metadatastream);
}

static void ff_open_alac(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	_free_ff_data();

	ff->input_format = AV(ff, find_input_format, "mp4");
	if (ff->input_format == NULL) {
		LOG_ERROR("mp4 format not supported by ffmpeg library");
	}

	ff->wma = false;
	ff->wma_mmsh = 0;

	LOG_INFO("open alac");
}

static void ff_close(void) {
	_free_ff_data();
}

static bool load_ff() {
#if !LINKALL
	void *handle_codec = NULL, *handle_format = NULL, *handle_util = NULL;
	char name[30];
	char *err;

	// we try to load the ffmpeg library version which matches the header file we are compiled with as structs differ between versions

	sprintf(name, LIBAVCODEC, LIBAVCODEC_VERSION_MAJOR);
	handle_codec = dlopen(name, RTLD_NOW);
	if (!handle_codec) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	sprintf(name, LIBAVFORMAT, LIBAVFORMAT_VERSION_MAJOR);
	handle_format = dlopen(name, RTLD_NOW);
	if (!handle_format) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	sprintf(name, LIBAVUTIL, LIBAVUTIL_VERSION_MAJOR);
	handle_util = dlopen(name, RTLD_NOW);
	if (!handle_util) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	ff->avcodec_version = dlsym(handle_codec, "avcodec_version");
	ff->avcodec_find_decoder = dlsym(handle_codec, "avcodec_find_decoder");
	ff->avcodec_open2 = dlsym(handle_codec, "avcodec_open2");
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55,28,1)
	ff->av_frame_alloc = dlsym(handle_codec, "av_frame_alloc");
	ff->av_frame_free = dlsym(handle_codec, "av_frame_free");
#else
	ff->avcodec_alloc_frame = dlsym(handle_codec, "avcodec_alloc_frame");
	ff->avcodec_free_frame = dlsym(handle_codec, "avcodec_free_frame");
#endif
	ff->avcodec_decode_audio4 = dlsym(handle_codec, "avcodec_decode_audio4");
	ff->avcodec_alloc_context3 = dlsym(handle_format, "avcodec_alloc_context3");
	ff->avcodec_free_context = dlsym(handle_format, "avcodec_free_context");
	ff->avcodec_parameters_to_context = dlsym(handle_format, "avcodec_parameters_to_context");
	ff->av_init_packet = dlsym(handle_codec, "av_init_packet");
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57,24,102)
	ff->av_packet_unref = dlsym(handle_codec, "av_packet_unref");
#else
	ff->av_free_packet = dlsym(handle_codec, "av_free_packet");
#endif

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}
	
	LOG_INFO("loaded "LIBAVCODEC" (%u.%u.%u)", LIBAVCODEC_VERSION_MAJOR, ff->avcodec_version() >> 16, (ff->avcodec_version() >> 8) & 0xff, ff->avcodec_version() & 0xff);

	ff->avformat_version = dlsym(handle_format, "avformat_version");
	ff->avformat_alloc_context = dlsym(handle_format, "avformat_alloc_context");
	ff->avformat_free_context = dlsym(handle_format, "avformat_free_context");
	ff->avformat_open_input = dlsym(handle_format, "avformat_open_input");
	ff->avformat_find_stream_info = dlsym(handle_format, "avformat_find_stream_info");
	ff->avio_alloc_context = dlsym(handle_format, "avio_alloc_context");
	ff->av_read_frame = dlsym(handle_format, "av_read_frame");
	ff->av_find_input_format= dlsym(handle_format, "av_find_input_format");
	ff->av_register_all = dlsym(handle_format, "av_register_all");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded "LIBAVFORMAT" (%u.%u.%u)", LIBAVFORMAT_VERSION_MAJOR, ff->avformat_version() >> 16, (ff->avformat_version() >> 8) & 0xff, ff->avformat_version() & 0xff);

	ff->avutil_version = dlsym(handle_util, "avutil_version");
	ff->av_log_set_callback = dlsym(handle_util, "av_log_set_callback");
	ff->av_log_set_level = dlsym(handle_util, "av_log_set_level");
	ff->av_strerror = dlsym(handle_util, "av_strerror");
	ff->av_malloc = dlsym(handle_util, "av_malloc");
	ff->av_freep = dlsym(handle_util, "av_freep");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded "LIBAVUTIL" (%u.%u.%u)", LIBAVUTIL_VERSION_MAJOR, ff->avutil_version() >> 16, (ff->avutil_version() >> 8) & 0xff, ff->avutil_version() & 0xff);

#endif

	return true;
}

static int ff_log_level = 0;

void av_err_callback(void *avcl, int level, const char *fmt, va_list vl) {
	if (level > ff_log_level) return;
	fprintf(stderr, "%s ffmpeg: ", logtime());
	vfprintf(stderr, fmt, vl);
	fflush(stderr);
}

static bool registered = false;

struct codec *register_ff(const char *codec) {
	if (!registered) {

		ff = malloc(sizeof(struct ff_s));
		if (!ff) {
			return NULL;
		}

		memset(ff, 0, sizeof(struct ff_s));

		if (!load_ff()) {
			return NULL;
		}

		switch (loglevel) {
		case lERROR:
			ff_log_level = AV_LOG_ERROR; break;
		case lWARN:
			ff_log_level = AV_LOG_WARNING; break;
		case lINFO:
			ff_log_level = AV_LOG_INFO; break;
		case lDEBUG:
			ff_log_level = AV_LOG_VERBOSE; break;
		default: break;
		}

		AV(ff, log_set_callback, av_err_callback);

		AV(ff, register_all);
		
		registered = true;
	}

	if (!strcmp(codec, "wma")) {

		static struct codec ret = { 
			'w',         // id
			"wma,wmap",  // types
			READ_SIZE,   // min read
			WRITE_SIZE,  // min space
			ff_open_wma, // open
			ff_close,    // close
			ff_decode,   // decode
		};
		
		LOG_INFO("using ffmpeg to decode wma,wmap");
		return &ret;
	}

	if (!strcmp(codec, "alc")) {

		static struct codec ret = { 
			'l',         // id
			"alc",       // types
			READ_SIZE,   // min read
			WRITE_SIZE,  // min space
			ff_open_alac,// open
			ff_close,    // close
			ff_decode,   // decode
		};
		
		LOG_INFO("using ffmpeg to decode alc");		
		return &ret;
	}

	return NULL;
}

#endif

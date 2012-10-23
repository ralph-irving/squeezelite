/* 
 *  Squeezelite - lightweight headless squeezeplay emulator for linux
 *
 *  (c) Adrian Smith 2012, triode1@btinternet.com
 *  
 *  Unreleased - license details to be added here...
 */

#include "squeezelite.h"

#include <mad.h>

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;

struct decodestate decode;

#define LOCK_S   pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK_S pthread_mutex_unlock(&streambuf->mutex)
#define LOCK_O   pthread_mutex_lock(&outputbuf->mutex)
#define UNLOCK_O pthread_mutex_unlock(&outputbuf->mutex)

#define READBUF_SIZE 2048

static struct mad_stream mad_stream;
static struct mad_frame mad_frame;
static struct mad_synth mad_synth;
static u8_t readbuf[READBUF_SIZE + MAD_BUFFER_GUARD];
static unsigned readbuf_len;

// based on libmad minimad.c scale
static inline u32_t scale(mad_fixed_t sample) {
	sample += (1L << (MAD_F_FRACBITS - 24));
	
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;
	
	return (s32_t)(sample >> (MAD_F_FRACBITS + 1 - 24)) << 8;
}

static void mad_decode(void) {
	LOCK_S;
	size_t bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	bool end = (stream.state <= DISCONNECT && bytes == 0);

	if (mad_stream.next_frame && readbuf_len) {
		readbuf_len -= mad_stream.next_frame - readbuf;
		memmove(readbuf, mad_stream.next_frame, readbuf_len);
	}

	bytes = min(bytes, READBUF_SIZE - readbuf_len);
	memcpy(readbuf + readbuf_len, streambuf->readp, bytes);
	readbuf_len += bytes;
	_buf_inc_readp(streambuf, bytes);

	if (end) {
		memset(readbuf + readbuf_len, 0, MAD_BUFFER_GUARD);
		readbuf_len += MAD_BUFFER_GUARD;
	}
	UNLOCK_S;

	mad_stream_buffer(&mad_stream, readbuf, readbuf_len);

	while (true) {

		if (mad_frame_decode(&mad_frame, &mad_stream) == -1) {
			if (mad_stream.error == MAD_ERROR_BUFLEN) {
				return;
			}
			if (!MAD_RECOVERABLE(mad_stream.error)) {
				LOG_WARN("mad_frame_decode error: %s", mad_stream_errorstr(&mad_stream));
				LOG_INFO("unrecoverable - stopping decoder");
				LOCK_O;
				decode.state = DECODE_COMPLETE;
				UNLOCK_O;
			} else {
				LOG_DEBUG("mad_frame_decode error: %s", mad_stream_errorstr(&mad_stream));
			}
			return;
		};

		mad_synth_frame(&mad_synth, &mad_frame);

		LOCK_O;
		
		if (decode.new_stream) {
			LOG_INFO("setting track_start");
			output.next_sample_rate = mad_synth.pcm.samplerate; 
			output.track_start = outputbuf->writep;
			decode.new_stream = false;
		}
		
		if (mad_synth.pcm.length > _buf_space(outputbuf)) {
			LOG_WARN("too many samples - dropping samples");
			mad_synth.pcm.length = _buf_space(outputbuf);
		}
		
		size_t frames = mad_synth.pcm.length;
		s32_t *iptrl = mad_synth.pcm.samples[0];
		s32_t *iptrr = mad_synth.pcm.samples[1];

		while (frames > 0) {
			size_t f = min(frames, _buf_cont_write(outputbuf) / BYTES_PER_FRAME);
			s32_t *optr = (s32_t *)outputbuf->writep;
			size_t count = f;
			while (count--) {
				*optr++ = scale(*iptrl++);
				*optr++ = scale(*iptrr++);
			}
			frames -= f;
			_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
		}

		LOG_SDEBUG("write %u frames", mad_synth.pcm.length);

		UNLOCK_O;
	}
}

static void mad_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	readbuf_len = 0;
	mad_stream_init(&mad_stream);
	mad_frame_init(&mad_frame);
	mad_synth_init(&mad_synth);
}

static void mad_close(void) {
	mad_synth_finish(&mad_synth);
	mad_frame_finish(&mad_frame);
	mad_stream_finish(&mad_stream);
}

struct codec register_mad(void) {
	struct codec ret = { 
		.id    = 'm',
		.types = "mp3",
		.open  = mad_open,
		.close = mad_close,
		.decode= mad_decode,
		.min_space = 102400,
		.min_read_bytes = READBUF_SIZE,
	};
	return ret;
}

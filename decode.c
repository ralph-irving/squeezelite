/* 
 *  Squeezelite - lightweight headless squeezeplay emulator for linux
 *
 *  (c) Adrian Smith 2012, triode1@btinternet.com
 *  
 *  Unreleased - license details to be added here...
 */

// decode thread

#include "squeezelite.h"

log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;

#define LOCK_S   pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK_S pthread_mutex_unlock(&streambuf->mutex)
#define LOCK_O   pthread_mutex_lock(&outputbuf->mutex)
#define UNLOCK_O pthread_mutex_unlock(&outputbuf->mutex)

struct decodestate decode;
struct codec codecs[MAX_CODECS];
static struct codec *codec;
static bool running = true;

static void *decode_thread() {

	while (running) {

		LOCK_S;
		size_t bytes = _buf_used(streambuf);
		bool toend = (stream.state <= DISCONNECT);
		UNLOCK_S;
		LOCK_O;
		size_t space = _buf_space(outputbuf);
		decode_state state = decode.state;
		UNLOCK_O;

		if (state == DECODE_RUNNING) {
		
			LOG_SDEBUG("streambuf bytes: %u outputbuf space: %u", bytes, space);
			
			if (space > codec->min_space && bytes && (bytes > codec->min_read_bytes || toend)) {
				
				codec->decode();

			} else if (toend && bytes == 0) {

				LOG_INFO("decode complete");
				LOCK_O;
				decode.state = DECODE_COMPLETE;
				UNLOCK_O;

			} else {
				usleep(100000);
			}

		} else {
			usleep(100000);
		}

	}

	return 0;
}

static pthread_t thread;

void decode_init(log_level level) {
	loglevel = level;

	LOG_INFO("init decode");

	// register codecs
	memset(codecs, 0, sizeof(codecs));
	codecs[0] = register_flac();
	codecs[1] = register_pcm();
	codecs[2] = register_mad();

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, DECODE_THREAD_STACK_SIZE);
	pthread_create(&thread, &attr, decode_thread, NULL);
	pthread_attr_destroy(&attr);

	decode.new_stream = true;
	decode.state = DECODE_STOPPED;
}

void decode_close(void) {
	LOG_INFO("close decode");
	if (codec) {
		codec->close();
	}
	codec = NULL;
	running = false;
}

void codec_open(u8_t format, u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness) {
	LOG_INFO("codec open: '%c'", format);

	LOCK_O;
	decode.new_stream = true;
	decode.state = DECODE_STOPPED;
	UNLOCK_O;

	// find the required codec
	int i = 0;
	while (codecs[i].id && codecs[i].id != format) {
		i++;
	}
	
	if (codecs[i].id) {

		if (codec && codec != &codecs[i]) {
			LOG_INFO("closing codec");
			codec->close();
		}

		codec = &codecs[i];
	
		codec->open(sample_size, sample_rate, channels, endianness);

	} else {
		LOG_ERROR("codec not found");
	}
}

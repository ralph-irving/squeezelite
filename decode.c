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
struct codec *codecs[MAX_CODECS];
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
			
			if (space > codec->min_space && (bytes > codec->min_read_bytes || toend)) {
				
				state = codec->decode();

				if (state != DECODE_RUNNING) {

					LOG_INFO("decode %s", state == DECODE_COMPLETE ? "complete" : "error");

					LOCK_O;
					decode.state = state;
					if (output.fade_mode) _checkfade(false);
					UNLOCK_O;

					wake_controller();
				}

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

void decode_init(log_level level, const char *opt) {
	loglevel = level;

	LOG_INFO("init decode");

	// register codecs
	// alc,wma,wmap,wmal,aac,spt,ogg,ogf,flc,aif,pcm,mp3
	int i = 0;
	if (!opt || !strcmp(opt, "aac"))  codecs[i++] = register_faad();
	if (!opt || !strcmp(opt, "ogg"))  codecs[i++] = register_vorbis();
	if (!opt || !strcmp(opt, "flac")) codecs[i++] = register_flac();
	if (!opt || !strcmp(opt, "pcm"))  codecs[i++] = register_pcm();
	if (!opt || !strcmp(opt, "mp3"))  codecs[i++] = register_mad();

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
	int i;
	for (i = 0; i < MAX_CODECS; ++i) {

		if (codecs[i] && codecs[i]->id == format) {

			if (codec && codec != codecs[i]) {
				LOG_INFO("closing codec");
				codec->close();
			}
			
			codec = codecs[i];
			
			codec->open(sample_size, sample_rate, channels, endianness);
			
			return;
		}
	}

	LOG_ERROR("codec not found");
}

/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2016, ralph_irving@hotmail.com
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
extern struct processstate process;

struct decodestate decode;
struct codec *codecs[MAX_CODECS];
struct codec *codec;
static bool running = true;

#define LOCK_S   mutex_lock(streambuf->mutex)
#define UNLOCK_S mutex_unlock(streambuf->mutex)
#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)
#define LOCK_D   mutex_lock(decode.mutex);
#define UNLOCK_D mutex_unlock(decode.mutex);

#if PROCESS
#define IF_DIRECT(x)    if (decode.direct) { x }
#define IF_PROCESS(x)   if (!decode.direct) { x }
#define MAY_PROCESS(x)  { x }
#else
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#define MAY_PROCESS(x)
#endif

static void *decode_thread() {

	while (running) {
		size_t bytes, space, min_space;
		bool toend;
		bool ran = false;

		LOCK_S;
		bytes = _buf_used(streambuf);
		toend = (stream.state <= DISCONNECT);
		UNLOCK_S;
		LOCK_O;
		space = _buf_space(outputbuf);
		UNLOCK_O;

		LOCK_D;

		if (decode.state == DECODE_RUNNING && codec) {
		
			LOG_SDEBUG("streambuf bytes: %u outputbuf space: %u", bytes, space);

			IF_DIRECT(
				min_space = codec->min_space;
			);
			IF_PROCESS(
				min_space = process.max_out_frames * BYTES_PER_FRAME;
			);
			
			if (space > min_space && (bytes > codec->min_read_bytes || toend)) {
				
				decode.state = codec->decode();

				IF_PROCESS(
					if (process.in_frames) {
						process_samples();
					}

					if (decode.state == DECODE_COMPLETE) {
						process_drain();
					}
				);

				if (decode.state != DECODE_RUNNING) {

					LOG_SQ_INFO("decode %s", decode.state == DECODE_COMPLETE ? "complete" : "error");

					LOCK_O;
					if (output.fade_mode) _checkfade(false);
					UNLOCK_O;

					wake_controller();
				}

				ran = true;
			}
		}
		
		UNLOCK_D;

		if (!ran) {
			usleep(100000);
		}
	}

	return 0;
}

static thread_type thread;

void decode_init(log_level level, const char *include_codecs, const char *exclude_codecs) {
	int i;

	loglevel = level;

	LOG_SQ_INFO("init decode, include codecs: %s exclude codecs: %s", include_codecs ? include_codecs : "", exclude_codecs);

	// register codecs
	// dsf,dff,alc,wma,wmap,wmal,aac,spt,ogg,ogf,flc,aif,pcm,mp3
	i = 0;
#if DSD
	if (!strstr(exclude_codecs, "dsd")  && (!include_codecs || strstr(include_codecs, "dsd")))  codecs[i++] = register_dsd();
#endif
#if FFMPEG
	if (!strstr(exclude_codecs, "alac") && (!include_codecs || strstr(include_codecs, "alac")))  codecs[i++] = register_ff("alc");
	if (!strstr(exclude_codecs, "wma")  && (!include_codecs || strstr(include_codecs, "wma")))   codecs[i++] = register_ff("wma");
#endif
	if (!strstr(exclude_codecs, "aac")  && (!include_codecs || strstr(include_codecs, "aac")))  codecs[i++] = register_faad();
	if (!strstr(exclude_codecs, "ogg")  && (!include_codecs || strstr(include_codecs, "ogg")))  codecs[i++] = register_vorbis();
	if (!strstr(exclude_codecs, "flac") && (!include_codecs || strstr(include_codecs, "flac"))) codecs[i++] = register_flac();
	if (!strstr(exclude_codecs, "pcm")  && (!include_codecs || strstr(include_codecs, "pcm")))  codecs[i++] = register_pcm();

	// try mad then mpg for mp3 unless command line option passed
	if (!(strstr(exclude_codecs, "mp3") || strstr(exclude_codecs, "mad")) &&
		(!include_codecs || strstr(include_codecs, "mp3") || strstr(include_codecs, "mad")))	codecs[i] = register_mad();
	if (!(strstr(exclude_codecs, "mp3") || strstr(exclude_codecs, "mpg")) && !codecs[i] &&
		(!include_codecs || strstr(include_codecs, "mp3") || strstr(include_codecs, "mpg")))    codecs[i] = register_mpg();

	mutex_create(decode.mutex);

#if LINUX || OSX || FREEBSD
	pthread_attr_t attr;
	pthread_attr_init(&attr);
#ifdef PTHREAD_STACK_MIN
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + DECODE_THREAD_STACK_SIZE);
#endif
	pthread_create(&thread, &attr, decode_thread, NULL);
	pthread_attr_destroy(&attr);
#endif
#if WIN
	thread = CreateThread(NULL, DECODE_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&decode_thread, NULL, 0, NULL);
#endif

	decode.new_stream = true;
	decode.state = DECODE_STOPPED;

	MAY_PROCESS(
		decode.direct = true;
		decode.process = false;
	);
}

void decode_close(void) {
	LOG_SQ_INFO("close decode");
	LOCK_D;
	if (codec) {
		codec->close();
		codec = NULL;
	}
	running = false;
	UNLOCK_D;
#if LINUX || OSX || FREEBSD
	pthread_join(thread, NULL);
#endif
	mutex_destroy(decode.mutex);
}

void decode_flush(void) {
	LOG_SQ_INFO("decode flush");
	LOCK_D;
	decode.state = DECODE_STOPPED;
	IF_PROCESS(
		process_flush();
	);
	UNLOCK_D;
}

unsigned decode_newstream(unsigned sample_rate, unsigned supported_rates[]) {

	// called with O locked to get sample rate for potentially processed output stream
	// release O mutex during process_newstream as it can take some time

	MAY_PROCESS(
		if (decode.process) {
			UNLOCK_O;
			sample_rate = process_newstream(&decode.direct, sample_rate, supported_rates);
			LOCK_O;
		}
	);

	return sample_rate;
}

void codec_open(u8_t format, u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness) {
	int i;

	LOG_SQ_INFO("codec open: '%c'", format);

	LOCK_D;

	decode.new_stream = true;
	decode.state = DECODE_STOPPED;

	MAY_PROCESS(
		decode.direct = true; // potentially changed within codec when processing enabled
	);

	// find the required codec
	for (i = 0; i < MAX_CODECS; ++i) {

		if (codecs[i] && codecs[i]->id == format) {

			if (codec && codec != codecs[i]) {
				LOG_SQ_INFO("closing codec: '%c'", codec->id);
				codec->close();
			}
			
			codec = codecs[i];
			
			codec->open(sample_size, sample_rate, channels, endianness);

			decode.state = DECODE_READY;

			UNLOCK_D;
			return;
		}
	}

	UNLOCK_D;

	LOG_SQ_ERROR("codec not found");
}


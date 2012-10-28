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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <poll.h>

#define VERSION "v0.1"

#define STREAMBUF_SIZE (2 * 1024 * 1024)
#define OUTPUTBUF_SIZE (44100 * 8 * 10)

#define MAX_HEADER 2048

#define STREAM_THREAD_STACK_SIZE (PTHREAD_STACK_MIN * 4)
#define DECODE_THREAD_STACK_SIZE (PTHREAD_STACK_MIN * 4)
#define OUTPUT_THREAD_STACK_SIZE (PTHREAD_STACK_MIN * 4)

#define ALSA_BUFFER_TIME 20000
#define ALSA_PERIOD_COUNT 4

typedef u_int8_t  u8_t;
typedef u_int16_t u16_t;
typedef u_int32_t u32_t;
typedef u_int64_t u64_t;
typedef int16_t   s16_t;
typedef int32_t   s32_t;
typedef int64_t   s64_t;

#define BYTES_PER_FRAME 8

#define min(a,b) (((a) < (b)) ? (a) : (b))

// logging
typedef enum { ERROR = 0, WARN, INFO, DEBUG, SDEBUG } log_level;

const char *logtime(void);
void logprint(const char *fmt, ...);

#define LOG_ERROR(fmt, ...) if (loglevel >= ERROR) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  if (loglevel >= WARN)  logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  if (loglevel >= INFO)  logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) if (loglevel >= DEBUG) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_SDEBUG(fmt, ...) if (loglevel >= SDEBUG) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)

// utils.c (non logging)
u32_t gettime_ms(void);
void get_mac(u8_t *mac);

// buffer.c
struct buffer {
	u8_t *buf;
	u8_t *readp;
	u8_t *writep;
	u8_t *wrap;
	size_t size;
	size_t base_size;
	pthread_mutex_t mutex;
};

// _* called with mutex locked
unsigned _buf_used(struct buffer *buf);
unsigned _buf_space(struct buffer *buf);
unsigned _buf_cont_read(struct buffer *buf);
unsigned _buf_cont_write(struct buffer *buf);
void _buf_inc_readp(struct buffer *buf, unsigned by);
void _buf_inc_writep(struct buffer *buf, unsigned by);
void buf_flush(struct buffer *buf);
void buf_adjust(struct buffer *buf, size_t mod);
void buf_init(struct buffer *buf, size_t size);
void buf_destroy(struct buffer *buf);

// slimproto.c
void slimproto(log_level level, const char *addr, u8_t mac[6]);
void wake_controller(void);

// stream.c
typedef enum { STOPPED = 0, DISCONNECT, STREAMING_BUFFERING, STREAMING_FILE, STREAMING_HTTP, SEND_HEADERS, RECV_HEADERS } stream_state;
typedef enum { DISCONNECT_OK = 0, LOCAL_DISCONNECT = 1, REMOTE_DISCONNECT = 2, UNREACHABLE = 3, TIMEOUT = 4 } disconnect_code;

struct streamstate {
	stream_state state;
	disconnect_code disconnect;
	char *header;
	size_t header_len;
	bool sent_headers;
	u64_t bytes;
	unsigned threshold;
};

void stream_init(log_level level, unsigned stream_buf_size);
void stream_close(void);
void stream_local(const char *filename);
void stream_sock(u32_t ip, u16_t port, const char *header, size_t header_len, unsigned threshold);

// decode.c
typedef enum { DECODE_STOPPED = 0, DECODE_RUNNING, DECODE_COMPLETE } decode_state;

struct decodestate {
	decode_state state;
	bool new_stream;
};

struct codec {
	char id;
	char *types;
	unsigned min_read_bytes;
	unsigned min_space;
	void (*open)(u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness);
	void (*close)(void);
	void (*decode)(void);
};

#define MAX_CODECS 5

void decode_init(log_level level, const char *opt);
void decode_close(void);
void codec_open(u8_t format, u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness);

// output.c
typedef enum { OUTPUT_STOPPED = 0, OUTPUT_BUFFER, OUTPUT_RUNNING, 
			   OUTPUT_PAUSE_FRAMES, OUTPUT_SKIP_FRAMES, OUTPUT_START_AT } output_state;

struct outputstate {
	output_state state;
	const char *device;
	unsigned buffer_time;
	unsigned period_count;
	bool  track_started; 
	unsigned frames_played;
	unsigned current_sample_rate;
	unsigned max_sample_rate;
	unsigned alsa_frames;
	u32_t updated;
	u32_t current_replay_gain;
	union {
		u32_t pause_frames;
		u32_t skip_frames;
		u32_t start_at;
	};
	unsigned next_sample_rate; // set in decode thread
	u8_t  *track_start;        // set in decode thread
	u32_t gainL;               // set by slimproto
	u32_t gainR;               // set by slimproto
	u32_t next_replay_gain;    // set by slimproto
	unsigned threshold;        // set by slimproto
};

void alsa_list_pcm(void);
void output_init(log_level level, const char *device, unsigned output_buf_size, unsigned period_time, unsigned period_count);
void output_flush(void);
void output_close(void);
void stream_disconnect(void);

// codecs
struct codec *register_flac(void);
struct codec *register_pcm(void);
struct codec *register_mad(void);

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

// make may define: PORTAUDIO, SELFPIPE or RESAMPLE to influence build

#define VERSION "v1.3.1-338"

// build detection
#if defined(linux)
#define LINUX     1
#define OSX       0
#define WIN       0
#elif defined (__APPLE__)
#define LINUX     0
#define OSX       1
#define WIN       0
#elif defined (_MSC_VER)
#define LINUX     0
#define OSX       0
#define WIN       1
#elif defined (__sun)
#define SUN       1
#define LINUX     1
#define PORTAUDIO 1
#define PA18API   1
#define OSX       0
#define WIN       0
#else
#error unknown target
#endif

#if LINUX && !defined(PORTAUDIO)
#define ALSA      1
#define PORTAUDIO 0
#else
#define ALSA      0
#define PORTAUDIO 1
#endif

#if SUN
#define EVENTFD   0
#define WINEVENT  0
#define SELFPIPE  1
#elif LINUX && !defined(SELFPIPE)
#define EVENTFD   1
#define SELFPIPE  0
#define WINEVENT  0
#endif
#if (LINUX && !EVENTFD) || OSX
#define EVENTFD   0
#define SELFPIPE  1
#define WINEVENT  0
#endif
#if WIN
#define EVENTFD   0
#define SELFPIPE  0
#define WINEVENT  1
#endif

#if defined(RESAMPLE)
#undef  RESAMPLE
#define RESAMPLE  1 // resampling
#define PROCESS   1 // any sample processing (only resampling at present)
#else
#define RESAMPLE  0
#define PROCESS   0
#endif

#if defined(FFMPEG)
#undef FFMPEG
#define FFMPEG    1
#else
#define FFMPEG    0
#endif

#if LINUX && defined(VISEXPORT)
#undef VISEXPORT
#define VISEXPORT 1 // visulizer export support uses linux shared memory
#else
#define VISEXPORT 0
#endif

#if defined(LINKALL)
#undef LINKALL
#define LINKALL   1 // link all libraries at build time - requires all to be available at run time
#else
#define LINKALL   0
#endif


#if !LINKALL

// dynamically loaded libraries at run time
#if LINUX
#define LIBFLAC "libFLAC.so.8"
#define LIBMAD  "libmad.so.0"
#define LIBMPG "libmpg123.so.0"
#define LIBVORBIS "libvorbisfile.so.3"
#define LIBTREMOR "libvorbisidec.so.1"
#define LIBFAAD "libfaad.so.2"
#define LIBAVUTIL   "libavutil.so.%d"
#define LIBAVCODEC  "libavcodec.so.%d"
#define LIBAVFORMAT "libavformat.so.%d"
#define LIBSOXR "libsoxr.so.0"
#endif

#if OSX
#define LIBFLAC "libFLAC.8.dylib"
#define LIBMAD  "libmad.0.dylib"
#define LIBMPG "libmpg123.0.dylib"
#define LIBVORBIS "libvorbisfile.3.dylib"
#define LIBTREMOR "libvorbisidec.1.dylib"
#define LIBFAAD "libfaad.2.dylib"
#define LIBAVUTIL   "libavutil.%d.dylib"
#define LIBAVCODEC  "libavcodec.%d.dylib"
#define LIBAVFORMAT "libavformat.%d.dylib"
#define LIBSOXR "libsoxr.0.dylib"
#endif

#if WIN
#define LIBFLAC "libFLAC.dll"
#define LIBMAD  "libmad-0.dll"
#define LIBMPG "libmpg123-0.dll"
#define LIBVORBIS "libvorbisfile.dll"
#define LIBTREMOR "libvorbisidec.dll"
#define LIBFAAD "libfaad2.dll"
#define LIBAVUTIL   "avutil-%d.dll"
#define LIBAVCODEC  "avcodec-%d.dll"
#define LIBAVFORMAT "avformat-%d.dll"
#define LIBSOXR "libsoxr.dll"
#endif

#endif // !LINKALL

// config options
#define STREAMBUF_SIZE (2 * 1024 * 1024)
#define OUTPUTBUF_SIZE (44100 * 8 * 10)
#define OUTPUTBUF_SIZE_CROSSFADE (OUTPUTBUF_SIZE * 12 / 10)

#define MAX_HEADER 4096 // do not reduce as icy-meta max is 4080

#if ALSA
#define ALSA_BUFFER_TIME  40
#define ALSA_PERIOD_COUNT 4
#define OUTPUT_RT_PRIORITY 45
#endif

#ifndef SUN
#define SL_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>

#if LINUX || OSX
#include <unistd.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <poll.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#if SUN
#include <sys/types.h>
#endif /* SUN */

#define STREAM_THREAD_STACK_SIZE  64 * 1024
#define DECODE_THREAD_STACK_SIZE 128 * 1024
#define OUTPUT_THREAD_STACK_SIZE  64 * 1024
#define thread_t pthread_t;
#define closesocket(s) close(s)
#define last_error() errno
#define ERROR_WOULDBLOCK EWOULDBLOCK

#ifdef SUN
typedef uint8_t  u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef uint64_t u64_t;
#else
typedef u_int8_t  u8_t;
typedef u_int16_t u16_t;
typedef u_int32_t u32_t;
typedef u_int64_t u64_t;
#endif /* SUN */
typedef int16_t   s16_t;
typedef int32_t   s32_t;
typedef int64_t   s64_t;

#define mutex_type pthread_mutex_t
#define mutex_create(m) pthread_mutex_init(&m, NULL)
#define mutex_create_p(m) pthread_mutexattr_t attr; pthread_mutexattr_init(&attr); pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT); pthread_mutex_init(&m, &attr); pthread_mutexattr_destroy(&attr)
#define mutex_lock(m) pthread_mutex_lock(&m)
#define mutex_unlock(m) pthread_mutex_unlock(&m)
#define mutex_destroy(m) pthread_mutex_destroy(&m)
#define thread_type pthread_t

#endif

#if WIN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>

#define STREAM_THREAD_STACK_SIZE (1024 * 64)
#define DECODE_THREAD_STACK_SIZE (1024 * 128)
#define OUTPUT_THREAD_STACK_SIZE (1024 * 64)

typedef unsigned __int8  u8_t;
typedef unsigned __int16 u16_t;
typedef unsigned __int32 u32_t;
typedef unsigned __int64 u64_t;
typedef __int16 s16_t;
typedef __int32 s32_t;
typedef __int64 s64_t;

typedef BOOL bool;
#define true TRUE
#define false FALSE

#define inline __inline

#define mutex_type HANDLE
#define mutex_create(m) m = CreateMutex(NULL, FALSE, NULL)
#define mutex_create_p mutex_create
#define mutex_lock(m) WaitForSingleObject(m, INFINITE)
#define mutex_unlock(m) ReleaseMutex(m)
#define mutex_destroy(m) CloseHandle(m)
#define thread_type HANDLE

#define usleep(x) Sleep(x/1000)
#define sleep(x) Sleep(x*1000)
#define last_error() WSAGetLastError()
#define ERROR_WOULDBLOCK WSAEWOULDBLOCK
#define open _open
#define read _read

#define in_addr_t u32_t
#define socklen_t int
#define ssize_t int

#define RTLD_NOW 0

#endif

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

typedef u32_t frames_t;
typedef int sockfd;

#if EVENTFD
#include <sys/eventfd.h>
#define event_event int
#define event_handle struct pollfd
#define wake_create(e) e = eventfd(0, 0)
#define wake_signal(e) eventfd_write(e, 1)
#define wake_clear(e) eventfd_t val; eventfd_read(e, &val)
#define wake_close(e) close(e)
#endif

#if SELFPIPE
#define event_handle struct pollfd
#define event_event struct wake
#define wake_create(e) pipe(e.fds); set_nonblock(e.fds[0]); set_nonblock(e.fds[1])
#define wake_signal(e) write(e.fds[1], ".", 1)
#define wake_clear(e) char c[10]; read(e, &c, 10)
#define wake_close(e) close(e.fds[0]); close(e.fds[1])
struct wake { 
	int fds[2];
};
#endif

#if WINEVENT
#define event_event HANDLE
#define event_handle HANDLE
#define wake_create(e) e = CreateEvent(NULL, FALSE, FALSE, NULL)
#define wake_signal(e) SetEvent(e)
#define wake_close(e) CloseHandle(e)
#endif

// printf/scanf formats for u64_t
#if LINUX && __WORDSIZE == 64
#define FMT_u64 "%lu"
#define FMT_x64 "%lx"
#elif __GLIBC_HAVE_LONG_LONG || defined __GNUC__ || WIN || SUN
#define FMT_u64 "%llu"
#define FMT_x64 "%llx"
#else
#error can not support u64_t
#endif

#define FIXED_ONE 0x10000

#define BYTES_PER_FRAME 8

#define min(a,b) (((a) < (b)) ? (a) : (b))

// logging
typedef enum { lERROR = 0, lWARN, lINFO, lDEBUG, lSDEBUG } log_level;

const char *logtime(void);
void logprint(const char *fmt, ...);

#define LOG_ERROR(fmt, ...) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  if (loglevel >= lWARN)  logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  if (loglevel >= lINFO)  logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) if (loglevel >= lDEBUG) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_SDEBUG(fmt, ...) if (loglevel >= lSDEBUG) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)

// utils.c (non logging)
typedef enum { EVENT_TIMEOUT = 0, EVENT_READ, EVENT_WAKE } event_type;

char *next_param(char *src, char c);
u32_t gettime_ms(void);
void get_mac(u8_t *mac);
void set_nonblock(sockfd s);
int connect_timeout(sockfd sock, const struct sockaddr *addr, socklen_t addrlen, int timeout);
void server_addr(char *server, in_addr_t *ip_ptr, unsigned *port_ptr);
void set_readwake_handles(event_handle handles[], sockfd s, event_event e);
event_type wait_readwake(event_handle handles[], int timeout);
void packN(u32_t *dest, u32_t val);
void packn(u16_t *dest, u16_t val);
u32_t unpackN(u32_t *src);
u16_t unpackn(u16_t *src);
#if OSX
void set_nosigpipe(sockfd s);
#else
#define set_nosigpipe(s)
#endif
#if SUN
void init_daemonize(void);
int daemon(int,int);
#endif
#if WIN
void winsock_init(void);
void winsock_close(void);
void *dlopen(const char *filename, int flag);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);
int poll(struct pollfd *fds, unsigned long numfds, int timeout);
#endif
#if LINUX
void touch_memory(u8_t *buf, size_t size);
#endif

// buffer.c
struct buffer {
	u8_t *buf;
	u8_t *readp;
	u8_t *writep;
	u8_t *wrap;
	size_t size;
	size_t base_size;
	mutex_type mutex;
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
void _buf_resize(struct buffer *buf, size_t size);
void buf_init(struct buffer *buf, size_t size);
void buf_destroy(struct buffer *buf);

// slimproto.c
void slimproto(log_level level, char *server, u8_t mac[6], const char *name);
void slimproto_stop(void);
void wake_controller(void);

// stream.c
typedef enum { STOPPED = 0, DISCONNECT, STREAMING_WAIT,
			   STREAMING_BUFFERING, STREAMING_FILE, STREAMING_HTTP, SEND_HEADERS, RECV_HEADERS } stream_state;
typedef enum { DISCONNECT_OK = 0, LOCAL_DISCONNECT = 1, REMOTE_DISCONNECT = 2, UNREACHABLE = 3, TIMEOUT = 4 } disconnect_code;

struct streamstate {
	stream_state state;
	disconnect_code disconnect;
	char *header;
	size_t header_len;
	bool sent_headers;
	bool cont_wait;
	u64_t bytes;
	unsigned threshold;
	u32_t meta_interval;
	u32_t meta_next;
	u32_t meta_left;
	bool  meta_send;
};

void stream_init(log_level level, unsigned stream_buf_size);
void stream_close(void);
void stream_file(const char *header, size_t header_len, unsigned threshold);
void stream_sock(u32_t ip, u16_t port, const char *header, size_t header_len, unsigned threshold, bool cont_wait);
bool stream_disconnect(void);

// decode.c
typedef enum { DECODE_STOPPED = 0, DECODE_RUNNING, DECODE_COMPLETE, DECODE_ERROR } decode_state;

struct decodestate {
	decode_state state;
	bool new_stream;
	mutex_type mutex;
#if PROCESS
	bool direct;
	bool process;
#endif
};

#if PROCESS
struct processstate {
	u8_t *inbuf, *outbuf;
	unsigned max_in_frames, max_out_frames;
	unsigned in_frames, out_frames;
	unsigned in_sample_rate, out_sample_rate;
	unsigned long total_in, total_out;
};
#endif

struct codec {
	char id;
	char *types;
	unsigned min_read_bytes;
	unsigned min_space;
	void (*open)(u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness);
	void (*close)(void);
	decode_state (*decode)(void);
};

void decode_init(log_level level, const char *opt);
void decode_close(void);
void decode_flush(void);
unsigned decode_newstream(unsigned sample_rate, unsigned max_sample_rate);
void codec_open(u8_t format, u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness);

#if PROCESS
// process.c
void process_samples(void);
void process_drain(void);
void process_flush(void);
unsigned process_newstream(bool *direct, unsigned raw_sample_rate, unsigned max_sample_rate);
void process_init(char *opt);
#endif

#if RESAMPLE
// resample.c
void resample_samples(struct processstate *process);
bool resample_drain(struct processstate *process);
bool resample_newstream(struct processstate *process, unsigned raw_sample_rate, unsigned max_sample_rate);
void resample_flush(void);
bool resample_init(char *opt);
#endif

// output.c
typedef enum { OUTPUT_OFF = -1, OUTPUT_STOPPED = 0, OUTPUT_BUFFER, OUTPUT_RUNNING, 
			   OUTPUT_PAUSE_FRAMES, OUTPUT_SKIP_FRAMES, OUTPUT_START_AT } output_state;

typedef enum { FADE_INACTIVE = 0, FADE_DUE, FADE_ACTIVE } fade_state;
typedef enum { FADE_UP = 1, FADE_DOWN, FADE_CROSS } fade_dir;
typedef enum { FADE_NONE = 0, FADE_CROSSFADE, FADE_IN, FADE_OUT, FADE_INOUT } fade_mode;

struct outputstate {
	output_state state;
	const char *device;
#if ALSA
	unsigned buffer;
	unsigned period;
#endif
	bool  track_started; 
#if PORTAUDIO
	bool  pa_reopen;
	unsigned latency;
	int osx_playnice;
#endif
	unsigned frames_played;
	unsigned current_sample_rate;
	unsigned max_sample_rate;
	unsigned device_frames;
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
	fade_state fade;
	u8_t *fade_start;
	u8_t *fade_end;
	fade_dir fade_dir;
	fade_mode fade_mode;       // set by slimproto
	unsigned fade_secs;        // set by slimproto
};

void list_devices(void);
#if ALSA
void output_init(log_level level, const char *device, unsigned output_buf_size, unsigned alsa_buffer, unsigned alsa_period, const char *alsa_sample_fmt, bool mmap, unsigned max_rate, unsigned rt_priority);
#endif
#if PORTAUDIO
#ifndef PA18API
void output_init(log_level level, const char *device, unsigned output_buf_size, unsigned latency, int osx_playnice, unsigned max_rate);
#else
void output_init(log_level level, const char *device, unsigned output_buf_size, unsigned pa_frames, unsigned pa_nbufs, unsigned max_rate);
#endif /* PA18API */
#endif
#if VISEXPORT
void output_vis_init(u8_t *mac);
#endif
void output_flush(void);
void output_close(void);
// _* called with mutex locked
void _checkfade(bool);
void _pa_open(void);

// codecs
#define MAX_CODECS 8

struct codec *register_flac(void);
struct codec *register_pcm(void);
struct codec *register_mad(void);
struct codec *register_mpg(void);
struct codec *register_vorbis(void);
struct codec *register_faad(void);
struct codec *register_ff(const char *codec);

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

// stream thread

#include "squeezelite.h"

#include <fcntl.h>

static log_level loglevel;

static struct buffer buf;
struct buffer *streambuf = &buf;

#if (0)
#define LOCK   LOG_INFO("lock"); pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK LOG_INFO("unlock"); pthread_mutex_unlock(&streambuf->mutex)
#else
#define LOCK   pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK pthread_mutex_unlock(&streambuf->mutex)
#endif

static int fd;

struct streamstate stream;

static void send_header(void) {
	char *ptr = stream.header;
	int len = stream.header_len;

	size_t n, try = 0;
	
	while (len) {
		n = send(fd, ptr, len, 0);
		if (n <= 0) {
			if (n < 0 && errno == EAGAIN && try < 10) {
				LOG_SDEBUG("retrying (%d) writing to socket", ++try);
				usleep(100);
				continue;
			}
			LOG_WARN("failed writing to socket: %s", strerror(errno));
			stream.disconnect = LOCAL_DISCONNECT;
			stream.state = DISCONNECT;
			wake_controller();
			return;
		}
		LOG_SDEBUG("wrote %d bytes to socket", n);
		ptr += n;
		len -= n;
	}
	LOG_SDEBUG("wrote header");
}

static bool running = true;

static void *stream_thread() {

	size_t header_len = 0;

	while (running) {

		if (fd < 0) {
			usleep(100000);
			continue;
		}

		struct pollfd pollinfo = { .fd = fd, .events = 0 };
		
		LOCK;
		size_t space = min(_buf_space(streambuf), _buf_cont_write(streambuf));

		if (stream.state > DISCONNECT && space) {
			pollinfo.events = POLLIN;
			if (stream.state == SEND_HEADERS) {
				pollinfo.events |= POLLOUT;
			}
		}

		UNLOCK;
		
		if (poll(&pollinfo, 1, 100)) {

			if ((pollinfo.revents & POLLOUT) && stream.state == SEND_HEADERS) {
				send_header();
				header_len = 0;
				LOCK;
				stream.state = RECV_HEADERS;
				UNLOCK;
				continue;
			}
					
			if (pollinfo.revents & POLLIN) {
				
				// get response headers
				if (stream.state == RECV_HEADERS) {

					// read one byte at a time to catch end of header
					char c;
					static int endtok;

					LOCK;
					int n = recv(fd, &c, 1, 0);
					if (n <= 0) {
						if (n < 0 && errno == EAGAIN) {
							continue;
						}
						LOG_WARN("error reading headers: %s", n ? strerror(errno) : "closed");
						stream.state = STOPPED;
						stream.disconnect = LOCAL_DISCONNECT;
						close(fd);
						fd = -1;
						wake_controller();
						UNLOCK;
						continue;
					}

					*(stream.header + header_len) = c;
					header_len++;

					if (header_len > MAX_HEADER - 1) {
						LOG_ERROR("received headers too long");
						stream.state = DISCONNECT;
						stream.disconnect = LOCAL_DISCONNECT;
						close(fd);
						fd = -1;
						wake_controller();
					}

					if (header_len > 1 && (c == '\r' || c == '\n')) {
						endtok++;
						if (endtok == 4) {
							*(stream.header + header_len) = '\0';
							stream.header_len = header_len;
							LOG_INFO("headers: len: %d\n%s", header_len, stream.header);
							stream.state = STREAMING_BUFFERING;
							wake_controller();
						}
					} else {
						endtok = 0;
					}
				
					UNLOCK;
					continue;
				}
				
				// stream body into streambuf
				LOCK;
				
				space = min(_buf_space(streambuf), _buf_cont_write(streambuf));
				
				int n = stream.state == STREAMING_FILE ? read(fd, streambuf->writep, space) : recv(fd, streambuf->writep, space, 0);
				if (n == 0) {
					LOG_INFO("end of stream");
					stream.state = DISCONNECT;
					stream.disconnect = DISCONNECT_OK;
					close(fd);
					fd = -1;
					wake_controller();
				}
				if (n < 0 && errno != EAGAIN) {
					LOG_WARN("error reading: %s", strerror(errno));
					stream.state = DISCONNECT;
					stream.disconnect = REMOTE_DISCONNECT;
					close(fd);
					fd = -1;
					wake_controller();
				}
				
				if (n > 0) {
					_buf_inc_writep(streambuf, n);
					stream.bytes += n;
					if (stream.state == STREAMING_BUFFERING && stream.bytes > stream.threshold) {
						stream.state = STREAMING_HTTP;
						wake_controller();
					}
				}
				
				UNLOCK;
				
				LOG_SDEBUG("streambuf read %d bytes", n);
			}
			
		} else {
			
			LOG_SDEBUG("poll timeout");
		}
	}

	return 0;
}

static pthread_t thread;

void stream_init(log_level level, unsigned stream_buf_size) {
	loglevel = level;

	LOG_INFO("init stream");
	LOG_DEBUG("streambuf size: %u", stream_buf_size);

	buf_init(streambuf, stream_buf_size);
	stream.state = STOPPED;
	stream.header = malloc(MAX_HEADER);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, STREAM_THREAD_STACK_SIZE);
	pthread_create(&thread, &attr, stream_thread, NULL);
	pthread_attr_destroy(&attr);
}

void stream_close(void) {
	LOG_INFO("close stream");
	LOCK;
	running = false;
	UNLOCK;
	pthread_join(thread,NULL);
	free(stream.header);
	buf_destroy(streambuf);
}

void stream_local(const char *filename) {
	buf_flush(streambuf);

	LOCK;
	fd = open(filename, O_RDONLY);
	stream.state = STREAMING_FILE;
	if (fd < 0) {
		LOG_WARN("can't open file: %s", filename);
		stream.state = DISCONNECT;
	}
	stream.bytes = 0;
	UNLOCK;
}

void stream_sock(u32_t ip, u16_t port, const char *header, size_t header_len, unsigned threshold) {
    struct sockaddr_in addr;

	int sock = socket(AF_INET, SOCK_STREAM, 0);

	if (sock < 0) {
		LOG_ERROR("failed to create socket");
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port = port;

	LOG_INFO("connecting to %s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		LOG_INFO("unable to connect to server");
		LOCK;
		stream.state = DISCONNECT;
		stream.disconnect = UNREACHABLE;
		UNLOCK;
		return;
	}

	int flags = fcntl(sock, F_GETFL,0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);

	buf_flush(streambuf);

	if (header_len > MAX_HEADER - 1) {
		LOG_WARN("request header too long");
		LOCK;
		stream.state = DISCONNECT;
		stream.disconnect = UNREACHABLE;
		UNLOCK;
		return;
	}


	LOCK;

	fd = sock;
	stream.state = SEND_HEADERS;
	stream.header_len = header_len;
	memcpy(stream.header, header, header_len);
	*(stream.header+header_len) = '\0';

	char *tok;
	if ((tok = strstr(stream.header, "Icy-Metadata: 1"))) {
		LOG_INFO("adjusting icy meta request - not supported");
		*(tok+14) = '0';
	}

	LOG_INFO("header: %s", stream.header);

	stream.sent_headers = false;
	stream.bytes = 0;
	stream.threshold = threshold;

	UNLOCK;
}

void stream_disconnect(void) {
	LOCK;
	close(fd);
	fd = -1;
	stream.state = STOPPED;
	UNLOCK;
}

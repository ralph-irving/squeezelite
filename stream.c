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

// stream thread

#include "squeezelite.h"

#include <fcntl.h>

#if SUN
#include <signal.h>
#endif
static log_level loglevel;

static struct buffer buf;
struct buffer *streambuf = &buf;

#define LOCK   mutex_lock(streambuf->mutex)
#define UNLOCK mutex_unlock(streambuf->mutex)

static sockfd fd;

struct streamstate stream;

static void send_header(void) {
	char *ptr = stream.header;
	int len = stream.header_len;

	unsigned try = 0;
	ssize_t n;
	
	while (len) {
		n = send(fd, ptr, len, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && last_error() == ERROR_WOULDBLOCK && try < 10) {
				LOG_SDEBUG("retrying (%d) writing to socket", ++try);
				usleep(1000);
				continue;
			}
			LOG_INFO("failed writing to socket: %s", strerror(last_error()));
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

static void _disconnect(stream_state state, disconnect_code disconnect) {
	stream.state = state;
	stream.disconnect = disconnect;
	closesocket(fd);
	fd = -1;
	wake_controller();
}

static void *stream_thread() {

	while (running) {

		struct pollfd pollinfo;
		size_t space;

		LOCK;

		space = min(_buf_space(streambuf), _buf_cont_write(streambuf));

		if (fd < 0 || !space || stream.state <= STREAMING_WAIT) {
			UNLOCK;
			usleep(100000);
			continue;
		}

		if (stream.state == STREAMING_FILE) {

			int n = read(fd, streambuf->writep, space);
			if (n == 0) {
				LOG_INFO("end of stream");
				_disconnect(DISCONNECT, DISCONNECT_OK);
			}
			if (n > 0) {
				_buf_inc_writep(streambuf, n);
				stream.bytes += n;
				LOG_SDEBUG("streambuf read %d bytes", n);
			}
			if (n < 0) {
				LOG_WARN("error reading: %s", strerror(last_error()));
				_disconnect(DISCONNECT, REMOTE_DISCONNECT);
			}

			UNLOCK;
			continue;

		} else {

			pollinfo.fd = fd;
			pollinfo.events = POLLIN;
			if (stream.state == SEND_HEADERS) {
				pollinfo.events |= POLLOUT;
			}
		}

		UNLOCK;

		if (poll(&pollinfo, 1, 100)) {

			LOCK;

			// check socket has not been closed while in poll
			if (fd < 0) {
				UNLOCK;
				continue;
			}

			if ((pollinfo.revents & POLLOUT) && stream.state == SEND_HEADERS) {
				send_header();
				stream.header_len = 0;
				stream.state = RECV_HEADERS;
				UNLOCK;
				continue;
			}
					
			if (pollinfo.revents & (POLLIN | POLLHUP)) {

				// get response headers
				if (stream.state == RECV_HEADERS) {

					// read one byte at a time to catch end of header
					char c;
					static int endtok;

					int n = recv(fd, &c, 1, 0);
					if (n <= 0) {
						if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
							UNLOCK;
							continue;
						}
						LOG_INFO("error reading headers: %s", n ? strerror(last_error()) : "closed");
						_disconnect(STOPPED, LOCAL_DISCONNECT);
						UNLOCK;
						continue;
					}

					*(stream.header + stream.header_len) = c;
					stream.header_len++;

					if (stream.header_len > MAX_HEADER - 1) {
						LOG_ERROR("received headers too long: %u", stream.header_len);
						_disconnect(DISCONNECT, LOCAL_DISCONNECT);
					}

					if (stream.header_len > 1 && (c == '\r' || c == '\n')) {
						endtok++;
						if (endtok == 4) {
							*(stream.header + stream.header_len) = '\0';
							LOG_INFO("headers: len: %d\n%s", stream.header_len, stream.header);
							stream.state = stream.cont_wait ? STREAMING_WAIT : STREAMING_BUFFERING;
							wake_controller();
						}
					} else {
						endtok = 0;
					}
				
					UNLOCK;
					continue;
				}
				
				// receive icy meta data
				if (stream.meta_interval && stream.meta_next == 0) {

					if (stream.meta_left == 0) {
						// read meta length
						u8_t c;
						int n = recv(fd, &c, 1, 0);
						if (n <= 0) {
							if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
								UNLOCK;
								continue;
							}
							LOG_INFO("error reading icy meta: %s", n ? strerror(last_error()) : "closed");
							_disconnect(STOPPED, LOCAL_DISCONNECT);
							UNLOCK;
							continue;
						}
						stream.meta_left = 16 * c;
						stream.header_len = 0; // amount of received meta data
						// MAX_HEADER must be more than meta max of 16 * 255
					}

					if (stream.meta_left) {
						int n = recv(fd, stream.header + stream.header_len, stream.meta_left, 0);
						if (n <= 0) {
							if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
								UNLOCK;
								continue;
							}
							LOG_INFO("error reading icy meta: %s", n ? strerror(last_error()) : "closed");
							_disconnect(STOPPED, LOCAL_DISCONNECT);
							UNLOCK;
							continue;
						}
						stream.meta_left -= n;
						stream.header_len += n;
					}
					
					if (stream.meta_left == 0) {
						if (stream.header_len) {
							*(stream.header + stream.header_len) = '\0';
							LOG_INFO("icy meta: len: %u\n%s", stream.header_len, stream.header);
							stream.meta_send = true;
							wake_controller();
						}
						stream.meta_next = stream.meta_interval;
						UNLOCK;
						continue;
					}

				// stream body into streambuf
				} else {
					int n;

					space = min(_buf_space(streambuf), _buf_cont_write(streambuf));
					if (stream.meta_interval) {
						space = min(space, stream.meta_next);
					}
					
					n = recv(fd, streambuf->writep, space, 0);
					if (n == 0) {
						LOG_INFO("end of stream");
						_disconnect(DISCONNECT, DISCONNECT_OK);
					}
					if (n < 0 && last_error() != ERROR_WOULDBLOCK) {
						LOG_INFO("error reading: %s", strerror(last_error()));
						_disconnect(DISCONNECT, REMOTE_DISCONNECT);
					}
					
					if (n > 0) {
						_buf_inc_writep(streambuf, n);
						stream.bytes += n;
						if (stream.meta_interval) {
							stream.meta_next -= n;
						}
					}

					if (stream.state == STREAMING_BUFFERING && stream.bytes > stream.threshold) {
						stream.state = STREAMING_HTTP;
						wake_controller();
					}
				
					LOG_SDEBUG("streambuf read %d bytes", n);
				}
			}

			UNLOCK;
			
		} else {
			
			LOG_SDEBUG("poll timeout");
		}
	}

	return 0;
}

static thread_type thread;

void stream_init(log_level level, unsigned stream_buf_size) {
	loglevel = level;

	LOG_INFO("init stream");
	LOG_DEBUG("streambuf size: %u", stream_buf_size);

	buf_init(streambuf, stream_buf_size);
	if (streambuf->buf == NULL) {
		LOG_ERROR("unable to malloc buffer");
		exit(0);
	}
	
#if SUN
	signal(SIGPIPE, SIG_IGN);	/* Force sockets to return -1 with EPIPE on pipe signal */
#endif
	stream.state = STOPPED;
	stream.header = malloc(MAX_HEADER);
	*stream.header = '\0';

	fd = -1;

#if LINUX || FREEBSD
	touch_memory(streambuf->buf, streambuf->size);
#endif

#if LINUX || OSX || FREEBSD
	pthread_attr_t attr;
	pthread_attr_init(&attr);
#ifdef PTHREAD_STACK_MIN	
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + STREAM_THREAD_STACK_SIZE);
#endif
	pthread_create(&thread, &attr, stream_thread, NULL);
	pthread_attr_destroy(&attr);
#endif
#if WIN
	thread = CreateThread(NULL, STREAM_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&stream_thread, NULL, 0, NULL);
#endif
}

void stream_close(void) {
	LOG_INFO("close stream");
	LOCK;
	running = false;
	UNLOCK;
#if LINUX || OSX || FREEBSD
	pthread_join(thread, NULL);
#endif
	free(stream.header);
	buf_destroy(streambuf);
}

void stream_file(const char *header, size_t header_len, unsigned threshold) {
	buf_flush(streambuf);

	LOCK;

	stream.header_len = header_len;
	memcpy(stream.header, header, header_len);
	*(stream.header+header_len) = '\0';

	LOG_INFO("opening local file: %s", stream.header);

#if WIN
	fd = open(stream.header, O_RDONLY | O_BINARY);
#else
	fd = open(stream.header, O_RDONLY);
#endif

	stream.state = STREAMING_FILE;
	if (fd < 0) {
		LOG_INFO("can't open file: %s", stream.header);
		stream.state = DISCONNECT;
	}
	wake_controller();
	
	stream.cont_wait = false;
	stream.meta_interval = 0;
	stream.meta_next = 0;
	stream.meta_left = 0;
	stream.meta_send = false;
	stream.sent_headers = false;
	stream.bytes = 0;
	stream.threshold = threshold;

	UNLOCK;
}

void stream_sock(u32_t ip, u16_t port, const char *header, size_t header_len, unsigned threshold, bool cont_wait) {
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

	set_nonblock(sock);
	set_nosigpipe(sock);

	if (connect_timeout(sock, (struct sockaddr *) &addr, sizeof(addr), 10) < 0) {
		LOG_INFO("unable to connect to server");
		LOCK;
		stream.state = DISCONNECT;
		stream.disconnect = UNREACHABLE;
		UNLOCK;
		return;
	}

	buf_flush(streambuf);

	LOCK;

	fd = sock;
	stream.state = SEND_HEADERS;
	stream.cont_wait = cont_wait;
	stream.meta_interval = 0;
	stream.meta_next = 0;
	stream.meta_left = 0;
	stream.meta_send = false;
	stream.header_len = header_len;
	memcpy(stream.header, header, header_len);
	*(stream.header+header_len) = '\0';

	LOG_INFO("header: %s", stream.header);

	stream.sent_headers = false;
	stream.bytes = 0;
	stream.threshold = threshold;

	UNLOCK;
}

bool stream_disconnect(void) {
	bool disc = false;
	LOCK;
	if (fd != -1) {
		closesocket(fd);
		fd = -1;
		disc = true;
	}
	stream.state = STOPPED;
	UNLOCK;
	return disc;
}

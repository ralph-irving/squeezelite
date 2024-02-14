/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2024, ralph_irving@hotmail.com
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

#define _GNU_SOURCE

#include "squeezelite.h"

#include <fcntl.h>

#if USE_SSL
#include "openssl/ssl.h"
#include "openssl/err.h"
#endif

#if SUN
#include <signal.h>
#endif

#if USE_LIBOGG
#include "ogg/ogg.h"
#endif 

struct {
	bool flac;
	u64_t serial;
#if USE_LIBOGG
	bool active;
	ogg_stream_state state;
	ogg_packet packet;
	ogg_sync_state sync;
	ogg_page page;
#if !LINKALL
	struct {
		void* handle;
		int (*ogg_stream_init)(ogg_stream_state* os, int serialno);
		int (*ogg_stream_clear)(ogg_stream_state* os);
		int (*ogg_stream_reset_serialno)(ogg_stream_state* os, int serialno);
		int (*ogg_stream_pagein)(ogg_stream_state* os, ogg_page* og);
		int (*ogg_stream_packetout)(ogg_stream_state* os, ogg_packet* op);
		int (*ogg_sync_clear)(ogg_sync_state* oy);
		char* (*ogg_sync_buffer)(ogg_sync_state* oy, long size);
		int (*ogg_sync_wrote)(ogg_sync_state* oy, long bytes);
		ogg_int64_t(*ogg_page_granulepos)(const ogg_page* og);
		int  (*ogg_page_serialno)(const ogg_page* og);
		int (*ogg_page_bos)(const ogg_page* og);
		int (*ogg_sync_pageout)(ogg_sync_state* oy, ogg_page* og);
	} dl;
#endif
#else
	enum { STREAM_OGG_OFF, STREAM_OGG_SYNC, STREAM_OGG_HEADER, STREAM_OGG_SEGMENTS, STREAM_OGG_PAGE } state;
	size_t want, miss, match;
	u8_t* data, segments[255];
#pragma pack(push, 1)
	struct {
		char pattern[4];
		u8_t version, type;
		u64_t granule;
		u32_t serial, page, checksum;
		u8_t count;
	} header;
#pragma pack(pop)
#endif
} ogg;

static log_level loglevel;

static struct buffer buf;
struct buffer *streambuf = &buf;

#define LOCK   mutex_lock(streambuf->mutex)
#define UNLOCK mutex_unlock(streambuf->mutex)

static sockfd fd;
static struct sockaddr_in addr;
static char host[256];
static int header_mlen;

struct streamstate stream;

#if USE_LIBOGG
#if LINKALL
#define OG(h, fn, ...) (ogg_ ## fn)(__VA_ARGS__)
#else
#define OG(h, fn, ...) (h)->ogg_##fn(__VA_ARGS__)
#endif
#endif

#if USE_SSL
static SSL_CTX *SSLctx;
static SSL *ssl;
static bool ssl_error;

static int _last_error(void) {
	if (!ssl) return last_error();
	return ssl_error ? ECONNABORTED : ERROR_WOULDBLOCK;
}

static int _recv(int fd, void *buffer, size_t bytes, int options) {
	int n;
	if (!ssl) return recv(fd, buffer, bytes, options);
	n = SSL_read(ssl, (u8_t*) buffer, bytes);
	if (n <= 0) {
		int err = SSL_get_error(ssl, n);
		if (err == SSL_ERROR_ZERO_RETURN) return 0;
		ssl_error = (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE);
	}
	return n;
}

static int _send(int fd, void *buffer, size_t bytes, int options) {
	int n = 0;
	int err;
	if (!ssl) return send(fd, buffer, bytes, options);
	do {
		ERR_clear_error();
		if ((n = SSL_write(ssl, (u8_t*) buffer, bytes)) >= 0) break;
		err = SSL_get_error(ssl, n);
		ssl_error = (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE);
	} while (!ssl_error);
	return n;
}

/*
can't mimic exactly poll as SSL is a real pain. Even if SSL_pending returns
0, there might be bytes to read but when select (poll) return > 0, there might
be no frame available. As well select (poll) < 0 does not mean that there is
no data pending
*/
static int _poll(struct pollfd *pollinfo, int timeout) {
	if (!ssl) return poll(pollinfo, 1, timeout);
	if (pollinfo->events & POLLIN && SSL_pending(ssl)) {
		if (pollinfo->events & POLLOUT) poll(pollinfo, 1, 0);
		pollinfo->revents = POLLIN;
		return 1;
	}
	return poll(pollinfo, 1, timeout);
}
#else
#define _recv(fd, buf, n, opt) recv(fd, buf, n, opt)
#define _send(fd, buf, n, opt) send(fd, buf, n, opt)
#define _poll(pollinfo, timeout) poll(pollinfo, 1, timeout)
#define _last_error() last_error()
#endif // USE_SSL


static bool send_header(void) {
	char *ptr = stream.header;
	int len = stream.header_len;

	unsigned try = 0;
	ssize_t n;
	int error;
	
	while (len) {
		n = _send(fd, ptr, len, MSG_NOSIGNAL);
		if (n <= 0) {
			error = _last_error();
#if WIN
			if (n < 0 && (error == ERROR_WOULDBLOCK || error == WSAENOTCONN) && try < 10) {
#else
			if (n < 0 && error == ERROR_WOULDBLOCK && try < 10) {
#endif
				LOG_DEBUG("retrying (%d) writing to socket", ++try);
				usleep(1000);
				continue;
			}
			LOG_WARN("failed writing to socket: %s", strerror(last_error()));
			stream.disconnect = LOCAL_DISCONNECT;
			stream.state = DISCONNECT;
			wake_controller();
			return false;
		}
		LOG_SDEBUG("wrote %d bytes to socket", n);
		ptr += n;
		len -= n;
	}
	LOG_SDEBUG("wrote header");
	return true;
}

static bool running = true;

static void _disconnect(stream_state state, disconnect_code disconnect) {
	stream.state = state;
	stream.disconnect = disconnect;
#if USE_LIBOGG
	if (ogg.active) {
		OG(&ogg.dl, stream_clear, &ogg.state);
		OG(&ogg.dl, sync_clear, &ogg.sync);
	}
#else
	if (ogg.state == STREAM_OGG_PAGE && ogg.data) free(ogg.data);
	ogg.data = NULL;
#endif
#if USE_SSL
	if (ssl) {
		SSL_shutdown(ssl);
		SSL_free(ssl);
		ssl = NULL;
	}
#endif
	closesocket(fd);
	fd = -1;
	wake_controller();
}

static int connect_socket(bool use_ssl) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);

	if (sock < 0) {
		LOG_ERROR("failed to create socket");
		return -1;
	}

	LOG_INFO("connecting to %s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	set_nonblock(sock);
	set_nosigpipe(sock);
	set_recvbufsize(sock);

	if (connect_timeout(sock, (struct sockaddr *) &addr, sizeof(addr), 10) < 0) {
		LOG_INFO("unable to connect to server");
		closesocket(sock);
		return -1;
	}

#if USE_SSL
	if (use_ssl) {
		ssl = SSL_new(SSLctx);
		SSL_set_fd(ssl, sock);

		// add SNI
		if (*host) SSL_set_tlsext_host_name(ssl, host);

		while (1) {
			int status, err = 0;

			ERR_clear_error();
			status = SSL_connect(ssl);

			// successful negotiation
			if (status == 1) break;

			// error or non-blocking requires more time
			if (status < 0) {
				err = SSL_get_error(ssl, status);
				if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
					usleep(1000);
					continue;
				}
			}

			LOG_WARN("unable to open SSL socket %d (%d)", status, err);
			closesocket(sock);
			SSL_free(ssl);
			ssl = NULL;

			return -1;
		}
	}
#endif

	return sock;
}

static u32_t inline itohl(u32_t littlelong) {
#if SL_LITTLE_ENDIAN
	return littlelong;
#else
	return __builtin_bswap32(littlelong);
#endif
}

/* https://xiph.org/ogg/doc/framing.html 
 * https://xiph.org/flac/ogg_mapping.html
 * https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-610004.2 */

#if !USE_LIBOGG
static size_t memfind(const u8_t* haystack, size_t n, const char* needle, size_t len, size_t* offset) {
	size_t i;
	for (i = 0; i < n && *offset != len; i++) *offset = (haystack[i] == needle[*offset]) ? *offset + 1 : 0;
	return i;
}

/* this mode is made to save memory and CPU by not calling ogg decoding function and never having
 * full packets (as a vorbis_comment can have a very large artwork. It works only at the page 
 * level, which means there is a risk of missing the searched comment if they are not on the 
 * first page of the vorbis_comment packet... nothing is perfect */
static void stream_ogg(size_t n) {
	if (ogg.state == STREAM_OGG_OFF) return;
	u8_t* p = streambuf->writep;

	while (n) {
		size_t consumed = min(ogg.miss, n);

		// copy as many bytes as possible and come back later if we don't have enough
		if (ogg.data) {
			memcpy(ogg.data + ogg.want - ogg.miss, p, consumed);
			ogg.miss -= consumed;
			if (ogg.miss) return;
		}

		// we have what we want, let's parse
		switch (ogg.state) {
		case STREAM_OGG_SYNC:
			ogg.miss -= consumed;
			if (consumed) break;

			// we have to memorize position in case any of last 3 bytes match...
			size_t pos = memfind(p, n, "OggS", 4, &ogg.match);
			if (ogg.match == 4) {
				consumed = pos - ogg.match;
				ogg.state = STREAM_OGG_HEADER;
				ogg.miss = ogg.want = sizeof(ogg.header);
				ogg.data = (u8_t*) &ogg.header;
				ogg.match = 0;
			} else {
				if (!ogg.match) LOG_INFO("no OggS at expected position %zu/%zu", pos, n);
				return;
			}
			break;
		case STREAM_OGG_HEADER:
			if (!memcmp(ogg.header.pattern, "OggS", 4)) {
				ogg.miss = ogg.want = ogg.header.count;
				ogg.data = ogg.segments;
				ogg.state = STREAM_OGG_SEGMENTS;
				// granule and page are also in little endian but that does not matter
				ogg.header.serial = itohl(ogg.header.serial);
			} else {
				ogg.state = STREAM_OGG_SYNC;
				ogg.data = NULL;
			}
			break;
		case STREAM_OGG_SEGMENTS:
			// calculate size of page using lacing values
			for (size_t i = 0; i < ogg.want; i++) ogg.miss += ogg.data[i];
			ogg.want = ogg.miss;

			// acquire serial number when we are looking for headers and hit a bos
			if (ogg.serial == ULLONG_MAX && (ogg.header.type & 0x02)) ogg.serial = ogg.header.serial;

			// we have overshot and missed header, reset serial number to restart search (O and -1 are le/be)
			if (ogg.header.serial == ogg.serial && ogg.header.granule && ogg.header.granule != -1) ogg.serial = ULLONG_MAX;

			// not our serial (the above protected us from granule > 0)
			if (ogg.header.serial != ogg.serial) {
				// otherwise, jump over data
				ogg.state = STREAM_OGG_SYNC;
				ogg.data = NULL;
			} else {
				ogg.state = STREAM_OGG_PAGE;
				ogg.data = malloc(ogg.want);
			}
			break;
		case STREAM_OGG_PAGE: {
			char** tag = (char* []){ "\x3vorbis", "OpusTags", NULL };
			size_t ofs = 0;

			/* with OggFlac, we need the next page (packet) - VorbisComment is wrapped into a FLAC_METADATA
			 * and except with vorbis, comment packet starts a new page but even in vorbis, it won't span
			 * accross multiple pages */
			if (ogg.flac) ofs = 4;
			else if (!memcmp(ogg.data, "\x7f""FLAC", 5)) ogg.flac = true;
			else for (size_t n = 0; *tag; tag++, ofs = 0) if ((ofs = memfind(ogg.data, ogg.want, *tag, strlen(*tag), &n)) && n == strlen(*tag)) break;
	
			if (ofs) {
				// u32:len,char[]:vendorId, u32:N, N x (u32:len,char[]:comment)
				char* p = (char*) ogg.data + ofs;
				p += itohl(*p) + 4;
				u32_t count = itohl(*p);
				p += 4;

				// LMS metadata format for Ogg is "Ogg", N x (u16:len,char[]:comment)
				memcpy(stream.header, "Ogg", 3);
				stream.header_len = 3;

				for (u32_t len; count--; p += len) {
					len = itohl(*p);
					p += 4;

					// only report what we use and don't overflow (network byte order)
					if (!strncasecmp(p, "TITLE=", 6) || !strncasecmp(p, "ARTIST=", 7) || !strncasecmp(p, "ALBUM=", 6)) {
						if (stream.header_len + len > MAX_HEADER) break;
						stream.header[stream.header_len++] = len >> 8;
						stream.header[stream.header_len++] = len;
						memcpy(stream.header + stream.header_len, p, len);
						stream.header_len += len;
						LOG_INFO("metadata: %.*s", len, p);
					}
				}

				ogg.flac = false;
				ogg.serial = ULLONG_MAX;
				stream.meta_send = true;
				wake_controller();
				LOG_INFO("metadata length: %u", stream.header_len - 3);
			}

			free(ogg.data);
			ogg.data = NULL;
			ogg.state = STREAM_OGG_SYNC;
			break;
		}
		default: 
			break;
		}

		p += consumed;
		n -= consumed;
	}
}
#else
static void stream_ogg(size_t n) {
	if (!ogg.active) return;

	// fill sync buffer with all what we have
	char* buffer = OG(&ogg.dl, sync_buffer, &ogg.sync, n);
	memcpy(buffer, streambuf->writep, n);
	OG(&ogg.dl, sync_wrote, &ogg.sync, n);

	// extract a page from sync buffer
	while (OG(&ogg.dl, sync_pageout, &ogg.sync, &ogg.page) > 0) {
		uint32_t serial = OG(&ogg.dl, page_serialno, &ogg.page);

		// set stream serialno if we wait for a new one (no multiplexed streams)
		if (ogg.serial == ULLONG_MAX && OG(&ogg.dl, page_bos, &ogg.page)) {
			ogg.serial = serial;
			OG(&ogg.dl, stream_reset_serialno, &ogg.state, serial);	
		}

		// if we overshot, restart searching for headers
		int64_t granule = OG(&ogg.dl, page_granulepos, &ogg.page);
		if (ogg.serial == serial && granule && granule != -1) ogg.serial = ULLONG_MAX;

		// if we don't have a serial number or it's not us, don't bring page in to avoid build-up
		if (ogg.serial != serial) continue;

		// bring new page in (there should be one but multiplexed streams are not supported)
		if (OG(&ogg.dl, stream_pagein, &ogg.state, &ogg.page)) continue;

		// get a packet (there might be more than one in a page)
		while (OG(&ogg.dl, stream_packetout, &ogg.state, &ogg.packet) > 0) {
			size_t ofs = 0;

			// if case of OggFlac, VorbisComment is a flac METADATA_BLOC as 2nd packet (4 bytes in)
			if (ogg.flac) ofs = 4;
			else if (!memcmp(ogg.packet.packet, "\x7f""FLAC", 5)) ogg.flac = true;
			else for (char** tag = (char* []){ "\x3vorbis", "OpusTags", NULL }; *tag && !ofs; tag++) if (!memcmp(ogg.packet.packet, *tag, strlen(*tag))) ofs = strlen(*tag);

			if (!ofs) continue;

			// u32:len,char[]:vendorId, u32:N, N x (u32:len,char[]:comment)
			char* p = (char*)ogg.packet.packet + ofs;
			p += itohl(*p) + 4;
			u32_t count = itohl(*p);
			p += 4;

			// LMS metadata format for Ogg is "Ogg", N x (u16:len,char[]:comment)
			memcpy(stream.header, "Ogg", 3);
			stream.header_len = 3;

			for (u32_t len; count--; p += len) {
				len = itohl(*p);
				p += 4;

				// only report what we use and don't overflow (network byte order)
				if (!strncasecmp(p, "TITLE=", 6) || !strncasecmp(p, "ARTIST=", 7) || !strncasecmp(p, "ALBUM=", 6)) {
					if (stream.header_len + len > MAX_HEADER) break;
					stream.header[stream.header_len++] = len >> 8;
					stream.header[stream.header_len++] = len;
					memcpy(stream.header + stream.header_len, p, len);
					stream.header_len += len;
					LOG_INFO("metadata: %.*s", len, p);
				}
			}

			// ogg_packet_clear does not need to be called as metadata packets terminate a page
			ogg.flac = false;
			ogg.serial = ULLONG_MAX;
			stream.meta_send = true;
			wake_controller();
			LOG_INFO("metadata length: %u", stream.header_len - 3);

			// return as we might have more than one metadata set but we want the first one
			return;
		}
	}
}
#endif

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

		if (_poll(&pollinfo, 100)) {

			LOCK;

			// check socket has not been closed while in poll
			if (fd < 0) {
				UNLOCK;
				continue;
			}

			if ((pollinfo.revents & POLLOUT) && stream.state == SEND_HEADERS) {
				if (send_header()) stream.state = RECV_HEADERS;
				header_mlen = stream.header_len;
				stream.header_len = 0;
				UNLOCK;
				continue;
			}
					
			if (pollinfo.revents & (POLLIN | POLLHUP)) {

				// get response headers
				if (stream.state == RECV_HEADERS) {

					// read one byte at a time to catch end of header
					char c;
					static int endtok;

					int n = _recv(fd, &c, 1, 0);
					if (n <= 0) {
						if (n < 0 && _last_error() == ERROR_WOULDBLOCK) {
							UNLOCK;
							continue;
						}
						LOG_INFO("error reading headers: %s", n ? strerror(last_error()) : "closed");
#if USE_SSL
						if (!ssl && !stream.header_len) {
							int sock;
							closesocket(fd);
							fd = -1;
							stream.header_len = header_mlen;
							LOG_INFO("now attempting with SSL");

							// must be performed locked in case slimproto sends a disconnects
							sock = connect_socket(true);
						
							if (sock >= 0) {
								fd = sock;
								stream.state = SEND_HEADERS;
								UNLOCK;
								continue;
							}
						}
#endif
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
						int n = _recv(fd, &c, 1, 0);
						if (n <= 0) {
							if (n < 0 && _last_error() == ERROR_WOULDBLOCK) {
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
						int n = _recv(fd, stream.header + stream.header_len, stream.meta_left, 0);
						if (n <= 0) {
							if (n < 0 && _last_error() == ERROR_WOULDBLOCK) {
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
					int error;

					space = min(_buf_space(streambuf), _buf_cont_write(streambuf));
					if (stream.meta_interval) {
						space = min(space, stream.meta_next);
					}
					
					n = _recv(fd, streambuf->writep, space, 0);
					if (n == 0) {
						LOG_INFO("end of stream (%u bytes)", stream.bytes);
						_disconnect(DISCONNECT, DISCONNECT_OK);
					}
					if (n < 0) {
						error = _last_error();
						if (error != ERROR_WOULDBLOCK) {
							LOG_INFO("error reading: %s (%d)", strerror(error), error);
							_disconnect(DISCONNECT, REMOTE_DISCONNECT);
						}
					}
					
					if (n > 0) {
						stream_ogg(n);
						_buf_inc_writep(streambuf, n);
						stream.bytes += n;
						if (stream.meta_interval) {
							stream.meta_next -= n;
						}
					} else {
						UNLOCK;
						continue;
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
	
#if USE_SSL	
	if (SSLctx) {
		SSL_CTX_free(SSLctx);
	}	
#endif	

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
		exit(1);
	}

#if USE_LIBOGG && !LINKALL
	ogg.dl.handle = dlopen(LIBOGG, RTLD_NOW);
	if (!ogg.dl.handle) {
		LOG_INFO("ogg dlerror: %s", dlerror());
	}
	ogg.dl.ogg_stream_init = dlsym(ogg.dl.handle, "ogg_stream_init");
	ogg.dl.ogg_stream_clear = dlsym(ogg.dl.handle, "ogg_stream_clear");
	ogg.dl.ogg_stream_reset_serialno = dlsym(ogg.dl.handle, "ogg_stream_reset_serialno");
	ogg.dl.ogg_stream_pagein = dlsym(ogg.dl.handle, "ogg_stream_pagein");
	ogg.dl.ogg_stream_packetout = dlsym(ogg.dl.handle, "ogg_stream_packetout");
	ogg.dl.ogg_sync_clear = dlsym(ogg.dl.handle, "ogg_sync_clear");
	ogg.dl.ogg_sync_buffer = dlsym(ogg.dl.handle, "ogg_sync_buffer");
	ogg.dl.ogg_sync_wrote = dlsym(ogg.dl.handle, "ogg_sync_wrote");
	ogg.dl.ogg_sync_pageout = dlsym(ogg.dl.handle, "ogg_sync_pageout");
	ogg.dl.ogg_page_bos = dlsym(ogg.dl.handle, "ogg_page_bos");
	ogg.dl.ogg_page_serialno = dlsym(ogg.dl.handle, "ogg_page_serialno");
	ogg.dl.ogg_page_granulepos = dlsym(ogg.dl.handle, "ogg_page_granulepos");
#endif
	
#if USE_SSL
#if !LINKALL && !NO_SSLSYM
	if (ssl_loaded) {
#endif
	SSL_library_init();
	SSLctx = SSL_CTX_new(SSLv23_client_method());
	if (SSLctx == NULL) {
		LOG_ERROR("unable to allocate SSL context");
		exit(1);
	}	
	SSL_CTX_set_options(SSLctx, SSL_OP_NO_SSLv2);
#if !LINKALL && !NO_SSLSYM
	}
#endif	
	ssl = NULL;
#endif
	
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

void stream_sock(u32_t ip, u16_t port, bool use_ssl, bool use_ogg, const char* header, size_t header_len, unsigned threshold, bool cont_wait) {
	char* p;
	int sock;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port = port;

	*host = '\0';
	p = strcasestr(header, "Host:");
	if (p) {
		sscanf(p, "Host:%255s", host);
		if ((p = strchr(host, ':')) != NULL) *p = '\0';
	}

	port = ntohs(port);
	sock = connect_socket(use_ssl || port == 443);

	// try one more time with plain socket
	if (sock < 0 && port == 443 && !use_ssl) sock = connect_socket(false);

	if (sock < 0) {
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
	*(stream.header + header_len) = '\0';

	LOG_INFO("header: %s", stream.header);

	stream.sent_headers = false;
	stream.bytes = 0;
	stream.threshold = threshold;

#if USE_LIBOGG
#if !LINKALL
	ogg.active = use_ogg && ogg.dl.handle;
#else 
	ogg.active = use_ogg;
#endif
	if (use_ogg) {
		OG(&ogg.dl, stream_clear, &ogg.state);
		OG(&ogg.dl, sync_clear, &ogg.sync);
		OG(&ogg.dl, stream_init, &ogg.state, -1);
	}
#else
	ogg.miss = ogg.match = 0;
	ogg.state = use_ogg ? STREAM_OGG_SYNC : STREAM_OGG_OFF;
#endif
	ogg.flac = false;
	ogg.serial = ULLONG_MAX;

	UNLOCK;
}

bool stream_disconnect(void) {
	bool disc = false;
	LOCK;
#if USE_SSL
	if (ssl) {
		SSL_shutdown(ssl);
		SSL_free(ssl);
		ssl = NULL;
	}
#endif
	if (fd != -1) {
		closesocket(fd);
		fd = -1;
		disc = true;
	}
	stream.state = STOPPED;
#if USE_LIBOGG
	if (ogg.active) {
		OG(&ogg.dl, stream_clear, &ogg.state);
		OG(&ogg.dl, sync_clear, &ogg.sync);
	}
#else
	if (ogg.state == STREAM_OGG_PAGE && ogg.data) free(ogg.data);
	ogg.data = NULL;
#endif

	UNLOCK;
	return disc;
}

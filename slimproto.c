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

#include "squeezelite.h"
#include "slimproto.h"

static log_level loglevel;

#define PORT 3483

#define MAXBUF 4096

#if IS_LITTLE_ENDIAN
#define LOCAL_PLAYER_IP   0x0100007f // 127.0.0.1
#define LOCAL_PLAYER_PORT 0x9b0d     // 3483
#else
#define LOCAL_PLAYER_IP   0x7f000001 // 127.0.0.1
#define LOCAL_PLAYER_PORT 0x0d9b     // 3483
#endif

static int sock;
static int efd;
static in_addr_t slimproto_ip;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;

extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;

extern struct codec *codecs[];

#define LOCK_S   pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK_S pthread_mutex_unlock(&streambuf->mutex)
#define LOCK_O   pthread_mutex_lock(&outputbuf->mutex)
#define UNLOCK_O pthread_mutex_unlock(&outputbuf->mutex)

static struct {
	u32_t updated;
	u32_t stream_start;
	u32_t stream_full;
	u32_t stream_size;
	u64_t stream_bytes;
	u32_t output_full;
	u32_t output_size;
	u32_t frames_played;
	u32_t alsa_frames;
	u32_t current_sample_rate;
	u32_t last;
	stream_state stream_state;
} status;

int autostart;
bool sentSTMu, sentSTMo, sentSTMl;

void send_packet(u8_t *packet, size_t len) {
	u8_t *ptr = packet;
	size_t n;

	// may block
	while (len) {
		n = send(sock, ptr, len, 0);
		if (n < 0) {
			LOG_INFO("failed writing to socket: %s", strerror(errno));
			return;
		}
		ptr += n;
		len -= n;
	}
}

static void sendHELO(bool reconnect, const char *cap, u8_t mac[6]) {
	const char *capbase = "Model=squeezelite,ModelName=SqueezeLite,AccuratePlayPoints=1,HasDigitalOut=1,";
	
	struct HELO_packet pkt = {
		.opcode = "HELO",
		.length = htonl(sizeof(struct HELO_packet) - 8 + strlen(capbase) + strlen(cap)),
		.deviceid = 12, // squeezeplay
		.revision = 0, 
	};
	packn(&pkt.wlan_channellist, reconnect ? 0x4000 : 0x0000);
	packN(&pkt.bytes_received_H, (u64_t)status.stream_bytes >> 32);
	packN(&pkt.bytes_received_L, (u64_t)status.stream_bytes & 0xffffffff);
	memcpy(pkt.mac, mac, 6);

	LOG_INFO("mac: %02x:%02x:%02x:%02x:%02x:%02x", pkt.mac[0], pkt.mac[1], pkt.mac[2], pkt.mac[3], pkt.mac[4], pkt.mac[5]);

	send_packet((u8_t *)&pkt, sizeof(pkt));
	send_packet((u8_t *)capbase, strlen(capbase));
	send_packet((u8_t *)cap, strlen(cap));
}

static void sendSTAT(const char *event, u32_t server_timestamp) {
	struct STAT_packet pkt;
	u32_t now = gettime_ms();
	u32_t ms_played;

	if (status.current_sample_rate) {
		ms_played = (u32_t)(((u64_t)(status.frames_played - status.alsa_frames) * (u64_t)1000) / (u64_t)status.current_sample_rate);
		if (now > status.updated) ms_played += (now - status.updated);
	} else {
		ms_played = 0;
	}
	
	memset(&pkt, 0, sizeof(struct STAT_packet));
	memcpy(&pkt.opcode, "STAT", 4);
	pkt.length = htonl(sizeof(struct STAT_packet) - 8);
	memcpy(&pkt.event, event, 4);
	// num_crlf
	// mas_initialized; mas_mode;
	packN(&pkt.stream_buffer_fullness, status.stream_full);
	packN(&pkt.stream_buffer_size, status.stream_size);
	packN(&pkt.bytes_received_H, (u64_t)status.stream_bytes >> 32);
	packN(&pkt.bytes_received_L, (u64_t)status.stream_bytes & 0xffffffff);
	pkt.signal_strength = 0xffff;
	packN(&pkt.jiffies, now);
	packN(&pkt.output_buffer_size, status.output_size);
	packN(&pkt.output_buffer_fullness, status.output_full);
	packN(&pkt.elapsed_seconds, ms_played / 1000);
	// voltage;
	packN(&pkt.elapsed_milliseconds, ms_played);
	pkt.server_timestamp = server_timestamp; // keep this is server format - don't unpack/pack
	// error_code;

	LOG_INFO("STAT: %s", event);

	if (loglevel == SDEBUG) {
		LOG_SDEBUG("received bytesL: %u streambuf: %u outputbuf: %u calc elapsed: %u real elapsed: %u (diff: %u) alsa: %u delay: %d",
				   (u32_t)status.stream_bytes, status.stream_full, status.output_full, ms_played, now - status.stream_start,
				   ms_played - now + status.stream_start, status.alsa_frames * 1000 / status.current_sample_rate, now - status.updated);
	}

	send_packet((u8_t *)&pkt, sizeof(pkt));
}

static void sendDSCO(disconnect_code disconnect) {
	struct DSCO_packet pkt;

	memset(&pkt, 0, sizeof(pkt));
	memcpy(&pkt.opcode, "DSCO", 4);
	pkt.length = htonl(sizeof(pkt) - 8);
	pkt.reason = disconnect & 0xFF;

	LOG_INFO("DSCO: %d", disconnect);

	send_packet((u8_t *)&pkt, sizeof(pkt));
}

static void sendRESP(const char *header, size_t len) {
	struct RESP_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "RESP", 4);
	pkt_header.length = htonl(sizeof(pkt_header) + len - 8);

	LOG_INFO("RESP");

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header));
	send_packet((u8_t *)header, len);
}

static void sendMETA(const char *meta, size_t len) {
	struct META_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "META", 4);
	pkt_header.length = htonl(sizeof(pkt_header) + len - 8);

	LOG_INFO("META");

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header));
	send_packet((u8_t *)meta, len);
}

static void sendSETDName(const char *name) {
	struct SETD_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "SETD", 4);

	pkt_header.id = 0; // id 0 is playername S:P:Squeezebox2
	pkt_header.length = htonl(sizeof(pkt_header) + strlen(name) + 1 - 8);

	LOG_INFO("set playername: %s", name);

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header));
	send_packet((u8_t *)name, strlen(name) + 1);
}

static void process_strm(u8_t *pkt, int len) {
	struct strm_packet *strm = (struct strm_packet *)pkt;

	LOG_INFO("strm command %c", strm->command);

	switch(strm->command) {
	case 't':
		sendSTAT("STMt", strm->replay_gain); // STMt replay_gain is no longer used to track latency, but support it
		break;
	case 'q':
		stream_disconnect();
		buf_flush(streambuf);
		buf_flush(outputbuf);
		break;
	case 'f':
		stream_disconnect();
		buf_flush(streambuf);
		buf_flush(outputbuf);
		break;
	case 'p':
		{
			unsigned interval = unpackN(&strm->replay_gain);
			LOCK_O;
			output.pause_frames = interval * status.current_sample_rate / 1000;
			output.state = interval ? OUTPUT_PAUSE_FRAMES : OUTPUT_STOPPED;				
			UNLOCK_O;
			if (!interval) sendSTAT("STMp", 0);
			LOG_INFO("pause interval: %u", interval);
		}
		break;
	case 'a':
		{
			unsigned interval = unpackN(&strm->replay_gain);
			LOCK_O;
			output.skip_frames = interval * status.current_sample_rate / 1000;
			output.state = OUTPUT_SKIP_FRAMES;				
			UNLOCK_O;
			LOG_INFO("skip ahead interval: %u", interval);
		}
		break;
	case 'u':
		{
			unsigned jiffies = unpackN(&strm->replay_gain);
			LOCK_O;
			output.state = jiffies ? OUTPUT_START_AT : OUTPUT_RUNNING;
			output.start_at = jiffies;
			decode.state = DECODE_RUNNING;
			UNLOCK_O;
			LOG_INFO("unpause at: %u now: %u", jiffies, gettime_ms());
			sendSTAT("STMr", 0);
		}
		break;
	case 's':
		{
			unsigned header_len = len - sizeof(struct strm_packet);
			char *header = (char *)(pkt + sizeof(struct strm_packet));
			in_addr_t ip = strm->server_ip; // keep in network byte order
			u16_t port = strm->server_port; // keep in network byte order
			if (ip == 0) ip = slimproto_ip; 

			LOG_INFO("strm s autostart: %c transition period: %u transition type: %u", 
					 strm->autostart, strm->transition_period, strm->transition_type - '0');

			autostart = strm->autostart - '0';
			sendSTAT("STMf", 0);
			if (header_len > MAX_HEADER -1) {
				LOG_WARN("header too long: %u", header_len);
				break;
			}
			codec_open(strm->format, strm->pcm_sample_size, strm->pcm_sample_rate, strm->pcm_channels, strm->pcm_endianness);
			if (ip == LOCAL_PLAYER_IP && port == LOCAL_PLAYER_PORT) {
				// extension to slimproto for LocalPlayer - header is filename not http header, don't expect cont
				stream_file(header, header_len, strm->threshold * 1024);
				autostart -= 2;
			} else {
				stream_sock(ip, port, header, header_len, strm->threshold * 1024, autostart >= 2);
			}
			sendSTAT("STMc", 0);
			sentSTMu = sentSTMo = sentSTMl = false;
			LOCK_O;
			output.threshold = strm->output_threshold;
			output.next_replay_gain = unpackN(&strm->replay_gain);
			output.fade_mode = strm->transition_type - '0';
			output.fade_secs = strm->transition_period;
			LOG_INFO("set fade mode: %u", output.fade_mode);
			UNLOCK_O;
		}
		break;
	default:
		LOG_INFO("unhandled strm %c", strm->command);
		break;
	}
}

static void process_cont(u8_t *pkt, int len) {
	struct cont_packet *cont = (struct cont_packet *)pkt;
	cont->metaint = unpackN(&cont->metaint);

	LOG_INFO("cont metaint: %u loop: %u", cont->metaint, cont->loop);

	if (autostart > 1) {
		autostart -= 2;
		LOCK_S;
		if (stream.state == STREAMING_WAIT) {
			stream.state = STREAMING_BUFFERING;
			stream.meta_interval = stream.meta_next = cont->metaint;
		}
		UNLOCK_S;
		wake_controller();
	}
}

static void process_aude(u8_t *pkt, int len) {
	struct aude_packet *aude = (struct aude_packet *)pkt;

	LOG_INFO("enable spdif: %d dac: %d", aude->enable_spdif, aude->enable_dac);

	LOCK_O;
	if (!aude->enable_spdif && output.state != OUTPUT_OFF) {
		output.state = OUTPUT_OFF;
	}
	if (aude->enable_spdif && output.state == OUTPUT_OFF) {
		output.state = OUTPUT_STOPPED;
	}
	UNLOCK_O;
}

static void process_audg(u8_t *pkt, int len) {
	struct audg_packet *audg = (struct audg_packet *)pkt;
	audg->gainL = unpackN(&audg->gainL);
	audg->gainR = unpackN(&audg->gainR);

	LOG_INFO("audg gainL: %u gainR: %u adjust: %u", audg->gainL, audg->gainR, audg->adjust);

	LOCK_O;
	output.gainL = audg->adjust ? audg->gainL : FIXED_ONE;
	output.gainR = audg->adjust ? audg->gainR : FIXED_ONE;
	UNLOCK_O;
}

struct handler {
	char opcode[5];
	void (*handler)(u8_t *, int);
};

static struct handler handlers[] = {
	{ "strm", process_strm },
	{ "cont", process_cont },
	{ "aude", process_aude },
	{ "audg", process_audg },
	{ "",     NULL  },
};

static void process(u8_t *pack, int len) {
	struct handler *h = handlers;
	while (h->handler && strncmp((char *)pack, h->opcode, 4)) { h++; }

	if (h->handler) {
		LOG_INFO("%s", h->opcode);
		h->handler(pack, len);
	} else {
		pack[4] = '\0';
		LOG_INFO("unhandled %s", (char *)pack);
	}
}

static void slimproto_run() {
	struct pollfd pollinfo[2] = { { .fd = sock, .events = POLLIN }, { .fd = efd, .events = POLLIN } };
	static u8_t buffer[MAXBUF];
	int  expect = 0;
	int  got    = 0;

	while (true) {

		bool wake = false;

		if (poll(pollinfo, 2, 1000)) {

			if (pollinfo[0].revents) {

				if (expect > 0) {
					int n = recv(sock, buffer + got, expect, 0);
					if (n <= 0) {
						LOG_INFO("error reading from socket: %s", n ? strerror(errno) : "closed");
						return;
					}
					expect -= n;
					got += n;
					if (expect == 0) {
						process(buffer, got);
						got = 0;
					}
				} else if (expect == 0) {
					int n = recv(sock, buffer + got, 2 - got, 0);
					if (n <= 0) {
						LOG_INFO("error reading from socket: %s", n ? strerror(errno) : "closed");
						return;
					}
					got += n;
					if (got == 2) {
						expect = buffer[0] << 8 | buffer[1]; // length pack 'n'
						got = 0;
						if (expect > MAXBUF) {
							LOG_ERROR("FATAL: slimproto packet too big: %d > %d", expect, MAXBUF);
							return;
						}
					}
				} else {
					LOG_ERROR("FATAL: negative expect");
					return;
				}

			}

			if (pollinfo[1].revents) {
				uint64_t u;
				u = read(efd, &u, sizeof(uint64_t));
				wake = true;
			}
		}

		// update playback state when woken or every 100ms
		u32_t now = gettime_ms();
		static u32_t last = 0;

		if (wake || now - last > 100 || last > now) {
			last = now;

			bool _sendSTMs = false;
			bool _sendDSCO = false;
			bool _sendRESP = false;
			bool _sendMETA = false;
			bool _sendSTMd = false;
			bool _sendSTMt = false;
			bool _sendSTMl = false;
			bool _sendSTMu = false;
			bool _sendSTMo = false;
			bool _sendSTMn = false;
			disconnect_code disconnect;
			static char header[MAX_HEADER];
			size_t header_len = 0;

			LOCK_S;
			status.stream_full = _buf_used(streambuf);
			status.stream_size = streambuf->size;
			status.stream_bytes = stream.bytes;
			status.stream_state = stream.state;
						
			if (stream.state == DISCONNECT) {
				disconnect = stream.disconnect;
				stream.state = STOPPED;
				_sendDSCO = true;
			}
			if (!stream.sent_headers && 
				(stream.state == STREAMING_HTTP || stream.state == STREAMING_WAIT || stream.state == STREAMING_BUFFERING)) {
				header_len = stream.header_len;
				memcpy(header, stream.header, header_len);
				_sendRESP = true;
				stream.sent_headers = true;
			}
			if (stream.meta_send) {
				header_len = stream.header_len;
				memcpy(header, stream.header, header_len);
				_sendMETA = true;
				stream.meta_send = false;
			}
			UNLOCK_S;
			
			LOCK_O;
			status.output_full = _buf_used(outputbuf);
			status.output_size = outputbuf->size;
			status.frames_played = output.frames_played;
			status.current_sample_rate = output.current_sample_rate;
			status.updated = output.updated;
			status.alsa_frames = output.alsa_frames;
			
			if (output.track_started) {
				_sendSTMs = true;
				output.track_started = false;
				status.stream_start = output.updated;
			}
			if (decode.state == DECODE_COMPLETE) {
				_sendSTMd = true;
				decode.state = DECODE_STOPPED;
			}
			if (decode.state == DECODE_ERROR) {
				_sendSTMn = true;
				decode.state = DECODE_STOPPED;
			}
			if ((status.stream_state == STREAMING_HTTP || status.stream_state == STREAMING_FILE) && !sentSTMl 
				&& decode.state == DECODE_STOPPED) {
				if (autostart == 0) {
					_sendSTMl = true;
					sentSTMl = true;
				} else if (autostart == 1) {
					decode.state = DECODE_RUNNING;
					if (output.state == OUTPUT_STOPPED) {
						output.state = OUTPUT_BUFFER;
					}
				}
				// autostart 2 and 3 require cont to be received first
			}
			if (output.state == OUTPUT_RUNNING && !sentSTMu && status.output_full == 0 && status.stream_state <= DISCONNECT) {
				_sendSTMu = true;
				sentSTMu = true;
			}
			if (output.state == OUTPUT_RUNNING && !sentSTMo && status.output_full == 0 && status.stream_state == STREAMING_HTTP) {
				_sendSTMo = true;
				sentSTMo = true;
			}
			if (decode.state == DECODE_RUNNING && now - status.last > 1000) {
				_sendSTMt = true;
				status.last = now;
			}
			UNLOCK_O;
		
			// send packets once locks released as packet sending can block
			if (_sendDSCO) sendDSCO(disconnect);
			if (_sendSTMs) sendSTAT("STMs", 0);
			if (_sendSTMd) sendSTAT("STMd", 0);
			if (_sendSTMt) sendSTAT("STMt", 0);
			if (_sendSTMl) sendSTAT("STMl", 0);
			if (_sendSTMu) sendSTAT("STMu", 0);
			if (_sendSTMo) sendSTAT("STMo", 0);
			if (_sendSTMn) sendSTAT("STMn", 0);
			if (_sendRESP) sendRESP(header, header_len);
			if (_sendMETA) sendMETA(header, header_len);
		}
	}
}

// called from other threads to wake state machine above
void wake_controller(void) {
	uint64_t u = 1;
	u = write(efd, &u, sizeof(uint64_t));
}

in_addr_t discover_server(void) {
    struct sockaddr_in d;
    struct sockaddr_in s;

	int disc_sock = socket(AF_INET, SOCK_DGRAM, 0);

	int enable = 1;
	setsockopt(disc_sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));

	memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
	d.sin_port = htons(PORT);
    d.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	char *buf = "e";

	struct pollfd pollinfo = { .fd = disc_sock, .events = POLLIN };

	do {

		LOG_INFO("sending discovery");
		memset(&s, 0, sizeof(s));

		if (sendto(disc_sock, buf, 1, 0, (struct sockaddr *)&d, sizeof(d)) < 0) {
			LOG_INFO("error sending disovery");
		}

		if (poll(&pollinfo, 1, 5000)) {
			char readbuf[10];
			socklen_t slen = sizeof(s);
			recvfrom(disc_sock, readbuf, 10, 0, (struct sockaddr *)&s, &slen);
			LOG_INFO("got response from: %s:%d", inet_ntoa(s.sin_addr), ntohs(s.sin_port));
		}

	} while (s.sin_addr.s_addr == 0);

	close(disc_sock);

	return s.sin_addr.s_addr;
}

void slimproto(log_level level, const char *addr, u8_t mac[6], const char *name) {
    struct sockaddr_in serv_addr;
	static char buf[128];
	bool reconnect = false;

	efd = eventfd(0, 0);

	loglevel = level;
	slimproto_ip = addr ? inet_addr(addr) : discover_server();

	LOCK_O;
	sprintf(buf, "MaxSampleRate=%u", output.max_sample_rate); 
	int i;
	for (i = 0; i < MAX_CODECS; i++) {
		if (codecs[i] && codecs[i]->id && strlen(buf) < 128 - 10) {
			strcat(buf, ",");
			strcat(buf, codecs[i]->types);
		}
	}
	UNLOCK_O;

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = slimproto_ip;
	serv_addr.sin_port = htons(PORT);

	LOG_INFO("connecting to %s:%d", inet_ntoa(serv_addr.sin_addr), ntohs(serv_addr.sin_port));
	LOG_DEBUG("cap: %s", buf);

	while (true) {

		sock = socket(AF_INET, SOCK_STREAM, 0);

		if (connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {

			LOG_INFO("unable to connect to server");
			sleep(5);

		} else {
		
			LOG_INFO("connected");

			// check if this is a local player now we are connected & signal to server via 'loc' format
			// this requires LocalPlayer server plugin to enable direct file access
			if (!reconnect) {
				struct sockaddr_in our_addr;
				socklen_t len;

				len = sizeof(our_addr);
				getsockname(sock, (struct sockaddr *) &our_addr, &len);

				if (our_addr.sin_addr.s_addr == serv_addr.sin_addr.s_addr) {
					LOG_INFO("local player");
					strcat(buf, ",");
					strcat(buf, "loc");
				}

				reconnect = true;
			}

			sendHELO(reconnect, buf, mac);

			if (name) {
				sendSETDName(name);
				name = NULL;
			}

			slimproto_run();

			usleep(100000);
		}

		close(sock);
	}

	close(efd);
}

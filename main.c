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

#include "squeezelite.h"

#include <signal.h>

#define TITLE "Squeezelite " VERSION ", Copyright 2012, 2013 Adrian Smith."

static void usage(const char *argv0) {
	printf(TITLE " See -t for license terms\n"
		   "Usage: %s [options] [<server_ip>]\n"
		   "  <server_ip>\t\tConnect to server server at given IP address, otherwise uses autodiscovery\n"
		   "  -o <output device>\tSpecify output device, default \"default\"\n"
		   "  -l \t\t\tList output devices\n"
#if ALSA
		   "  -a <time>:<count>\tSpecify ALSA buffer_time (ms) and period_count, default 20:4\n"
#endif
#if PORTAUDIO
		   "  -a <latency>\t\tSpecify output target latency in ms\n"
#endif
		   "  -b <stream>:<output>\tSpecify internal Stream and Output buffer sizes in Kbytes\n"
		   "  -c <codec1>,<codec2>\tRestrict codecs those specified, otherwise loads all available codecs; known codecs: flac,pcm,mp3,ogg,aac\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output, level: info|debug|sdebug\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
		   "  -m <mac addr>\t\tSet mac address, format: ab:cd:ef:12:34:56\n"
		   "  -n <name>\t\tSet the player name\n"
		   "  -r <rate>\t\tMax sample rate for output device, enables output device to be off when squeezelite is started\n"
#if ALSA
		   "  -w \t\t\tDisable ASLA mmap output\n"
#endif
#if LINUX
		   "  -z \t\t\tDaemonize\n"
#endif
		   "  -t \t\t\tLicense terms\n"
		   "\n",
		   argv0);
}

static void license(void) {
	printf(TITLE "\n\n"
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n"
		   );
}

void sighandler(int signum) {
	slimproto_stop();
}

int main(int argc, char **argv) {
	char *server = NULL;
	char *output_device = "default";
	char *codecs = NULL;
	char *name = NULL;
	char *logfile = NULL;
	u8_t mac[6];
	unsigned stream_buf_size = STREAMBUF_SIZE;
	unsigned output_buf_size =  OUTPUTBUF_SIZE;
	unsigned max_rate = 0;
#if LINUX
	bool daemonize = false;
#endif
#if ALSA
	unsigned alsa_buffer_time = ALSA_BUFFER_TIME;
	unsigned alsa_period_count = ALSA_PERIOD_COUNT;
	unsigned alsa_mmap = true;
#endif
#if PORTAUDIO
	unsigned pa_latency = 0;
#endif
	
	log_level log_output = lWARN;
	log_level log_stream = lWARN;
	log_level log_decode = lWARN;
	log_level log_slimproto = lWARN;

	char *optarg = NULL;
	int optind = 1;

	get_mac(mac);

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("oabcdfmnr", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("ltwz", opt)) {
			optarg = NULL;
			optind += 1;
		} else {
			usage(argv[0]);
            exit(0);
		}

		switch (opt[0]) {
        case 'o':
            output_device = optarg;
            break;
		case 'a': 
			{
#if ALSA				
				char *t = strtok(optarg, ":");
				char *c = strtok(NULL, ":");
				if (t) alsa_buffer_time  = atoi(t) * 1000;
				if (c) alsa_period_count = atoi(c);
#endif
#if PORTAUDIO
				pa_latency = atoi(optarg);
#endif
			}
			break;
		case 'b': 
			{
				char *s = strtok(optarg, ":");
				char *o = strtok(NULL, ":");
				if (s) stream_buf_size = atoi(s) * 1024;
				if (o) output_buf_size = atoi(o) * 1024;
			}
			break;
		case 'c':
			codecs = optarg;
			break;
        case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "slimproto")) log_slimproto = new;
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    log_stream = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    log_decode = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    log_output = new;
				} else {
					usage(argv[0]);
					exit(0);
				}
			}
            break;
		case 'f':
			logfile = optarg;
			break;
		case 'm':
			{
				int byte = 0;
				char *tmp;
				char *t = strtok(optarg, ":");
				while (t && byte < 6) {
					mac[byte++] = (u8_t)strtoul(t, &tmp, 16);
					t = strtok(NULL, ":");
				}
			}
			break;
		case 'r':
			max_rate = atoi(optarg);
			break;
		case 'n':
			name = optarg;
			break;
		case 'l':
			list_devices();
			exit(0);
			break;
#if ALSA
		case 'w':
			alsa_mmap = false;
			break;
#endif
#if LINUX
		case 'z':
			daemonize = true;
			break;
#endif
		case 't':
			license();
			exit(0);
        default:
			break;
        }
    }

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif

	// remaining argument should be a server address
	if (argc == optind + 1) {
		server = argv[optind];
	}

	if (logfile) {
		if (!freopen(logfile, "a", stdout) || !freopen(logfile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", logfile, strerror(errno));
		}
	}

#if LINUX
	if (daemonize) {
		if (daemon(0, logfile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif

#if WIN
	winsock_init();
#endif

#if ALSA
	output_init(log_output, output_device, output_buf_size, alsa_buffer_time, alsa_period_count, alsa_mmap, max_rate);
#endif
#if PORTAUDIO
	output_init(log_output, output_device, output_buf_size, pa_latency, max_rate);
#endif
	stream_init(log_stream, stream_buf_size);
	decode_init(log_decode, codecs);

	slimproto(log_slimproto, server, mac, name);
	
	decode_close();
	stream_close();
	output_close();

#if WIN
	winsock_close();
#endif

	exit(0);
}

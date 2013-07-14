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
		   "Usage: %s [options]\n"
		   "  -s <server>[:<port>]\tConnect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -o <output device>\tSpecify output device, default \"default\"\n"
		   "  -l \t\t\tList output devices\n"
#if ALSA
		   "  -a <b>:<p>:<f>:<m>\tSpecify ALSA params to open output device, b = buffer time in ms or size in bytes, p = period count or size in bytes, f sample format (16|24|24_3|32), m = use mmap (0|1)\n"
#endif
#if PORTAUDIO
#if PA18API
		   "  -a <frames>:<buffers>\tSpecify output target 4 byte frames per buffer, number of buffers\n"
#else
		   "  -a <latency>\t\tSpecify output target latency in ms\n"
#endif
#endif
		   "  -b <stream>:<output>\tSpecify internal Stream and Output buffer sizes in Kbytes\n"
		   "  -c <codec1>,<codec2>\tRestrict codecs those specified, otherwise loads all available codecs; known codecs: flac,pcm,mp3,ogg,aac (mad,mpg for specific mp3 codec)\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output, level: info|debug|sdebug\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
		   "  -m <mac addr>\t\tSet mac address, format: ab:cd:ef:12:34:56\n"
		   "  -n <name>\t\tSet the player name\n"
#if ALSA
		   "  -p <priority>\t\tSet real time priority of output thread (1-99)\n"
#endif
		   "  -r <rate>\t\tMax sample rate for output device, enables output device to be off when squeezelite is started\n"
#if RESAMPLE
		   "  -u [params]\t\tUpsample to max rate for device, params = <quality>:<flags>:<attenuation>:<precision>:<passband_end>:<stopband_start>:<phase_response>,\n" 
		   "  \t\t\t quality = (v|h|m|l|q)(|L|I|M)(|s),\n"
		   "  \t\t\t flags = num in hex,\n"
		   "  \t\t\t attenuation = attenuation in dB to apply (default is -1db if not explicitly set),\n"
		   "  \t\t\t precision = number of bits precision (NB. HQ = 20. VHQ = 28),\n"
		   "  \t\t\t passband_end = number in percent (0dB pt. bandwidth to preserve. nyquist = 100%%),\n"
		   "  \t\t\t stopband_start = number in percent (Aliasing/imaging control. > passband_end),\n"
		   "  \t\t\t phase_response = 0-100 (0 = minimum / 50 = linear / 100 = maximum)\n"
#endif
#if LINUX || SUN
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

static void sighandler(int signum) {
	slimproto_stop();

	// remove ourselves in case above does not work, second SIGINT will cause non gracefull shutdown
	signal(signum, SIG_DFL);
}

int main(int argc, char **argv) {
	char *server = NULL;
	char *output_device = "default";
	char *codecs = NULL;
	char *name = NULL;
	char *logfile = NULL;
	u8_t mac[6];
	unsigned stream_buf_size = STREAMBUF_SIZE;
	unsigned output_buf_size = 0; // set later
	unsigned max_rate = 0;
	char *upsample = NULL;
#if LINUX || SUN
	bool daemonize = false;
#endif
#if ALSA
	unsigned alsa_buffer = ALSA_BUFFER_TIME;
	unsigned alsa_period = ALSA_PERIOD_COUNT;
	char *alsa_sample_fmt = NULL;
	bool alsa_mmap = true;
	unsigned rt_priority = OUTPUT_RT_PRIORITY;
#endif
#if PORTAUDIO
#ifndef PA18API
	unsigned pa_latency = 0;
#else
	unsigned pa_frames = 0;
	unsigned pa_nbufs = 0;
#endif /* PA18API */
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
		if (strstr("oabcdfmnprs", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
#if RESAMPLE
		} else if (strstr("ltuz", opt)) {
#else
		} else if (strstr("ltz", opt)) {
#endif
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
				char *t = next_param(optarg, ':');
				char *c = next_param(NULL, ':');
				char *s = next_param(NULL, ':');
				char *m = next_param(NULL, ':');
				if (t) alsa_buffer = atoi(t);
				if (c) alsa_period = atoi(c);
				if (s) alsa_sample_fmt = s;
				if (m) alsa_mmap = atoi(m);
#endif
#if PORTAUDIO
#ifndef PA18API
				pa_latency = (unsigned)atoi(optarg);
#else
				char *t = next_param(optarg, ':');
				char *c = next_param(NULL, ':');
				if (t) pa_frames  = atoi(t);
				if (c) pa_nbufs = atoi(c);
#endif
#endif
			}
			break;
		case 'b': 
			{
				char *s = next_param(optarg, ':');
				char *o = next_param(NULL, ':');
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
		case 's':
			server = optarg;
			break;
		case 'n':
			name = optarg;
			break;
#if ALSA
		case 'p':
			rt_priority = atoi(optarg);
			if (rt_priority > 99 || rt_priority < 1) {
				usage(argv[0]);
				exit(0);
			}
			break;
#endif
		case 'l':
			list_devices();
			exit(0);
			break;
#if RESAMPLE
		case 'u':
			if (optind < argc && argv[optind] && argv[optind][0] != '-') {
				upsample = argv[optind++];
			} else {
				upsample = "";
			}
			break;
#endif
#if LINUX || SUN
		case 'z':
			daemonize = true;
#if SUN
			init_daemonize();
#endif /* SUN */
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

	// set the output buffer size if not specified on the command line to take account of upsampling
	if (!output_buf_size) {
		output_buf_size = OUTPUTBUF_SIZE;
		if (upsample) {
			output_buf_size *= max_rate ? max_rate / 44100 : 8;
		}
	}

	if (logfile) {
		if (!freopen(logfile, "a", stdout) || !freopen(logfile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", logfile, strerror(errno));
		}
	}

#if LINUX || SUN
	if (daemonize) {
		if (daemon(0, logfile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif

#if WIN
	winsock_init();
#endif

	stream_init(log_stream, stream_buf_size);

#if ALSA
	output_init(log_output, output_device, output_buf_size, alsa_buffer, alsa_period, alsa_sample_fmt, alsa_mmap, 
				max_rate, rt_priority);
#endif
#if PORTAUDIO
#ifndef PA18API
	output_init(log_output, output_device, output_buf_size, pa_latency, max_rate);
#else
	output_init(log_output, output_device, output_buf_size, pa_frames, pa_nbufs, max_rate);
#endif /* PA18API */
#endif

	decode_init(log_decode, codecs);

#if RESAMPLE
	if (upsample) {
		process_init(upsample);
	}
#endif

	slimproto(log_slimproto, server, mac, name);
	
	decode_close();
	stream_close();
	output_close();

#if WIN
	winsock_close();
#endif

	exit(0);
}

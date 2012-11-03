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

#include <signal.h>

//static log_level loglevel = INFO;

#define TITLE "Squeezelite " VERSION ", Copyright 2012 Adrian Smith."

static void usage(const char *argv0) {
	printf(TITLE " See -t for license terms\n"
		   "Usage: %s [options] [<server_ip>]\n"
		   "  <server_ip>\t\tConnect to server server at given IP address, otherwise uses autodiscovery\n"
		   "  -o <output device>\tSpecify ALSA output device\n"
		   "  -l \t\t\tList ALSA output devices\n"
		   "  -a <time>:<count>\tSpecify ALSA buffer_time and period_count\n"
		   "  -b <stream>:<output>\tSpecify internal Stream and Output buffer sizes in Kbytes\n"
		   "  -c <codec1>,<codec2>\tRestrict codecs those specified, otherwise loads all available codecs; known codecs: flac,pcm,mp3\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output, level: info|debug|sdebug\n"
		   "  -m <mac addr>\t\tSet mac address, format: ab:cd:ef:12:34:56\n"
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

int main(int argc, char **argv) {
	char *server = NULL;
	char *output_device = "default";
	char *codecs = NULL;
	u8_t mac[6];
	unsigned stream_buf_size = STREAMBUF_SIZE;
	unsigned output_buf_size =  OUTPUTBUF_SIZE;
	unsigned alsa_buffer_time = ALSA_BUFFER_TIME;
	unsigned alsa_period_count = ALSA_PERIOD_COUNT;

	log_level log_output = WARN;
	log_level log_stream = WARN;
	log_level log_decode = WARN;
	log_level log_slimproto = WARN;

#if defined(SIGHUP)
	signal(SIGHUP, SIG_IGN);
#endif

	get_mac(mac);

	int opt;

    while ((opt = getopt(argc, argv, "o:a:b:c:d:m:lt")) != -1) {
        switch (opt) {
        case 'o':
            output_device = optarg;
            break;
		case 'a': 
			{
				char *t = strtok(optarg, ":");
				char *c = strtok(NULL, ":");
				if (t) alsa_buffer_time  = atoi(t);
				if (c) alsa_period_count = atoi(c);
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
				log_level new = WARN;
				if (l && v) {
					if (!strcmp(v, "info"))   new = INFO;
					if (!strcmp(v, "debug"))  new = DEBUG;
					if (!strcmp(v, "sdebug")) new = SDEBUG;
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
		case 'm':
			{
				int byte = 0;
				char *tmp;
				char *t = strtok(optarg, ":");
				while (t && byte < 6) {
					mac[byte++] = strtoul(t, &tmp, 16);
					t = strtok(NULL, ":");
				}
			}
			break;
		case 'l':
			alsa_list_pcm();
			exit(0);
			break;
		case 't':
			license();
			exit(0);
        default:
            usage(argv[0]);
            exit(0);
        }
    }

	// remaining argument should be a server address
	if (argc == optind + 1) {
		server = argv[optind];
	}

	output_init(log_output, output_device, output_buf_size, alsa_buffer_time, alsa_period_count);
	stream_init(log_stream, stream_buf_size);
	decode_init(log_decode, codecs);

	slimproto(log_slimproto, server, mac);
	
	decode_close();
	stream_close();
	output_close();

	exit(0);
}

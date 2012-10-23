/* 
 *  Squeezelite - lightweight headless squeezeplay emulator for linux
 *
 *  (c) Adrian Smith 2012, triode1@btinternet.com
 *  
 *  Unreleased - license details to be added here...
 */

#include "squeezelite.h"

#include <stdlib.h>

//static log_level loglevel = INFO;

static void usage(const char *argv0) {
	fprintf(stderr, 
			"Copyright statement...\n"
			"Usage: %s [options] [<server_ip>]\n"
			"  <server_ip>\t\tConnect to server server at given IP address, otherwise uses autodiscovery\n"
			"  -o <output device>\tSpecify ALSA output device\n"
			"  -a <time>:<count>\tSpecify ALSA buffer_time and period_count\n"
			"  -b <stream>:<output>\tSpecify internal Stream and Output buffer sizes in Kbytes\n"
			"  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output, level: info|debug|sdebug\n",
			argv0);
}

int main(int argc, char **argv) {
	char *server = NULL;
	char *output_device = "default";
	unsigned stream_buf_size = STREAMBUF_SIZE;
	unsigned output_buf_size =  OUTPUTBUF_SIZE;
	unsigned alsa_buffer_time = ALSA_BUFFER_TIME;
	unsigned alsa_period_count = ALSA_PERIOD_COUNT;

	log_level log_output = WARN;
	log_level log_stream = WARN;
	log_level log_decode = WARN;
	log_level log_slimproto = WARN;

	int opt;

    while ((opt = getopt(argc, argv, "o:a:b:d:")) != -1) {
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
        case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = WARN;
				if (!strcmp(v, "info"))   new = INFO;
				if (!strcmp(v, "debug"))  new = DEBUG;
				if (!strcmp(v, "sdebug")) new = SDEBUG;
				if (!strcmp(l, "all") || !strcmp(l, "slimproto")) log_slimproto = new;
				if (!strcmp(l, "all") || !strcmp(l, "stream"))    log_stream = new;
				if (!strcmp(l, "all") || !strcmp(l, "decode"))    log_decode = new;
				if (!strcmp(l, "all") || !strcmp(l, "output"))    log_output = new;
			}
            break;
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
	decode_init(log_decode);

	slimproto(log_slimproto, server);
	
	decode_close();
	stream_close();
	output_close();

	exit(0);
}

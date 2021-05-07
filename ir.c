/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2021, ralph_irving@hotmail.com
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

// ir thread - linux only

#if IR

#include <lirc/lirc_client.h>

/* Avoid name conflicts from syslog.h inclusion in lirc client header */
#ifdef LOG_DEBUG
#undef LOG_DEBUG
#endif
#ifdef LOG_INFO
#undef LOG_INFO
#endif

#include "squeezelite.h"

#define LIRC_CLIENT_ID "squeezelite"

static log_level loglevel;

struct irstate ir;

static struct lirc_config *config = NULL;
static sockfd fd = -1;

static thread_type thread;

#define LOCK_I   mutex_lock(ir.mutex)
#define UNLOCK_I mutex_unlock(ir.mutex)

#if !LINKALL
struct lirc {
	// LIRC symbols to be dynamically loaded
	int (* lirc_init)(char *prog, int verbose);
	int (* lirc_deinit)(void);
	int (* lirc_readconfig)(char *file, struct lirc_config **config, int (check) (char *s));
	void (* lirc_freeconfig)(struct lirc_config *config);
	int (* lirc_nextcode)(char **code);
	int (* lirc_code2char)(struct lirc_config *config, char *code, char **string);
};

static struct lirc *i;
#endif

#if LINKALL
#define LIRC(h, fn, ...) (lirc_ ## fn)(__VA_ARGS__)
#else
#define LIRC(h, fn, ...) (h)->lirc_##fn(__VA_ARGS__)
#endif

// cmds based on entries in Slim_Device_Remote.ir
// these may appear as config entries in .lircrc files
static struct {
	char  *cmd;
	u32_t code;
} cmdmap[] = {
	{ "voldown",  0x768900ff },
	{ "volup",    0x7689807f },
	{ "rew",      0x7689c03f },
	{ "fwd",      0x7689a05f },
	{ "pause",    0x768920df },
	{ "play",     0x768910ef },
	{ "power",    0x768940bf },
	{ "muting",   0x7689c43b },
	{ "power_on", 0x76898f70 },
	{ "power_off",0x76898778 },
	{ "preset_1", 0x76898a75 },
	{ "preset_2", 0x76894ab5 },
	{ "preset_3", 0x7689ca35 },
	{ "preset_4", 0x76892ad5 },
	{ "preset_5", 0x7689aa55 },
	{ "preset_6", 0x76896a95 },
	{ NULL,       0          },
};

// selected lirc namespace button names as defaults - some support repeat
static struct {
	char  *lirc;
	u32_t code;
	bool  repeat;
} keymap[] = {
	{ "KEY_VOLUMEDOWN", 0x768900ff, true  },
	{ "KEY_VOLUMEUP",   0x7689807f, true  },
	{ "KEY_PREVIOUS",   0x7689c03f, true  },
	{ "KEY_REWIND",     0x7689c03f, true  },
	{ "KEY_NEXT",       0x7689a05f, true  },
	{ "KEY_FORWARD",    0x7689a05f, true  },
	{ "KEY_PAUSE",      0x768920df, true  },
	{ "KEY_PLAY",       0x768910ef, false },
	{ "KEY_POWER",      0x768940bf, false },
	{ "KEY_MUTE",       0x7689c43b, false },
	{ "KEY_0",          0x76899867, true  },
	{ "KEY_1",          0x7689f00f, true  },
	{ "KEY_2",          0x768908f7, true  },
	{ "KEY_3",          0x76898877, true  },
	{ "KEY_4",          0x768948b7, true  },
	{ "KEY_5",          0x7689c837, true  },
	{ "KEY_6",          0x768928d7, true  },
	{ "KEY_7",          0x7689a857, true  },
	{ "KEY_8",          0x76896897, true  },
	{ "KEY_9",          0x7689e817, true  },
	{ "KEY_FAVORITES",  0x768918e7, false },
	{ "KEY_FAVORITES",  0x7689e21d, false },
	{ "KEY_SEARCH",     0x768958a7, false },
	{ "KEY_SEARCH",     0x7689629d, false },
	{ "KEY_SHUFFLE",    0x7689d827, false },
	{ "KEY_SLEEP",      0x7689b847, false },
	{ "KEY_INSERT",     0x7689609f, false }, // Add
	{ "KEY_UP",         0x7689e01f, true  },
	{ "KEY_LEFT",       0x7689906f, true  },
	{ "KEY_RIGHT",      0x7689d02f, true  },
	{ "KEY_DOWN",       0x7689b04f, true  },
	{ "KEY_HOME",       0x768922dd, false },
	{ "KEY_MEDIA_REPEAT", 0x768938c7, false },
	{ "KEY_AGAIN",      0x768938c7, false },
//	{ "KEY_TITLE",      0x76897887, false }, // Now Playing
//	{ "KEY_TITLE",      0x7689a25d, false }, // Now Playing
//	{ "KEY_TEXT",       0x7689f807, false }, // Size 
//	{ "KEY_BRIGHTNESS_CYCLE", 0x768904fb, false },
	{ NULL,             0         , false },
};

static u32_t ir_cmd_map(const char *c) {
	int i;
	for (i = 0; cmdmap[i].cmd; i++) {
		if (!strcmp(c, cmdmap[i].cmd)) {
			return cmdmap[i].code;
		}
	}
	return 0;
}

static u32_t ir_key_map(const char *c, const char *r) {
	int i;
	for (i = 0; keymap[i].lirc; i++) {
		if (!strcmp(c, keymap[i].lirc)) {
			// inputlirc issues "0", while LIRC uses "00"
			if (keymap[i].repeat || !strcmp(r, "0") || !strcmp(r,"00")) {
				return keymap[i].code;
			}
			LOG_DEBUG("repeat suppressed");
			break;
		}
	}
	return 0;
}

static void *ir_thread() {
	char *code;
	
	while (fd > 0 && LIRC(i, nextcode, &code) == 0) {
		
		u32_t now = gettime_ms();
		u32_t ir_code = 0;
		
		if (code == NULL) continue;
		
		if (config) {
			// allow lirc_client to decode then lookup cmd in our table
			// we can only send one IR event to slimproto so break after first one
			char *c;
			while (LIRC(i, code2char, config, code, &c) == 0 && c != NULL) {
				ir_code = ir_cmd_map(c);
				if (ir_code) {
					LOG_DEBUG("ir cmd: %s -> %x", c, ir_code);
				}
			}
		}

		if (!ir_code) {
			// try to match on lirc button name if it is from the standard namespace
			// this allows use of non slim remotes without a specific entry in .lircrc
			char *b, *r;
			strtok(code, " \n");     // discard
			r = strtok(NULL, " \n"); // repeat count
			b = strtok(NULL, " \n"); // key name
			if (r && b) {
				ir_code = ir_key_map(b, r);
				LOG_DEBUG("ir lirc: %s [%s] -> %x", b, r, ir_code);
			}
		}

		if (ir_code) {
			LOCK_I;
			if (ir.code) {
				LOG_DEBUG("code dropped");
			}
			ir.code = ir_code;
			ir.ts = now;
			UNLOCK_I;
			wake_controller();
		}
		
		free(code);
	}
	
	return 0;
}

#if !LINKALL
static bool load_lirc() {
	void *handle = dlopen(LIBLIRC, RTLD_NOW);
	char *err;

	if (!handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	i->lirc_init = dlsym(handle, "lirc_init");
	i->lirc_deinit = dlsym(handle, "lirc_deinit");
	i->lirc_readconfig = dlsym(handle, "lirc_readconfig");
	i->lirc_freeconfig = dlsym(handle, "lirc_freeconfig");
	i->lirc_nextcode = dlsym(handle, "lirc_nextcode");
	i->lirc_code2char = dlsym(handle, "lirc_code2char");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded "LIBLIRC);
	return true;
}
#endif

void ir_init(log_level level, char *lircrc) {
	loglevel = level;

#if !LINKALL
	i = malloc(sizeof(struct lirc));
	if (!i || !load_lirc()) {
		return;
	}
#endif

	fd = LIRC(i, init, LIRC_CLIENT_ID, 0);

	if (fd > 0) {
		if (LIRC(i, readconfig, lircrc, &config, NULL) != 0) {
			LOG_WARN("error reading config: %s", lircrc);
		}
		else {
			LOG_DEBUG("loaded lircrc config: %s", lircrc);
		}	

		mutex_create(ir.mutex);

		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + IR_THREAD_STACK_SIZE);
		pthread_create(&thread, &attr, ir_thread, NULL);
		pthread_attr_destroy(&attr);

	} else {
		LOG_WARN("failed to connect to lircd - ir processing disabled");
	}
}

void ir_close(void) {
	if (fd > 0) {
		fd = -1;
		if (config) {
			LIRC(i, freeconfig, config);
		}
		LIRC(i, deinit);

		pthread_cancel(thread);
		pthread_join(thread, NULL);
		mutex_destroy(ir.mutex);
	}
}

#endif //#if IR

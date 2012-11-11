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

// automatically select between floating point (preferred) and fixed point libraries:
#define LIBVORBIS "libvorbisfile.so.3"
#define LIBTREMOR "libvorbisidec.so.1"

// NOTE: works with Tremor version here: http://svn.xiph.org/trunk/Tremor, not vorbisidec.1.0.2 currently in ubuntu

// we take common definations from <vorbis/vorbisfile.h> even though we can use tremor at run time
// tremor's OggVorbis_File struct is normally smaller so this is ok, but padding added to malloc in case it is bigger
#define OV_EXCLUDE_STATIC_CALLBACKS

#include <vorbis/vorbisfile.h>
#include <dlfcn.h>

struct vorbis {
	OggVorbis_File *vf;
	// vorbis symbols to be dynamically loaded - from either vorbisfile or vorbisidec (tremor) version of library
	vorbis_info *(* ov_info)(OggVorbis_File *vf, int link);
	int (* ov_clear)(OggVorbis_File *vf);
	long (* ov_read)(OggVorbis_File *vf, char *buffer, int length, int bigendianp, int word, int sgned, int *bitstream);
	long (* ov_read_tremor)(OggVorbis_File *vf, char *buffer, int length, int *bitstream);
	int (* ov_open_callbacks)(void *datasource, OggVorbis_File *vf, const char *initial, long ibytes, ov_callbacks callbacks);
};

static struct vorbis *v;

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;

#define LOCK_S   pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK_S pthread_mutex_unlock(&streambuf->mutex)
#define LOCK_O   pthread_mutex_lock(&outputbuf->mutex)
#define UNLOCK_O pthread_mutex_unlock(&outputbuf->mutex)

typedef u_int32_t frames_t;

static size_t _read(void *ptr, size_t size, size_t nmemb, void *datasource) {
	LOCK_S;
	size_t bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	bytes = min(bytes, size * nmemb);

	memcpy(ptr, streambuf->readp, bytes);
	_buf_inc_readp(streambuf, bytes);
	UNLOCK_S;

	return bytes / size;
}

// these are needed for older versions of tremor, later versions and libvorbis allow NULL to be used
static int _seek(void *datasource, ogg_int64_t offset, int whence) { return -1; }
static int _close(void *datasource) { return 0; }
static long _tell(void *datasource) { return 0; }

static void vorbis_decode(void) {
	static int channels;

	LOCK_O;

	if (decode.new_stream) {
		ov_callbacks cbs = { .read_func = _read, .seek_func = NULL, .close_func = NULL, .tell_func = NULL };
		if (v->ov_read_tremor) {
			cbs.seek_func = _seek; cbs.close_func = _close; cbs.tell_func = _tell;
		}

		int err;
		if ((err = v->ov_open_callbacks(streambuf, v->vf, NULL, 0, cbs)) < 0) {
			LOG_WARN("open_callbacks error: %d", err);
			decode.state = DECODE_COMPLETE;
			UNLOCK_O;
			return;
		}

		struct vorbis_info *info = v->ov_info(v->vf, -1);

		LOG_INFO("setting track_start");
		output.next_sample_rate = info->rate; 
		output.track_start = outputbuf->writep;
		decode.new_stream = false;

		channels = info->channels;

		if (channels > 2) {
			LOG_WARN("too many channels: %d", channels);
			decode.state = DECODE_COMPLETE;
			UNLOCK_O;
			return;
		}
	}

	frames_t frames = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
	int bytes = frames * 2 * channels; // samples returned are 16 bits

	int stream, n;
	// write the decoded frames into outputbuf even though they are 16 bits per sample, then unpack them
	if (v->ov_read) {
		n = v->ov_read(v->vf, (char *)outputbuf->writep, bytes, 0, 2, 1, &stream);
	} else {
		n = v->ov_read_tremor(v->vf, (char *)outputbuf->writep, bytes, &stream);
	}

	if (n > 0) {

		frames = n / 2 / channels;
		frames_t count = frames * channels;

		// work backward to unpack samples to 4 bytes per sample
		s16_t *iptr = (s16_t *)outputbuf->writep + count;
		s32_t *optr = (s32_t *)outputbuf->writep + frames * 2;

		if (channels == 2) {
			while (count--) {
				*--optr = *--iptr << 16;
			}
		} else if (channels == 1) {
			while (count--) {
				*--optr = *--iptr << 16;
				*--optr = *iptr   << 16;
			}
		}

		_buf_inc_writep(outputbuf, frames * BYTES_PER_FRAME);

		LOG_SDEBUG("wrote %u frames", frames);

	} else if (n == 0) {

		LOG_INFO("end of stream");
		decode.state = DECODE_COMPLETE;
	
	} else {

		LOG_INFO("ov_read error: %d", n);
		decode.state = DECODE_COMPLETE;
	}

	UNLOCK_O;
}

static void vorbis_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	if (!v->vf) {
		v->vf = malloc(sizeof(OggVorbis_File) + 128); // add some padding as struct size may be larger
	} else {
		v->ov_clear(v->vf);
	}
}

static void vorbis_close(void) {
	v->ov_clear(v->vf);
	free(v->vf);
	v->vf = NULL;
}

static bool load_vorbis() {
	bool tremor = false;
	void *handle = dlopen(LIBVORBIS, RTLD_NOW);
	if (!handle) {
		handle = dlopen(LIBTREMOR, RTLD_NOW);
		if (handle) {
			tremor = true;
		} else {
			LOG_WARN("dlerror: %s", dlerror());
			return false;
		}
	}

	v = malloc(sizeof(struct vorbis));
	v->vf = NULL;
	v->ov_read = tremor ? NULL : dlsym(handle, "ov_read");
	v->ov_read_tremor = tremor ? dlsym(handle, "ov_read") : NULL;
	v->ov_info = dlsym(handle, "ov_info");
	v->ov_clear = dlsym(handle, "ov_clear");
	v->ov_open_callbacks = dlsym(handle, "ov_open_callbacks");
	
	char *err;
	if ((err = dlerror()) != NULL) {
		LOG_WARN("dlerror: %s", err);		
		return false;
	}
	
	LOG_INFO("loaded %s", tremor ? LIBTREMOR : LIBVORBIS);
	return true;
}

struct codec *register_vorbis(void) {
	static struct codec ret = { 
		.id    = 'o',
		.types = "ogg",
		.open  = vorbis_open,
		.close = vorbis_close,
		.decode= vorbis_decode,
		.min_space = 20480,
		.min_read_bytes = 2048,
	};

	if (!load_vorbis()) {
		return NULL;
	}

	return &ret;
}

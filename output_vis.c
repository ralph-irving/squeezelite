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

// Export audio samples for visualizer process (16 bit only best endevours)

#include "squeezelite.h"

#if VISEXPORT

#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#if OSX
#include <mach/clock.h>
#include <mach/mach.h>

static int pthread_rwlock_timedwrlock( pthread_rwlock_t * restrict rwlock, const struct timespec * restrict abs_timeout )
{
    ( void )rwlock;
    ( void )abs_timeout;
    
    return 0;
}
#endif

#define VIS_BUF_SIZE 16384
#define VIS_LOCK_NS  1000000 // ns to wait for vis wrlock

static struct vis_t {
	pthread_rwlock_t rwlock;
	u32_t buf_size;
	u32_t buf_index;
	bool running;
	u32_t rate;
	time_t updated;
	s16_t buffer[VIS_BUF_SIZE];
} *vis_mmap = NULL;

static char vis_shm_path[40];
static int vis_fd = -1;

static log_level loglevel;

// attempt to write audio to vis_mmap but do not wait more than VIS_LOCK_NS to get wrlock
// this can result in missing audio export to the mmap region, but this is preferable dropping audio
void _vis_export(struct buffer *outputbuf, struct outputstate *output, frames_t out_frames, bool silence) {
	if (vis_mmap) {
		int err;
		
		err = pthread_rwlock_trywrlock(&vis_mmap->rwlock);
		if (err) {
			struct timespec ts;
#if OSX
			clock_serv_t cclock;
			mach_timespec_t mts;
			host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
			clock_get_time(cclock, &mts);
			mach_port_deallocate(mach_task_self(), cclock);
			ts.tv_sec = mts.tv_sec;
			ts.tv_nsec = mts.tv_nsec;
#else
			clock_gettime(CLOCK_REALTIME, &ts);
#endif
			ts.tv_nsec += VIS_LOCK_NS;
			if (ts.tv_nsec > 1000000000) {
				ts.tv_sec  += 1;
				ts.tv_nsec -= 1000000000;
			}
			err = pthread_rwlock_timedwrlock(&vis_mmap->rwlock, &ts);
		}
		
		if (err) {
			LOG_DEBUG("failed to get wrlock - skipping visulizer export");
			
		} else {
			
			if (silence) {
				vis_mmap->running = false;
			} else {
				frames_t vis_cnt = out_frames;
				s32_t *ptr = (s32_t *) outputbuf->readp;
				unsigned i = vis_mmap->buf_index;
				
				if (!output->current_replay_gain) {
					while (vis_cnt--) {
						vis_mmap->buffer[i++] = *(ptr++) >> 16;
						vis_mmap->buffer[i++] = *(ptr++) >> 16;
						if (i == VIS_BUF_SIZE) i = 0;
					}
				} else {
					while (vis_cnt--) {
						vis_mmap->buffer[i++] = gain(*(ptr++), output->current_replay_gain) >> 16;
						vis_mmap->buffer[i++] = gain(*(ptr++), output->current_replay_gain) >> 16;
						if (i == VIS_BUF_SIZE) i = 0;
					}
				}
				
				vis_mmap->updated = time(NULL);
				vis_mmap->running = true;
				vis_mmap->buf_index = i;
				vis_mmap->rate = output->current_sample_rate;
			}
			
			pthread_rwlock_unlock(&vis_mmap->rwlock);
		}
	}
}

void vis_stop(void) {
	if (vis_mmap) {
		pthread_rwlock_wrlock(&vis_mmap->rwlock);
		vis_mmap->running = false;
		pthread_rwlock_unlock(&vis_mmap->rwlock);
	}
}

void output_vis_init(log_level level, u8_t *mac) {
	loglevel = level;

	sprintf(vis_shm_path, "/squeezelite-%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	mode_t old_mask = umask(000); // allow any user to read our shm when created

#if OSX
	/* ftruncate on MacOS only works on the initial segment creation */
	shm_unlink(vis_shm_path);
#endif	
	vis_fd = shm_open(vis_shm_path, O_CREAT | O_RDWR, 0666);
	if (vis_fd != -1) {
		if (ftruncate(vis_fd, sizeof(struct vis_t)) == 0) {
			vis_mmap = (struct vis_t *)mmap(NULL, sizeof(struct vis_t), PROT_READ | PROT_WRITE, MAP_SHARED, vis_fd, 0);
		}
	}
	
	if (vis_mmap > 0) {
		pthread_rwlockattr_t attr;
		pthread_rwlockattr_init(&attr);
		pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
		pthread_rwlock_init(&vis_mmap->rwlock, &attr);
		vis_mmap->buf_size = VIS_BUF_SIZE;
		vis_mmap->running = false;
		vis_mmap->rate = 44100;
		pthread_rwlockattr_destroy(&attr);
		LOG_INFO("opened visulizer shared memory as %s", vis_shm_path);
	} else {
		LOG_WARN("unable to open visualizer shared memory");
		vis_mmap = NULL;
	}

	umask(old_mask);
}

#endif // VISEXPORT

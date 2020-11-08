/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2017, ralph_irving@hotmail.com
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
 * libhdcd integration Thierry Leconte 2020
 */

// HDCD decoding using libhdcd - only included if HDCD set

#include "squeezelite.h"

#if HDCD

#include "hdcd/hdcd_decode.h"

extern log_level loglevel;

static hdcd_state_stereo hdcd_ctx;

void hdcd_samples(struct processstate *process) {
	_hdcd_process_stereo(&hdcd_ctx,(ISAMPLE_T *)(process->inbuf), process->in_frames);
}

bool hdcd_newstream(struct processstate *process, unsigned raw_sample_rate, unsigned supported_rates[]) {
	if(raw_sample_rate!=44100) return false;
	LOG_INFO("hdcd - reset");
	_hdcd_reset_stereo(&hdcd_ctx, raw_sample_rate, 16, 0, HDCD_FLAG_TGM_LOG_OFF);
	return true;
}

void hdcd_flush(void) {
	LOG_INFO("hdcd - flush %d",_hdcd_detected(&hdcd_ctx));
}
#endif // #if HDCD

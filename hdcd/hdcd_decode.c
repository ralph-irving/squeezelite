/*
 *  Copyright (C) 2010, Chris Moeller,
 *  All rights reserved.
 *  Optimizations by Gumboot
 *  Additional work by Burt P.
 *  Original code reverse engineered from HDCD decoder library by Christopher Key,
 *  which was likely reverse engineered from Windows Media Player.
 *
 *  Redistribution and use in source and binary forms, with or without modification,
 *  are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. The names of its contributors may not be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "hdcd_decode.h"
#include "hdcd_tables.h"

// code was developed as part of FFmpeg and uses these macros
#if !defined(FFMIN)
#define FFMIN(x, y) ((x) <= (y) ? (x) : (y))
#endif
#if !defined(FFMAX)
#define FFMAX(x, y) ((x) >= (y) ? (x) : (y))
#endif

/** convert to float from 4-bit (3.1) fixed-point
 *  the always-negative value is stored positive, so make it negative */
#define GAINTOFLOAT(g) (g) ? -(float)(g>>1) - ((g & 1) ? 0.5 : 0.0) : 0.0

/** apply gain, 11-bit (3.8) fixed point,
 *  always negative but stored positive. */
#define APPLY_GAIN(s,g) do{int64_t s64 = (s); s64 *= gaintab[g]; s = (int32_t)(s64 >> 23); }while(0);

/** used in _hdcd_scan_x() and _hdcd_integrate_x() */
#define HDCD_MAX_CHANNELS 2

/** internal data structure identities **/
enum {
    HDCD_SID_UNDEF           = 0,
    HDCD_SID_STATE           = 1,
    HDCD_SID_STATE_STEREO    = 2,
    HDCD_SID_DETECTION_DATA  = 3,
    HDCD_SID_LOGGER          = 4,
};

static void _hdcd_reset(hdcd_state *state, unsigned rate, unsigned bits, int sustain_period_ms, int flags)
{
    int i;
    uint32_t sustain_reset;

    /* check parameters */
    if (!state) return;
    if (!rate) rate = 44100;
    if (!bits) bits = 16;
    if (!sustain_period_ms)
        sustain_period_ms = 2000;
    else {
        sustain_period_ms = FFMIN(sustain_period_ms, 60000);
        sustain_period_ms = FFMAX(sustain_period_ms, 100);
    }
    /* Formerly: sustain_period_ms * rate / 1000, but
     * max period (60000) * max sample rate (192000) overflows uint32.
     * Attempted to do it as uint64, but causes a strange problem with
     * the i686-w64-mingw32 build.
     * Valid HDCD rate is always a multiple of 100,
     * worst case is now  60000 * 1920; fits in uint32. */
    sustain_reset = sustain_period_ms * (rate/100) / 10;

    /* initialize memory area */
    memset(state, 0, sizeof(*state));
    state->sid = HDCD_SID_STATE;

    /* set options */
    state->decoder_options = flags;
    state->cdt_period = sustain_period_ms;
    state->rate = rate;
    state->bits = bits;

    /* decoding state */
    state->window = 0;
    state->readahead = 32;
    state->arg = 0;
    state->control = 0;
    state->running_gain = 0;
    state->sustain_reset = sustain_reset;
    state->sustain = 0;

    /* reset all counters */
    state->code_counterA = 0;
    state->code_counterA_almost = 0;
    state->code_counterB = 0;
    state->code_counterB_checkfails = 0;
    state->code_counterC = 0;
    state->code_counterC_unmatched = 0;
    state->count_peak_extend = 0;
    state->count_transient_filter = 0;
    for(i = 0; i < 16; i++) state->gain_counts[i] = 0;
    state->max_gain = 0;
    state->count_sustain_expired = -1;

}

void _hdcd_reset_stereo(hdcd_state_stereo *state, unsigned rate, unsigned bits, int sustain_period_ms, int flags)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->sid = HDCD_SID_STATE_STEREO;
    _hdcd_reset(&state->channel[0], rate, bits, sustain_period_ms, flags);
    _hdcd_reset(&state->channel[1], rate, bits, sustain_period_ms, flags);
    state->val_target_gain = 0;
    state->count_tg_mismatch = 0;
}

static int _hdcd_integrate_x(hdcd_state *states, int channels, int *flag, const int32_t *samples, int count, int stride)
{
    uint32_t bits[HDCD_MAX_CHANNELS];
    int result = count;
    int i, j, f;
    *flag = 0;

    memset(bits, 0, sizeof(bits));
    if (stride < channels) stride = channels;

    for (i = 0; i < channels; i++)
        result = FFMIN(states[i].readahead, result);

    for (j = result - 1; j >= 0; j--) {
        for (i = 0; i < channels; i++) {
            bits[i] |= ((*samples>>16) & 1) << j;
	    samples++;
	}
        samples += stride - channels;
    }

    for (i = 0; i < channels; i++) {
        states[i].window = (states[i].window << result) | bits[i];
        states[i].readahead -= result;

        if (states[i].readahead == 0) {
            uint32_t wbits = (uint32_t)(states[i].window ^ states[i].window >> 5 ^ states[i].window >> 23);
            if (states[i].arg) {
                f = 0;
                if ((wbits & 0x0fa00500) == 0x0fa00500) {
                    /* A: 8-bit code  0x7e0fa005[..] */
                    if ((wbits & 0xc8) == 0) {
                        /*                   [..pt gggg]
                         * 0x0fa005[..] -> 0b[00.. 0...], gain part doubled (shifted left 1) */
                        states[i].control = (wbits & 255) + (wbits & 7);
                        f = 1;
                        states[i].code_counterA++;
                    } else {
                        /* one of bits 3, 6, or 7 was not 0 */
                        states[i].code_counterA_almost++;
                    }
                } else if ((wbits & 0xa0060000) == 0xa0060000) {
                    /* B: 8-bit code, 8-bit XOR check, 0x7e0fa006[....] */
                    if (((wbits ^ (~wbits >> 8 & 255)) & 0xffff00ff) == 0xa0060000) {
                        /*          check:   [..pt gggg ~(..pt gggg)]
                         * 0xa006[....] -> 0b[.... ....   .... .... ] */
                        states[i].control = wbits >> 8 & 255;
                        f = 1;
                        states[i].code_counterB++;
                    } else {
                        /* XOR check failed */
                        states[i].code_counterB_checkfails++;
                    }
                }
                if (f) {
                    *flag |= (1<<i);
                    /* update counters */
                    if (states[i].control & 16) states[i].count_peak_extend++;
                    if (states[i].control & 32) states[i].count_transient_filter++;
                    states[i].gain_counts[states[i].control & 15]++;
                    states[i].max_gain = FFMAX(states[i].max_gain, (states[i].control & 15));
                }
                states[i].arg = 0;
            }
            if (wbits == 0x7e0fa005 || wbits == 0x7e0fa006) {
                /* 0x7e0fa00[.]-> [0b0101 or 0b0110] */
                states[i].readahead = (wbits & 3) * 8;
                states[i].arg = 1;
                states[i].code_counterC++;
            } else {
                if (wbits)
                    states[i].readahead = readaheadtab[wbits & 0xff];
                else
                    states[i].readahead = 31; /* ffwd over digisilence */
            }
        }
    }
    return result;
}

static int _hdcd_scan_x(hdcd_state *states, int channels, const int32_t *samples, int max, int stride)
{
    int result;
    int i;
    int cdt_active[HDCD_MAX_CHANNELS];
    memset(cdt_active, 0, sizeof(cdt_active));

    if (stride < channels) stride = channels;

    /* code detect timers for each channel */
    for(i = 0; i < channels; i++) {
        if (states[i].sustain > 0) {
            cdt_active[i] = 1;
            if (states[i].sustain <=  (unsigned)max) {
                states[i].control = 0;
                max = states[i].sustain;
            }
            states[i].sustain -= max;
        }
    }

    result = 0;
    while (result < max) {
        int flag;
        int consumed = _hdcd_integrate_x(states, channels, &flag, samples, max - result, stride);
        result += consumed;
        if (flag) {
            /* reset timer if code detected in a channel */
            for(i = 0; i < channels; i++) {
                if (flag & (1<<i)) {
                    states[i].sustain = states[i].sustain_reset;
                    /* if this is the first reset then change
                     * from never set, to never expired */
                    if (states[i].count_sustain_expired == -1)
                        states[i].count_sustain_expired = 0;
                }
            }
            break;
        }
        samples += consumed * stride;
    }

    for(i = 0; i < channels; i++) {
        /* code detect timer expired */
        if (cdt_active[i] && states[i].sustain == 0)
            states[i].count_sustain_expired++;
    }

    return result;
}

/** apply HDCD decoding parameters to a series of samples */
static int _hdcd_envelope(int32_t *samples, int count, int stride, int bits, int gain, int target_gain, int extend)
{
    int i, limit = count * stride;

    int pe_level = peak_ext_level, shft = 15;
    if (bits != 16) {
        pe_level = (1 << (bits - 1)) - (0x8000 - peak_ext_level);
        shft = 32 - bits - 1;
    }

    if (extend) {
        for (i = 0; i < limit; i += stride) {
            int32_t sample = samples[i]>>16;
            int32_t asample = abs(sample) - pe_level;
            if (asample >= 0) {
                if (asample > pe_max_asample ) asample = pe_max_asample;
                sample = sample >= 0 ? peaktab[asample] : -peaktab[asample];
            } else
                sample <<= shft;

            samples[i] = sample;
        }
    } else {
        for (i = 0; i < limit; i += stride)
            samples[i] >>= 16-shft;
    }

    if (gain <= target_gain) {
        int len = FFMIN(count, target_gain - gain);
        /* attenuate slowly */
        for (i = 0; i < len; i++) {
            ++gain;
            APPLY_GAIN(*samples, gain);
            samples += stride;
        }
        count -= len;
    } else {
        int len = FFMIN(count, (gain - target_gain) >> 3);
        /* amplify quickly */
        for (i = 0; i < len; i++) {
            gain -= 8;
            APPLY_GAIN(*samples, gain);
            samples += stride;
        }
        if (gain - 8 < target_gain)
            gain = target_gain;
        count -= len;
    }

    /* hold a steady level */
    if (gain == 0) {
        if (count > 0)
            samples += count * stride;
    } else {
        while (--count >= 0) {
            APPLY_GAIN(*samples, gain);
            samples += stride;
        }
    }

    return gain;
}

/** extract fields from control code */
static void _hdcd_control(hdcd_state *state, int *peak_extend, int *target_gain)
{
    *peak_extend = (state->control & 16 || state->decoder_options & HDCD_FLAG_FORCE_PE);
    *target_gain = (state->control & 15) << 7;
}

typedef enum {
    HDCD_OK=0,
    HDCD_TG_MISMATCH
} hdcd_control_result;

static hdcd_control_result _hdcd_control_stereo(hdcd_state_stereo *state, int *peak_extend0, int *peak_extend1)
{
    int target_gain[2];
    _hdcd_control(&state->channel[0], peak_extend0, &target_gain[0]);
    _hdcd_control(&state->channel[1], peak_extend1, &target_gain[1]);
    if (target_gain[0] == target_gain[1])
        state->val_target_gain = target_gain[0];
    else {
        return HDCD_TG_MISMATCH;
    }
    return HDCD_OK;
}

void _hdcd_process_stereo(hdcd_state_stereo *state, int32_t *samples, int count)
{
    const int stride = 2;
    int gain[2] = {state->channel[0].running_gain, state->channel[1].running_gain};
    int peak_extend[2];
    int lead = 0;
    int ctlret;

    ctlret = _hdcd_control_stereo(state, &peak_extend[0], &peak_extend[1]);
    while (count > lead) {
        int envelope_run, run;

        run = _hdcd_scan_x(&state->channel[0], 2, samples + lead * stride, count - lead, 0) + lead;
        envelope_run = run - 1;

        if (ctlret == HDCD_TG_MISMATCH)
            state->count_tg_mismatch += envelope_run;

        gain[0] = _hdcd_envelope(samples, envelope_run, stride, state->channel[0].bits, gain[0], state->val_target_gain, peak_extend[0]);
        gain[1] = _hdcd_envelope(samples + 1, envelope_run, stride, state->channel[1].bits, gain[1], state->val_target_gain, peak_extend[1]);

        samples += envelope_run * stride;
        count -= envelope_run;
        lead = run - envelope_run;

        ctlret = _hdcd_control_stereo(state, &peak_extend[0], &peak_extend[1]);
    }
    if (lead > 0) {
        if (ctlret == HDCD_TG_MISMATCH)
            state->count_tg_mismatch += lead;

       gain[0] = _hdcd_envelope(samples, lead, stride, state->channel[0].bits, gain[0], state->val_target_gain, peak_extend[0]);
       gain[1] = _hdcd_envelope(samples + 1, lead, stride, state->channel[1].bits, gain[1], state->val_target_gain, peak_extend[1]);
    }

    state->channel[0].running_gain = gain[0];
    state->channel[1].running_gain = gain[1];

}

int _hdcd_detected(hdcd_state_stereo *state)
{
  if(
	state->channel[0].code_counterB || state->channel[0].code_counterA || state->channel[0].code_counterA_almost ||
	state->channel[1].code_counterB || state->channel[1].code_counterA || state->channel[1].code_counterA_almost 
  	)
	return 1 ; 

  return 0;
}

char *_hdcd_stats(hdcd_state_stereo *state)
{
  static char str[256];
  sprintf(str,"HDCD Code A:%d/B:%d PE:%d Transient Filter:%d Max gain:%g",
		state->channel[0].code_counterA ,state->channel[0].code_counterB,
		state->channel[0].count_peak_extend, state->channel[0].count_transient_filter, GAINTOFLOAT(state->channel[0].max_gain)
	);
  return str;
}


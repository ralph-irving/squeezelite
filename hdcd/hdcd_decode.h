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

/*
 * HDCD is High Definition Compatible Digital
 * http://wiki.hydrogenaud.io/index.php?title=High_Definition_Compatible_Digital
 *
 * More information about HDCD-encoded audio CDs:
 * http://www.audiomisc.co.uk/HFN/HDCD/Enigma.html
 * http://www.audiomisc.co.uk/HFN/HDCD/Examined.html
 */

#ifndef _HDCD_DECODE2_H_
#define _HDCD_DECODE2_H_

#include <stdint.h>
#include <stdarg.h>


#define HDCD_FLAG_FORCE_PE         128
#define HDCD_FLAG_TGM_LOG_OFF       64

typedef struct {
    uint32_t sid; /**< internal struct identity = HDCD_SID_STATE */

    int decoder_options;  /**< as flags HDCD_FLAG_* */

    uint64_t window;
    unsigned char readahead;

    /** arg is set when a packet prefix is found.
     *  control is the active control code, where
     *  bit 0-3: target_gain, 4-bit (3.1) fixed-point value
     *  bit 4  : peak_extend
     *  bit 5  : transient_filter
     *  bit 6,7: always zero */
    uint8_t arg, control;
    unsigned int sustain, sustain_reset; /**< code detect timer */

    int running_gain; /**< 11-bit (3.8) fixed point, extended from target_gain */

    int bits;             /**< sample bit depth: 16, 20, 24 */
    int rate;             /**< sample rate */
    int cdt_period;       /**< cdt period in ms */

    /** counters */
    int code_counterA;            /**< 8-bit format packet */
    int code_counterA_almost;     /**< looks like an A code, but a bit expected to be 0 is 1 */
    int code_counterB;            /**< 16-bit format packet, 8-bit code, 8-bit XOR of code */
    int code_counterB_checkfails; /**< looks like a B code, but doesn't pass the XOR check */
    int code_counterC;            /**< packet prefix was found, expect a code */
    int code_counterC_unmatched;  /**< told to look for a code, but didn't find one */
    int count_peak_extend;        /**< valid packets where peak_extend was enabled */
    int count_transient_filter;   /**< valid packets where filter was detected */
    /** target_gain is a 4-bit (3.1) fixed-point value, always
     *  negative, but stored positive.
     *  The 16 possible values range from -7.5 to 0.0 dB in
     *  steps of 0.5, but no value below -6.0 dB should appear. */
    int gain_counts[16];
    int max_gain;
    /** occurences of code detect timer expiring without detecting
     *  a code. -1 for timer never set. */
    int count_sustain_expired;
} hdcd_state;

typedef struct {
    uint32_t sid; /**< internal struct identity = HDCD_SID_STATE_STEREO */
    hdcd_state channel[2];      /**< individual channel states       */
    int val_target_gain;        /**< last valid matching target_gain */
    int count_tg_mismatch;      /**< target_gain mismatch samples  */
} hdcd_state_stereo;

/* stereo versions */
void _hdcd_reset_stereo(hdcd_state_stereo *state, unsigned rate, unsigned bits, int sustain_period_ms, int flags);
void _hdcd_process_stereo(hdcd_state_stereo *state, int *samples, int count);
int _hdcd_detected(hdcd_state_stereo *states);

#endif


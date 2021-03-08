/*
 *  alac_wrapper.c - ALAC decoder wrapper
 *
 *  (c) Philippe, philippe_44@outlook.com
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

#include <stdlib.h>

#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include "alac_wrapper.h"

typedef struct alac_codec_s {
	ALACDecoder *Decoder;
	unsigned block_size, frames_per_packet;
} alac_codec_t;

/*----------------------------------------------------------------------------*/
extern "C" struct alac_codec_s *alac_create_decoder(int magic_cookie_size, unsigned char *magic_cookie,
											unsigned char *sample_size, unsigned *sample_rate,
											unsigned char *channels, unsigned int *block_size) {
	struct alac_codec_s *codec = (struct alac_codec_s*) malloc(sizeof(struct alac_codec_s));

	codec->Decoder = new ALACDecoder;
	codec->Decoder->Init(magic_cookie, magic_cookie_size);

	*channels = codec->Decoder->mConfig.numChannels;
	*sample_rate = codec->Decoder->mConfig.sampleRate;
	*sample_size = codec->Decoder->mConfig.bitDepth;

	codec->frames_per_packet = codec->Decoder->mConfig.frameLength;
	*block_size = codec->block_size = codec->frames_per_packet * (*channels) * (*sample_size) / 8;

	return codec;
}

/*----------------------------------------------------------------------------*/
extern "C" void alac_delete_decoder(struct alac_codec_s *codec) {
	delete (ALACDecoder*) codec->Decoder;
	free(codec);
}

/*----------------------------------------------------------------------------*/
extern "C" bool alac_to_pcm(struct alac_codec_s *codec, unsigned char* input,
							unsigned char *output, char channels, unsigned *out_frames) {
	BitBuffer input_buffer;

	BitBufferInit(&input_buffer, input, codec->block_size);
	return codec->Decoder->Decode(&input_buffer, output, codec->frames_per_packet, channels, out_frames) == ALAC_noErr;
}


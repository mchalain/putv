/*****************************************************************************
 * filter_rescale.c
 * this file is part of https://github.com/ouistiti-project/putv
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "filter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define filter_dbg(...)

rescale_t *rescale_init(rescale_t *input, int outbits, jitter_format_t outformat)
{
	switch (outformat)
	{
	case 0:
	break;
	case PCM_8bits_mono:
		outbits = 8;
	break;
	case PCM_16bits_LE_mono:
	case PCM_16bits_LE_stereo:
		outbits = 16;
	break;
	case PCM_24bits3_LE_stereo:
	case PCM_24bits4_LE_stereo:
		outbits = 24;
	break;
	case PCM_32bits_LE_stereo:
	case PCM_32bits_BE_stereo:
		outbits = 32;
	break;
	default:
		return NULL;
	}

	if (input == NULL)
		input = calloc(1, sizeof(*input));
	input->outbits = outbits;
	input->one = ((sample_t)1 << outbits);
	return input;
}

/**
 * @brief this function comes from mad decoder
 *
 * @arg sample          the 32bits sample
 * @arg bitspersample   the length of scaling (16 or 24)
 *
 * @return sample
 */
sample_t rescale_cb(void *arg, sample_t sample, int bitspersample, int samplerate, int channel)
{
	rescale_t *ctx = (rescale_t *)arg;
	if (bitspersample <= ctx->outbits)
		return sample;
	filter_dbg("filter: rescale");

	sample_t one = ((sample_t)1 << bitspersample);
	//sample &= (one - 1);
	/* round */
	sample += (1L << (bitspersample - ctx->outbits));
	/* clip */
	if (sample >= one)
		sample = one - 1;
	else if (sample < -one)
		sample = -one;
	/* quantize */
	sample = sample >> (bitspersample + 1 - ctx->outbits);
	return sample;
}

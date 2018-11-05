/*****************************************************************************
 * jitter_ring.c
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

typedef struct filter_ctx_s filter_ctx_t;
struct filter_ctx_s
{
	unsigned int samplerate;
	unsigned int samplesize;
	unsigned int nchannels;
};
#define FILTER_CTX
#include "filter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

filter_ctx_t *filter_init(unsigned int samplerate, unsigned int samplesize, unsigned int nchannels)
{
	filter_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->samplerate = samplerate;
	ctx->samplesize = samplesize;
	ctx->nchannels = nchannels;
	return ctx;
}

void filter_destroy(filter_ctx_t *ctx)
{
	free(ctx);
}

static int sampled(filter_ctx_t *ctx, signed int sample, int bitspersample, unsigned char *out)
{
	int i = 3;
	for (i = 0; i < ctx->samplesize; i++)
	{
		int shift = ((ctx->samplesize - i - 1) * 8);
		//int shift = (i * 8);
		//dbg("shift %d %d", i, shift);
		if (shift < 0)
			break;
		*(out + i) = (sample >> (bitspersample - shift ) ) & 0x00FF;
	}
	return i;
}

static int filter_run(filter_ctx_t *ctx, filter_audio_t *audio, unsigned char *buffer, size_t size)
{
	int i;
	int j;
	int bufferlen = 0;

	for (i = 0; i < audio->nsamples; i++)
	{
		signed int sample = audio->samples[0][i];
		for (j = 0; j < ctx->nchannels; j++)
		{
			if (j < audio->nchannels)
				sample = audio->samples[(j % audio->nchannels)][i];
			bufferlen += sampled(ctx, sample, audio->bitspersample,
						buffer + bufferlen);
			if (bufferlen >= size)
				goto filter_exit;
		}
		if (bufferlen >= size)
			goto filter_exit;
	}
filter_exit:
	audio->nsamples -= i;
	for (j = 0; j < audio->nchannels; j++)
		audio->samples[j] += i;
	return bufferlen;
}

const filter_ops_t *filter_pcm = &(filter_ops_t)
{
	.init = filter_init,
	.run = filter_run,
	.destroy = filter_destroy,
};

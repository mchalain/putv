/*****************************************************************************
 * filter_boost.c
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
#include <math.h>

#include "filter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define filter_dbg(...)

uint32_t filter_rms1(uint32_t rms, sample_t sample, uint64_t nbs);
uint32_t filter_rms_sqappend(uint32_t sqrms, sample_t sample, int bitspersample);
uint32_t filter_rms_finish(uint32_t sqrms, int bitspersample, int laps);

stats_t *stats_init(stats_t *input)
{
	if (input == NULL)
		input = calloc(1, sizeof(*input));
	return input;
}

sample_t stats_cb(void *arg, sample_t sample, int bitspersample, int samplerate, int channel)
{
	stats_t *ctx = (stats_t *)arg;
	if (channel != 0)
		return sample;

	filter_dbg("filter: stat");
	if (ctx->lapswindow == 0)
		ctx->lapswindow = samplerate * 10; // 10s
	sample_t asample = 0;
	asample = labs(sample);
	if (sample == INT32_MIN)
	{
		uint64_t max = filter_maxvalue(bitspersample);
		fprintf(stdout, "peak %u / %ld\t", ctx->peak, max);
		fprintf(stdout, "rms %u\t", ctx->rms);
		fprintf(stdout, "boost %ld\t", ((max * 3) / ctx->peak) - 3);
		fprintf(stdout, "\n");
	}
	else
	{
		ctx->nbs++;
		if (ctx->peak < asample)
			ctx->peak = asample;
		if (ctx->bitspersample == 0)
			ctx->bitspersample = bitspersample;
/*
		if (ctx->mean < UINT64_MAX)
		{
			uint64_t mean = ctx->mean;
			mean *= (ctx->nbs - 1);
			mean += asample + 1;
			ctx->mean = mean / ctx->nbs;
		}
*/
		//ctx->rms = filter_rms1(ctx->rms, ctx->nbs);
		ctx->rms = filter_rms_sqappend(ctx->rms, sample, bitspersample);
		if (ctx->nbs % ctx->lapswindow == 0)
		{
			uint32_t rms = filter_rms_finish(ctx->rms, bitspersample, ctx->lapswindow);
			warn("filter: RMS on 10s %.010u peak %.010u sqRMS %.010u", rms, ctx->peak, ctx->rms);
			ctx->rms = 0;
		}
	}
	return sample;
}

uint32_t filter_rms1(uint32_t rms, sample_t sample, uint64_t nbs)
{
	uint64_t sqrms = ((uint64_t)rms * (uint64_t)rms);
	if (rms < nbs)
	{
		sqrms *= (nbs - 1);
		sqrms /= nbs;
	}
	else
	{
		sqrms /= nbs;
		sqrms *= (nbs - 1);
	}
	uint64_t sqsample = ((uint64_t)sample * (uint64_t)sample);
	sqsample /= nbs;
	rms = sqrtl(sqrms + sqsample);
	return rms;
}

uint32_t filter_rms_sqappend(uint32_t sqrms, sample_t sample, int bitspersample)
{
	sample_t samplescale = sample >> (bitspersample >> 1);
	return sqrms + (samplescale * samplescale);
}

uint32_t filter_rms_finish(uint32_t sqrms, int bitspersample, int laps)
{
	return ((sample_t)sqrt(sqrms / laps)) << (bitspersample >> 1);
}

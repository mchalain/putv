/*****************************************************************************
 * heartbeat_samples.c
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
#include <time.h>

typedef struct heartbeat_ctx_s heartbeat_ctx_t;
struct heartbeat_ctx_s
{
	unsigned int samplerate;
	unsigned int samplesize;
	unsigned int nchannels;
	struct timespec clock;
};
#define HEARTBEAT_CTX
#include "heartbeat.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define heartbeat_dbg(...)

heartbeat_ctx_t *heartbeat_init(unsigned int samplerate, unsigned int samplesize, unsigned int nchannels)
{
	heartbeat_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->samplerate = samplerate;
	ctx->samplesize = samplesize;
	ctx->nchannels = nchannels;

	return ctx;
}

void heartbeat_destroy(heartbeat_ctx_t *ctx)
{
	free(ctx);
}

static int heartbeat_wait(heartbeat_ctx_t *ctx, void *arg)
{
	heartbeat_samples_t *beat = (heartbeat_samples_t *)arg;
	clockid_t clockid = CLOCK_REALTIME;
	if (ctx->samplerate == 0)
		return -1;
	unsigned long msec = beat->nsamples * 1000 / ctx->samplerate;
	if (ctx->clock.tv_sec == 0 && ctx->clock.tv_nsec == 0)
		clock_gettime(clockid, &ctx->clock);
	ctx->clock.tv_nsec += (msec % 1000) * 1000000;
	ctx->clock.tv_sec += msec / 1000;
	if (ctx->clock.tv_nsec > 1000000000)
	{
		ctx->clock.tv_nsec -= 1000000000;
		ctx->clock.tv_sec += 1;
	}
	struct timespec rest;
	memset(&rest, 0, sizeof(rest));
	struct timespec now = {0, 0};
	clock_gettime(clockid, &now);
	if (now.tv_sec > ctx->clock.tv_sec ||
		(now.tv_sec == ctx->clock.tv_sec && now.tv_nsec > ctx->clock.tv_nsec))
	{
		now.tv_sec -= ctx->clock.tv_sec;
		now.tv_nsec -= ctx->clock.tv_nsec;
		if (now.tv_nsec < 0)
		{
			now.tv_nsec += 1000000000;
			now.tv_sec -= 1;
		}
		if (now.tv_nsec > 10000000)
			heartbeat_dbg("heartbeat to late %u.%u", now.tv_sec, now.tv_nsec);
		//clock_gettime(clockid, &ctx->clock);
		return -1;
	}
	int flags = TIMER_ABSTIME;
	while (clock_nanosleep(clockid, flags, &ctx->clock, &rest) != 0)
	{
		err("heartbeat hook");
	}
	heartbeat_dbg("heartbeat: boom %d", msec);
	clock_gettime(clockid, &ctx->clock);
	beat->nsamples = 0;

	return 0;
}

const heartbeat_ops_t *heartbeat_samples = &(heartbeat_ops_t)
{
	.init = heartbeat_init,
	.wait = heartbeat_wait,
	.destroy = heartbeat_destroy,
};

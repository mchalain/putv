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
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

#include "jitter.h"
typedef struct heartbeat_ctx_s heartbeat_ctx_t;
struct heartbeat_ctx_s
{
	struct timespec clock;
	pthread_mutex_t mutex;
	uint64_t nsamples;
	unsigned int samplerate;
	unsigned int samplesize;
	unsigned int nchannels;
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

static clockid_t clockid = CLOCK_REALTIME;

static heartbeat_ctx_t *heartbeat_init(void *arg)
{
	heartbeat_samples_t *config = (heartbeat_samples_t *)arg;
	heartbeat_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->samplerate = config->samplerate;
	ctx->nchannels = FORMAT_NCHANNELS(config->format);
	ctx->samplesize = FORMAT_SAMPLESIZE(config->format) / 8;
	if (config->nchannels != 0)
		ctx->nchannels = config->nchannels;

	pthread_mutex_init(&ctx->mutex, NULL);
	return ctx;
}

static void heartbeat_destroy(heartbeat_ctx_t *ctx)
{
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
}

static void heartbeat_start(heartbeat_ctx_t *ctx)
{
	dbg("heartbeat: start");
	pthread_mutex_lock(&ctx->mutex);
	clock_gettime(clockid, &ctx->clock);
	ctx->nsamples = 0;
	pthread_mutex_unlock(&ctx->mutex);
}

static int heartbeat_wait(heartbeat_ctx_t *ctx, void *arg)
{
	beat_samples_t *beat = (beat_samples_t *)arg;
	if (ctx->samplerate == 0)
		return -1;

	if (ctx->clock.tv_sec == 0 && ctx->clock.tv_nsec == 0)
		return -1;

	if (beat->nsamples == 0)
		return 0;
	pthread_mutex_lock(&ctx->mutex);
	ctx->nsamples = beat->nsamples;
//	if (ctx->nsamples < ctx->samplerate / 200) // 10 ms * nbchannels
//	{
//		pthread_mutex_unlock(&ctx->mutex);
//		return 0;
//	}

	unsigned long usec = ctx->nsamples * HEARTBEAT_COEF_1000;
	int divider = ctx->samplerate / 100;
	usec /= divider;
	usec *= 10;
#if 1
	ctx->clock.tv_nsec += (usec % 1000000) * 1000;
	ctx->clock.tv_sec += usec / 1000000;
#else
	ctx->clock.tv_nsec +=
		(ctx->nsamples * HEARTBEAT_COEF_1000 / ctx->samplerate) * 1000000;

	ctx->clock.tv_sec += (ctx->nsamples / ctx->samplerate);
#endif
	if (ctx->clock.tv_nsec > 1000000000)
	{
		ctx->clock.tv_nsec -= 1000000000;
		ctx->clock.tv_sec += 1;
	}
	int flags = TIMER_ABSTIME;
	while (clock_nanosleep(clockid, flags, &ctx->clock, NULL) != 0)
	{
		if (errno == EINTR)
			continue;
		else
		{
			if (errno == EFAULT)
				heartbeat_dbg("heartbeat to late %lu.%09lu", ctx->clock.tv_sec, ctx->clock.tv_nsec);
			pthread_mutex_unlock(&ctx->mutex);
			return -1;
		}
	}
#ifdef DEBUG
	struct timespec now;
	clock_gettime(clockid, &now);
	heartbeat_dbg("heartbeat: boom %lu.%09lu %ld.%06ld", now.tv_sec, now.tv_nsec, usec / 1000000, usec % 1000000);
#endif
	ctx->nsamples = 0;
	beat->nsamples = 0;

	pthread_mutex_unlock(&ctx->mutex);
	return 0;
}

static int heartbeat_lock(heartbeat_ctx_t *ctx)
{
	return pthread_mutex_lock(&ctx->mutex);
}

static int heartbeat_unlock(heartbeat_ctx_t *ctx)
{
	return pthread_mutex_unlock(&ctx->mutex);
}

const heartbeat_ops_t *heartbeat_samples = &(heartbeat_ops_t)
{
	.init = heartbeat_init,
	.start = heartbeat_start,
	.wait = heartbeat_wait,
	.lock = heartbeat_lock,
	.unlock = heartbeat_unlock,
	.destroy = heartbeat_destroy,
};

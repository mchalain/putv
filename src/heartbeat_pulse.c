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
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

#include "jitter.h"
typedef struct heartbeat_ctx_s heartbeat_ctx_t;
struct heartbeat_ctx_s
{
	int run;
	unsigned int ms;
	struct timespec clock;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_t thread;
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

#define HEARTBEAT_POLICY REALTIME_SCHED
#define HEARTBEAT_PRIORITY 65

static heartbeat_ctx_t *heartbeat_init(void *arg)
{
	heartbeat_pulse_t *config = (heartbeat_pulse_t *)arg;
	heartbeat_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ms = config->ms;
	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->cond, NULL);

	return ctx;
}

static void heartbeat_destroy(heartbeat_ctx_t *ctx)
{
	ctx->run = 0;
	pthread_join(ctx->thread, NULL);
	pthread_mutex_destroy(&ctx->mutex);
	pthread_cond_destroy(&ctx->cond);
	free(ctx);
}

static void *_heartbeat_thread(void *arg)
{
	heartbeat_ctx_t *ctx = (heartbeat_ctx_t *)arg;
	clockid_t clockid = CLOCK_REALTIME;
	int flags = 0;
	struct timespec clock;
	struct timespec rest;

	ctx->run = 1;
	while (ctx->run)
	{
		clock.tv_sec = 0;
		clock.tv_nsec = ctx->ms * HEARTBEAT_COEF_1000 * 1000;
		clock_nanosleep(clockid, flags, &clock, &rest);
		pthread_cond_broadcast(&ctx->cond);
	}
}

static void heartbeat_start(heartbeat_ctx_t *ctx)
{
#ifdef USE_REALTIME
	int ret;

	pthread_attr_t attr;
	struct sched_param params;

	pthread_attr_init(&attr);

	ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	if (ret < 0)
		err("setdetachstate error %s", strerror(errno));
	ret = pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS);
	if (ret < 0)
		err("setscope error %s", strerror(errno));
	ret = pthread_attr_setschedpolicy(&attr, HEARTBEAT_POLICY);
	if (ret < 0)
		err("setschedpolicy error %s", strerror(errno));
	params.sched_priority = HEARTBEAT_PRIORITY;
	ret = pthread_attr_setschedparam(&attr, &params);
	if (ret < 0)
		err("setschedparam error %s", strerror(errno));
	if (getuid() == 0)
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	else
	{
		warn("run server as root to use realtime");
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
	}
	if (ret < 0)
		err("setinheritsched error %s", strerror(errno));
	pthread_create(&ctx->thread, &attr, _heartbeat_thread, ctx);
	pthread_attr_destroy(&attr);
#else
	pthread_create(&ctx->thread, NULL, _heartbeat_thread, ctx);
#endif
}

static int heartbeat_wait(heartbeat_ctx_t *ctx, void *arg)
{
	beat_pulse_t *beat = (beat_pulse_t *)arg;
	uint16_t pulses = beat->pulses;
	pthread_mutex_lock(&ctx->mutex);
	while (pulses > 0)
	{
		pthread_cond_wait(&ctx->cond, &ctx->mutex);
		pulses --;
	}
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

const heartbeat_ops_t *heartbeat_pulse = &(heartbeat_ops_t)
{
	.init = heartbeat_init,
	.start = heartbeat_start,
	.wait = heartbeat_wait,
	.lock = heartbeat_lock,
	.unlock = heartbeat_unlock,
	.destroy = heartbeat_destroy,
};

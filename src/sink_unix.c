/*****************************************************************************
 * sink_unix.c
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
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>

#define __USE_GNU
#include <pthread.h>

#include <unistd.h>
#include <poll.h>

#include "player.h"
#include "jitter.h"
#include "unix_server.h"
typedef struct sink_s sink_t;
typedef struct sink_ctx_s sink_ctx_t;
struct sink_ctx_s
{
	player_ctx_t *player;
	const sink_t *ops;
	const char *filepath;
	pthread_t thread;
	pthread_t thread2;
	jitter_t *in;
	state_t state;
	char *out;
	size_t length;
	unsigned int samplerate;
	int samplesize;
	int nchannels;
#ifndef USE_REALTIME
	thread_info_t *clients[MAX_CLIENTS];
#endif
#ifdef SYNC_ACK
	pthread_cond_t ack;
#endif
	pthread_cond_t event;
	pthread_mutex_t mutex;
	int counter;
	int nbclients;
};
#define SINK_CTX
#include "sink.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define sink_dbg(...)

#define SERVER_POLICY REALTIME_SCHED
#define SERVER_PRIORITY 45
#define SINK_POLICY REALTIME_SCHED
#define SINK_PRIORITY 65

static const char *jitter_name = "unix socket";
static sink_ctx_t *sink_init(player_ctx_t *player, const char *filepath)
{
	int count = 2;
	jitter_format_t format = SINK_BITSSTREAM;
	sink_ctx_t *ctx = calloc(1, sizeof(*ctx));

	ctx->ops = sink_unix;
	ctx->filepath = filepath;

	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->event, NULL);
#ifdef SYNC_ACK
	pthread_cond_init(&ctx->ack, NULL);
#endif

	unsigned int size;
	jitter_t *jitter = jitter_scattergather_init(jitter_name, 6, size);
	jitter->ctx->frequence = DEFAULT_SAMPLERATE;
	jitter->ctx->thredhold = 2;
	jitter->format = format;
	ctx->in = jitter;
	ctx->out = calloc(1, size);

	ctx->player = player;

	return ctx;
}

static jitter_t *sink_jitter(sink_ctx_t *ctx)
{
	return ctx->in;
}

static int sink_unxiclient(thread_info_t *info)
{
	sink_ctx_t *ctx = (sink_ctx_t *)info->userctx;
	int ret;
	int run = 1;

#ifdef SINK_UNIX_WAITCLIENT
	if ( ctx->nbclients == 0)
	{
		player_state(ctx->player, STATE_PLAY);
	}
#endif

	ctx->nbclients++;

#ifndef USE_REALTIME
	int i;
	for (i = 0; i < MAX_CLIENTS; i++)
	{
		if (ctx->clients[i] == NULL)
		{
			ctx->clients[i] = info;
			break;
		}
	}
	if (i < MAX_CLIENTS)
	{
		while (run)
		{
			struct pollfd poll_set[1];
			int numfds = 0;
			poll_set[0].fd = info->sock;
			numfds++;
			ret = poll(poll_set, numfds, -1);
			if (poll_set[0].revents & POLLHUP)
				run = 0;
		}
	}
#else
	int counter = ctx->counter;
	while (run)
	{
		pthread_mutex_lock(&ctx->mutex);
		while (ctx->counter <= counter)
		{
			pthread_cond_wait(&ctx->event, &ctx->mutex);
		}
		sink_dbg("send %ld %d %d", ctx->length, ctx->counter, counter);
		if (ctx->length > 0)
		{
			ret = send(info->sock, ctx->out, ctx->length, MSG_NOSIGNAL| MSG_DONTWAIT);
			if (ret < 0)
				run = 0;
		}
		counter = ctx->counter;
		pthread_mutex_unlock(&ctx->mutex);
#ifdef SYNC_ACK
		pthread_cond_broadcast(&ctx->ack);
#endif
	}
#endif
	ctx->nbclients--;
#ifdef SINK_UNIX_WAITCLIENT
	if ( ctx->nbclients == 0)
	{
		player_state(ctx->player, STATE_STOP);
	}
#endif
	return ret;
}

static void *sink_thread(void *arg)
{
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	int run = 1;

	/* start decoding */
	dbg("sink: thread run");
	while (run)
	{
		unsigned char *buff = ctx->in->ops->peer(ctx->in->ctx);
		if (buff == NULL)
		{
			run = 0;
			break;
		}
		pthread_mutex_lock(&ctx->mutex);
		int length = ctx->in->ops->length(ctx->in->ctx);
		memcpy(ctx->out, buff, length);
		ctx->length = length;
		ctx->counter++;

#ifndef USE_REALTIME
		int i;
		int ret;
		for (i = 0; i < MAX_CLIENTS; i++)
		{
			thread_info_t *info = ctx->clients[i];
			if (ctx->length > 0 && info && info->sock != 0)
			{
				ret = send(info->sock, ctx->out, ctx->length, MSG_NOSIGNAL| MSG_DONTWAIT);
				if (ret < 0)
				{
					close(info->sock);
					ctx->clients[i] = NULL;
				}
			}
		}
#endif

		pthread_mutex_unlock(&ctx->mutex);
#ifdef SYNC_ACK
		pthread_mutex_lock(&ctx->mutex);
		pthread_cond_broadcast(&ctx->event);
		pthread_cond_wait(&ctx->ack, &ctx->mutex);
		pthread_mutex_unlock(&ctx->mutex);
#else
		pthread_cond_broadcast(&ctx->event);
		pthread_yield();
#endif
		sink_dbg("sink: boom %d", ctx->counter);
		ctx->in->ops->pop(ctx->in->ctx, ctx->length);
		pthread_yield();
	}
	dbg("sink: thread end");
	return NULL;
}

static void *server_thread(void *arg)
{
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	unixserver_run(sink_unxiclient, ctx, ctx->filepath);
	return NULL;
}

static int sink_run(sink_ctx_t *ctx)
{
#ifdef USE_REALTIME
	pthread_attr_t attr;
	struct sched_param params;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SINK_POLICY);
	params.sched_priority = SINK_PRIORITY;
	pthread_attr_setschedparam(&attr, &params);
	pthread_create(&ctx->thread, &attr, sink_thread, ctx);
	pthread_attr_destroy(&attr);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS);
	pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SERVER_POLICY);
	params.sched_priority = SERVER_PRIORITY;
	pthread_attr_setschedparam(&attr, &params);
	pthread_create(&ctx->thread2, &attr, server_thread, ctx);
	pthread_attr_destroy(&attr);
#else
	pthread_create(&ctx->thread, NULL, sink_thread, ctx);
	pthread_create(&ctx->thread2, NULL, server_thread, ctx);
#endif

	return 0;
}

static void sink_destroy(sink_ctx_t *ctx)
{
	pthread_join(ctx->thread2, NULL);
	pthread_join(ctx->thread, NULL);
	jitter_scattergather_destroy(ctx->in);
	free(ctx->out);
	free(ctx);
}

const sink_t *sink_unix = &(sink_t)
{
	.init = sink_init,
	.jitter = sink_jitter,
	.run = sink_run,
	.destroy = sink_destroy,
};

#ifndef SINK_GET
#define SINK_GET
const sink_t *sink_get(sink_ctx_t *ctx)
{
	return ctx->ops;
}
#endif

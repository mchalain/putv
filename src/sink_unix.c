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
#include <pwd.h>

#include "player.h"
#include "jitter.h"
#include "encoder.h"
#include "unix_server.h"
typedef struct sink_s sink_t;
typedef struct sink_ctx_s sink_ctx_t;
struct sink_ctx_s
{
	player_ctx_t *player;
	jitter_t *in;
	event_listener_t *listener;

	const char *filepath;
	pthread_t thread;
	pthread_t thread2;
	state_t state;
#ifdef SINK_UNIX_ASYNC
	char *out;
	size_t length;
	pthread_cond_t ack;
	pthread_cond_t event;
	pthread_mutex_t mutex;
#else
	thread_info_t *clients[MAX_CLIENTS];
#endif
	int counter;
	unsigned int samplerate;
	char samplesize;
	char nchannels;
	char nbclients;
	char ackstate;
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

#ifdef USE_REALTIME
// REALTIME_SCHED is set from the Makefile to SCHED_RR
#define SERVER_POLICY REALTIME_SCHED
#define SERVER_PRIORITY 45
#define SINK_POLICY REALTIME_SCHED
#define SINK_PRIORITY 65
#endif

#define BUFFERSIZE ENCODER_FRAME_SIZE

static const char *jitter_name = "unix socket";
static sink_ctx_t *sink_init(player_ctx_t *player, const char *url)
{
	int count = 2;

	const char *path = NULL;

	if (strstr(url, "://") != NULL)
	{
		path = strstr(url, "file://");
		if (path != NULL)
			path += 7;
		else if ((path = strstr(url, "unix://")) != NULL)
			path += 7;
		else
			return NULL;
	}
	else
	{
		path = url;
	}
	if (path[0] == '~')
	{
		struct passwd *pw = NULL;
		pw = getpwuid(geteuid());
		if (chdir(pw->pw_dir) == 0)
			path++;
		if (path[0] == '/')
			path++;
	}

	sink_ctx_t *ctx = calloc(1, sizeof(*ctx));

	ctx->filepath = path;

	jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, 6, BUFFERSIZE);
	jitter->ctx->frequence = 0;
	jitter->ctx->thredhold = 2;
	jitter->format = SINK_BITSSTREAM;
	ctx->in = jitter;

#ifdef SINK_UNIX_ASYNC
	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->event, NULL);
	pthread_cond_init(&ctx->ack, NULL);
	ctx->out = calloc(1, BUFFERSIZE);
#endif

	ctx->player = player;

	return ctx;
}

static unsigned int sink_attach(sink_ctx_t *ctx, const char *mime)
{
	return 0;
}

static jitter_t *sink_jitter(sink_ctx_t *ctx, unsigned int index)
{
	if (index == 0)
		return ctx->in;
	return NULL;
}

static const encoder_t *sink_encoder(sink_ctx_t *ctx)
{
	return ENCODER;
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
	if (ctx->nbclients == MAX_CLIENTS)
		return -1;
	ctx->nbclients++;

#ifdef SINK_UNIX_ASYNC
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
		ctx->ackstate = 1;
		pthread_mutex_unlock(&ctx->mutex);
		pthread_cond_broadcast(&ctx->ack);
	}
#else
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
#ifdef DEBUG
static void
display_sched_attr(int policy, struct sched_param *param)
{
   dbg("    policy=%s, priority=%d\n",
		   (policy == SCHED_FIFO)  ? "SCHED_FIFO" :
		   (policy == SCHED_RR)    ? "SCHED_RR" :
		   (policy == SCHED_OTHER) ? "SCHED_OTHER" :
		   "???",
		   param->sched_priority);
}
#endif

static void *sink_thread(void *arg)
{
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	int run = 1;

	warn("sink: running");
#ifdef DEBUG
	int policy;
	struct sched_param param;
	pthread_getschedparam(pthread_self(), &policy, &param);
	display_sched_attr(policy, &param);
#endif
	int ack = ctx->counter;
	while (run)
	{
		unsigned char *buff = ctx->in->ops->peer(ctx->in->ctx, NULL);
		if (buff == NULL)
		{
			run = 0;
			break;
		}
		ctx->counter++;
		int length = ctx->in->ops->length(ctx->in->ctx);

#ifdef SINK_UNIX_ASYNC
		pthread_mutex_lock(&ctx->mutex);
		memcpy(ctx->out, buff, length);
		ctx->length = length;
		pthread_mutex_unlock(&ctx->mutex);
		pthread_cond_broadcast(&ctx->event);
		pthread_mutex_lock(&ctx->mutex);
		while (ctx->ackstate == 0)
			pthread_cond_wait(&ctx->ack, &ctx->mutex);
		ctx->ackstate = 0;
		pthread_mutex_unlock(&ctx->mutex);
		sched_yield();
#else
		int i;
		int ret;
		for (i = 0; i < MAX_CLIENTS; i++)
		{
			thread_info_t *info = ctx->clients[i];
			if (length > 0 && info && info->sock != 0)
			{
				fd_set wfds;
				int maxfd = info->sock;
				FD_ZERO(&wfds);
				FD_SET(info->sock, &wfds);
				ret = select(maxfd + 1, NULL, &wfds, NULL,NULL);
				if (ret > 0 && FD_ISSET(info->sock, &wfds))
				{
					ret = send(info->sock, buff, length, MSG_NOSIGNAL| MSG_DONTWAIT);
					if (ret < 0 && errno != EAGAIN)
					{
						err("send errot %s", strerror(errno));
						close(info->sock);
						ctx->clients[i] = NULL;
					}
				}
			}
		}
#endif
		sink_dbg("sink: boom %d", ctx->counter);
		ctx->in->ops->pop(ctx->in->ctx, length);
		sched_yield();
	}
	dbg("sink: thread end");
	return NULL;
}

static void *server_thread(void *arg)
{
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	warn("server running");
#ifdef DEBUG
	int policy;
	struct sched_param param;
	pthread_getschedparam(pthread_self(), &policy, &param);
	display_sched_attr(policy, &param);
#endif
	unixserver_run(sink_unxiclient, ctx, ctx->filepath);
	return NULL;
}

static int sink_run(sink_ctx_t *ctx)
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
	ret = pthread_attr_setschedpolicy(&attr, SINK_POLICY);
	if (ret < 0)
		err("setschedpolicy error %s", strerror(errno));
	params.sched_priority = SINK_PRIORITY;
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
	pthread_create(&ctx->thread, &attr, sink_thread, ctx);
	pthread_attr_destroy(&attr);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	if (ret < 0)
		err("setdetachstate error %s", strerror(errno));
	pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS);
	if (ret < 0)
		err("setscope error %s", strerror(errno));
	pthread_attr_setschedpolicy(&attr, SERVER_POLICY);
	if (ret < 0)
		err("setschedpolicy error %s", strerror(errno));
	params.sched_priority = SERVER_PRIORITY;
	pthread_attr_setschedparam(&attr, &params);
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
	if (ctx->thread2)
	{
		pthread_join(ctx->thread2, NULL);
	}
	if (ctx->thread)
	{
		pthread_join(ctx->thread, NULL);
	}
	jitter_destroy(ctx->in);
#ifdef SINK_UNIX_ASYNC
	pthread_mutex_destroy(&ctx->mutex);
	pthread_cond_destroy(&ctx->event);
	pthread_cond_destroy(&ctx->ack);
	free(ctx->out);
#endif
	free(ctx);
}

const sink_ops_t *sink_unix = &(sink_ops_t)
{
	.name = "unix",
	.default_ = "unix:///tmp/stream",
	.init = sink_init,
	.jitter = sink_jitter,
	.attach = sink_attach,
	.encoder = sink_encoder,
	.run = sink_run,
	.destroy = sink_destroy,
};

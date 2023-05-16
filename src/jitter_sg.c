/*****************************************************************************
 * jitter_sg.c
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

#define __USE_GNU
#include <pthread.h>
#include <sys/mman.h>

#include "jitter.h"
#include "heartbeat.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif
typedef struct scatter_s scatter_t;
struct scatter_s
{
	enum
	{
		SCATTER_FREE,
		SCATTER_PULL,
		SCATTER_POP,
		SCATTER_READY,
	} state;
	unsigned char *data;
	size_t len;
	int channel;
	void *beat;
	scatter_t *next;
};

typedef struct jitter_private_s jitter_private_t;
struct jitter_private_s
{
	unsigned char *buffer;
	scatter_t *sg;
	scatter_t *in;
	scatter_t *out;
	pthread_mutex_t mutex;
	pthread_cond_t condpush;
	pthread_cond_t condpeer;
	unsigned int level;
	unsigned int nbchannels;
	enum
	{
		JITTER_STOP,
		JITTER_FILLING,
		JITTER_RUNNING,
		JITTER_OVERFLOW,
		JITTER_FLUSH,
		JITTER_COMPLETE,
	} state;
	int pause;
};

static unsigned char *jitter_pull(jitter_ctx_t *jitter);
static unsigned char *jitter_pull_channel(jitter_ctx_t *jitter, int channel);
static void jitter_push(jitter_ctx_t *jitter, size_t len, void *beat);
static unsigned char *jitter_peer(jitter_ctx_t *jitter, void **beat);
static unsigned char *jitter_peer_channel(jitter_ctx_t *jitter, int channel, void **beat);
static void jitter_pop(jitter_ctx_t *jitter, size_t len);
static void jitter_reset(jitter_ctx_t *jitter);

static const jitter_ops_t *jitter_scattergather;

static void jitter_scattergather_destroy(jitter_t *);

jitter_t *jitter_scattergather_init(const char *name, unsigned int count, size_t size)
{
	jitter_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->count = count;
	ctx->size = size;
	ctx->name = name;
	jitter_private_t *private = calloc(1, sizeof(*private));
	private->buffer = malloc(count * size);
	if (private->buffer == NULL)
	{
		err("jitter %s not enought memory %lu", name, count * size);
		free(private);
		free(ctx);
		return NULL;
	}
	pthread_mutex_init(&private->mutex, NULL);
	pthread_cond_init(&private->condpush, NULL);
	pthread_cond_init(&private->condpeer, NULL);

	// create the scatter gather
	private->sg = calloc(count, sizeof(*private->sg));
	if (private->sg == NULL)
	{
		err("jitter %s not enought memory", name);
		free(private->buffer);
		free(private);
		free(ctx);
		return NULL;
	}
	int i;
	scatter_t *it;
	for (i = 0; i < count; i++)
	{
		it = &private->sg[i];
		it->data = private->buffer + (i * size);
		it->next = &private->sg[i + 1];
	}
	// loop on the first element
	it->next = private->sg;
	private->in = private->out = private->sg;

	ctx->private = private;
	jitter_t *jitter = calloc(1, sizeof(*jitter));
	jitter->ctx = ctx;
	jitter->ops = jitter_scattergather;
	jitter->destroy = &jitter_scattergather_destroy;
	dbg("jitter %s create scattergather (%d*%ld) %p", name, count, size, private->sg);
	return jitter;
}

static void jitter_scattergather_destroy(jitter_t *jitter)
{
	jitter_ctx_t *ctx = jitter->ctx;
	jitter_private_t *private = (jitter_private_t *)ctx->private;

	jitter_reset(ctx);

	pthread_cond_destroy(&private->condpush);
	pthread_cond_destroy(&private->condpeer);
	pthread_mutex_destroy(&private->mutex);

	free(private->buffer);
	free(private->sg);
	free(private);
	free(ctx);
	free(jitter);
}

static void _jitter_init(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	if (jitter->thredhold == 0)
		private->state = JITTER_RUNNING;
	else
		private->state = JITTER_FILLING;
}

static heartbeat_t *jitter_heartbeat(jitter_ctx_t *ctx, heartbeat_t *new)
{
	heartbeat_t *old = ctx->heartbeat;
	if (new != NULL)
		ctx->heartbeat = new;
	return old;
}

#ifdef USE_REALTIME
static void jitter_lock(jitter_ctx_t *ctx)
{
	jitter_private_t *private = (jitter_private_t *)ctx->private;

	mlock(private->buffer, ctx->count * ctx->size);
	mlock(private->sg, ctx->count * sizeof(*private->sg));
}
#else
#define jitter_lock NULL
#endif

static unsigned char *jitter_pull(jitter_ctx_t *jitter)
{
	return jitter_pull_channel(jitter, 0);
}

static unsigned char *jitter_pull_channel(jitter_ctx_t *jitter, int channel)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	if (private->nbchannels < channel + 1)
		private->nbchannels = channel + 1;
	pthread_mutex_lock(&private->mutex);
	if (private->state == JITTER_STOP)
		_jitter_init(jitter);
	while (private->in->state != SCATTER_FREE)
	{
		if (private->state == JITTER_FLUSH)
			break;
		/**
		 * The scatter gather is full and we has to wait that the consumer
		 * free some buffer.
		 */

		jitter_dbg(jitter, "pull block on %p %d %d", private->in, private->state, private->level);
		pthread_cond_wait(&private->condpush, &private->mutex);

	}
	unsigned char *ret= NULL;
	if (private->state != JITTER_FLUSH &&
		private->state != JITTER_STOP &&
		private->in->state == SCATTER_FREE)
	{
		private->in->state = SCATTER_PULL;
		ret = private->in->data;
		private->in->channel = channel;
	}
	pthread_mutex_unlock(&private->mutex);
	jitter_dbg(jitter, "pull %p", private->in);
	return ret;
}

static void jitter_push(jitter_ctx_t *jitter, size_t len, void *beat)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	jitter_dbg(jitter, "push %p", private->in);
	if (private->in->state != SCATTER_PULL)
	{
		/**
		 * this situation should be exist. It may arrive
		 * if the push is called twice on the same buffer.
		 */
		pthread_cond_broadcast(&private->condpeer);
		return;
	}
	if (len == 0)
	{
		/**
		 * TODO check the ring buffer push 0 kills the jitter
		 */
		/**
		 * the producer push empty buffer to end the stream
		 */
		jitter_dbg(jitter, "push 0");
		pthread_mutex_lock(&private->mutex);
		private->in->state = SCATTER_FREE;
		private->state = JITTER_COMPLETE;
		pthread_mutex_unlock(&private->mutex);
	}
	else
	{
		if (len < jitter->size)
		{
			jitter_dbg(jitter, "scatter not full (%lu)", len);
		}
		pthread_mutex_lock(&private->mutex);
		private->in->len = len;
		private->in->beat = beat;
		private->in->state = SCATTER_READY;
		private->level++;
		private->in = private->in->next;
		pthread_mutex_unlock(&private->mutex);
		/**
		 * The standard case uses a thread to consume the buffers.
		 * But here the consumer is set durring the initalization
		 * and it is called by the same thread that the producer.
		 */
		if (jitter->consume != NULL)
		{
			private->out->state = SCATTER_POP;
			int tlen = 0;
#ifdef HEARTBEAT
			if (private->out->beat && jitter->heartbeat != NULL)
			{
				heartbeat_t *heartbeat = jitter->heartbeat;
				heartbeat->ops->wait(heartbeat->ctx, private->out->beat);
				jitter_dbg(jitter, "boom");
				private->out->beat = NULL;
			}
#endif
			do
			{
				int ret;
				ret = jitter->consume(jitter->consumer,
					private->out->data + tlen, len - tlen);
				if (ret > 0)
					tlen += ret;
				if (ret <= 0)
				{
					tlen = ret;
					break;
				}
			} while (tlen < len);
			if (tlen > 0)
				jitter_pop(jitter, tlen);
			else
				return;
		}
		/**
		 * End of the consumer. The rest is the normal case of the push
		 * function.
		 */
	}

	if (private->state == JITTER_RUNNING)
	{
		/**
		 * The buffer is running and the input
		 * has to send an event to the consumer
		 * that a new buffer is ready */
		pthread_cond_broadcast(&private->condpeer);
#if defined(HEARTBEAT)
		if (jitter->heartbeat != NULL)
		{
			if (private->in->beat)
				sched_yield();
		}
#endif
	}
	else if (private->state == JITTER_FILLING &&
			private->level == jitter->thredhold)
	{
		/**
		 * The scatter gather is filling and reaches the thredhold.
		 * The sg may run and send event to the next buffer.
		 */
		private->state = JITTER_RUNNING;
	}
}

static unsigned char *jitter_peer(jitter_ctx_t *jitter, void **beat)
{
	return jitter_peer_channel(jitter, 0, beat);
}

static unsigned char *jitter_peer_channel(jitter_ctx_t *jitter, int channel, void **beat)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	pthread_mutex_lock(&private->mutex);
	if (private->state == JITTER_STOP)
		_jitter_init(jitter);
	pthread_mutex_unlock(&private->mutex);
	if (private->out->state == SCATTER_FREE)
	{
		if ((private->in == private->out) && (jitter->produce != NULL))
		{
			/**
			 * In standard case a thread produce buffer and another one
			 * consume buffer. In this case the producer runs inside
			 * the consumer thread.
			 */
			do
			{
				/**
				 * The producer must push enougth data to start
				 * the running, otherwise the peer will block.
				 */
				int len = 0;
				do
				{
					int ret;
					ret = jitter->produce(jitter->producter,
						private->in->data + len, jitter->size - len);
					if (ret > 0)
						len += ret;
					if (ret <= 0)
					{
						len = ret;
						break;
					}
				} while (len < jitter->size);
				if (len > 0)
					jitter_push(jitter, len, NULL);
				else
				{
					dbg("produce nothing");
					return NULL;
				}
			} while (private->state == JITTER_FILLING);
			/**
			 * The end of the producer and returns to the standard case.
			 */
		}
		else if (private->state == JITTER_COMPLETE)
		{
			/**
			 * The consumer find the empty buffer to stop the stream
			 */
			jitter_dbg(jitter, "peer empty on %p", private->out);
			return NULL;
		}
	}
	pthread_mutex_lock(&private->mutex);
	while (((private->state == JITTER_FILLING) ||
			(private->out->state != SCATTER_READY)) ||
			private->pause)
	{
		/**
		 * The scatter gather is empty and the producer fills.
		 * The consumer is waiting that the thredhold is reached.
		 */
		jitter_dbg(jitter, "peer block on %p %d %d", private->out, private->state, private->out->state);
		pthread_cond_wait(&private->condpeer, &private->mutex);
		if (private->out->channel != channel)
			jitter_pop(jitter, private->out->len);
	}
	private->out->state = SCATTER_POP;
	pthread_mutex_unlock(&private->mutex);
#ifdef HEARTBEAT
	while (private->out->beat && jitter->heartbeat != NULL)
	{
		if (beat != NULL)
		{
			*beat = private->out->beat;
			private->out->beat = NULL;
			break;
		}
		/**
		 * The heartbeat is set by the producer.
		 * The scatter gather releases the buffer to the consumer
		 * when the heart beats
		 */
		int ret;
		heartbeat_t *heartbeat = jitter->heartbeat;
		ret = heartbeat->ops->wait(heartbeat->ctx, private->out->beat);
		jitter_dbg(jitter, "boom");
		if (ret == -1)
			heartbeat->ops->start(heartbeat->ctx);
		private->out->beat = NULL;
	}
#endif
	return private->out->data;
}

static void jitter_pop(jitter_ctx_t *jitter, size_t len)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	jitter_dbg(jitter, "pop %p %d", private->out, private->state);
	if ((private->state == JITTER_STOP) ||
		(private->out->state != SCATTER_POP))
	{
		/**
		 * This case should never become, except if the pop function
		 * is called twice.
		 */
		private->out->state = SCATTER_FREE;
		pthread_cond_broadcast(&private->condpush);
		return;
	}

	/**
	 * len = -1 equivalent as buffer full
	 * len is unused except for debug and in this case
	 * len = -1 equivalent len = MAX_SIZE_T
	 * if ((len + 1) == 0)
	 *	len = private->out->len;
	 */
	if (private->out->len > len)
	{
		dbg("buffer %s pop not empty %ld/%ld", jitter->name, len, private->out->len);
	}

	pthread_mutex_lock(&private->mutex);
	private->out->state = SCATTER_FREE;
	private->level--;
	private->out = private->out->next;
	if (private->level == 0 && jitter->thredhold > 0)
	{
		/**
		 * The producer empties the jitter. It requests to the producer
		 * to fill buffers ans to reach the thredhold.
		 */
		private->state = JITTER_FILLING;
	}
	pthread_mutex_unlock(&private->mutex);
	pthread_cond_broadcast(&private->condpush);
}

/**
 * This function may be called by the producer.
 * It stops the buffer filling and waits that the producer uses all buffers.
 * When the jitter is empty, the producer may continue to fill the buffers.
 */
static void jitter_flush(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	if (private->state == JITTER_FLUSH)
		return;
	jitter_dbg(jitter, "flush on %p %d %d", private->in, private->in->state, private->out->state);
	pthread_mutex_lock(&private->mutex);
	private->state = JITTER_FLUSH;
	pthread_mutex_unlock(&private->mutex);

	pthread_cond_broadcast(&private->condpush);
	pthread_cond_broadcast(&private->condpeer);
}

static size_t jitter_length(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	if (private->out->state == SCATTER_POP)
		return private->out->len;
	return -1;
}

/**
 * This function may be called by any thread to empty the stream and
 * leave the producer and the consumer to start from the beginning.
 */
static void jitter_reset(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	jitter_dbg(jitter, "reset");
	jitter_flush(jitter);

	pthread_mutex_lock(&private->mutex);
	private->state = JITTER_STOP;
	int i = 0;
	for (i = 0; i < jitter->count; i++)
	{
		private->in->state = SCATTER_FREE;
		private->in = private->in->next;
	}
	pthread_mutex_unlock(&private->mutex);

	pthread_cond_broadcast(&private->condpeer);
	pthread_cond_broadcast(&private->condpush);

	pthread_mutex_lock(&private->mutex);
	private->level = 0;
	private->state = JITTER_FILLING;
	private->in = private->out = private->sg;
	pthread_mutex_unlock(&private->mutex);
}

static int jitter_empty(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	if (private->state == JITTER_FILLING)
		return 1;
	return (private->out->state != SCATTER_READY);
}

static void jitter_pause(jitter_ctx_t *jitter, int enable)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	pthread_mutex_lock(&private->mutex);
	private->pause = enable;
	if ((private->state == JITTER_FLUSH) && !private->pause)
	{
		if (private->level > jitter->thredhold)
			private->state = JITTER_RUNNING;
		else
			private->state = JITTER_FILLING;
	}
	pthread_mutex_unlock(&private->mutex);
	pthread_cond_broadcast(&private->condpeer);
}

static unsigned int jitter_nbchannels(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	return private->nbchannels;
}

static const jitter_ops_t *jitter_scattergather = &(jitter_ops_t)
{
	.heartbeat = jitter_heartbeat,
	.reset = jitter_reset,
	.lock = jitter_lock,
	.pull = jitter_pull,
	.push = jitter_push,
	.peer = jitter_peer,
	.pop = jitter_pop,
	.flush = jitter_flush,
	.length = jitter_length,
	.empty = jitter_empty,
	.pause = jitter_pause,
	.nbchannels = jitter_nbchannels,
};

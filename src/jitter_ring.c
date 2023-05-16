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
#include <errno.h>

#define __USE_GNU
#include <pthread.h>
#include <sys/mman.h>

#include "jitter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define VARIATIC_OUTPUT 1
#define VARIATIC_INPUT 1

static unsigned char *jitter_pull(jitter_ctx_t *jitter);
static void jitter_push(jitter_ctx_t *jitter, size_t len, void *beat);
static unsigned char *jitter_peer(jitter_ctx_t *jitter, void **beat);
static void jitter_pop(jitter_ctx_t *jitter, size_t len);
static void jitter_reset(jitter_ctx_t *jitter);

static void jitter_ringbuffer_destroy(jitter_t *);

typedef struct jitter_private_s jitter_private_t;
struct jitter_private_s
{
	unsigned char *buffer;
	unsigned char *bufferstart;
	unsigned char *bufferend;
	unsigned char *in;
	unsigned char *out;
	pthread_mutex_t mutex;
	pthread_cond_t condpush;
	pthread_cond_t condpeer;
	int level;
	enum
	{
		JITTER_STOP,
		JITTER_FILLING,
		JITTER_RUNNING,
		JITTER_OVERFLOW,
		JITTER_FLUSH,
	} state;
	int pause;
};

static const jitter_ops_t *jitter_ringbuffer;

jitter_t *jitter_ringbuffer_init(const char *name, unsigned int count, size_t size)
{
	jitter_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->count = count;
	ctx->size = size;
	ctx->name = name;
	count += VARIATIC_OUTPUT;
	jitter_private_t *private = calloc(1, sizeof(*private));
	private->buffer = malloc((count + VARIATIC_INPUT) * size);
	private->bufferstart = private->buffer + (VARIATIC_OUTPUT * size);
	private->bufferend = private->buffer + (count * size);
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
	private->in = private->out = private->bufferstart;
	private->state = JITTER_FILLING;

	ctx->private = private;
	ctx->thredhold = 1;
	jitter_t *jitter = calloc(1, sizeof(*jitter));
	jitter->ctx = ctx;
	jitter->ops = jitter_ringbuffer;
	jitter->destroy = &jitter_ringbuffer_destroy;
	dbg("jitter %s create ring buffer %ld (%p - %p)", name, count * size, private->bufferstart, private->bufferend);

	return jitter;
}

static void jitter_ringbuffer_destroy(jitter_t *jitter)
{
	jitter_ctx_t *ctx = jitter->ctx;
	jitter_private_t *private = (jitter_private_t *)ctx->private;

	private->in = NULL;
	private->out = NULL;

	dbg("jitter %s destroy", ctx->name);
	jitter_reset(ctx);

	pthread_cond_destroy(&private->condpush);
	pthread_cond_destroy(&private->condpeer);
	pthread_mutex_destroy(&private->mutex);

	free(private->buffer);
	free(private);
	free(ctx);
	free(jitter);
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

	mlock(private->buffer, (ctx->count + VARIATIC_INPUT) * ctx->size);
}
#else
#define jitter_lock NULL
#endif

static unsigned char *jitter_pull(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	pthread_mutex_lock(&private->mutex);
	int state = private->state;
	while ((private->in != NULL) &&
		((private->level + jitter->size) > (jitter->size * jitter->count)))
	{
		if (private->state == JITTER_FLUSH)
			break;
		jitter_dbg(jitter, "pull block on %p (%d/%ld)", private->in, private->level, (jitter->size * jitter->count));
		pthread_cond_wait(&private->condpush, &private->mutex);
	}
	unsigned char *ret = NULL;
	if ((private->state == JITTER_RUNNING || private->state == JITTER_FILLING) &&
		((private->level + jitter->size) <= (jitter->size * jitter->count)))
	{
		ret = private->in;
	}
	else
	{
		jitter_dbg(jitter, "pull ring state: %d, level %d on %ld", private->state, private->level, jitter->count * jitter->size);
	}
	pthread_mutex_unlock(&private->mutex);
	return ret;
}

static void jitter_push(jitter_ctx_t *jitter, size_t len, void *beat)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	jitter_dbg(jitter, "push %p %ld, %p", private->in, len, private->in + len);
	pthread_mutex_lock(&private->mutex);
	private->level += len;
	private->in += len;
	if (len == 0)
	{
		jitter_dbg(jitter, "push 0");
		private->in = NULL;
	}
	if (private->in >= private->bufferend)
	{
		/**
		 *         |-| <------------------<  |-|
		 *    |----|----|----| ... |----|----|----|
		 *    |   start 1    2  count-2     end|  |
		 *    |                                in |
		 * variatic                           variatic
		 *   out                                 in
		 */
		len = VARIATIC_INPUT * (private->in - private->bufferend);
		memcpy(private->bufferstart, private->bufferend, len);
		private->in = private->bufferstart + len;
		jitter_dbg(jitter, "variatic move %ld",len);
	}
	pthread_mutex_unlock(&private->mutex);

	if (jitter->consume != NULL)
	{
		pthread_mutex_lock(&private->mutex);
		len = jitter->consume(jitter->consumer,
			private->out, len);
		jitter_dbg(jitter, "consume %ld", len);
		if (len > 0)
		{
			private->level -= len;
			if (private->in != NULL)
				private->in -= len;
			private->out = private->in;
		}
		else
		{
			private->state = JITTER_STOP;
		}
		pthread_mutex_unlock(&private->mutex);
	}
	if (private->state == JITTER_RUNNING)
	{
		pthread_cond_broadcast(&private->condpeer);
		jitter_dbg(jitter, "push A %ld/%d", len, private->level);
	}
	else if (private->state == JITTER_FILLING &&
			private->level >= (jitter->thredhold * jitter->size))
	{
		jitter_dbg(jitter, "push B %ld/%d", len, private->level);
		private->state = JITTER_RUNNING;
	}
	else
	{
		jitter_dbg(jitter, "push C %ld/%d", len, private->level);
	}
}

static unsigned char *jitter_peer(jitter_ctx_t *jitter, void **beat)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	pthread_mutex_lock(&private->mutex);
	/**
	 * The jitter is configurated to be use without input thread
	 * The producer push data in the same thread as the consumer
	 */
	if (jitter->produce != NULL)
	{
		/**
		 * chekck if there enought space into the buffer to push
		 * more data as least one block
		 */
		if 	(private->level <= ((jitter->count - 1) * jitter->size))
		{
			do
			{
				/**
				 * The producer must push enougth data to start
				 * the running, otherwise the peer will block.
				 */
				int len;
				len = jitter->produce(jitter->producter,
					private->in, jitter->size);
				/**
				 * To push the jitter has to be unlock
				 * like before to return
				 */
				pthread_mutex_unlock(&private->mutex);
				if (len > 0)
					jitter_push(jitter, len, NULL);
				else
				{
					return NULL;
				}
				pthread_mutex_lock(&private->mutex);
			} while (private->state == JITTER_FILLING);
		}
		else
		{
			private->state = JITTER_RUNNING;
		}
	}

	/**
	 * The variatic configuration allows to push and to pop
	 * a quantity of data different of the configurated block size.
	 */
	if ((private->out + jitter->size) > private->bufferend)
	{
		/**
		 *      |--| <------------------< |--|
		 *    |----|----|----| ... |----|----|----|
		 *    |   start 1    2  count-2   | end   |
		 *    |                          out      |
		 * variatic                           variatic
		 *   out                                 in
		 */
		int len = VARIATIC_OUTPUT * (private->bufferend - private->out);
		memcpy(private->bufferstart - len, private->out, len);
		private->out = private->bufferstart - len;
	}

	/**
	 * The checking of produce should be useless, but it's a secure addon
	 */
	while (((private->state == JITTER_FILLING) &&
			(private->in != NULL) &&
			(jitter->produce == NULL)) ||
			private->pause)
	{
		jitter_dbg(jitter, "peer block on %p %p %d", private->in, private->out + jitter->size, private->in <= private->out + jitter->size);
		pthread_cond_wait(&private->condpeer, &private->mutex);
	}

	if (private->state == JITTER_STOP)
	{
		private->out = NULL;
	}

	pthread_mutex_unlock(&private->mutex);
	jitter_dbg(jitter, "peer %p %d", private->out, private->level);
	return private->out;
}

static void jitter_pop(jitter_ctx_t *jitter, size_t len)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	if (len + 1 == 0)
		len = private->level;
	jitter_dbg(jitter, "pop start %p len %ld end %p, state %d",
							private->out, len, private->out + len,
							private->state);

	if (private->state == JITTER_FILLING)
	{
		warn("jitter pop during filling");
		return;
	}
	if (private->state == JITTER_STOP)
		return;

	pthread_mutex_lock(&private->mutex);
	private->out += len;
	private->level -= len;
	if (private->level <= jitter->size)
	{
		if (private->state == JITTER_RUNNING)
			private->state = JITTER_FILLING;
		else
			private->state = JITTER_STOP;
	}
	pthread_mutex_unlock(&private->mutex);

	jitter_dbg(jitter, "pop %ld/%d state %d", len, private->level, private->state);
	pthread_cond_broadcast(&private->condpush);
}

static void jitter_flush(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	if (private->state == JITTER_FLUSH)
		return;
	jitter_dbg(jitter, "flush");
	pthread_mutex_lock(&private->mutex);
	private->state = JITTER_FLUSH;
	pthread_mutex_unlock(&private->mutex);

	pthread_cond_broadcast(&private->condpush);
	pthread_cond_broadcast(&private->condpeer);
}

static size_t jitter_length(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	if (private->level < jitter->size)
		return private->level;
	return jitter->size;
}


static void jitter_reset(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	jitter_dbg(jitter, "reset");
	pthread_mutex_lock(&private->mutex);
	private->state = JITTER_STOP;
	pthread_mutex_unlock(&private->mutex);

	pthread_cond_broadcast(&private->condpush);
	pthread_cond_broadcast(&private->condpeer);

	pthread_mutex_lock(&private->mutex);
	private->in = private->bufferstart;
	private->out = private->bufferstart;
	private->level = 0;
	private->state = JITTER_FILLING;
	pthread_mutex_unlock(&private->mutex);
}

static int jitter_empty(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	return ((private->in > private->out) &&
		(private->out + jitter->size) > private->in);
}

static void jitter_pause(jitter_ctx_t *jitter, int enable)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	pthread_mutex_lock(&private->mutex);
	private->pause = enable;
	if ((private->state == JITTER_FLUSH) && !private->pause)
		private->state = JITTER_FILLING;
	pthread_mutex_unlock(&private->mutex);
	pthread_cond_broadcast(&private->condpeer);
}

static unsigned int jitter_nbchannels(jitter_ctx_t *jitter)
{
	return 1;
}

static const jitter_ops_t *jitter_ringbuffer = &(jitter_ops_t)
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

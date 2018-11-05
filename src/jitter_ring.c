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

#include <pthread.h>

#include "jitter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define jitter_dbg(...)

#define VARIATIC_OUTPUT 1
#define VARIATIC_INPUT 1

static unsigned char *jitter_pull(jitter_ctx_t *jitter);
static void jitter_push(jitter_ctx_t *jitter, size_t len, void *beat);
static unsigned char *jitter_peer(jitter_ctx_t *jitter);
static void jitter_pop(jitter_ctx_t *jitter, size_t len);
static void jitter_reset(jitter_ctx_t *jitter);

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

	ctx->private = private;
	jitter_t *jitter = calloc(1, sizeof(*jitter));
	jitter->ctx = ctx;
	jitter->ops = jitter_ringbuffer;
	dbg("jitter %s create ring buffer %ld (%p - %p)", name, count * size, private->bufferstart, private->bufferend);
	return jitter;
}

void jitter_ringbuffer_destroy(jitter_t *jitter)
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

static void _jitter_init(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	if (jitter->thredhold < 1)
		jitter->thredhold = 1;
	private->state = JITTER_FILLING;
}

static unsigned char *jitter_pull(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	pthread_mutex_lock(&private->mutex);
	if (private->state == JITTER_STOP)
		_jitter_init(jitter);

	while ((private->in != NULL) &&
		((private->level + jitter->size) > (jitter->size * jitter->count)))
	{
		jitter_dbg("jitter %s pull block on %p (%d/%d)", jitter->name, private->in, private->level, (jitter->size * jitter->count));
		pthread_cond_wait(&private->condpush, &private->mutex);
	}
	pthread_mutex_unlock(&private->mutex);
	return private->in;
}

static void jitter_push(jitter_ctx_t *jitter, size_t len, void *beat)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	jitter_dbg("jitter %s push %p %d, %p", jitter->name, private->in, len, private->in + len);
	pthread_mutex_lock(&private->mutex);
	private->level += len;
	private->in += len;
	if (len == 0)
	{
		jitter_dbg("jitter %s push 0", jitter->name);
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
		jitter_dbg("jitter variatic move %d",len);
	}
	pthread_mutex_unlock(&private->mutex);

	if (jitter->consume != NULL)
	{
		pthread_mutex_lock(&private->mutex);
		len = jitter->consume(jitter->consumer,
			private->out, len);
		if (len > 0)
		{
			private->out = private->in;
			private->level -= len;
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
		jitter_dbg("jitter %s push A %d/%d", jitter->name, len, private->level);
	}
	else if (private->state == JITTER_FILLING &&
			private->level >= (jitter->thredhold * jitter->size))
	{
		jitter_dbg("jitter %s push B %d/%d", jitter->name, len, private->level);
		private->state = JITTER_RUNNING;
	}
	else
	{
		jitter_dbg("jitter %s push C %d/%d", jitter->name, len, private->level);
	}
}

static unsigned char *jitter_peer(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;

	pthread_mutex_lock(&private->mutex);
	if (private->state == JITTER_STOP)
		_jitter_init(jitter);
	pthread_mutex_unlock(&private->mutex);
	if ((private->in <= (private->out + jitter->size)) &&
		(jitter->produce != NULL))
	{
		int len;
		pthread_mutex_lock(&private->mutex);
		len = jitter->produce(jitter->producter, 
			private->in, jitter->size);
		pthread_mutex_unlock(&private->mutex);
		if (len > 0)
			jitter_push(jitter, len, NULL);
		else
			return NULL;
		if (private->state == JITTER_FILLING)
		{
			dbg("jitter %s not enought data (%d/%ld)", jitter->name, private->level, jitter->size);
			return NULL;
		}
	}
	pthread_mutex_lock(&private->mutex);
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

	while (private->state == JITTER_FILLING && private->in != NULL)
	{
		dbg("jitter %s peer block on %p (%d/%ld)", jitter->name, private->out, private->level, jitter->size);
		pthread_cond_wait(&private->condpeer, &private->mutex);
	}

	/**
	 * this cas may be true if private->in is NULL
	 */
	if (private->level <= 0)
	{
		private->out = NULL;
	}

	pthread_mutex_unlock(&private->mutex);
	jitter_dbg("jitter %s peer %p %d", jitter->name, private->out, private->level);
	return private->out;
}

static void jitter_pop(jitter_ctx_t *jitter, size_t len)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	jitter_dbg("jitter %s pop %p %d, %p", jitter->name, private->out, len, private->out + len);

	if (private->state == JITTER_STOP)
		return;

	pthread_mutex_lock(&private->mutex);
	private->out += len;
	private->level -= len;
	pthread_mutex_unlock(&private->mutex);
	if (private->state == JITTER_RUNNING)
	{
		jitter_dbg("jitter %s pop A %d/%d", jitter->name, len, private->level);
		pthread_cond_broadcast(&private->condpush);
	}
	else
	{
		jitter_dbg("jitter %s pop B %d/%d %p %p", jitter->name, len, private->level, private->in, private->out);
	}
	if (private->level <= 0)
		private->state = JITTER_FILLING;
}

static void jitter_flush(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	private->state = JITTER_FLUSH;
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

	jitter_dbg("jitter %s reset", jitter->name);
	pthread_cond_broadcast(&private->condpush);
	pthread_cond_broadcast(&private->condpeer);
#if 0
	while (private->level > 0)
	{
		int previous = private->level;
		pthread_cond_broadcast(&private->condpeer);
		pthread_yield();
		if (private->level <= 0)
			break;
		pthread_mutex_lock(&private->mutex);
		int count = 0;
		while (private->level == previous)
		{
			pthread_cond_wait(&private->condpush, &private->mutex);
			if (count == 5)
			{
				private->level = 0;
				break;
			}
			count++;
		}
		pthread_mutex_unlock(&private->mutex);
	}
#endif

	pthread_mutex_lock(&private->mutex);
	private->in = private->bufferstart;
	private->out = private->bufferstart;
	private->level = 0;
	private->state = JITTER_STOP;
	pthread_mutex_unlock(&private->mutex);
}

static int jitter_empty(jitter_ctx_t *jitter)
{
	jitter_private_t *private = (jitter_private_t *)jitter->private;
	return ((private->in > private->out) &&
		(private->out + jitter->size) > private->in);
}

static const jitter_ops_t *jitter_ringbuffer = &(jitter_ops_t)
{
	.reset = jitter_reset,
	.pull = jitter_pull,
	.push = jitter_push,
	.peer = jitter_peer,
	.pop = jitter_pop,
	.flush = jitter_flush,
	.length = jitter_length,
	.empty = jitter_empty,
};

/*****************************************************************************
 * jitter_common.c
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
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "jitter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

extern jitter_t *jitter_scattergather_init(const char *name, unsigned count, size_t size);
extern jitter_t *jitter_ringbuffer_init(const char *name, unsigned count, size_t size);

#define MAXJITTERS 10
static jitter_t *_jitters[MAXJITTERS] = {0};
int __jitter_dbg__ = -1;

static pthread_mutex_t jitter_lock = PTHREAD_MUTEX_INITIALIZER;;

jitter_t *jitter_init(int type, const char *name, unsigned count, size_t size)
{
	jitter_t *jitter = NULL;
	int id = 0;

	pthread_mutex_lock(&jitter_lock);
	jitter_t *jit = _jitters[id];
	while (jit != NULL && id < MAXJITTERS)
		jit = _jitters[++id];
	if (id == MAXJITTERS)
		return NULL;
	if (!strcmp(name, JITTER_DBG))
	{
		__jitter_dbg__ = id;
		warn("jitter debug %s on %d", name, id);
	}

	if (type == JITTER_TYPE_SG)
		jitter = jitter_scattergather_init(name, count, size);
	else if (type == JITTER_TYPE_RING)
		jitter = jitter_ringbuffer_init(name, count, size);
	if (jitter != NULL)
		jitter->ctx->id = id;
	_jitters[id] = jitter;
	pthread_mutex_unlock(&jitter_lock);
	return jitter;
}

void jitter_destroy(jitter_t *jitter)
{
	int id = jitter->ctx->id;
	jitter->destroy(jitter);
	_jitters[id] = NULL;
}

/*****************************************************************************
 * sink_file.c
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
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "player.h"
typedef struct sink_s sink_t;
typedef struct sink_ctx_s sink_ctx_t;
struct sink_ctx_s
{
	const sink_t *ops;
	int fd;
	player_ctx_t *ctx;
	jitter_t *in;
};
#define SINK_CTX
#include "sink.h"
#include "jitter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif
#define sink_dbg(...)

#define BUFFERSIZE ENCODER_FRAME_SIZE

static const char *jitter_name = "file output";

static int sink_write(sink_ctx_t *ctx, unsigned char *buff, int len)
{
	int ret;
	ret = write(ctx->fd, buff, len);
	sink_dbg("sink: write %d", ret);
	if (ret < 0)
		err("sink file %d error: %s", ctx->fd, strerror(errno));
	if (ret == 0)
		dbg("sink: end of file %d", len);
	return ret;
}

static sink_ctx_t *sink_init(player_ctx_t *ctx, const char *path)
{
	int fd;
	if (!strcmp(path, "-"))
		fd = 1;
	else
		fd = open(path, O_RDWR | O_CREAT, 0644);

	if (fd >= 0)
	{
		sink_ctx_t *sink = calloc(1, sizeof(*sink));
		sink->ops = sink_file;
		sink->fd = fd;
		sink->ctx = ctx;

		//jitter_t *jitter = jitter_ringbuffer_init(jitter_name, 3, BUFFERSIZE);
		jitter_t *jitter = jitter_scattergather_init(jitter_name, 3, BUFFERSIZE);
		dbg("sink: add consumer to %s", jitter->ctx->name);
		jitter->ctx->consume = (consume_t)sink_write;
		jitter->ctx->consumer = (void *)sink;
		sink->in = jitter;
		return sink;
	}
	err("sink file error: %s", strerror(errno));
	return NULL;
}

static int sink_run(sink_ctx_t *sink)
{
	return 0;
}

static jitter_t *sink_jitter(sink_ctx_t *sink)
{
	return sink->in;
}

static void sink_destroy(sink_ctx_t *sink)
{
	//jitter_ringbuffer_destroy(sink->in);
	jitter_scattergather_destroy(sink->in);
	close(sink->fd);
	free(sink);
}

const sink_t *sink_file = &(sink_t)
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

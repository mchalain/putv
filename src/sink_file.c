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
#include "encoder.h"
typedef struct sink_s sink_t;
typedef struct sink_ctx_s sink_ctx_t;
struct sink_ctx_s
{
	player_ctx_t *player;
	jitter_t *in;
	event_listener_t *listener;

	const encoder_ops_t *encoder;
	int fd;
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

static sink_ctx_t *sink_init(player_ctx_t *player, const char *path)
{
	int fd;
	if (!strncmp(path, "file://", 7))
		path += 7;
	if (!strcmp(path, "-"))
		fd = 1;
	else
		fd = open(path, O_RDWR | O_CREAT, 0644);

	if (fd >= 0)
	{
		sink_ctx_t *ctx = calloc(1, sizeof(*ctx));
		ctx->fd = fd;
		ctx->player = player;
		ctx->encoder = encoder_check(path);

		jitter_t *jitter = jitter_init(JITTER_TYPE_RING, jitter_name, 1, BUFFERSIZE);
		dbg("sink: add consumer to %s", jitter->ctx->name);
		jitter->ctx->consume = (consume_t)sink_write;
		jitter->ctx->consumer = (void *)ctx;
		jitter->format = SINK_BITSSTREAM;
		ctx->in = jitter;

		return ctx;
	}
	err("sink file error: %s", strerror(errno));
	return NULL;
}

static int sink_run(sink_ctx_t *sink)
{
	return 0;
}

static jitter_t *sink_jitter(sink_ctx_t *ctx, unsigned int index)
{
	if (index == 0)
		return ctx->in;
	return NULL;
}

static unsigned int sink_attach(sink_ctx_t *ctx, encoder_t *encoder)
{
	return 0;
}

static const encoder_ops_t *sink_encoder(sink_ctx_t *ctx)
{
	return ctx->encoder;
}

static void sink_destroy(sink_ctx_t *sink)
{
	jitter_destroy(sink->in);
	close(sink->fd);
	free(sink);
}

const sink_ops_t *sink_file = &(sink_ops_t)
{
	.name = "file",
	.default_ = "file:///tmp/default.wav",
	.init = sink_init,
	.jitter = sink_jitter,
	.attach = sink_attach,
	.encoder = sink_encoder,
	.run = sink_run,
	.destroy = sink_destroy,
};

/*****************************************************************************
 * mux_passthrough.c
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

#include "player.h"
#include "encoder.h"
#include "event.h"
typedef struct mux_s mux_t;
typedef struct mux_ops_s mux_ops_t;
typedef struct mux_ctx_s mux_ctx_t;
struct mux_ctx_s
{
	player_ctx_t *ctx;
	jitter_t *inout;
	const char *mime;
};
#define MUX_CTX
#include "mux.h"
#include "media.h"
#include "jitter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static mux_ctx_t *mux_init(player_ctx_t *player, const char *unused)
{
	mux_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->mime = mime_octetstream;
	return ctx;
}

static jitter_t *mux_jitter(mux_ctx_t *ctx, unsigned int index)
{
	if (index > 0)
		return NULL;
	return ctx->inout;
}

static int mux_run(mux_ctx_t *ctx, jitter_t *sink_jitter)
{
	ctx->inout = sink_jitter;
	ctx->mime = utils_format2mime(sink_jitter->format);
	return 0;
}

static unsigned int mux_attach(mux_ctx_t *ctx, encoder_t *encoder)
{
	ctx->mime = encoder->ops->mime(encoder->ctx);
	return 0;
}

static const char *mux_mime(mux_ctx_t *ctx, unsigned int index)
{
	if (index > 0)
		return NULL;
	if (ctx->mime)
		return ctx->mime;
#ifdef DECODER_MAD
	return mime_audiomp3;
#elif defined(DECODER_FLAC)
	return mime_audioflac;
#else
	return mime_audiopcm;
#endif
}

static void mux_destroy(mux_ctx_t *ctx)
{
	free(ctx);
}

const mux_ops_t *mux_passthrough = &(mux_ops_t)
{
	.init = mux_init,
	.jitter = mux_jitter,
	.run = mux_run,
	.attach = mux_attach,
	.mime = mux_mime,
	.protocol = "udp",
	.destroy = mux_destroy,
};

/*****************************************************************************
 * decoder_passthrough.c
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
typedef struct decoder_s decoder_t;
typedef struct decoder_ops_s decoder_ops_t;
typedef struct decoder_ctx_s decoder_ctx_t;
struct decoder_ctx_s
{
	const decoder_ops_t *ops;
	jitter_t *inout;
};
#define DECODER_CTX
#include "decoder.h"
#include "media.h"
#include "jitter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static decoder_ctx_t *_decoder_init(player_ctx_t *player)
{
	decoder_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ops = decoder_passthrough;

	return ctx;
}

static jitter_t *_decoder_jitter(decoder_ctx_t *ctx, jitte_t jitte)
{
	return ctx->inout;
}

static int _decoder_run(decoder_ctx_t *ctx, jitter_t *jitter)
{
	ctx->inout = jitter;
	return 0;
}

static int _decoder_checkin(decoder_ctx_t *ctx, const char *path)
{
	if (!strncmp(path, "pcm://", 6))
		return 1;
	if (!strncmp(path, "alsa://", 6))
		return 1;
	char *ext = strrchr(path, '.');
	if (ext && !strcmp(ext, ".wav"))
		return 1;
	if (ext && !strcmp(ext, ".pcm"))
		return 1;
	return 0;
}

static const char *_decoder_mime(decoder_ctx_t *ctx)
{
	return mime_audiopcm;
}

static void _decoder_destroy(decoder_ctx_t *ctx)
{
	free(ctx);
}

const decoder_ops_t *decoder_passthrough = &(decoder_ops_t)
{
	.name = "passthrough",
	.checkin = _decoder_checkin,
	.init = _decoder_init,
	.jitter = _decoder_jitter,
	.run = _decoder_run,
	.mime = _decoder_mime,
	.destroy = _decoder_destroy,
};

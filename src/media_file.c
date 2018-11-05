/*****************************************************************************
 * media_file.c
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
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include "player.h"
#include "media.h"
#include "decoder.h"

struct media_ctx_s
{
	const char *url;
	int mediaid;
	unsigned int options;
};

#define OPTION_LOOP 0x0001
#define OPTION_RANDOM 0x0002

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static int media_count(media_ctx_t *ctx);
static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *data);
static int media_current(media_ctx_t *ctx, media_parse_t cb, void *data);
static int media_play(media_ctx_t *ctx, media_parse_t play, void *data);
static int media_next(media_ctx_t *ctx);
static int media_end(media_ctx_t *ctx);

static const char *utils_getmime(const char *path)
{
#ifdef DECODER_MAD
	if (!decoder_mad->check(path))
		return decoder_mad->mime;
#endif
#ifdef DECODER_FLAC
	if (!decoder_flac->check(path))
		return decoder_flac->mime;
#endif
	return mime_octetstream;
}

static int media_count(media_ctx_t *ctx)
{
	return 1;
}

static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	ctx->url = path;
	return 0;
}

static int media_remove(media_ctx_t *ctx, int id, const char *path)
{
	return -1;
}

static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *data)
{
	if (cb != NULL && cb(data, ctx->url, NULL, utils_getmime(ctx->url)) < 0)
		return -1;
	return 1;
}

static int media_current(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	return media_find(ctx, ctx->mediaid, cb, data);
}

static int media_list(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	return media_find(ctx, -1, cb, data);
}

static int media_play(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	int ret = -1;

	if (ctx->mediaid == 1 && ctx->url != NULL)
	{
		ret = cb(data, ctx->url, NULL, utils_getmime(ctx->url));
	}
	return ctx->mediaid;
}

static int media_next(media_ctx_t *ctx)
{
	if ((ctx->mediaid == 1) && !(ctx->options & OPTION_LOOP))
		media_end(ctx);
	else
		ctx->mediaid = 1;
	return ctx->mediaid;
}

static int media_end(media_ctx_t *ctx)
{
	ctx->mediaid = -1;
	return 0;
}

/**
 * the loop requires to restart the player.
 */
static void media_loop(media_ctx_t *ctx, int enable)
{
	if (enable)
		ctx->options |= OPTION_LOOP;
	else
		ctx->options &= ~OPTION_LOOP;
}

static void media_random(media_ctx_t *ctx, int enable)
{
}

static int media_options(media_ctx_t *ctx, media_options_t option, int enable)
{
	int ret = 0;
	if (option == MEDIA_LOOP)
	{
		media_loop(ctx, enable);
		ret = (ctx->options & OPTION_LOOP) == OPTION_LOOP;
	}
	else if (option == MEDIA_RANDOM)
	{
		ret = 0;
	}
	return ret;
}

static media_ctx_t *media_init(const char *url)
{
	media_ctx_t *ctx = NULL;
	if (url)
	{
		ctx = calloc(1, sizeof(*ctx));
		ctx->mediaid = 0;
		ctx->url = url;
	}
	return ctx;
}

static void media_destroy(media_ctx_t *ctx)
{
	free(ctx);
}

media_ops_t *media_file = &(media_ops_t)
{
	.init = media_init,
	.destroy = media_destroy,
	.next = media_next,
	.play = media_play,
	.list = media_list,
	.current = media_current,
	.find = media_find,
	.remove = media_remove,
	.insert = media_insert,
	.count = media_count,
	.end = media_end,
	.options = media_options,
};

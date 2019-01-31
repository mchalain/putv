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

typedef struct media_url_s media_url_t;

struct media_ctx_s
{
	media_url_t *media;
	media_url_t *current;
	unsigned int options;
};

struct media_url_s
{
	char *url;
	char *info;
	const char *mime;
	int id;
	media_url_t *next;
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
static int media_play(media_ctx_t *ctx, media_parse_t play, void *data);
static int media_next(media_ctx_t *ctx);
static int media_end(media_ctx_t *ctx);

static int media_count(media_ctx_t *ctx)
{
	return 1;
}

static media_url_t *_media_find(media_ctx_t *ctx, int id)
{
	media_url_t *media = ctx->media;
	int i = 0;
	while (i < id && media != NULL)
	{
		media = media->next;
		i++;
	}
	return media;
}

static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	int id = 0;
	media_url_t *media = ctx->media;
	if (media == NULL)
	{
		media = calloc(1, sizeof(*media));
		ctx->media = media;
	}
	else
	{
		while (media->next != NULL)
		{
			media = media->next;
			id ++;
		}
		media->next = calloc(1, sizeof(*media));
		media = media->next;
		id++;
	}
	media->url = strdup(path);
	if (info)
		media->info = strdup(info);
	if (mime)
		media->mime = mime;
	else
		media->mime = utils_getmime(path);
	media->id = id;
	return 0;
}

static int media_remove(media_ctx_t *ctx, int id, const char *path)
{
	media_url_t *media = _media_find(ctx, id);
	if (media != NULL)
	{
		free(media->url);
		free(media->info);
	}
	return 0;
}

static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *data)
{
	media_url_t *media = NULL;
	if (id != -1)
	{
		media = _media_find(ctx, id);
		if (media == NULL || (cb != NULL && cb(data, media->id, media->url, media->info, media->mime) < 0))
			return -1;
	}
	else
	{
		media_url_t *media = ctx->media;
		while (media != NULL)
		{
			if (cb != NULL && cb(data, media->id, media->url, media->info, media->mime) < 0)
				return -1;
			media = media->next;
		}
	}
	return 1;
}

static int media_list(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	return media_find(ctx, -1, cb, data);
}

static int media_play(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	int ret = -1;
	if (ctx->current != NULL)
	{
		ret = cb(data, ctx->current->id, ctx->current->url, ctx->current->info, ctx->current->mime);
		if (ret > -1)
			ret = ctx->current->id;
	}
	return ret;
}

static int media_next(media_ctx_t *ctx)
{
	int ret = -1;
	if (ctx->current != NULL)
		ctx->current = ctx->current->next;
	else
		ctx->current = ctx->media;
	if ((ctx->current == NULL) && (ctx->options & OPTION_LOOP))
	{
		dbg("media loop");
		ctx->current = ctx->media;
	}

	if (ctx->current != NULL)
		ret = ctx->current->id;
	return ret;
}

static int media_end(media_ctx_t *ctx)
{
	ctx->current = NULL;
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

static media_ctx_t *media_init(const char *url,...)
{
	media_ctx_t *ctx = NULL;
	if (url)
	{
		ctx = calloc(1, sizeof(*ctx));
		media_insert(ctx, url, NULL, utils_getmime(url));
	}
	return ctx;
}

static void media_destroy(media_ctx_t *ctx)
{
	media_url_t *media = ctx->media;
	while (media != NULL)
	{
		media_url_t *tofree = media;
		media = media->next;
		free(tofree);
	}
	free(ctx);
}

const media_ops_t *media_file = &(const media_ops_t)
{
	.init = media_init,
	.destroy = media_destroy,
	.next = media_next,
	.play = media_play,
	.list = media_list,
	.find = media_find,
	.remove = media_remove,
	.insert = media_insert,
	.count = media_count,
	.end = media_end,
	.options = media_options,
};

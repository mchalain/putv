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
	unsigned int lastid;
};

struct media_url_s
{
	char *url;
	char *info;
	const char *mime;
	unsigned int id;
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

#define media_dbg(...)

static int media_count(media_ctx_t *ctx);
static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *data);
static int media_play(media_ctx_t *ctx, int id, media_parse_t play, void *data);
static int media_next(media_ctx_t *ctx);
static int media_end(media_ctx_t *ctx);
static option_state_t media_loop(media_ctx_t *ctx, option_state_t enable);
static option_state_t media_random(media_ctx_t *ctx, option_state_t enable);

static const char str_mediafile[] = "url list";

static int media_count(media_ctx_t *ctx)
{
	media_url_t *media = ctx->media;
	int i = 0;
	while (media != NULL)
	{
		media = media->next;
		i++;
	}
	return i;
}

#ifdef MEDIA_FILE_LIST
static media_url_t *_media_find(media_ctx_t *ctx, int id)
{
	media_url_t *media = ctx->media;
	while (media != NULL)
	{
		if (media->id == id)
			break;
		media = media->next;
	}
	return media;
}

static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	int id = ctx->lastid;;
	media_url_t *media = ctx->media;

	media = calloc(1, sizeof(*media));

	if (mime)
		media->mime = mime;
	else
		media->mime = utils_getmime(path);

	if (!strcmp(media->mime, mime_directory))
	{
		err("media: try to insert directory into file media");
		free(media);
		return -1;
	}
	if (ctx->media != NULL)
		media->id = id + 1;
	ctx->lastid = media->id;

	media->next = ctx->media;
	ctx->media = media;

	media->url = strdup(path);
	if (info)
		media->info = strdup(info);
	else
		media->info = media_fillinfo(path, mime);
	return id;
}

static int media_append(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	int id = ctx->lastid;;
	media_url_t *media = ctx->media;

	if (media == NULL)
	{
		media = calloc(1, sizeof(*media));
		ctx->media = media;
		media->id = 0;
	}
	else
	{
		while (media->next != NULL) media = media->next;

		media->next = calloc(1, sizeof(*media));
		media = media->next;
		media->id = id + 1;
	}

	ctx->lastid = media->id;

	media->url = strdup(path);
	if (info)
		media->info = strdup(info);
	else
		media->info = media_fillinfo(path, mime);
	if (mime)
		media->mime = mime;
	else
		media->mime = utils_getmime(path);
	return id;
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
#else
static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *data)
{
	media_url_t *media = ctx->media;
	if (cb != NULL && cb(data, media->id, media->url, media->info, media->mime) < 0)
		return -1;
	return 1;
}
#endif

static int media_next(media_ctx_t *ctx)
{
	int ret = -1;
	if (ctx->current != NULL)
	{
		ctx->current = ctx->current->next;
		/**
		 * if the list is available the next may be not null.
		 * stop or replay for the last entry, depends of the LOOP
		 * option
		 */
		if ((ctx->current == NULL) &&
			(ctx->options & OPTION_LOOP))
		{
			media_dbg("media loop");
			ctx->current = ctx->media;
		}
	}
	else
	{
		/**
		 * first call to the next function
		 * has to set the current with the first
		 */
		ctx->current = ctx->media;
	}

	if (ctx->current != NULL)
		ret = ctx->current->id;
	return ret;
}

static int media_play(media_ctx_t *ctx, int id, media_parse_t cb, void *data)
{
	int ret = -1;

	/**
	 * We have to accept that ctx_>current->next == NULL
	 * otherwise we manage the loop.
	 */
	if (ctx->current != NULL && cb != NULL)
		ret = media_find(ctx, id, cb, data);
	if (ret > -1)
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
static option_state_t media_loop(media_ctx_t *ctx, option_state_t enable)
{
	if (enable == OPTION_ENABLE)
		ctx->options |= OPTION_LOOP;
	else if (enable == OPTION_DISABLE)
		ctx->options &= ~OPTION_LOOP;
	return (ctx->options & OPTION_LOOP)? OPTION_ENABLE: OPTION_DISABLE;
}

static option_state_t media_random(media_ctx_t *ctx, option_state_t enable)
{
	return OPTION_DISABLE;
}

static media_ctx_t *media_init(player_ctx_t *player, const char *arg,...)
{
	media_ctx_t *ctx = NULL;
	const char *mime = mime_octetstream;
	const char *protocol = NULL;
	const char *host = NULL;
	const char *port = NULL;
	const char *path = NULL;
	const char *search = NULL;
	void *data = utils_parseurl(arg, &protocol, &host, &port, &path, &search);
	if (path)
	{
		mime = utils_getmime(path);
	}
	if (protocol && mime == mime_octetstream)
	{
		mime = utils_getmime(protocol);
	}
	if (mime == mime_octetstream && search)
	{
		const char *string = NULL;
		string = strstr(search, "mime=");
		if (string)
		{
			string += 5;
			mime = utils_mime2mime(string);
		}
	}
	ctx = calloc(1, sizeof(*ctx));
#ifdef MEDIA_FILE_LIST
	if (media_insert(ctx, arg, NULL, mime) < 0)
	{
		free(ctx);
		ctx = NULL;
	}
	else
#else
	media_url_t *media;
	media = calloc(1, sizeof(*media));
	media->url = strdup(arg);
	media->info = media_fillinfo(path, mime);
	media->mime = mime;
	media->id = 0;
	ctx->media = media;
#endif
	warn("media: url %s", arg);

	free(data);
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
	.name = str_mediafile,
	.init = media_init,
	.destroy = media_destroy,
	.play = media_play,
	.next = media_next,
#ifdef MEDIA_FILE_LIST
	.list = media_list,
	.remove = media_remove,
	.insert = media_insert,
	.append = media_append,
#else
	.list = NULL,
	.remove = NULL,
	.insert = NULL,
#endif
	.find = media_find,
	.count = media_count,
	.end = media_end,
	.random = NULL,
	.loop = media_loop,
};

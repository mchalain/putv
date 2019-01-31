/*****************************************************************************
 * player.c
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
#include <stdio.h>
#include <unistd.h>

#define __USE_GNU
#include <pthread.h>

#include "player.h"
#include "media.h"
#include "jitter.h"
#include "src.h"
#include "decoder.h"
#include "encoder.h"
#include "sink.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

typedef struct player_decoder_s player_decoder_t;

struct player_decoder_s
{
	decoder_t *decoder;
	const src_t *src;
	int mediaid;
};

typedef struct player_event_s player_event_t;
struct player_event_s
{
	player_event_type_t type;
	player_event_cb_t cb;
	void *ctx;
	player_event_t *next;
};

struct player_ctx_s
{
	media_t *media;
	media_t *nextmedia;
	state_t state;
	player_event_t *events;
	player_decoder_t *current;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
};

player_ctx_t *player_init()
{
	player_ctx_t *ctx = calloc(1, sizeof(*ctx));
	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->cond, NULL);
	ctx->state = STATE_STOP;
	return ctx;
}

int player_change(player_ctx_t *ctx, const char *mediapath, int random, int loop)
{
	media_t *media = media_build(mediapath);
	if (media)
	{
		ctx->nextmedia = media;
		ctx->state != STATE_STOP;
		if (loop)
		{
			media->ops->options(media->ctx, MEDIA_LOOP, 1);
		}

		if (random)
		{
			media->ops->options(media->ctx, MEDIA_RANDOM, 1);
		}
		if (ctx->media == NULL)
			ctx->media = media;
	}
	return 0;
}

void player_next(player_ctx_t *ctx)
{
	if (ctx->media != NULL)
	{
		ctx->media->ops->next(ctx->media->ctx);
		player_state(ctx, STATE_CHANGE);
	}
}

media_t *player_media(player_ctx_t *ctx)
{
	return ctx->media;
}
	
void player_destroy(player_ctx_t *ctx)
{
	player_state(ctx, STATE_ERROR);
	pthread_yield();
	pthread_cond_destroy(&ctx->cond);
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
}

void player_onchange(player_ctx_t *ctx, player_event_cb_t callback, void *cbctx)
{
	player_event_t *event = calloc(1, sizeof(*event));
	event->cb = callback;
	event->ctx = cbctx;
	event->type = EVENT_ONCHANGE;
	event->next = ctx->events;
	ctx->events = event; 
}

state_t player_state(player_ctx_t *ctx, state_t state)
{
	if ((state != STATE_UNKNOWN) && ctx->state != state)
	{
		ctx->state = state;
		pthread_cond_broadcast(&ctx->cond);
		if (state == STATE_PAUSE)
		{
			player_event_t *it = ctx->events;
			while (it != NULL)
			{
				it->cb(it->ctx, ctx, ctx->state);
				it = it->next;
			}
		}
	}
	
	return ctx->state;
}

int player_mediaid(player_ctx_t *ctx)
{
	if (ctx->current != NULL)
	{
		return ctx->current->mediaid;
	}
	return -1;
}

int player_waiton(player_ctx_t *ctx, int state)
{
	if (ctx->state == STATE_STOP ||
		ctx->state == STATE_CHANGE ||
		ctx->state == STATE_ERROR)
		return -1;
	pthread_mutex_lock(&ctx->mutex);
	while (ctx->state == state)
	{
		pthread_cond_wait(&ctx->cond, &ctx->mutex);
	}
	pthread_mutex_unlock(&ctx->mutex);
	return 0;
}

struct _player_play_s
{
	player_ctx_t *ctx;
	player_decoder_t *dec;
};

static int _player_play(void* arg, int id, const char *url, const char *info, const char *mime)
{
	struct _player_play_s *data = (struct _player_play_s *)arg;
	player_ctx_t *player = data->ctx;
	src_t *src = NULL;

	dbg("player: prepare %d %s", id, url);
	src = src_build(player, url, mime);
	if (src != NULL)
	{
		data->dec = calloc(1, sizeof(*data->dec));
		data->dec->mediaid = id;
		data->dec->src = src;
		data->dec->decoder = src->audio[0];
		return 0;
	}
	else
	{
		dbg("player: src not found for %s", url);
		data->dec = NULL;
	}
	return -1;
}

int player_run(player_ctx_t *ctx, jitter_t *encoder_jitter)
{
	struct _player_play_s player =
	{
		.ctx = ctx,
		.dec = NULL,
	};
	while (ctx->state != STATE_ERROR)
	{
		pthread_mutex_lock(&ctx->mutex);
		while (ctx->state == STATE_STOP)
		{
			dbg("player: stop");
			encoder_jitter->ops->reset(encoder_jitter->ctx);
			pthread_cond_wait(&ctx->cond, &ctx->mutex);
		}
		pthread_mutex_unlock(&ctx->mutex);
		if (ctx->media != ctx->nextmedia)
		{
			if (ctx->media)
			{
				ctx->media->ops->destroy(ctx->media->ctx);
				free(ctx->media);
			}
			ctx->media = ctx->nextmedia;
		}
		if (ctx->media == NULL)
			break;
		ctx->media->ops->next(ctx->media->ctx);

		do
		{
			player.dec = NULL;
			ctx->media->ops->play(ctx->media->ctx, _player_play, &player);
			if (ctx->current != NULL)
			{
				dbg("player: wait");
				ctx->current->decoder->ops->destroy(ctx->current->decoder->ctx);
				ctx->current->src->ops->destroy(ctx->current->src->ctx);
				free(ctx->current);
				ctx->current = NULL;
			}
			if (ctx->state != STATE_STOP && player.dec != NULL)
			{
				ctx->current = player.dec;
				dbg("player: play");
				ctx->state = STATE_PLAY;
				ctx->current->decoder->ops->run(ctx->current->decoder->ctx, encoder_jitter);
				ctx->current->src->ops->run(ctx->current->src->ctx, ctx->current->decoder->ops->jitter(ctx->current->decoder->ctx));
				ctx->media->ops->next(ctx->media->ctx);
			}
			else {
				if (player.dec != NULL)
				{
					ctx->current = player.dec;
					ctx->current->decoder->ops->destroy(ctx->current->decoder->ctx);
					ctx->current->src->ops->destroy(ctx->current->src->ctx);
					free(ctx->current);
					ctx->current = NULL;
				}
				player_state(ctx, STATE_STOP);
			}
			player_event_t *it = ctx->events;
			while (it != NULL)
			{
				it->cb(it->ctx, ctx, ctx->state);
				it = it->next;
			}
		} while (ctx->state != STATE_STOP);
		ctx->media->ops->end(ctx->media->ctx);
	}
	return 0;
}

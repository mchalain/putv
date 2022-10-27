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
#include "filter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define player_dbg dbg

static void _player_autonext(void *arg, event_t event, void *eventarg);

struct player_ctx_s
{
	const char *filtername;

	media_t *media;
	state_t state;
	event_listener_t *listeners;

	src_t *src;
	src_t *nextsrc;

	pthread_cond_t cond;
	pthread_cond_t cond_int;
	pthread_mutex_t mutex;

	jitter_t *outstream[MAX_ESTREAM];
	int noutstreams;

};

player_ctx_t *player_init(const char *filtername)
{
	player_ctx_t *ctx = calloc(1, sizeof(*ctx));
	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->cond, NULL);
	pthread_cond_init(&ctx->cond_int, NULL);
	ctx->state = STATE_STOP;
	ctx->filtername = filtername;
	return ctx;
}

int player_change(player_ctx_t *ctx, const char *mediapath, int random, int loop, int now)
{
	media_t *media = NULL;
	if (mediapath != NULL)
	{
		media = media_build(ctx, mediapath);
	}
	if (media)
	{
		dbg("player: change media %s", media->ops->name);

		pthread_mutex_lock(&ctx->mutex);
		if (ctx->media)
		{
			ctx->media->ops->destroy(ctx->media->ctx);
			free(ctx->media);
		}
		ctx->media = media;
		pthread_mutex_unlock(&ctx->mutex);

		event_player_state_t event = {
				.playerctx = ctx,
				.state = ctx->state,
				.mediapath = mediapath,
		};
		player_sendevent(ctx, PLAYER_EVENT_CHANGE, &event);
	}
	if (ctx->media)
	{
		if (ctx->media->ops->loop && loop != -1)
		{
			ctx->media->ops->loop(ctx->media->ctx, loop);
		}
		if (ctx->media->ops->random && random != -1)
		{
			ctx->media->ops->random(ctx->media->ctx, random);
		}

		/**
		 * If stoped the next opus is ready with the new media
		 * Otherwise the next opsu comes from the previous media
		 */
		if (ctx->state != STATE_STOP)
		{
			int id = 0;
			if (ctx->media && ctx->media->ops->next)
			{
				id = ctx->media->ops->next(ctx->media->ctx);
			}
			player_play(ctx, id);
		}
		if (now)
		{
			player_state(ctx, STATE_CHANGE);
		}
	}
	return 0;
}

int player_next(player_ctx_t *ctx, int change)
{
	int nextid = -1;
	if (ctx->nextsrc != NULL)
		nextid = ctx->nextsrc->mediaid;
	if (ctx->media != NULL && change)
	{
		/**
		 * This allows to start the player with next command
		 */
		if (ctx->state == STATE_STOP)
			player_state(ctx, STATE_PLAY);
		else
		/**
		 * next command just request the main loop to complete
		 * the current entry to jump to the next one.
		 * The main loop will set the next one long time before
		 * than somebody request the jump to the next one.
		 */
			player_state(ctx, STATE_CHANGE);
	}
	return nextid;
}

media_t *player_media(player_ctx_t *ctx)
{
	media_t *media;
	pthread_mutex_lock(&ctx->mutex);
	media = ctx->media;
	pthread_mutex_unlock(&ctx->mutex);
	return media;
}

void player_destroy(player_ctx_t *ctx)
{
	player_state(ctx, STATE_ERROR);
	sched_yield();
	pthread_cond_destroy(&ctx->cond);
	pthread_cond_destroy(&ctx->cond_int);
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
}

void player_removeevent(player_ctx_t *ctx, int id)
{
	event_listener_t *listener = ctx->listeners;
	if (listener->id == id)
	{
		ctx->listeners = listener->next;
		free(listener);
		return;
	}
	while (listener != NULL)
	{
		event_listener_t *next = listener->next;
		if (next != NULL && next->id == id)
		{
			listener->next = next->next;
			free(next);
			return;
		}
		listener = listener->next;
	}
}

int player_eventlistener(player_ctx_t *ctx, event_listener_cb_t callback, void *cbctx, char *name)
{
	event_listener_t *listener = calloc(1, sizeof(*listener));
	if (ctx->listeners != NULL)
		listener->id = ctx->listeners->id + 1;
	listener->cb = callback;
	listener->arg = cbctx;
	listener->name = name;
	listener->next = ctx->listeners;
	ctx->listeners = listener;
	return listener->id;
}

int player_mediaid(player_ctx_t *ctx)
{
	if (ctx->src != NULL)
	{
		return ctx->src->mediaid;
	}
	return -1;
}

state_t player_state(player_ctx_t *ctx, state_t state)
{
	if ((state != STATE_UNKNOWN) && ctx->state != state)
	{
		if (pthread_mutex_trylock(&ctx->mutex) != 0)
		{
			player_dbg("player: change state trylock caller %p", __builtin_return_address(0));
			return ctx->state;
		}

		player_dbg("player: change state %d => %d",ctx->state, state);
		ctx->state = state;
		pthread_mutex_unlock(&ctx->mutex);
		pthread_cond_broadcast(&ctx->cond_int);
		sched_yield();
	}
	return ctx->state;
}

int player_waiton(player_ctx_t *ctx, int state)
{
	if (ctx->state == STATE_ERROR)
		return -1;
	if (ctx->state != state && state != STATE_UNKNOWN)
		return 0;
	pthread_mutex_lock(&ctx->mutex);
	do
	{
		dbg("player: waiton %d", state);
		pthread_cond_wait(&ctx->cond, &ctx->mutex);
	}
	while (ctx->state == state);
	pthread_mutex_unlock(&ctx->mutex);
	return 1;
}

static void _player_new_es(player_ctx_t *ctx, void *eventarg)
{
	event_new_es_t *event_data = (event_new_es_t *)eventarg;
	const src_t *src = event_data->src;
	warn("player: decoder build");
	decoder_t *decoder;
	decoder = decoder_build(ctx, event_data->mime);
	if (decoder == NULL)
		err("player: decoder not found for %s", event_data->mime);
	else
	{
		event_data->decoder = decoder;
		src->ops->attach(src->ctx, event_data->pid, event_data->decoder);
		jitter_t *outstream = NULL;
		int i;
		for ( i = 0; i < ctx->noutstreams; i++)
		{
			outstream = ctx->outstream[i];
			if (outstream->format & JITTER_AUDIO)
				break;
		}
		filter_t *filter = NULL;
		if (i < ctx->noutstreams)
			filter = filter_build(ctx->filtername, outstream, src->info);
		decoder->filter = filter;
		if (decoder->ops->prepare)
		{
			decoder->ops->prepare(decoder->ctx, filter, src->info);
		}
	}
}

static void _player_decode_es(player_ctx_t *ctx, void *eventarg)
{
	event_decode_es_t *event_data = (event_decode_es_t *)eventarg;
	if (event_data->decoder != NULL && ctx->noutstreams < MAX_ESTREAM)
	{
		int i;
		for ( i = 0; i < ctx->noutstreams; i++)
		{
			jitter_t *outstream = ctx->outstream[i];
			if (event_data->decoder->ops->run(event_data->decoder->ctx, outstream) == 0)
				break;
		}
	}
}

static void _player_listener(void *arg, event_t event, void *eventarg)
{
	player_ctx_t *ctx = (player_ctx_t *)arg;
	switch (event)
	{
		case SRC_EVENT_NEW_ES:
			_player_new_es(ctx, eventarg);
		break;
		case SRC_EVENT_DECODE_ES:
			_player_decode_es(ctx, eventarg);
		break;
	}
}

static int _player_play(void* arg, int id, const char *url, const char *info, const char *mime)
{
	player_ctx_t *ctx = (player_ctx_t *)arg;
	src_t *src = NULL;

	dbg("player: prepare %d %s %s", id, url, mime);
	src = src_build(ctx, url, mime, id, info);
	if (src != NULL)
	{
		if (ctx->nextsrc != NULL && ctx->nextsrc != src)
		{
			src_destroy(ctx->nextsrc);
			ctx->nextsrc = NULL;
		}
		ctx->nextsrc = src;

		if (src->ops->eventlistener)
		{
			src->ops->eventlistener(src->ctx, _player_listener, ctx);
			if (src->ops->prepare != NULL)
				src->ops->prepare(src->ctx, src->info);
		}
		else
		{
			const event_new_es_t event_new = {.pid = 0, .src = src, .mime = mime, .jitte = JITTE_LOW};
			_player_listener(ctx, SRC_EVENT_NEW_ES, (void *)&event_new);
			const event_decode_es_t event_decode = {.pid = 0, .src = src, .decoder = event_new.decoder};
			_player_listener(ctx, SRC_EVENT_DECODE_ES, (void *)&event_decode);
		}
		warn("player: next %s", url);
		return 0;
	}
	else
	{
		err("player: src not found for %s", url);
		ctx->nextsrc = NULL;
	}
	return -1;
}

int player_play(player_ctx_t *ctx, int id)
{
	int ret = -1;
	if (ctx->media && ctx->media->ops->play != NULL)
	{
		ret = ctx->media->ops->play(ctx->media->ctx, id, _player_play, ctx);
		// TO CHECK
		//ret -= 1;
	}
	return ret;
}

int player_subscribe(player_ctx_t *ctx, estream_t type, jitter_t *encoder_jitter)
{
	if (type == ES_AUDIO)
	{
		ctx->outstream[ctx->noutstreams] = encoder_jitter;
		ctx->noutstreams++;
	}
	return 0;
}

static int _player_stateengine(player_ctx_t *ctx, int state, int pause)
{
	int i;
	switch (state)
	{
		case STATE_STOP:
			dbg("player: stoping");
			for (i = 0; i < ctx->noutstreams; i++)
				ctx->outstream[i]->ops->flush(ctx->outstream[i]->ctx);
			if (ctx->src != NULL)
			{
				src_destroy(ctx->src);
				ctx->src = NULL;
			}
			if (ctx->nextsrc != NULL)
			{
				src_destroy(ctx->nextsrc);
				ctx->nextsrc = NULL;
			}
			if (ctx->media != NULL)
			{
				if (ctx->media->ops->end)
					ctx->media->ops->end(ctx->media->ctx);
				else
					ctx->media->ops->find(ctx->media->ctx, -1, NULL, NULL);
			}
			for (i = 0; i < ctx->noutstreams; i++)
				ctx->outstream[i]->ops->reset(ctx->outstream[i]->ctx);
			dbg("player: stop");
		break;
		case STATE_PLAY:
		{
			dbg("player: playing");
			if (pause)
				break;
			int id;
			/// prepare nextsrc
			if (ctx->media->ops->next)
				id = ctx->media->ops->next(ctx->media->ctx);
			else if (ctx->media->ops->end)
				id = ctx->media->ops->end(ctx->media->ctx);
			/**
			 * media will call _player_play
			 * and this one will set ctx->nextsrc
			 */
			ctx->media->ops->play(ctx->media->ctx, id, _player_play, ctx);
			/**
			 * there isn't any stream in the player
			 * the new one is on nextsrc, then player pass on CHANGE
			 * to switch nextsrc to src
			 */
			if (ctx->src == NULL)
				state = STATE_CHANGE | pause;
		}
		break;
		case STATE_CHANGE:
			if (ctx->src != NULL)
			{
				dbg("player: wait");
				src_destroy(ctx->src);
				ctx->src = NULL;
			}
			ctx->src = ctx->nextsrc;
			ctx->nextsrc = NULL;
			for (i = 0; i < ctx->noutstreams; i++)
				ctx->outstream[i]->ops->pause(ctx->outstream[i]->ctx, pause);

			if (ctx->src != NULL)
			{
				dbg("player: play");
				/**
				 * the src needs to be ready before the decoder
				 * to set a producer if it's needed
				 */
				ctx->src->ops->run(ctx->src->ctx);
				state = STATE_PLAY | pause;
			}
			else
			{
				state = STATE_STOP;
			}
		break;
	}
	return state;
}

int player_run(player_ctx_t *ctx)
{
	if (ctx->noutstreams == 0)
		return -1;

	int last_state = STATE_STOP;
	warn("player: running");
	while (last_state != STATE_ERROR)
	{
		pthread_mutex_lock(&ctx->mutex);
		while (last_state == (ctx->state & ~STATE_PAUSE_MASK))
		{
			pthread_cond_wait(&ctx->cond_int, &ctx->mutex);
			if (last_state == (ctx->state & ~STATE_PAUSE_MASK))
				pthread_cond_broadcast(&ctx->cond);
			for (int i = 0; i < ctx->noutstreams; i++)
				ctx->outstream[i]->ops->pause(ctx->outstream[i]->ctx, (ctx->state & STATE_PAUSE_MASK));
		}

		last_state = ctx->state & ~STATE_PAUSE_MASK;
		pthread_mutex_unlock(&ctx->mutex);

		if (ctx->media == NULL)
		{
			err("media not available");
			player_state(ctx, STATE_STOP);
		}

		int new_state;
		new_state = _player_stateengine(ctx, ctx->state & ~STATE_PAUSE_MASK, ctx->state & STATE_PAUSE_MASK);

		/******************
		 * event manager  *
		 ******************/
		event_player_state_t event = {
				.playerctx = ctx,
				.state = ctx->state,
				.mediapath = NULL,
		};
		player_sendevent(ctx, PLAYER_EVENT_CHANGE, &event);

		if (last_state != (new_state & ~STATE_PAUSE_MASK))
		{
			ctx->state = new_state;
			pthread_cond_broadcast(&ctx->cond);
		}

	}
	return 0;
}

void player_sendevent(player_ctx_t *ctx, event_t event, void *data)
{
	event_listener_t *it = ctx->listeners;
	while (it != NULL)
	{
		dbg("player: event %d to %d (%s)", event, it->id, it->name);
		it->cb(it->arg, event, data);
		it = it->next;
	}
}

src_t *player_source(player_ctx_t *ctx)
{
	return ctx->src;
}

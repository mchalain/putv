/*****************************************************************************
 * sink_alsa.c
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
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <tinyalsa/asoundlib.h>

#include "player.h"
#include "encoder.h"
#include "jitter.h"
typedef struct sink_s sink_t;
typedef struct sink_ctx_s sink_ctx_t;
struct sink_ctx_s
{
	player_ctx_t *player;
	jitter_t *in;
	event_listener_t *listener;

	const sink_t *ops;
	struct pcm *playback_handle;
	pthread_t thread;
	state_t state;

};
#define SINK_CTX
#include "sink.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static const char *jitter_name = "tinyalsa";
static sink_ctx_t *alsa_init(player_ctx_t *player, const char *soundcard)
{
	int ret;
	sink_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ops = sink_tinyalsa;
	ctx->player = player;

	size_t size = 128;
	struct pcm_config config =
	{
		.channels = 2,
		.rate = 44100,
		.period_size = 1024,
		.period_count = 4,
		.format = PCM_FORMAT_S16_LE,
	};
#if SINK_ALSA_FORMAT == PCM_8bits_mono
	config.format = PCM_FORMAT_S8;
	config.channels = 1;
#elif SINK_ALSA_FORMAT == PCM_16bits_LE_mono
	config.format = PCM_FORMAT_S16_LE;
	config.channels = 1;
#elif SINK_ALSA_FORMAT == PCM_16bits_LE_stereo
	config.format = PCM_FORMAT_S16_LE;
#elif SINK_ALSA_FORMAT == PCM_24bits3_LE_stereo
	config.format = PCM_FORMAT_S24_LE;
#elif SINK_ALSA_FORMAT == PCM_32bits_LE_stereo
	config.format = PCM_FORMAT_S32_LE;
#else
	jitter_format_t format = SINK_ALSA_FORMAT;
	switch (format)
	{
		case PCM_32bits_LE_stereo:
			config.format = PCM_FORMAT_S32_LE;
		break;
		case PCM_24bits3_LE_stereo:
			config.format = PCM_FORMAT_S24_LE;
		break;
		case PCM_16bits_LE_stereo:
			config.format = PCM_FORMAT_S16_LE;
		break;
		case PCM_16bits_LE_mono:
			config.format = PCM_FORMAT_S16_LE;
			config.channels = 1;
		break;
	}
#endif
	ctx->playback_handle = pcm_open(0, 0, PCM_OUT, &config);

	jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, 10, size);
	ctx->in = jitter;
	jitter->format = SINK_ALSA_FORMAT;

	return ctx;
error:
	err("sink: init error %s", strerror(errno));
	free(ctx);
	return NULL;
}

static unsigned int sink_attach(sink_ctx_t *ctx, encoder_t *encoder)
{
	return 0;
}

static jitter_t *alsa_jitter(sink_ctx_t *ctx, unsigned int index)
{
	if (index == 0)
		return ctx->in;
	return NULL;
}

static const encoder_ops_t *sink_encoder(sink_ctx_t *ctx)
{
	return encoder_passthrough;
}

static void *alsa_thread(void *arg)
{
	int ret;
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	/* start decoding */
	while (ctx->state != STATE_ERROR)
	{
		unsigned char *buff = ctx->in->ops->peer(ctx->in->ctx, NULL);
		dbg("sink: play %ld", ctx->in->ctx->size);
		ret = pcm_writei(ctx->playback_handle, buff, ctx->in->ctx->size);
		ctx->in->ops->pop(ctx->in->ctx, ret);
		if (ret < 0)
			ctx->state = STATE_ERROR;
		else
		{
			dbg("sink: play %d", ret);
		}
	}
	return NULL;
}

static int alsa_run(sink_ctx_t *ctx)
{
	pthread_create(&ctx->thread, NULL, alsa_thread, ctx);
	return 0;
}

static void alsa_destroy(sink_ctx_t *ctx)
{
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
	pcm_close(ctx->playback_handle);
	jitter_destroy(ctx->in);
	free(ctx);
}

const sink_ops_t *sink_tinyalsa = &(sink_ops_t)
{
	.name = "tinyalsa",
	.init = alsa_init,
	.jitter = alsa_jitter,
	.attach = sink_attach,
	.encoder = sink_encoder,
	.run = alsa_run,
	.destroy = alsa_destroy,
};

/*****************************************************************************
 * src_alsa.c
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
#include <alsa/asoundlib.h>

#include "player.h"
#include "jitter.h"
#include "filter.h"
typedef struct src_s src_t;
typedef struct src_ctx_s src_ctx_t;
struct src_ctx_s
{
	player_ctx_t *player;
	const src_t *ops;
	const char *soundcard;
	snd_pcm_t *handle;
	pthread_t thread;
	jitter_t *out;
	state_t state;
	unsigned int samplerate;
	int samplesize;
	int nchannels;
	filter_t filter;
};
#define SRC_CTX
#include "src.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define src_dbg dbg

static int _pcm_open(src_ctx_t *ctx, snd_pcm_format_t pcm_format, unsigned int rate, unsigned long *size)
{
	int ret;
	int dir;

	ret = snd_pcm_open(&ctx->handle, ctx->soundcard, SND_PCM_STREAM_CAPTURE, 0);

	snd_pcm_hw_params_t *hw_params;
	ret = snd_pcm_hw_params_malloc(&hw_params);
	if (ret < 0)
	{
		err("src: malloc");
		goto error;
	}

	ret = snd_pcm_hw_params_any(ctx->handle, hw_params);
	if (ret < 0)
	{
		err("src: get params");
		goto error;
	}
	//int resample = 1;
	//ret = snd_pcm_hw_params_set_rate_resample(handle, params, resample);
	ret = snd_pcm_hw_params_set_access(ctx->handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (ret < 0)
	{
		err("src: access");
		goto error;
	}
pcm_format = SND_PCM_FORMAT_S16;
	ret = snd_pcm_hw_params_set_format(ctx->handle, hw_params, pcm_format);
	if (ret < 0)
	{
		err("src: format");
		goto error;
	}
	dir=0;
	ret = snd_pcm_hw_params_set_rate_near(ctx->handle, hw_params, &rate, &dir);
	if (ret < 0)
	{
		err("src: rate");
		goto error;
	}
	ret = snd_pcm_hw_params_set_channels(ctx->handle, hw_params, ctx->nchannels);
	if (ret < 0)
	{
		err("src: channels %d", ctx->nchannels);
		goto error;
	}

	if (size && *size > 0)
	{
		dir = 0;
		//snd_pcm_hw_params_set_buffer_size_near(ctx->handle, hw_params, size);
		snd_pcm_hw_params_set_period_size_near(ctx->handle, hw_params, size, &dir);
	}

	ret = snd_pcm_hw_params(ctx->handle, hw_params);
	if (ret < 0)
	{
		err("src: set params");
		goto error;
	}

	snd_pcm_uframes_t buffer_size;
	snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size);
	dbg("buffer size %lu", buffer_size);
	dbg("sample rate %lu", rate);
	snd_pcm_uframes_t periodsize;
	snd_pcm_hw_params_get_period_size(hw_params, &periodsize, 0);
	dbg("period size %lu", periodsize);
	dbg("sample size %d", ctx->samplesize);
	dbg("nchannels %u", ctx->nchannels);
	if (size)
		*size = periodsize;

	ret = snd_pcm_prepare(ctx->handle);
	if (ret < 0) {
		err("src: prepare");
		goto error;
	}
dbg("hello 1");
	ctx->samplerate = rate;

error:
	snd_pcm_hw_params_free(hw_params);
dbg("hello 2");
	return ret;
}

static int _pcm_close(src_ctx_t *ctx)
{
	snd_pcm_drain(ctx->handle);
	snd_pcm_close(ctx->handle);
	return 0;
}

static const char *jitter_name = "alsa";
static src_ctx_t *alsa_init(player_ctx_t *player, const char *soundcard)
{
	int count = 2;
	src_ctx_t *ctx = calloc(1, sizeof(*ctx));

	ctx->ops = src_alsa;
	ctx->soundcard = soundcard;

	ctx->player = player;

	return ctx;
}

static void *alsa_thread(void *arg)
{
	int ret;
	src_ctx_t *ctx = (src_ctx_t *)arg;
	snd_pcm_format_t pcm_format;
	switch (ctx->out->format)
	{
		case PCM_32bits_LE_stereo:
			pcm_format = SND_PCM_FORMAT_S32_LE;
			ctx->samplesize = 4;
			ctx->nchannels = 2;
		break;
		case PCM_24bits_LE_stereo:
			pcm_format = SND_PCM_FORMAT_S24_LE;
			ctx->samplesize = 3;
			ctx->nchannels = 2;
		break;
		case PCM_16bits_LE_stereo:
			pcm_format = SND_PCM_FORMAT_S16_LE;
			ctx->samplesize = 2;
			ctx->nchannels = 2;
		break;
		case PCM_16bits_LE_mono:
			pcm_format = SND_PCM_FORMAT_S16_LE;
			ctx->samplesize = 2;
			ctx->nchannels = 1;
		break;
		default:
			dbg("src alsa: format error %d",  ctx->out->format);
	}
	if (ctx->out->ctx->frequence == 0)
		ctx->out->ctx->frequence = 48000;

	int divider = ctx->samplesize * ctx->nchannels;

	unsigned long size = ctx->out->ctx->size / divider;
dbg("src size %lu", size);
	if (_pcm_open(ctx, pcm_format, ctx->out->ctx->frequence, &size) < 0)
	{
		err("src: pcm error %s", strerror(errno));
		return NULL;
	}

dbg("dst size %lu", size);
dbg("hello 3");
	snd_pcm_start(ctx->handle);
	/* start decoding */
	unsigned char *buff = NULL;
	while (ctx->state != STATE_ERROR)
	{
		if (player_waiton(ctx->player, STATE_PAUSE) < 0)
		{
			if (player_state(ctx->player, STATE_UNKNOWN) == STATE_ERROR)
			{
				snd_pcm_drain(ctx->handle);
				ctx->state = STATE_ERROR;
				continue;
			}
		}

		ret = 0;
		if (buff == NULL)
			buff = ctx->out->ops->pull(ctx->out->ctx);
		unsigned char *buff2 = NULL;
dbg("hello 4");
		while ((ret = snd_pcm_avail_update (ctx->handle)) < size)
		{
			if (ret >= 0 && snd_pcm_state(ctx->handle) == SND_PCM_STATE_XRUN)
				ret=-EPIPE;

			if (ret < 0)
				break;
			ret = snd_pcm_wait (ctx->handle, 1000);
		}
		if (ret > 0)
		{
			if (ret > size)
				ret = size;
			buff2 = malloc(ret * divider);
#ifdef LBENDIAN
			ret = snd_pcm_readi(ctx->handle, buff2, ret);
#else
dbg("buff %lu %lu", ctx->out->ctx->size, ret * 4);
			ret = snd_pcm_readi(ctx->handle, buff, ret);
#endif
		}
dbg("hello 1");
		if (ret == -EPIPE)
		{
			warn("pcm recover");
			ret = snd_pcm_recover(ctx->handle, ret, 0);
		}
		else if (ret < 0)
		{
			ctx->state = STATE_ERROR;
			err("src: error write pcm %d", ret);
		}
		else if (ret > 0)
		{
#ifdef LBENDIAN
			int i;
			for (i = 0; i < ret; i++)
			{
				buff[i*2] = buff2[(i*2) + 1];
				buff[(i*2) + 1] = buff2[i*2];
			}
#endif
			ctx->out->ops->push(ctx->out->ctx, ret * divider, NULL);
			buff = NULL;
			src_dbg("src: play %d", ret);
		}
		free(buff2);
	}
	dbg("src: thread end");
	return NULL;
}

static int alsa_run(src_ctx_t *ctx, jitter_t *jitter)
{
	ctx->out = jitter;
	pthread_create(&ctx->thread, NULL, alsa_thread, ctx);
	return 0;
}

static void alsa_destroy(src_ctx_t *ctx)
{
	pthread_join(ctx->thread, NULL);
	_pcm_close(ctx);
	ctx->filter.ops->destroy(ctx->filter.ctx);
	free(ctx);
}

const src_t *src_alsa = &(src_t)
{
	.init = alsa_init,
	.run = alsa_run,
	.destroy = alsa_destroy,
};

#ifndef SRC_GET
#define SRC_GET
const src_t *src_get(src_ctx_t *ctx)
{
	return ctx->ops;
}
#endif

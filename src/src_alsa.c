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
#include "event.h"
typedef struct src_s src_t;
typedef struct src_ops_s src_ops_t;
typedef struct src_ctx_s src_ctx_t;
struct src_ctx_s
{
	player_ctx_t *player;
	const char *soundcard;
	snd_pcm_t *handle;
	pthread_t thread;
	jitter_t *out;
	state_t state;

	unsigned int samplerate;
	int samplesize;
	int nchannels;
	snd_pcm_format_t format;
	unsigned long periodsize;

	filter_t filter;
	decoder_t *estream;
	long pid;
	event_listener_t *listener;
};
#define SRC_CTX
#include "src.h"
#include "media.h"
#include "decoder.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define src_dbg(...)

#define LATENCY 0

static int _pcm_open(src_ctx_t *ctx, snd_pcm_format_t pcm_format, unsigned int rate, unsigned long *size)
{
	int ret;
	int dir;

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
	snd_pcm_uframes_t periodsize;
	snd_pcm_hw_params_get_period_size(hw_params, &periodsize, 0);
	src_dbg("src alsa config :");
	src_dbg("\tbuffer size %lu", buffer_size);
	src_dbg("\tperiod size %lu", periodsize);
	src_dbg("\tsample rate %u", rate);
	src_dbg("\tsample size %d", ctx->samplesize);
	src_dbg("\tnchannels %u", ctx->nchannels);
	if (size)
		*size = periodsize;

error:
	snd_pcm_hw_params_free(hw_params);

	if (ret < 0)
	{
		err("src: pcm error %s", strerror(errno));
	}
	else
		ret = snd_pcm_prepare(ctx->handle);
	return ret;
}

static int _pcm_close(src_ctx_t *ctx)
{
	snd_pcm_drain(ctx->handle);
	snd_pcm_close(ctx->handle);
	return 0;
}

static const char *jitter_name = "alsa";
static src_ctx_t *_src_init(player_ctx_t *player, const char *url, const char *mime)
{
	int count = 2;
	int samplerate = DEFAULT_SAMPLERATE;
	snd_pcm_format_t format = SND_PCM_FORMAT_S32_LE;
	int nchannels = 2, samplesize =4;
	src_ctx_t *ctx = NULL;
	const char *soundcard;
	char *setting;

	soundcard = utils_getpath(url, "alsa://", &setting, 1);
	if (soundcard == NULL)
		soundcard = utils_getpath(url, "pcm://", &setting, 1);
	if (soundcard == NULL)
	{
		soundcard = url;
	}

	int ret;
	snd_pcm_t *handle;
	ret = snd_pcm_open(&handle, soundcard, SND_PCM_STREAM_CAPTURE, 0);
	if (ret == 0)
	{
#ifdef SINK_ALSA_CONFIG
		while (setting != NULL)
		{
			if (!strncmp(setting + 1, "format=", 7))
			{
				setting += 8;
				if (!strncmp(setting, "8", 4))
				{
					format = SND_PCM_FORMAT_S8;
					samplesize = 1;
				}
				if (!strncmp(setting, "16le", 4))
				{
					format = SND_PCM_FORMAT_S16_LE;
					samplesize = 2;
				}
				if (!strncmp(setting, "24le", 4))
				{
					format = SND_PCM_FORMAT_S24_3LE;
					samplesize = 3;
				}
				if (!strncmp(setting, "32le", 4))
				{
					format = SND_PCM_FORMAT_S32_LE;
					samplesize = 4;
				}
			}
			if (!strncmp(setting + 1, "samplerate=", 11))
			{
				setting += 12;
				samplerate = atoi(setting);
			}
			setting = strchr(setting, ',');
		}
#endif
		ctx = calloc(1, sizeof(*ctx));
		ctx->soundcard = soundcard;
		ctx->player = player;
		ctx->handle = handle;
		dbg("src: %s on %s", src_alsa->name, soundcard);
		ctx->format = format;
		ctx->samplesize = samplesize;
		ctx->nchannels = nchannels;
		ctx->samplerate = samplerate;
		ctx->periodsize = LATENCY * 1000 / ctx->samplerate;
	}
	return ctx;
}

static void *_src_thread(void *arg)
{
	int ret;
	src_ctx_t *ctx = (src_ctx_t *)arg;


	int divider = ctx->samplesize * ctx->nchannels;
	unsigned long nsamples = ctx->periodsize;

	snd_pcm_start(ctx->handle);
	/* start decoding */
	unsigned char *buff = NULL;
	while (ctx->state != STATE_ERROR)
	{
		ret = 0;
		if (buff == NULL)
		{
			buff = ctx->out->ops->pull(ctx->out->ctx);
			/**
			 * the pipe is broken. close the src and the decoder
			 */
			if (buff == NULL)
				break;
			nsamples = ctx->out->ctx->size / divider;
		}

		while ((ret = snd_pcm_avail_update (ctx->handle)) < nsamples)
		{
			if (ret >= 0 && snd_pcm_state(ctx->handle) == SND_PCM_STATE_XRUN)
				ret=-EPIPE;

			if (ret < 0)
				break;
			ret = snd_pcm_wait (ctx->handle, 1000);
		}
		if (ret > 0)
		{
			if (ret > nsamples)
				ret = nsamples;
#ifdef LBENDIAN
			unsigned char *buff2 = NULL;
			buff2 = malloc(ret * divider);
			ret = snd_pcm_readi(ctx->handle, buff2, ret);
			int i;
			for (i = 0; i < ret; i++)
			{
				buff[i*2] = buff2[(i*2) + 1];
				buff[(i*2) + 1] = buff2[i*2];
			}
			free(buff2);
#else
			ret = snd_pcm_readi(ctx->handle, buff, ret);
#endif
		}
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
			ctx->out->ops->push(ctx->out->ctx, ret * divider, NULL);
			buff = NULL;
		}
	}
	dbg("src: thread end");
	ctx->out->ops->flush(ctx->out->ctx);
	const src_t src = { .ops = src_alsa, .ctx = ctx};
	event_end_es_t event = {.pid = ctx->pid, .src = &src, .decoder = ctx->estream};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		listener->cb(listener->arg, SRC_EVENT_END_ES, (void *)&event);
		listener = listener->next;
	}
	return NULL;
}

static int _src_prepare(src_ctx_t *ctx, const char *info)
{
	const src_t src = { .ops = src_alsa, .ctx = ctx};
	event_new_es_t event = {.pid = ctx->pid, .src = &src, .mime = mime_audiopcm, .jitte = JITTE_LOW};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		err("src: alsa new es event");
		listener->cb(listener->arg, SRC_EVENT_NEW_ES, (void *)&event);
		listener = listener->next;
	}
	_pcm_open(ctx, ctx->format, ctx->samplerate, &ctx->periodsize);
	return 0;
}

static int _src_run(src_ctx_t *ctx)
{
	const src_t src = { .ops = src_alsa, .ctx = ctx};
	event_decode_es_t event_decode = {.pid = ctx->pid, .src = &src, .decoder = ctx->estream};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		err("src: alsa start decode event");
		listener->cb(listener->arg, SRC_EVENT_DECODE_ES, (void *)&event_decode);
		listener = listener->next;
	}
	if (ctx->out == NULL)
	{
		ctx->out = ctx->estream->ops->jitter(ctx->estream->ctx, JITTE_LOW);
		int divider = ctx->samplesize * ctx->nchannels;
		ctx->periodsize = (ctx->out->ctx->size / divider) * 5;
		dbg("src: latency %lums", (ctx->periodsize * 1000) / ctx->samplerate);
		_pcm_open(ctx, ctx->format, ctx->samplerate, &ctx->periodsize);
	}
	pthread_create(&ctx->thread, NULL, _src_thread, ctx);
	return 0;
}

static const char *_src_mime(src_ctx_t *ctx, int index)
{
	if (index > 0)
		return NULL;
	return mime_audiopcm;
}

static void _src_eventlistener(src_ctx_t *ctx, event_listener_cb_t cb, void *arg)
{
	event_listener_t *listener = calloc(1, sizeof(*listener));
	listener->cb = cb;
	listener->arg = arg;
	if (ctx->listener == NULL)
		ctx->listener = listener;
	else
	{
		/**
		 * add listener to the end of the list. this allow to call
		 * a new listener with the current event when the function is
		 * called from a callback
		 */
		event_listener_t *previous = ctx->listener;
		while (previous->next != NULL) previous = previous->next;
		previous->next = listener;
	}
}

static int _src_attach(src_ctx_t *ctx, long index, decoder_t *decoder)
{
	if (index > 0)
		return -1;
	ctx->estream = decoder;
	ctx->out = ctx->estream->ops->jitter(ctx->estream->ctx, JITTE_MID);

	return 0;
}

static decoder_t *_src_estream(src_ctx_t *ctx, long index)
{
	return ctx->estream;
}

static void _src_destroy(src_ctx_t *ctx)
{
	if (ctx->estream != NULL)
		ctx->estream->ops->destroy(ctx->estream->ctx);
	pthread_join(ctx->thread, NULL);
	ctx->filter.ops->destroy(ctx->filter.ctx);
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		event_listener_t *next = listener->next;
		free(listener);
		listener = next;
	}
	_pcm_close(ctx);
	free(ctx);
}

const src_ops_t *src_alsa = &(src_ops_t)
{
	.name = "alsa",
	.protocol = "pcm://|alsa://",
	.init = _src_init,
	.run = _src_run,
	.eventlistener = _src_eventlistener,
	.prepare = _src_prepare,
	.attach = _src_attach,
	.estream = _src_estream,
	.destroy = _src_destroy,
};

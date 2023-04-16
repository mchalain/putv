/*****************************************************************************
 * sink_pulse.c
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
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <pulse/simple.h>
#include <pulse/error.h>

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

	pa_simple *playback_handle;

	pthread_t thread;
	state_t state;
	jitter_format_t format;
	unsigned int samplerate;
	int buffersize;
	char samplesize;
	char nchannels;

	unsigned char *noise;
	unsigned int noisecnt;
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

#define sink_dbg(...)

#define LATENCE_MS 5
#define NB_BUFFER 16

#ifdef USE_REALTIME
// REALTIME_SCHED is set from the Makefile to SCHED_RR
#define SINK_POLICY REALTIME_SCHED
#endif

#ifndef ALSA_MIXER
#define ALSA_MIXER "Master"
#endif

extern const sink_ops_t *sink_alsa;

typedef struct pcm_config_s
{
	int samplesize;
	int nchannels;
	pa_sample_format_t pa_format;
} pcm_config_t;

static int _pa_config(jitter_format_t format, pcm_config_t *config)
{
	jitter_format_t downformat = format;
	switch (format)
	{
		case PCM_32bits_LE_stereo:
			config->pa_format = PA_SAMPLE_S32LE;
			downformat = PCM_24bits4_LE_stereo;
			config->samplesize = 4;
			config->nchannels = 2;
		break;
		case PCM_24bits4_LE_stereo:
			config->pa_format = PA_SAMPLE_S24_32LE;
			downformat = PCM_16bits_LE_stereo;
			config->samplesize = 4;
			config->nchannels = 2;
		break;
		case PCM_24bits3_LE_stereo:
			config->pa_format = PA_SAMPLE_S24LE;
			downformat = PCM_16bits_LE_stereo;
			config->samplesize = 3;
			config->nchannels = 2;
		break;
		case PCM_16bits_LE_stereo:
			config->pa_format = PA_SAMPLE_S16LE;
			config->samplesize = 2;
			config->nchannels = 2;
		break;
		case PCM_16bits_LE_mono:
			config->pa_format = PA_SAMPLE_S16LE;
			config->samplesize = 2;
			config->nchannels = 1;
		break;
		case PCM_8bits_mono:
			config->pa_format = PA_SAMPLE_U8;
			downformat = PCM_16bits_LE_mono;
			config->samplesize = 1;
			config->nchannels = 1;
		break;
		default:
			err("format not found");
			config->pa_format = PA_SAMPLE_S16LE;
			config->samplesize = 2;
			config->nchannels = 2;
		break;
	}
	return downformat;
}

static int _pa_open(sink_ctx_t *ctx, jitter_format_t format, unsigned int rate, unsigned int *size)
{
	int ret = 0;
	jitter_format_t downformat = -1;
	pcm_config_t config = {0};
	_pa_config(format, &config);

	pa_sample_spec spec = {
		.format = PA_SAMPLE_S16LE,
	        .rate = 44100,
        	.channels = 2
	};

	spec.format = config.pa_format;
	if (rate == 0)
		rate = 44100;
	spec.rate = rate;
	spec.channels = config.nchannels;

	ctx->format = format;
	ctx->nchannels = config.nchannels;
	ctx->samplesize = config.samplesize;
	ctx->samplerate = rate;

	int error;
	char *name = PACKAGE;
	ctx->playback_handle = pa_simple_new(NULL, name, PA_STREAM_PLAYBACK, NULL, "Music", &spec, NULL, NULL, &error);
	if (ctx->playback_handle == NULL)
	{
		ret = -1;
		err("sink: new stream error %s", pa_strerror(error));
		goto error;
	}

error:
	return ret;
}

static int _pa_close(sink_ctx_t *ctx)
{
	pa_simple_drain(ctx->playback_handle, NULL);
	pa_simple_free(ctx->playback_handle);
	return 0;
}

static const char *jitter_name = "pulse";
static sink_ctx_t *sink_init(player_ctx_t *player, const char *arg)
{
	int samplerate = DEFAULT_SAMPLERATE;
	jitter_format_t format = SINK_ALSA_FORMAT;
	sink_ctx_t *ctx = calloc(1, sizeof(*ctx));

	unsigned int size = LATENCE_MS * samplerate / 1000;
	if (_pa_open(ctx, format, samplerate, &size) < 0)
	{
		free(ctx);
		return NULL;
	}

	jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, NB_BUFFER, size);
	jitter->ctx->thredhold = NB_BUFFER/2;
	jitter->format = ctx->format;
	ctx->in = jitter;
	ctx->in->ctx->frequence = DEFAULT_SAMPLERATE;

	ctx->player = player;

	return ctx;
}

static jitter_t *sink_jitter(sink_ctx_t *ctx, unsigned int index)
{
	if (index == 0)
		return ctx->in;
	return NULL;
}

static void *sink_thread(void *arg)
{
	int ret;
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	int divider = ctx->samplesize * ctx->nchannels;

	/* start decoding */
	while (ctx->in->ops->empty(ctx->in->ctx))
		usleep(LATENCE_MS * 1000);
	while (ctx->state != STATE_ERROR)
	{
		unsigned char *buff = NULL;
		int length = 0;
		buff = ctx->in->ops->peer(ctx->in->ctx, NULL);
		if (buff == NULL)
			continue;
		length = ctx->in->ops->length(ctx->in->ctx);
		int error;

		ret = pa_simple_write(ctx->playback_handle, buff, length, &error);
		if (ret < 0)
		{
			warn("sink: write error %s", pa_strerror(error));
			ctx->state = STATE_ERROR;
		}
		dbg("sink: write");
		ctx->in->ops->pop(ctx->in->ctx, ret * divider);
	}
	dbg("sink: thread end");
	return NULL;
}

static unsigned int sink_attach(sink_ctx_t *ctx, const char *mime)
{
	return 0;
}

static const encoder_ops_t *sink_encoder(sink_ctx_t *ctx)
{
	return encoder_passthrough;
}

static int sink_run(sink_ctx_t *ctx)
{
#ifdef USE_REALTIME
	int ret;

	pthread_attr_t attr;
	struct sched_param params;

	pthread_attr_init(&attr);

	ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	if (ret < 0)
		err("setdetachstate error %s", strerror(errno));
	ret = pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS);
	if (ret < 0)
		err("setscope error %s", strerror(errno));
	ret = pthread_attr_setschedpolicy(&attr, SINK_POLICY);
	if (ret < 0)
		err("setschedpolicy error %s", strerror(errno));
	pthread_attr_setschedparam(&attr, &params);
	if (params.sched_priority > sched_get_priority_min(SINK_POLICY))
		params.sched_priority -= 1;
	else
		params.sched_priority = sched_get_priority_min(SINK_POLICY);
	ret = pthread_attr_setschedparam(&attr, &params);
	if (ret < 0)
		err("setschedparam error %s", strerror(errno));
	if (getuid() == 0)
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	else
	{
		warn("run server as root to use realtime");
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
	}
	if (ret < 0)
		err("setinheritsched error %s", strerror(errno));
	pthread_create(&ctx->thread, &attr, sink_thread, ctx);
	pthread_attr_destroy(&attr);
#else
	pthread_create(&ctx->thread, NULL, sink_thread, ctx);
#endif
	return 0;
}

static void sink_destroy(sink_ctx_t *ctx)
{
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
	_pa_close(ctx);

	jitter_destroy(ctx->in);
	free(ctx);
}

const sink_ops_t *sink_pulse = &(sink_ops_t)
{
	.name = "pulse",
	.default_ = "pulse:",
	.init = sink_init,
	.jitter = sink_jitter,
	.attach = sink_attach,
	.encoder = sink_encoder,
	.run = sink_run,
	.destroy = sink_destroy,
};

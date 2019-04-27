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
#include <alsa/asoundlib.h>

#include "player.h"
#include "jitter.h"
typedef struct sink_s sink_t;
typedef struct sink_ctx_s sink_ctx_t;
struct sink_ctx_s
{
	player_ctx_t *player;
	char *soundcard;
	snd_pcm_t *playback_handle;
	snd_mixer_t *mixer;
	snd_mixer_elem_t* mixerchannel;

	pthread_t thread;
	jitter_t *in;
	state_t state;
	jitter_format_t format;
	unsigned int samplerate;
	int buffersize;
	char samplesize;
	char nchannels;
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

#define LATENCE_MS 50
#define NB_BUFFER 3

#ifdef USE_REALTIME
// REALTIME_SCHED is set from the Makefile to SCHED_RR
#define SINK_POLICY REALTIME_SCHED
#define SINK_PRIORITY 65
#endif

#ifndef ALSA_MIXER
#ifdef SINK_ALSA_MIXER_CH
#define ALSA_MIXER SINK_ALSA_MIXER_CH
#else
#define ALSA_MIXER "Master"
#endif
#endif

#ifdef SINK_ALSA_MIXER
void _mixer_setvolume(sink_ctx_t *ctx, unsigned int volume)
{
    long min, max;
    snd_mixer_selem_get_playback_volume_range(ctx->mixerchannel, &min, &max);
	if (volume > 100)
		volume == 100;
    snd_mixer_selem_set_playback_volume_all(ctx->mixerchannel, volume * max / 100);
}

unsigned int _mixer_getvolume(sink_ctx_t *ctx)
{
	long volume;
    long min, max;

    snd_mixer_selem_get_playback_volume_range(ctx->mixerchannel, &min, &max);
    snd_mixer_selem_get_playback_volume(ctx->mixerchannel, 0, &volume);

    return (unsigned int) volume * 100 / max;
}
#endif

static int _pcm_open(sink_ctx_t *ctx, jitter_format_t format, unsigned int rate, unsigned int *size)
{
	int ret;

	ret = snd_pcm_open(&ctx->playback_handle, ctx->soundcard, SND_PCM_STREAM_PLAYBACK, 0);

	snd_pcm_format_t pcm_format;
	switch (format)
	{
		case PCM_32bits_LE_stereo:
			pcm_format = SND_PCM_FORMAT_S32_LE;
			ctx->samplesize = 4;
			ctx->nchannels = 2;
		break;
		case PCM_24bits4_LE_stereo:
			pcm_format = SND_PCM_FORMAT_S24_LE;
			ctx->samplesize = 4;
			ctx->nchannels = 2;
		break;
		case PCM_24bits3_LE_stereo:
			pcm_format = SND_PCM_FORMAT_S24_3LE;
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
	}

	snd_pcm_hw_params_t *hw_params;
	ret = snd_pcm_hw_params_malloc(&hw_params);
	if (ret < 0)
	{
		err("sink: malloc");
		goto error;
	}

	ret = snd_pcm_hw_params_any(ctx->playback_handle, hw_params);
	if (ret < 0)
	{
		err("sink: get params");
		goto error;
	}
	//int resample = 1;
	//ret = snd_pcm_hw_params_set_rate_resample(handle, params, resample);
	ret = snd_pcm_hw_params_set_access(ctx->playback_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (ret < 0)
	{
		err("sink: access");
		goto error;
	}

	ret = snd_pcm_hw_params_set_format(ctx->playback_handle, hw_params, pcm_format);
	if (ret < 0)
	{
		pcm_format = SND_PCM_FORMAT_S24_LE;
		ctx->samplesize = 4;
		ctx->nchannels = 2;
		format = PCM_32bits_LE_stereo;
		ret = snd_pcm_hw_params_set_format(ctx->playback_handle, hw_params, pcm_format);
		if (ret < 0)
		{
			err("sink: format");
			goto error;
		}
		warn("sink: alsa downgrade to 24bits over 32bits");
	}
	ctx->format = format;

	unsigned int trate = rate;
	if (rate == 0)
		trate = 44100;
	ret = snd_pcm_hw_params_set_rate_near(ctx->playback_handle, hw_params, &trate, NULL);
	if (ret < 0)
	{
		err("sink: rate");
		goto error;
	}

	ret = snd_pcm_hw_params_set_channels(ctx->playback_handle, hw_params, ctx->nchannels);
	if (ret < 0)
	{
		err("sink: channels");
		goto error;
	}

	snd_pcm_uframes_t periodsize = 0;
	snd_pcm_uframes_t buffersize = 0;
	if (*size > 0)
	{
		int dir = 0;
		buffersize = *size;
		periodsize = (*size / NB_BUFFER);
		ret = snd_pcm_hw_params_set_buffer_size_near(ctx->playback_handle, hw_params, &buffersize);
		dbg("try setting %ld %ld", periodsize, buffersize);
		if (ret < 0)
		{
			err("sink: buffer_size");
			goto error;
		}

		ret = snd_pcm_hw_params_set_period_size_near(ctx->playback_handle, hw_params, &periodsize, &dir);
		if (ret < 0)
		{
			err("sink: period_size");
			goto error;
		}
		dbg("set %ld %ld", periodsize, buffersize);
	}

	ret = snd_pcm_hw_params(ctx->playback_handle, hw_params);
	if (ret < 0)
	{
		err("sink: set params");
		goto error;
	}

	snd_pcm_hw_params_get_buffer_size(hw_params, &buffersize);
	snd_pcm_hw_params_get_period_size(hw_params, &periodsize, 0);
	dbg("sink alsa config :\n" \
		"\tbuffer size %lu\n" \
		"\tperiod size %lu\n" \
		"\tsample rate %d\n" \
		"\tsample size %d\n" \
		"\tnchannels %u",
		buffersize,
		periodsize,
		rate,
		ctx->samplesize,
		ctx->nchannels);
	ctx->buffersize = periodsize * ctx->samplesize * ctx->nchannels;
	*size = ctx->buffersize;

	ret = snd_pcm_prepare(ctx->playback_handle);
	if (ret < 0) {
		err("sink: prepare");
		goto error;
	}

	ctx->samplerate = rate;

error:
	snd_pcm_hw_params_free(hw_params);
	return ret;
}

static int _pcm_close(sink_ctx_t *ctx)
{
	snd_pcm_drain(ctx->playback_handle);
	snd_pcm_close(ctx->playback_handle);
	return 0;
}

static const char *jitter_name = "alsa";
static sink_ctx_t *alsa_init(player_ctx_t *player, const char *soundcard)
{
	int samplerate = DEFAULT_SAMPLERATE;
	jitter_format_t format = SINK_ALSA_FORMAT;
	sink_ctx_t *ctx = calloc(1, sizeof(*ctx));

	ctx->soundcard = strdup(soundcard);
#ifdef SINK_ALSA_CONFIG
	char *setting = strchr(ctx->soundcard, ':');
	if (setting != NULL)
	{
		*setting = '\0';
		setting++;
		if (!strncmp(setting, "16le", 4))
			format = PCM_16bits_LE_stereo;
		if (!strncmp(setting, "24le", 4))
			format = PCM_24bits4_LE_stereo;
		if (!strncmp(setting, "32le", 4))
			format = PCM_32bits_LE_stereo;
		setting = strchr(ctx->soundcard, ',');
		if (setting != NULL)
			samplerate = atoi(++setting);
	}
#endif

	unsigned int size = LATENCE_MS * samplerate / 1000;
	if (_pcm_open(ctx, format, samplerate, &size) < 0)
	{
		err("sink: init error %s", strerror(errno));
		free(ctx);
		return NULL;
	}

#ifdef SINK_ALSA_MIXER
    snd_mixer_selem_id_t *sid;

    snd_mixer_open(&ctx->mixer, 0);
    snd_mixer_attach(ctx->mixer, ctx->soundcard);
    snd_mixer_selem_register(ctx->mixer, NULL, NULL);
    snd_mixer_load(ctx->mixer);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, ALSA_MIXER);
    ctx->mixerchannel = snd_mixer_find_selem(ctx->mixer, sid);
#endif
	
	jitter_t *jitter = jitter_scattergather_init(jitter_name, NB_BUFFER, size);
#ifdef SAMPLERATE_AUTO
	jitter->ctx->frequence = 0;
#else
	jitter->ctx->frequence = DEFAULT_SAMPLERATE;
#endif
	jitter->ctx->thredhold = 2;
	jitter->format = ctx->format;

	ctx->in = jitter;

	ctx->player = player;

	return ctx;
}

static jitter_t *alsa_jitter(sink_ctx_t *ctx)
{
	return ctx->in;
}

static int _alsa_checksamplerate(sink_ctx_t *ctx)
{
	int ret = 0;
	if(ctx->in->ctx->frequence && (ctx->in->ctx->frequence != ctx->samplerate))
	{
		_pcm_close(ctx);
		int size = ctx->buffersize;
		_pcm_open(ctx, ctx->in->format, ctx->in->ctx->frequence, &size);
	}
#ifdef SAMPLERATE_AUTO
	ctx->in->ctx->frequence = 0;
#else
	ctx->in->ctx->frequence = DEFAULT_SAMPLERATE;
#endif
	return ret;
}

static void *sink_thread(void *arg)
{
	int ret;
	int divider = 2;
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	switch (ctx->in->format)
	{
		case PCM_32bits_LE_stereo:
			divider = 8;
		break;
		case PCM_24bits4_LE_stereo:
			divider = 8;
		break;
		case PCM_24bits3_LE_stereo:
			divider = 6;
		break;
		case PCM_16bits_LE_stereo:
			divider = 4;
		break;
		case PCM_16bits_LE_mono:
			divider = 2;
		break;
	}
	/* start decoding */
	while (ctx->state != STATE_ERROR)
	{
		if (player_waiton(ctx->player, STATE_PAUSE) < 0)
		{
			if (player_state(ctx->player, STATE_UNKNOWN) == STATE_ERROR)
			{
				snd_pcm_drain(ctx->playback_handle);
				ctx->state = STATE_ERROR;
				continue;
			}
		}

		unsigned char *buff = ctx->in->ops->peer(ctx->in->ctx);
		if (buff != NULL)
		{
			int length = ctx->in->ops->length(ctx->in->ctx);
			_alsa_checksamplerate(ctx);
			//snd_pcm_mmap_begin
			ret = snd_pcm_writei(ctx->playback_handle, buff, length / divider);
			sink_dbg("sink  alsa : write %d/%d %d/%d %d", ret * divider, length, ret, length / divider, divider);
			if (ret == -EPIPE)
			{
				warn("pcm recover");
				ret = snd_pcm_recover(ctx->playback_handle, ret, 0);
			}
			ctx->in->ops->pop(ctx->in->ctx, ret * divider);
			if (ret < 0)
			{
				ctx->state = STATE_ERROR;
				err("sink: error write pcm %d", ret);
			}
			else
			{
				sink_dbg("sink: play %d", ret);
			}
		}
	}
	dbg("sink: thread end");
	return NULL;
}

static int alsa_run(sink_ctx_t *ctx)
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
	params.sched_priority = SINK_PRIORITY;
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

static void alsa_destroy(sink_ctx_t *ctx)
{
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
	_pcm_close(ctx);
#ifdef SINK_ALSA_MIXER
	if (ctx->mixer)
		snd_mixer_close(ctx->mixer);
#endif

	jitter_scattergather_destroy(ctx->in);
	free(ctx->soundcard);
	free(ctx);
}

const sink_ops_t *sink_alsa = &(sink_ops_t)
{
	.init = alsa_init,
	.jitter = alsa_jitter,
	.run = alsa_run,
	.destroy = alsa_destroy,
};

#ifdef SINK_ALSA_MIXER
const sink_ops_t *sink_alsa_mixer = &(sink_ops_t)
{
	.init = alsa_init,
	.jitter = alsa_jitter,
	.run = alsa_run,
	.destroy = alsa_destroy,

	.getvolume = _mixer_getvolume,
	.setvolume = _mixer_setvolume,
};
#endif

static sink_t _sink = {0};
sink_t *sink_build(player_ctx_t *player, const char *arg)
{
	const sink_ops_t *sinkops = NULL;
#ifdef SINK_ALSA_MIXER
	sinkops = sink_alsa_mixer;
	_sink.ctx = sinkops->init(player, arg);
	if (_sink.ctx == NULL)
#endif
	{
		sinkops = sink_alsa;
		_sink.ctx = sinkops->init(player, arg);
	}
	if (_sink.ctx == NULL)
		return NULL;
	_sink.ops = sinkops;
	return &_sink;
}

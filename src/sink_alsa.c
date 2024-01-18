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
#include <fcntl.h>

#include "player.h"
#include "jitter.h"
#include "media.h"
#include "encoder.h"
typedef struct sink_s sink_t;
typedef struct sink_ctx_s sink_ctx_t;
struct sink_ctx_s
{
	player_ctx_t *player;
	jitter_t *in;
	event_listener_t *listener;

	const char *soundcard;
	snd_pcm_t *playback_handle;
	char *mixerch;
	snd_mixer_t *mixer;
	snd_mixer_elem_t* mixerchannel;

	pthread_t thread;
	state_t state;
	jitter_format_t format;
	unsigned int samplerate;
	unsigned int buffersize;
	char samplesize;
	char nchannels;

#ifdef SINK_ALSA_NOISE
	unsigned char *noise;
	unsigned int noisecnt;
#endif

#ifdef SINK_DUMP
	int dumpfd;
#endif
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

#ifdef SINK_ALSA_MIXER
static void _sink_volume_cb(void *arg, event_t event, void *data)
{
	if (event != PLAYER_EVENT_VOLUME)
		return;
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	event_player_volume_t *edata = (event_player_volume_t *)data;
	sink_dbg("sink: volume required %d", edata->volume);

	if (ctx->mixerchannel == NULL)
		return;
	long volume = 0;
	long min = 0, max = 0;
	snd_mixer_selem_get_playback_volume_range(ctx->mixerchannel, &min, &max);
	if (edata->volume > 0  && edata->volume < 100)
	{
		volume = ((edata->volume * (max - min)) / 100) + min + 1;
		dbg("alsa: volume %d [%ld - %ld - %ld]", edata->volume, min, volume, max);
		snd_mixer_selem_set_playback_volume_all(ctx->mixerchannel, volume);
		edata->changed = 1;
	}
	snd_mixer_selem_get_playback_volume(ctx->mixerchannel, 0, &volume);
	edata->volume = ((volume - min) * 100) / (max - min);
	sink_dbg("sink: volume event %d", edata->volume);
}
#endif

typedef struct pcm_config_s
{
	int samplesize;
	int nchannels;
	snd_pcm_format_t pcm_format;
} pcm_config_t;

static int _pcm_config(jitter_format_t format, pcm_config_t *config)
{
	jitter_format_t downformat = format;
	switch (format)
	{
		case PCM_32bits_LE_stereo:
			config->pcm_format = SND_PCM_FORMAT_S32_LE;
			downformat = PCM_24bits4_LE_stereo;
			config->samplesize = 4;
			config->nchannels = 2;
		break;
		case PCM_24bits4_LE_stereo:
			config->pcm_format = SND_PCM_FORMAT_S24_LE;
			downformat = PCM_16bits_LE_stereo;
			config->samplesize = 4;
			config->nchannels = 2;
		break;
		case PCM_24bits3_LE_stereo:
			config->pcm_format = SND_PCM_FORMAT_S24_3LE;
			downformat = PCM_16bits_LE_stereo;
			config->samplesize = 3;
			config->nchannels = 2;
		break;
		case PCM_16bits_LE_stereo:
			config->pcm_format = SND_PCM_FORMAT_S16_LE;
			config->samplesize = 2;
			config->nchannels = 2;
		break;
		case PCM_16bits_LE_mono:
			config->pcm_format = SND_PCM_FORMAT_S16_LE;
			config->samplesize = 2;
			config->nchannels = 1;
		break;
		case PCM_8bits_mono:
			config->pcm_format = SND_PCM_FORMAT_S8;
			downformat = PCM_16bits_LE_mono;
			config->samplesize = 2;
			config->nchannels = 1;
		break;
	}
	return downformat;
}

static int _pcm_open(sink_ctx_t *ctx, jitter_format_t format, unsigned int *rate, unsigned int *size)
{
	int ret;
	if (ctx->playback_handle == NULL)
	{
		sink_dbg("sink: open %s", ctx->soundcard);
		ret = snd_pcm_open(&ctx->playback_handle, ctx->soundcard, SND_PCM_STREAM_PLAYBACK, 0);
		if (ret < 0)
			return ret;
	}
	else
		snd_pcm_drain(ctx->playback_handle);

	pcm_config_t config = {0};
	jitter_format_t downformat = _pcm_config(format, &config);

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

	ret = snd_pcm_hw_params_set_format(ctx->playback_handle, hw_params, config.pcm_format);
	if (ret < 0)
	{
		format = downformat;
		_pcm_config(format, &config);
		ret = snd_pcm_hw_params_set_format(ctx->playback_handle, hw_params, config.pcm_format);
		if (ret < 0)
		{
			err("sink: format");
			goto error;
		}
		warn("sink: alsa downgrade to 24bits over 32bits");
	}
	ctx->format = format;
	ctx->nchannels = config.nchannels;
	ctx->samplesize = config.samplesize;

	if (*rate == 0)
		*rate = 44100;
	ret = snd_pcm_hw_params_set_rate_near(ctx->playback_handle, hw_params, rate, NULL);
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

	unsigned int periods = NB_BUFFER/2;
	snd_pcm_uframes_t periodsize = 0;
	snd_pcm_uframes_t buffersize = 0;
	if (*size > 0)
	{
		int dir = 0;
		buffersize = *size * periods;
		periodsize = *size;
		dbg("set %ld %ld", buffersize, periodsize);
		ret = snd_pcm_hw_params_set_periods(ctx->playback_handle, hw_params, periods, dir);
		if (ret < 0)
		{
			err("sink: periods %s", snd_strerror(ret));
			goto error;
		}

		ret = snd_pcm_hw_params_set_buffer_size_near(ctx->playback_handle, hw_params, &buffersize);
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
	}

	ret = snd_pcm_hw_params(ctx->playback_handle, hw_params);
	if (ret < 0)
	{
		err("sink: set params");
		goto error;
	}

	snd_pcm_hw_params_get_buffer_size(hw_params, &buffersize);
	snd_pcm_hw_params_get_period_size(hw_params, &periodsize, 0);
	snd_pcm_hw_params_get_periods(hw_params, &periods, 0);
	unsigned int periodtime = 0;
	snd_pcm_hw_params_get_period_time(hw_params, &periodtime, 0);
	dbg("sink alsa config :\n" \
		"\tbuffer size %lu\n" \
		"\tperiod size %lu\n" \
		"\tnb periods %d\n" \
		"\tperiod time %fms\n" \
		"\tsample rate %d\n" \
		"\tsample size %d\n" \
		"\tnchannels %u",
		buffersize,
		periodsize,
		periods,
		((double)periodtime) / 1000,
		*rate,
		ctx->samplesize,
		ctx->nchannels);
	*size = periodsize * ctx->samplesize * ctx->nchannels;

	ret = snd_pcm_prepare(ctx->playback_handle);
	if (ret < 0)
	{
		err("sink: prepare");
		goto error;
	}

error:
	if (ret < 0)
		err("alsa: pcm error %s", snd_strerror(ret));
	snd_pcm_hw_params_free(hw_params);
	return ret;
}

static int _pcm_close(sink_ctx_t *ctx)
{
	if (ctx->playback_handle)
	{
		snd_pcm_drain(ctx->playback_handle);
		snd_pcm_close(ctx->playback_handle);
		ctx->playback_handle = NULL;
	}
	return 0;
}

static void _alsa_error(const char *file, int line, const char *function, int err, const char *fmt, ...)
{
	err("sink: alsa snd lib error (%d) %s %d into %s", err, file, line, function);
}

static void _sink_playerstate_cb(void *arg, event_t event, void *data)
{
	if (event != PLAYER_EVENT_CHANGE)
		return;
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	event_player_state_t *edata = (event_player_state_t *)data;
	ctx->state = edata->state;
}

static const char *jitter_name = "alsa";
static sink_ctx_t *alsa_init(player_ctx_t *player, const char *url)
{
	int samplerate = DEFAULT_SAMPLERATE;
	jitter_format_t format = SINK_ALSA_FORMAT;
	sink_ctx_t *ctx = calloc(1, sizeof(*ctx));

	const char *soundcard;
	char *setting;

	soundcard = utils_getpath(url, "alsa://", &setting, 1);
	if (soundcard == NULL)
		soundcard = utils_getpath(url, "pcm://", &setting, 1);
	if (soundcard == NULL)
	{
		soundcard = url;
	}
	ctx->mixerch = ALSA_MIXER;
#ifdef SINK_ALSA_CONFIG
	while (setting != NULL)
	{
		if (!strncmp(setting, "format=", 7))
		{
			setting += 7;
			if (!strncmp(setting, "8", 1))
				format = PCM_8bits_mono;
			if (!strncmp(setting, "16le", 4))
				format = PCM_16bits_LE_stereo;
			if (!strncmp(setting, "24le", 4))
				format = PCM_24bits4_LE_stereo;
			if (!strncmp(setting, "32le", 4))
				format = PCM_32bits_LE_stereo;
		}
		if (!strncmp(setting, "samplerate=", 11))
		{
			setting += 11;
			samplerate = atoi(setting);
		}
		if (!strncmp(setting, "mixer=", 6))
		{
			setting += 6;
			ctx->mixerch = setting;
		}
		setting = strchr(setting, ',');
		if (setting)
		{
			*setting = '\0';
			setting++;
		}
	}
#endif

	ctx->soundcard = soundcard;
	ctx->buffersize = LATENCE_MS * samplerate / 1000;
	ctx->samplerate = samplerate;

	snd_lib_error_set_handler(_alsa_error);
	if (_pcm_open(ctx, format, &ctx->samplerate, &ctx->buffersize) < 0)
	{
		err("sink: init error %s", strerror(errno));
		free(ctx);
		return NULL;
	}
	player_eventlistener(player, _sink_playerstate_cb, ctx, "sink_alsa");

#ifdef SINK_ALSA_MIXER
	if (strncasecmp(ctx->mixerch, "disable", 7) &&
		strncasecmp(ctx->mixerch, "none", 4))
	{
		snd_mixer_selem_id_t *sid;

		snd_mixer_open(&ctx->mixer, 0);
		snd_mixer_attach(ctx->mixer, soundcard);
		snd_mixer_selem_register(ctx->mixer, NULL, NULL);
		snd_mixer_load(ctx->mixer);

		snd_mixer_selem_id_alloca(&sid);
		snd_mixer_selem_id_set_index(sid, 0);
		snd_mixer_selem_id_set_name(sid, ctx->mixerch);
		ctx->mixerchannel = snd_mixer_find_selem(ctx->mixer, sid);
		if (ctx->mixerchannel == NULL)
		{
			warn("sink: alsa mixer not found %s", ctx->mixerch);
		}
		else
		{
			player_eventlistener(player, _sink_volume_cb, ctx, "sink_alsa");
		}
	}
#endif

	dbg("sink: alsa card %s mixer %s", soundcard, ctx->mixerch);
	jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, NB_BUFFER, ctx->buffersize);
	jitter->ctx->thredhold = NB_BUFFER/2;
	jitter->format = ctx->format;
	ctx->in = jitter;

#ifdef SAMPLERATE_AUTO
	ctx->in->ctx->frequence = 0;
#else
	ctx->in->ctx->frequence = DEFAULT_SAMPLERATE;
#endif

#ifdef SINK_ALSA_NOISE
	ctx->noise = malloc(ctx->buffersize);
	int i = 0;
	for (i = 0; i < ctx->buffersize; i++)
	{
		ctx->noise[i] = (char)random();
	}
#endif

	ctx->player = player;

	return ctx;
}

static jitter_t *alsa_jitter(sink_ctx_t *ctx, unsigned int index)
{
	if (index == 0)
		return ctx->in;
	return NULL;
}

static int _alsa_checksamplerate(sink_ctx_t *ctx)
{
	int ret = 0;
	if(ctx->in->ctx->frequence && (ctx->in->ctx->frequence != ctx->samplerate))
	{
		int size = ctx->buffersize;
		int samplerate = ctx->in->ctx->frequence;
		_pcm_close(ctx);
		ret = _pcm_open(ctx, ctx->in->format, &samplerate, &size);
		if (ret == 0)
			ctx->samplerate = samplerate;
#ifdef SINK_ALSA_NOISE
		free(ctx->noise);
		ctx->noise = malloc(ctx->buffersize);
		int i = 0;
		for (i = 0; i < ctx->buffersize; i++)
		{
			ctx->noise[i] = (char)random();
		}
#endif
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
	int ret = 0;
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	int divider = ctx->samplesize * ctx->nchannels;

#ifdef SINK_DUMP
	ctx->dumpfd = open("./alsa_dump.wav", O_RDWR | O_CREAT, 0644);
#endif

	/* start decoding */
	while (ctx->in == NULL || ctx->in->ops->empty(ctx->in->ctx))
	{
		sched_yield();
		usleep(LATENCE_MS * 1000);
	}
	while (ctx->state != STATE_ERROR)
	{
		unsigned char *buff = NULL;
		if (! ret && ctx->playback_handle)
			ret = snd_pcm_wait(ctx->playback_handle, 1000);
		if (ret == -ESTRPIPE) {
			while ((ret = snd_pcm_resume(ctx->playback_handle)) == -EAGAIN)
				sleep(1);
		}
		if (ret == -EPIPE)
		{
			ret = snd_pcm_prepare(ctx->playback_handle);
		}
		if (ret < 0)
		{
			err("sink alsa: pcm wait error %s", snd_strerror(ret));
			ctx->state = STATE_ERROR;
			continue;
		}

		size_t length = 0;
#ifdef SINK_ALSA_NOISE
		if (ctx->in->ops->empty(ctx->in->ctx))
		{
			snd_pcm_sframes_t samples;
			snd_pcm_delay(ctx->playback_handle, &samples);
			/**
			 * alsa needs at least 3 periods to run correctly
			 */
			if (samples * ctx->samplesize * ctx->nchannels > ctx->buffersize * 3)
			{
				sched_yield();
				usleep(LATENCE_MS * 1000);
				continue;
			}
			length = ctx->buffersize;
			buff = ctx->noise;
			ctx->noisecnt ++;
		}
		else
#endif
		{
			buff = ctx->in->ops->peer(ctx->in->ctx, NULL);
			if (buff == NULL)
				continue;
			length = ctx->in->ops->length(ctx->in->ctx);
		}
		if (ctx->state == STATE_STOP)
			_pcm_close(ctx);
		else if (_alsa_checksamplerate(ctx) != 0)
		{
			err("sink: bad samplerate during running on alsa");
			player_state(ctx->player, STATE_CHANGE);
			continue;
		}
		//snd_pcm_mmap_begin
		if (length > 0 && ctx->playback_handle)
		{
			ret = snd_pcm_writei(ctx->playback_handle, buff, length / divider);
#ifdef SINK_DUMP
			write(ctx->dumpfd, buff, length);
#endif
			if (ret < 0)
				err("sink alsa: pcm wait error %s", snd_strerror(ret));
			else
			{
				sink_dbg("sink: alsa write %d/%d %d/%d %d", ret * divider, length, ret, length / divider, divider);
			}
		}
		else if (ctx->playback_handle)
		{
			warn("sink: no data to push on alsa");
		}
		else
		{
			dbg("sink: alsa runout");
		}
#ifdef SINK_ALSA_NOISE
		if (buff == ctx->noise)
		{
			int i = 0;
			char *tmp;
			for (i = 0; i < ctx->buffersize; i++)
			{
				tmp = &ctx->noise[i];
				*tmp = (char)random();
			}
		}
		else
#endif
			ctx->in->ops->pop(ctx->in->ctx, ret * divider);
	}
	warn("sink: alsa thread end");
#ifdef SINK_DUMP
	close(ctx->dumpfd);
#endif
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

static int alsa_run(sink_ctx_t *ctx)
{
	int ret;

	pthread_attr_t attr;
	struct sched_param params;

	pthread_attr_init(&attr);

#ifdef USE_REALTIME
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
	params.sched_priority = sched_get_priority_min(SINK_POLICY) + 5;
	ret = pthread_attr_setschedparam(&attr, &params);
	if (ret < 0)
		err("setschedparam error %s", strerror(errno));
	if (getuid() == 0)
	{
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	}
	else
	{
		warn("run server as root to use realtime");
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
	}
	if (ret < 0)
		err("setinheritsched error %s", strerror(errno));
	warn("sink: alsa realtime %s prio %d",
			(SINK_POLICY == SCHED_RR)?"rr_sched":"fifo", params.sched_priority);
#endif
	warn("sink: alsa start thread");
	ret = pthread_create(&ctx->thread, &attr, sink_thread, ctx);
	pthread_attr_destroy(&attr);
	if (ret < 0)
		err("pthread error %s", strerror(errno));
	return ret;
}

static void alsa_destroy(sink_ctx_t *ctx)
{
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
	warn("sink: alsa join thread");
	_pcm_close(ctx);
#ifdef SINK_ALSA_MIXER
	if (ctx->mixer)
		snd_mixer_close(ctx->mixer);
#endif

#ifdef SINK_ALSA_NOISE
	free(ctx->noise);
#endif
	jitter_destroy(ctx->in);
	free(ctx);
}

const sink_ops_t *sink_alsa = &(sink_ops_t)
{
	.name = "alsa",
	.default_ = "alsa:default",
	.init = alsa_init,
	.jitter = alsa_jitter,
	.attach = sink_attach,
	.encoder = sink_encoder,
	.run = alsa_run,
	.destroy = alsa_destroy,
};

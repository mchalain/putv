/*****************************************************************************
 * filter_pcm.c
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>

#include "media.h"

# define SIZEOF_INT 4

#if SIZEOF_INT >= 4
typedef   signed int sample_t;
#else
typedef   signed long sample_t;
#endif

typedef struct filter_ctx_s filter_ctx_t;
typedef struct filter_audio_s filter_audio_t;
typedef sample_t (*sample_get_t)(filter_ctx_t *ctx, filter_audio_t *audio, int channel, unsigned int index);
typedef sample_t (*sampled_t)(void * ctx, sample_t sample, int bitlength, int samplerate, int channel);

typedef struct sampled_ctx_s sampled_ctx_t;
struct sampled_ctx_s
{
	sampled_t cb;
	void *arg;
	sampled_ctx_t *next;
};

struct filter_ctx_s
{
	sample_get_t get;
	sampled_ctx_t *sampled;
	unsigned int samplerate;
	unsigned char samplesize;
	unsigned char shift;
	unsigned char nchannels;
	unsigned char channel;
#ifdef FILTER_DUMP
	int dumpfd;
#endif
};
#define FILTER_CTX
#include "filter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define filter_dbg(...)

static sample_t filter_get(filter_ctx_t *ctx, filter_audio_t *audio, int channel, unsigned int index);

static int filter_set(filter_ctx_t *ctx,...);
static int filter_setoptions(filter_ctx_t *ctx, va_list params);
static void filter_destroy(filter_ctx_t *ctx);

static filter_ctx_t *filter_init(jitter_format_t format, int samplerate)
{
	filter_ctx_t *ctx = calloc(1, sizeof(*ctx));

	if (samplerate == 0)
		samplerate = 44100;
	filter_set(ctx, FILTER_FORMAT, format, FILTER_SAMPLERATE, samplerate, 0);
#ifdef FILTER_DUMP
	ctx->dumpfd = open("./filter_dump.wav", O_RDWR | O_CREAT, 0644);
#endif
	return ctx;
}

static int filter_set(filter_ctx_t *ctx, ...)
{
	va_list params;
	va_start(params, ctx);
	filter_setoptions(ctx, params);
	va_end(params);
}

static int filter_setformat(filter_ctx_t *ctx, jitter_format_t format)
{
	unsigned char samplesize = 4;
	unsigned char shift = 24;
	unsigned char nchannels = 2;
	switch (format)
	{
	case PCM_8bits_mono:
		samplesize = 2;
		shift = 8;
		nchannels = 1;
	break;
	case PCM_16bits_LE_mono:
		samplesize = 2;
		shift = 16;
		nchannels = 1;
	break;
	case PCM_16bits_LE_stereo:
		samplesize = 2;
		shift = 16;
		nchannels = 2;
	break;
	case PCM_24bits3_LE_stereo:
		samplesize = 3;
		shift = 24;
		nchannels = 2;
	break;
	case PCM_24bits4_LE_stereo:
		samplesize = 4;
		shift = 24;
		nchannels = 2;
	break;
	case PCM_32bits_BE_stereo:
	case PCM_32bits_LE_stereo:
		samplesize = 4;
		shift = 32;
		nchannels = 2;
	break;
	default:
		err("decoder out format not supported %d", format);
		return -1;
	}
	ctx->samplesize = samplesize;
	ctx->shift = shift;
	ctx->nchannels = nchannels;
	return 0;
}

static int filter_setoptions(filter_ctx_t *ctx, va_list params)
{
	sampled_ctx_t *sampleditem = NULL;
	int code = (int) va_arg(params, int);
	while (code != 0)
	{
		switch(code)
		{
		case FILTER_SAMPLED:
			sampleditem = calloc(1, sizeof(*ctx->sampled));
			sampleditem->next = ctx->sampled;
			ctx->sampled = sampleditem;
			ctx->sampled->cb = (sampled_t) va_arg(params, sampled_t);
			ctx->sampled->arg = (void *) va_arg(params, void *);
		break;
		case FILTER_FORMAT:
			filter_setformat(ctx, (jitter_format_t) va_arg(params, jitter_format_t));
		break;
		case FILTER_SAMPLERATE:
			ctx->samplerate = (unsigned int) va_arg(params, unsigned int);
		break;
		}
		code = (int) va_arg(params, int);
	}
	return 0;
}

static void filter_destroy(filter_ctx_t *ctx)
{
	sampled_ctx_t *sampleditem = ctx->sampled;
	while (sampleditem != NULL)
	{
		ctx->sampled = sampleditem->next;
		sampleditem->cb(sampleditem->arg, INT32_MIN, ctx->samplesize, ctx->samplerate, 0);
		free(sampleditem);
		sampleditem = ctx->sampled;
	}
#ifdef FILTER_DUMP
	close(ctx->dumpfd);
#endif
	free(ctx);
}

int sampled_change(filter_ctx_t *ctx, sample_t sample, int bitspersample, int samplerate, int channel, unsigned char *out)
{
	sampled_ctx_t *sampleditem = ctx->sampled;
	while (sampleditem != NULL)
	{
//		int length = ((ctx->shift) > bitspersample)?bitspersample:ctx->shift;
		sample = sampleditem->cb(sampleditem->arg, sample, bitspersample, samplerate, channel);
		sampleditem = sampleditem->next;
	}
#if FILTER_DUMP == 3
	write(ctx->dumpfd, &sample, ctx->samplesize);
#endif

	int i = 0, j = 0;
	for (i = 0; i < ctx->samplesize; i++)
	{
		if ((i * 8) < (ctx->shift - bitspersample))
		{
			out[i] = 0;
			j++;
			continue;
		}
		if ((i * 8) > (ctx->shift))
		{
			out[i] = 0;
			continue;
		}
		out[i] = sample >> ((i - j) * 8);
	}
	return ctx->samplesize;
}

static sample_t filter_get(filter_ctx_t *ctx, filter_audio_t *audio, int channel, unsigned int index)
{
	sample_t *channelsample = audio->samples[(channel % audio->nchannels)];
	return channelsample[index];
}

static sample_t filter_getinterleaved(filter_ctx_t *ctx, filter_audio_t *audio, int channel, unsigned int index)
{
	sample_t *channelsample = audio->samples[0];
	index *= audio->nchannels;
	index += channel;
	return channelsample[index];
}

static int filter_run(filter_ctx_t *ctx, filter_audio_t *audio, unsigned char *buffer, size_t size)
{
	int j;
	int i;
	int bufferlen = 0;
	sample_get_t get = filter_get;
	if (audio->mode == AUDIO_MODE_INTERLEAVED)
		get = filter_getinterleaved;
	for (i = 0; i < audio->nsamples; i++)
	{
		sample_t sample;
		for (j = 0; j < ctx->nchannels; j++)
		{
			if (bufferlen >= size)
				goto filter_exit;

			sample = get(ctx, audio, j, i);
#if FILTER_DUMP == 1
			write(ctx->dumpfd, &sample, ctx->samplesize);
#endif
			int len = sampled_change(ctx, sample, audio->bitspersample,
					audio->samplerate, j, buffer + bufferlen);
#if FILTER_DUMP == 2
			write(ctx->dumpfd, &buffer[bufferlen], ctx->samplesize);
#endif
			bufferlen += len;
		}
	}
filter_exit:
	audio->nsamples -= i;
	for (j = 0; j < audio->nchannels; j++)
	{
		if (audio->mode == AUDIO_MODE_INTERLEAVED)
		{
			audio->samples[0] += i;
			continue;
		}
		audio->samples[j] += i;
	}
	return bufferlen;
}

const filter_ops_t *filter_pcm = &(filter_ops_t)
{
	.name = "pcm",
	.init = filter_init,
	.set = filter_set,
	.run = filter_run,
	.destroy = filter_destroy,
};

static filter_t *_filter_build_pcm(const char *query, jitter_t *jitter, const char *info, const filter_ops_t *filterops)
{
	filter_t *filter = calloc(1, sizeof (*filter));
	jitter_format_t format = jitter->format;
	int samplerate = jitter_samplerate(jitter);

	filter->ops = filterops;
	filter->ctx = filter->ops->init(format, samplerate);

	int replaygain = 0;
	if (info != NULL)
		replaygain = media_boost(info);
	if (query)
	{
		const char *boostvalue = strstr(query, "boost=");
		if (boostvalue != NULL)
			sscanf(boostvalue, "boost=%d", &replaygain);
	}
	if (replaygain > 0)
	{
		warn("filter: install boost filter %ddB", replaygain);
		boost_t *boost = boost_init(&filter->boost, replaygain);
		filter->ops->set(filter->ctx, FILTER_SAMPLED, boost_cb, boost);
	}

#ifdef FILTER_STATS
	if (query && strstr(query, "stats") != NULL)
	{
		warn("filter: install statistics filter");
		stats_t *stats = stats_init(&filter->stats);
		filter->ops->set(filter->ctx, FILTER_SAMPLED, stats_cb, stats);
	}
#endif

#ifdef FILTER_ONECHANNEL
	if (query && strstr(query, "mono=left") != NULL)
	{
		mono_t *mono = mono_init(&filter->mono, 0);
		filter->ops->set(filter->ctx, FILTER_SAMPLED, mono_cb, mono);
	}
	if (query && strstr(query, "mono=right") != NULL)
	{
		mono_t *mono = mono_init(&filter->mono, 1);
		filter->ops->set(filter->ctx, FILTER_SAMPLED, mono_cb, mono);
	}
#endif
#ifdef FILTER_MIXED
	if (query && strstr(query, "mono=mixed") != NULL)
	{
		mixed_t *mixed = NULL;
#define JITTER_AUDIO_STEREO (JITTER_AUDIO | JITTER_AUDIO_INTERLEAVED)
		if ((jitter->format & JITTER_AUDIO_STEREO) == JITTER_AUDIO_STEREO)
			mixed = mixed_init(&filter->mixed, 2);
		else
			mixed = mixed_init(&filter->mixed, 1);
		filter->ops->set(filter->ctx, FILTER_SAMPLED, mixed_cb, mixed);
	}
#endif

	return filter;
}

filter_t *filter_build(const char *name, jitter_t *jitter, const char *info)
{
	filter_t *filter = NULL;
	const char *query = strchr(name, '?');
	int length = strlen(name);
	if (query)
	{
		length = query - name;
		query++;
	}
	if (!strncmp(name, filter_pcm->name, length))
		filter = _filter_build_pcm(query, jitter, info, filter_pcm);

	return filter;
}

sample_t filter_minvalue(int bitspersample)
{
	sample_t min = (~(((sample_t)0x1) << (bitspersample - 1))) + 1;
	return min;
}

sample_t filter_maxvalue(int bitspersample)
{
	sample_t max = -(~(((sample_t)0x1) << (bitspersample - 1))) - 2;
	return max;
}

int filter_filloutput(filter_t *filter, filter_audio_t *audio, jitter_t *out)
{
	static char *outbuffer = NULL;
	static int outbufferlen = 0;
#ifdef DECODER_HEARTBEAT
	static beat_samples_t beat;
#endif
	int pcm_length = audio->nsamples;

	if (jitter_samplerate(out) == 0)
	{
		filter_dbg("filter: change samplerate to %u", audio->samplerate);
		out->ctx->frequence = audio->samplerate;
	}
	else if (jitter_samplerate(out) != audio->samplerate)
	{
		err("filter: samplerate %d not supported", jitter_samplerate(out));
	}

	if (outbuffer == NULL)
	{
		outbuffer = out->ops->pull(out->ctx);
		/**
		 * the pipe is broken. close the src and the decoder
		 */
		if (outbuffer == NULL)
		{
			return -1;
		}
	}

	outbufferlen += filter->ops->run(filter->ctx, audio,
			outbuffer + outbufferlen, out->ctx->size - outbufferlen);

	if (outbufferlen >= out->ctx->size)
	{
		if (outbufferlen > out->ctx->size)
			err("decoder: out %d %ld", outbufferlen, out->ctx->size);
#ifdef DECODER_HEARTBEAT
		beat.nsamples += pcm_length - audio->nsamples;
		beat.nloops++;
		if (beat.nloops == out->ctx->count + 1)
		{
			decoder_dbg("decoder: heart boom %d", beat.nsamples);
			out->ops->push(out->ctx, out->ctx->size, &beat);
			beat.nsamples = 0;
			beat.nloops = 0;
		}
		else
#endif
			out->ops->push(out->ctx, out->ctx->size, NULL);
		outbuffer = NULL;
		outbufferlen = 0;
	}
#ifdef DECODER_HEARTBEAT
	else
	{
		beat->nsamples += pcm_length;
	}
#endif
	return outbufferlen;
}


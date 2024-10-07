/*****************************************************************************
 * encoder_faac.c
 * this file is part of https://github.com/ouistiti-project/putv
 *****************************************************************************
 * Copyright (C) 2024-2025
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
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <faac.h>

#include "player.h"
#include "jitter.h"
#include "heartbeat.h"
#include "media.h"

typedef struct encoder_ops_s encoder_ops_t;
typedef struct encoder_ctx_s encoder_ctx_t;
struct encoder_ctx_s
{
	const encoder_ops_t *ops;
	faacEncHandle encoder;
	unsigned int samplerate;
	unsigned char nchannels;
	unsigned char samplesize;
	unsigned long int samplesframe;
	unsigned int brate;
	unsigned long int maxbytesoutput;
	int dumpfd;
	pthread_t thread;
	player_ctx_t *player;
	jitter_t *in;
	unsigned char *inbuffer;
	jitter_t *out;
	unsigned char *outbuffer;
	heartbeat_t heartbeat;
};
#define ENCODER_CTX
#include "encoder.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define encoder_dbg(...)

#ifdef HEARTBEAT
#define ENCODER_HEARTBEAT
#endif

//#define DEFAULT_SAMPLERATE 48000
#define NB_BUFFERS 6
#ifndef DEFAULT_NCHANNELS
# define DEFAULT_NCHANNELS 2
#endif

#if DEFAULT_NCHANNELS == 1
# define LAME_NCHANNELS JOINT_STEREO
#else
# define LAME_NCHANNELS STEREO
#endif

#if (DEFAULT_SAMPLERATE == 0) || ! defined(DEFAULT_SAMPLERATE)
# warning samplerate set to 0
# undef DEFAULT_SAMPLERATE
# define 48000
#endif

// Lame supports only u16 samples
#define INPUT_FORMAT PCM_16bits_LE_stereo

#ifndef DEFAULT_BITRATE
# if DEFAULT_SAMPLERATE == 48000
/// output stream is more fluent if input and output framerate are the same
#  define OUTPUT_AVERAGE 1000
#  if defined(ENCODER_VBR)
#   define DEFAULT_BITRATE 5
#  else
#   define DEFAULT_BITRATE 224
#  endif
# else
#  define OUTPUT_AVERAGE 1484
#  if defined(ENCODER_VBR)
#   define DEFAULT_BITRATE 0
#  else
#   define DEFAULT_BITRATE 224
#  endif
# endif
#endif

#if defined(ENCODER_VBR) && (DEFAULT_BITRATE > 10)
# error Bitsrate for VBR is a factor in range 1 to 10
#endif

static const char *jitter_name = "faac encoder";
static int encoder_faac_init(encoder_ctx_t *ctx, int samplerate, int samplesize, int nchannels)
{
	if (ctx->encoder)
		faacEncClose(&ctx->encoder);
	ctx->encoder = faacEncOpen(samplerate, nchannels, &ctx->samplesframe, &ctx->maxbytesoutput);
	warn("encoder faac needs input %ld samples, output size %ld", ctx->samplesframe, ctx->maxbytesoutput);

	ctx->samplerate = samplerate;
	ctx->nchannels = nchannels;
	ctx->samplesize = samplesize / 8;

	faacEncConfigurationPtr config;
	config = faacEncGetCurrentConfiguration(ctx->encoder);
	//config->aacObjectType = LOW;
	config->mpegVersion = MPEG2;
	//config->useTns = 0;
	//config->shortctl = SHORTCTL_NORMAL;
	//config->jointmode = JOINT_NONE;
	//config->pnslevel = 0;
#if defined(ENCODER_VBR)
	//config->quantqual = 5; // keep default
	//config->bitRate = 0;
#else
	config->quantqual = 0;
	config->bitRate = DEFAULT_BITRATE / nchannels;
#endif
	//config->outputFormat = ADTS_STREAM;
	switch (samplesize)
	{
	case 32:
warn("samplesize 32");
		config->inputFormat = FAAC_INPUT_32BIT;
	break;
	case 24:
warn("samplesize 24");
		config->inputFormat = FAAC_INPUT_24BIT;
	break;
	case 16:
warn("samplesize 16");
		config->inputFormat = FAAC_INPUT_16BIT;
	break;
	default:
warn("samplesize %u", samplesize);
	}
	faacEncSetConfiguration(ctx->encoder, config);

	ctx->brate = config->bitRate * nchannels;
	warn("Average bitrate: %lu kbps", (config->bitRate + 500) / 1000 * nchannels);
	dbg("Quantization quality: %ld", config->quantqual);
	dbg("Bandwidth: %d Hz", config->bandWidth);
	dbg("inputFormat: %d bits", (config->inputFormat + 1) * 8);
	dbg("stream: %s", (config->outputFormat == ADTS_STREAM)?"ADTS":"RAW");
	dbg("version: MPEG%s", (config->mpegVersion == MPEG2)?"2":"4");
/*
	dbg("Complexity: %s joint %s",
		(config->aacObjectType == LOW)?"low":(config->aacObjectType == MAIN)?"main":"LTP",
		(config->jointmode == JOINT_MS)?"MS":(config->jointmode == JOINT_IS)?"IS":"",
		);
*/
	return 0;
}

static encoder_ctx_t *encoder_init(player_ctx_t *player)
{
	encoder_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ops = encoder_lame;
	ctx->player = player;

	encoder_faac_init(ctx, DEFAULT_SAMPLERATE, FORMAT_SAMPLESIZE(INPUT_FORMAT), FORMAT_NCHANNELS(INPUT_FORMAT));
#if ENCODER_DUMP == 1
	ctx->dumpfd = open("lame_dump.mp3", O_RDWR | O_CREAT, 0644);
#endif
#if ENCODER_DUMP == 2
	ctx->dumpfd = open("lame_dump.wav", O_RDWR | O_CREAT, 0644);
#endif
	unsigned long buffsize = ctx->samplesframe * ctx->samplesize * ctx->nchannels;
	warn("samples size %u", ctx->samplesize);
	warn("samples frame %lu", ctx->samplesframe);
	warn("buffer size %lu", buffsize);
	encoder_dbg("encoder: aac ready");
	jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, NB_BUFFERS, buffsize);
	ctx->in = jitter;
	jitter->format = INPUT_FORMAT;
	jitter->ctx->frequence = 0; // automatic freq
	jitter->ctx->thredhold = 1;

	return ctx;
}

static jitter_t *encoder_jitter(encoder_ctx_t *ctx)
{
	return ctx->in;
}

#ifdef DEBUG
static void encoder_message(const char *format, va_list ap)
{
	 vfprintf(stderr, format, ap);
}
#endif

static void *faac_thread(void *arg)
{
	int result = 0;
	int run = 1;
	encoder_ctx_t *ctx = (encoder_ctx_t *)arg;
	/* start decoding */
#ifdef ENCODER_HEARTBEAT
	clockid_t clockid = CLOCK_REALTIME;
	struct timespec start = {0, 0};
	clock_gettime(clockid, &start);
#endif
#ifdef ENCODER_HEARTBEAT
	ctx->heartbeat.ops->start(ctx->heartbeat.ctx);
#endif
	encoder_dbg("encoder: mp3 thread start");
	uint32_t nsamples = 0;
	while (run)
	{
		int ret = 0;

		ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx, NULL);
		unsigned int inlength = ctx->in->ops->length(ctx->in->ctx);
		dbg("inlength %d", inlength);
		inlength /= ctx->samplesize * ctx->nchannels;
		dbg("samples %d", inlength);
		nsamples += inlength;
		if (inlength < ctx->samplesframe)
			warn("encoder: frame too small %d %ld", inlength, ctx->in->ctx->size);
		if (ctx->in->ctx->frequence != ctx->samplerate)
		{
			warn("set faac samplerate %u",ctx->in->ctx->frequence);
			encoder_faac_init(ctx, ctx->in->ctx->frequence, ctx->samplesize * 8, ctx->nchannels);
		}
		if (ctx->outbuffer == NULL)
		{
			ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
		}
		if (ctx->inbuffer)
		{
#if ENCODER_DUMP == 2
			if (ctx->dumpfd > 0)
				write(ctx->dumpfd, ctx->inbuffer, inlength * ctx->samplesize * ctx->nchannels);
#endif
			dbg("outlength %d", ctx->out->ctx->size);
			ret = faacEncEncode(ctx->encoder,
					(int32_t *)ctx->inbuffer, inlength,
					ctx->outbuffer, ctx->out->ctx->size);
			dbg("faacEncEncode ret %d", ret);
#if ENCODER_DUMP == 1
			if (ctx->dumpfd > 0 && ret > 0)
				write(ctx->dumpfd, ctx->outbuffer, ret);
#endif
			//ctx->in->ops->pop(ctx->in->ctx, ctx->in->ctx->size);
			ctx->in->ops->pop(ctx->in->ctx, inlength * ctx->samplesize * ctx->nchannels);
		}
		else
		{
			ret = faacEncEncode(ctx->encoder,(int32_t *)NULL, 0,
					ctx->outbuffer, ctx->out->ctx->size);
#if ENCODER_DUMP == 1
			if (ctx->dumpfd > 0 && ret > 0)
				write(ctx->dumpfd, ctx->outbuffer, ret);
#endif
		}
		if (ret > 0)
		{
			encoder_dbg("encoder faac %d", ret);
			beat_t beat = {0};
#if defined (ENCODER_HEARTBEAT) && !defined (ENCODER_VBR)
			//ctx->heartbeat.ops->unlock(&ctx->heartbeat.ctx);
			beat.bitrate.length = ret;
			//ctx->heartbeat.ops->unlock(&ctx->heartbeat.ctx);
#elif defined (ENCODER_HEARTBEAT)
			if (nsamples > (ctx->samplerate / 20))
			{
				//ctx->heartbeat.ops->unlock(&ctx->heartbeat.ctx);
				beat.samples.nsamples = nsamples;
				nsamples = 0;
				//ctx->heartbeat.ops->unlock(&ctx->heartbeat.ctx);
			}
#endif
			ctx->out->ops->push(ctx->out->ctx, ret, &beat);
			ctx->outbuffer = NULL;
		}
		if (ret < 0)
		{
			if (ret == -1)
				err("faac error %d, too small buffer %ld", ret, ctx->out->ctx->size);
			else
				err("faac error %d", ret);
			run = 0;
		}
	}
	encoder_dbg("encoder: mp3 thread end");
#ifdef ENCODER_HEARTBEAT
	struct timespec stop = {0, 0};
	clock_gettime(clockid, &stop);
	stop.tv_sec -= start.tv_sec;
	stop.tv_nsec -= start.tv_nsec;
	if (stop.tv_nsec < 0)
	{
		stop.tv_nsec += 1000000000;
		stop.tv_sec -= 1;
	}
	dbg("encode during %lu.%lu", stop.tv_sec, stop.tv_nsec);
#endif
	return (void *)(intptr_t)result;
}

static int encoder_run(encoder_ctx_t *ctx, jitter_t *jitter)
{
	ctx->out = jitter;
#if defined (ENCODER_HEARTBEAT) && !defined (ENCODER_VBR)
	heartbeat_bitrate_t config;
	config.bitrate = ctx->brate;
	config.ms = jitter->ctx->size * jitter->ctx->count * 8 / config.bitrate;
	warn("encoder: output %u %u", jitter->ctx->size, ctx->maxbytesoutput);
	ctx->heartbeat.ops = heartbeat_bitrate;
	ctx->heartbeat.ctx = heartbeat_bitrate->init(&config);
	dbg("set heart %s %dms %dkbps", jitter->ctx->name, config.ms, config.bitrate);
	jitter->ops->heartbeat(jitter->ctx, &ctx->heartbeat);
#elif defined (ENCODER_HEARTBEAT)
	heartbeat_samples_t config;
	config.samplerate = DEFAULT_SAMPLERATE;
	config.format = INPUT_FORMAT;
	ctx->heartbeat.ops = heartbeat_samples;
	ctx->heartbeat.ctx = ctx->heartbeat.ops->init(&config);
	jitter->ops->heartbeat(jitter->ctx, &ctx->heartbeat);
#endif
	pthread_create(&ctx->thread, NULL, faac_thread, ctx);
	return 0;
}

static const char *encoder_mime(encoder_ctx_t *ctx)
{
	return mime_audiomp3;
}

static int encoder_samplerate(encoder_ctx_t *ctx)
{
	return DEFAULT_SAMPLERATE;
}

static jitter_format_t encoder_format(encoder_ctx_t *ctx)
{
	return MPEG2_3_MP3;
}

static void encoder_destroy(encoder_ctx_t *ctx)
{
#ifdef ENCODER_DUMP
	if (ctx->dumpfd > 0)
		close(ctx->dumpfd);
#endif
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
	faacEncClose(ctx->encoder);
#ifdef ENCODER_HEARTBEAT
	ctx->heartbeat.ops->destroy(ctx->heartbeat.ctx);
#endif
	/* release the decoder */
	jitter_destroy(ctx->in);
	free(ctx);
}

const encoder_ops_t *encoder_faac = &(encoder_ops_t)
{
	.name = "faac",
	.init = encoder_init,
	.type = ES_AUDIO,
	.jitter = encoder_jitter,
	.run = encoder_run,
	.mime = encoder_mime,
	.samplerate = encoder_samplerate,
	.format = encoder_format,
	.destroy = encoder_destroy,
};

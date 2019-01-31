/*****************************************************************************
 * encoder_lame.c
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
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <lame/lame.h>

#include "player.h"
#include "heartbeat.h"

typedef struct encoder_s encoder_t;
typedef struct encoder_ctx_s encoder_ctx_t;
struct encoder_ctx_s
{
	const encoder_t *ops;
	lame_global_flags *encoder;
	unsigned int samplerate;
	unsigned char nchannels;
	unsigned char samplesize;
	int samplesframe;
	int dumpfd;
	pthread_t thread;
	player_ctx_t *player;
	jitter_t *in;
	unsigned char *inbuffer;
	jitter_t *out;
	unsigned char *outbuffer;
	heartbeat_t heartbeat;
	heartbeat_samples_t beat;
	unsigned long nsamples;
};
#define ENCODER_CTX
#include "encoder.h"
#include "jitter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define encoder_dbg(...)

static const char *jitter_name = "lame encoder";
void error_report(const char *format, va_list ap)
{
	fprintf(stderr, format, ap);
}

static int encoder_lame_init(encoder_ctx_t *ctx)
{
	if (ctx->encoder)
		lame_close(ctx->encoder);
	ctx->encoder = lame_init();

	lame_set_out_samplerate(ctx->encoder, DEFAULT_SAMPLERATE);
	lame_set_in_samplerate(ctx->encoder, ctx->samplerate);
	lame_set_num_channels(ctx->encoder, ctx->nchannels);
	lame_set_quality(ctx->encoder, 5);
	//lame_set_mode(encoder->encoder, STEREO);
	//lame_set_mode(encoder->encoder, JOINT_STEREO);
	//lame_set_errorf(encoder->encoder, error_report);
	lame_set_VBR(ctx->encoder, vbr_off);
	//lame_set_VBR(encoder->encoder, vbr_default);
	lame_set_disable_reservoir(ctx->encoder, 1);
	lame_init_params(ctx->encoder);
	return 0;
}

static encoder_ctx_t *encoder_init(player_ctx_t *player)
{
	encoder_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ops = encoder_lame;
	ctx->player = player;

	ctx->nchannels = 2;
	ctx->samplerate = DEFAULT_SAMPLERATE;
	ctx->samplesize = sizeof(signed short);

	encoder_lame_init(ctx);
#ifdef LAME_DUMP
	ctx->dumpfd = open("lame_dump.mp3", O_RDWR | O_CREAT, 0644);
#endif
	//ctx->samplesframe = lame_get_framesize(ctx->encoder);
	ctx->samplesframe = 576;
	jitter_t *jitter = jitter_scattergather_init(jitter_name, 3,
				ctx->samplesframe * ctx->samplesize * ctx->nchannels);
	ctx->in = jitter;
	jitter->format = PCM_16bits_LE_stereo;
	jitter->ctx->frequence = 0;
	jitter->ctx->thredhold = 1;

	return ctx;
}

static jitter_t *encoder_jitter(encoder_ctx_t *ctx)
{
	return ctx->in;
}

static void *lame_thread(void *arg)
{
	int result = 0;
	int run = 1;
	encoder_ctx_t *ctx = (encoder_ctx_t *)arg;
	/* start decoding */
#ifdef HEARTBEAT
	clockid_t clockid = CLOCK_REALTIME;
	struct timespec start = {0, 0};
	clock_gettime(clockid, &start);
#endif
	while (run)
	{
		int ret = 0;

		ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx);
		unsigned int inlength = ctx->in->ops->length(ctx->in->ctx);
		inlength /= ctx->samplesize * ctx->nchannels;
		ctx->nsamples += inlength;
		if (inlength < ctx->samplesframe)
			warn("encoder lame: frame too small %d %ld %ld", inlength, ctx->nsamples, ctx->in->ctx->size);
		if (ctx->in->ctx->frequence != ctx->samplerate)
		{
			ctx->samplerate = ctx->in->ctx->frequence;
			encoder_lame_init(ctx);
		}
		if (ctx->outbuffer == NULL)
		{
			ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
		}
		if (ctx->inbuffer)
		{
			ret = lame_encode_buffer_interleaved(ctx->encoder,
					(short int *)ctx->inbuffer, inlength,
					ctx->outbuffer, ctx->out->ctx->size);
#ifdef LAME_DUMP
			if (ctx->dumpfd > 0 && ret > 0)
			{
				dbg("encoder lame dump %d", ret);
				write(ctx->dumpfd, ctx->outbuffer, ret);
			}
#endif
			ctx->in->ops->pop(ctx->in->ctx, ctx->in->ctx->size);
		}
		else
		{
			dbg("encoder lame flush");
			ret = lame_encode_flush_nogap(ctx->encoder, ctx->outbuffer, ctx->out->ctx->size);
			/* TODO : request media data from player to set new ID3 tag */
			lame_init_bitstream(ctx->encoder);
		}
		if (ret > 0)
		{
			heartbeat_samples_t *beat = NULL;
#ifdef HEARTBEAT
			encoder_dbg("encoder lame %d", ret);
			int nsamples = lame_get_mf_samples_to_encode(ctx->encoder);

			if (ctx->nsamples > nsamples)
			{
				ctx->beat.nsamples = ctx->nsamples - nsamples;
				if (ctx->beat.nsamples > (ctx->samplerate / 50))
				{
					beat = &ctx->beat;
					ctx->nsamples = 0;
				}
			}
#endif
			ctx->out->ops->push(ctx->out->ctx, ret, beat);
			ctx->outbuffer = NULL;
		}
		if (ret < 0)
		{
			if (ret == -1)
				err("lame error %d, not enought memory %ld", ret, ctx->out->ctx->size);
			else
				err("lame error %d", ret);
			run = 0;
		}
	}
#ifdef HEARTBEAT
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
#ifdef HEARTBEAT
	if (heartbeat_samples)
	{
		ctx->heartbeat.ops = heartbeat_samples;
		ctx->heartbeat.ctx = heartbeat_samples->init(ctx->samplerate, 2, ctx->nchannels);
		dbg("set heart %s", jitter->ctx->name);
		jitter->ctx->heart = heartbeat_samples->wait;
		jitter->ctx->heart_ctx = ctx->heartbeat.ctx;
	}
#endif
	pthread_create(&ctx->thread, NULL, lame_thread, ctx);
	return 0;
}

static void encoder_destroy(encoder_ctx_t *ctx)
{
#ifdef LAME_DUMP
	if (ctx->dumpfd > 0)
		close(ctx->dumpfd);
#endif
	pthread_join(ctx->thread, NULL);
	lame_close(ctx->encoder);
	/* release the decoder */
	jitter_scattergather_destroy(ctx->in);
	free(ctx);
}

const encoder_t *encoder_lame = &(encoder_t)
{
	.init = encoder_init,
	.jitter = encoder_jitter,
	.run = encoder_run,
	.destroy = encoder_destroy,
};

#ifndef ENCODER_GET
#define ENCODER_GET
const encoder_t *encoder_get(encoder_ctx_t *ctx)
{
	return ctx->ops;
}
#endif

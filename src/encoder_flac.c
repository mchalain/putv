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

#include <FLAC/stream_encoder.h>

#include "player.h"
#include "jitter.h"
#include "heartbeat.h"
#include "media.h"

typedef struct encoder_ops_s encoder_ops_t;
typedef struct encoder_ctx_s encoder_ctx_t;
struct encoder_ctx_s
{
	const encoder_ops_t *ops;
	FLAC__StreamEncoder *encoder;
	unsigned int samplerate;
	unsigned char nchannels;
	unsigned char samplesize;
	unsigned short samplesframe;
	uint64_t framescnt;
	uint64_t maxframes;
	int dumpfd;
	pthread_t thread;
	player_ctx_t *player;
	jitter_t *in;
	unsigned char *inbuffer;
	jitter_t *out;
	unsigned char *outbuffer;
	heartbeat_t heartbeat;
	beat_bitrate_t beat;
	size_t maxsize;
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

#ifndef DEFAULT_SAMPLERATE
#define DEFAULT_SAMPLERATE 44100
#endif

#define SAMPLES_FRAME 400
#define NB_BUFFERS 6
#define LATENCY 200 //ms
#define MAX_SAMPLES (44100 * 60 * 2)

static const char *jitter_name = "flac encoder";

static FLAC__StreamEncoderWriteStatus
_encoder_writecb(const FLAC__StreamEncoder *encoder,
		const FLAC__byte buffer[], size_t bytes,
		unsigned samples, unsigned current_frame, void *client_data)
{
	encoder_ctx_t *ctx = (encoder_ctx_t *)client_data;

	encoder_dbg("encoder: stream frame %u of %u samples", current_frame, samples);
#ifdef DEBUG
	if (ctx->maxsize < bytes) ctx->maxsize = bytes;
#endif
	int check = 1;
#ifdef ENCODER_DUMP
	if (ctx->dumpfd > 0)
	{
		write(ctx->dumpfd, buffer, bytes);
	}
#endif
//	if (samples == 0)
//		return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
	int ret = 0;
	if (ctx->outbuffer == NULL)
	{
		ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
	}

	if (ctx->outbuffer != NULL && ctx->out->ctx->size >= bytes)
	{
		memcpy(ctx->outbuffer, buffer, bytes);
		ret = bytes;
	}
	else if (ctx->outbuffer == NULL)
		warn("encoder: jitter closed");
	else
		warn("encoder: jitter too small %lu bytes for %ld", bytes, ctx->out->ctx->size);
	if (ret > 0)
	{
		encoder_dbg("encoder: flac %d", ret);
		beat_bitrate_t *beat = NULL;
#ifdef ENCODER_HEARTBEAT
		//ctx->heartbeat.ops->unlock(&ctx->heartbeat.ctx);
		ctx->beat.length = ctx->samplesframe;
		beat = &ctx->beat;
		//ctx->heartbeat.ops->unlock(&ctx->heartbeat.ctx);
#endif
		ctx->out->ops->push(ctx->out->ctx, bytes, beat);
		ctx->outbuffer = NULL;

		return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
	}
	return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}

static int encoder_flac_init(encoder_ctx_t *ctx)
{
	int ret = 0;

	/** reinitialize the encoder **/
	FLAC__stream_encoder_finish(ctx->encoder);

	ret = FLAC__stream_encoder_set_verify(ctx->encoder, true);
	FLAC__stream_encoder_set_streamable_subset(ctx->encoder, true);
	FLAC__stream_encoder_set_sample_rate(ctx->encoder, ctx->samplerate);
	FLAC__stream_encoder_set_bits_per_sample(ctx->encoder, 24);
	FLAC__stream_encoder_set_channels(ctx->encoder, ctx->nchannels);
	FLAC__stream_encoder_set_compression_level(ctx->encoder, 2);
	FLAC__stream_encoder_set_blocksize(ctx->encoder, SAMPLES_FRAME);
//	FLAC__stream_encoder_set_blocksize(ctx->encoder, 0);
	FLAC__stream_encoder_set_total_samples_estimate(ctx->encoder, ctx->maxframes * ctx->samplesframe);

	ctx->framescnt = 0;
	dbg("flac: initialized");
	return ret;
}

static encoder_ctx_t *encoder_init(player_ctx_t *player)
{
	encoder_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ops = encoder_flac;
	ctx->player = player;

	jitter_format_t format = PCM_24bits4_LE_stereo;

	ctx->nchannels = 2;
	ctx->samplerate = DEFAULT_SAMPLERATE;
	ctx->samplesize = sizeof(uint32_t);
	ctx->samplesframe = SAMPLES_FRAME;
	//ctx->samplesframe = LATENCY * DEFAULT_SAMPLERATE / 1000;
	// in streaminfg, the number of samples is clearly unknown => 0
	ctx->maxframes = 0;
	// otherwise
	// MAX_SAMPLES / SAMPLES_FRAME

	ctx->encoder = FLAC__stream_encoder_new();

	if (encoder_flac_init(ctx) < 0)
	{
		free(ctx);
		err("encoder: DISABLE flac error");
		return NULL;
	}
#ifdef ENCODER_DUMP
	ctx->dumpfd = open("dump.flac", O_RDWR | O_CREAT, 0644);
	err("dump %d", ctx->dumpfd);
#endif
	/**
	 * set samples frame to 3 framesize to have less than 1500 bytes
	 * but more than 1000 bytes into the output
	 */
	ctx->samplesframe = 576;
	unsigned long buffsize = ctx->samplesframe * ctx->samplesize * ctx->nchannels;
	warn("encoder FLAC config :\n" \
		"\tbuffer size %lu\n" \
		"\tsample rate %d\n" \
		"\tsample size %d\n" \
		"\tnchannels %u",
		buffsize,
		ctx->samplerate,
		ctx->samplesize,
		ctx->nchannels);
	jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, NB_BUFFERS, buffsize);
	ctx->in = jitter;
	jitter->format = format;
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

static void *_encoder_thread(void *arg)
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
	while (run)
	{
		int ret = 0;

		ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx, NULL);
		unsigned int inlength = ctx->in->ops->length(ctx->in->ctx);
		inlength /= ctx->samplesize * ctx->nchannels;
		if (inlength < ctx->samplesframe)
			warn("encoder: flac frame too small %d %ld", inlength, ctx->in->ctx->size);
		if (ctx->in->ctx->frequence != ctx->samplerate)
		{
			ctx->samplerate = ctx->in->ctx->frequence;
			encoder_flac_init(ctx);

			FLAC__StreamEncoderInitStatus init_status;
			init_status = FLAC__stream_encoder_init_stream(ctx->encoder, _encoder_writecb, NULL, NULL, NULL, ctx);
			if(init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
			{
				err("encoder: flac initializing encoder: %s\n", FLAC__StreamEncoderInitStatusString[init_status]);
				ret = -1;
			}
		}
		ctx->framescnt++;
		if (ctx->maxframes && ctx->framescnt > ctx->maxframes)
		{
			warn("encoder: max flac frames");
			encoder_flac_init(ctx);

			FLAC__StreamEncoderInitStatus init_status;
			init_status = FLAC__stream_encoder_init_stream(ctx->encoder, _encoder_writecb, NULL, NULL, NULL, ctx);
			if(init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
				err("encoder: flac initializing encoder: %s\n", FLAC__StreamEncoderInitStatusString[init_status]);
				ret = -1;
			}
		}

		if (ctx->inbuffer)
		{
			ret = FLAC__stream_encoder_process_interleaved(ctx->encoder,
					(int *)ctx->inbuffer, inlength);
			ctx->in->ops->pop(ctx->in->ctx, ctx->in->ctx->size);
		}
		if (ret < 0)
		{
			if (ret == -1)
				err("encoder: flac error %d, too small buffer %ld", ret, ctx->out->ctx->size);
			else
				err("encoder: flac error %d", ret);
			run = 0;
		}
	}
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
	int ret = 0;
	ctx->out = jitter;
	FLAC__StreamEncoderInitStatus init_status;
	init_status = FLAC__stream_encoder_init_stream(ctx->encoder, _encoder_writecb, NULL, NULL, NULL, ctx);
	if(init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		err("encoder: flac initializing encoder: %s\n", FLAC__StreamEncoderInitStatusString[init_status]);
		ret = -1;
	}

#ifdef ENCODER_HEARTBEAT
	heartbeat_samples_t config;
	config.samplerate = ctx->samplerate;
	config.format = ctx->in->format;
	ctx->heartbeat.ops = heartbeat_samples;
	ctx->heartbeat.ctx = ctx->heartbeat.ops->init(&config);
	int timeslot = ctx->samplesframe / config.samplerate;
	int bitrate = config.samplerate * ctx->samplesize * ctx->nchannels;
	dbg("set heart %s %uHz", jitter->ctx->name, ctx->samplerate);
	dbg("set heart %s %dbytes", jitter->ctx->name, ctx->samplesframe);
	dbg("set heart %s %dms %dkbps", jitter->ctx->name, timeslot, bitrate);
	jitter->ops->heartbeat(jitter->ctx, &ctx->heartbeat);
#endif
	if (ret == 0)
		pthread_create(&ctx->thread, NULL, _encoder_thread, ctx);
	return ret;
}

static const char *encoder_mime(encoder_ctx_t *encoder)
{
	return mime_audioflac;
}

static void encoder_destroy(encoder_ctx_t *ctx)
{
#ifdef ENCODER_DUMP
	if (ctx->dumpfd > 0)
		close(ctx->dumpfd);
#endif
	dbg("encoder: max buffer %lu", ctx->maxsize);
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
	FLAC__stream_encoder_finish(ctx->encoder);
	FLAC__stream_encoder_delete(ctx->encoder);
#ifdef ENCODER_HEARTBEAT
	ctx->heartbeat.ops->destroy(ctx->heartbeat.ctx);
#endif
	/* release the decoder */
	jitter_destroy(ctx->in);
	free(ctx);
}

const encoder_ops_t *encoder_flac = &(encoder_ops_t)
{
	.init = encoder_init,
	.type = ES_AUDIO,
	.jitter = encoder_jitter,
	.run = encoder_run,
	.mime = encoder_mime,
	.destroy = encoder_destroy,
};

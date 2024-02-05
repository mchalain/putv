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

#define LATENCY 100 //ms
/**
 * the samples frame must be up to 4608 bytes to be streamable
 */
//#define SAMPLES_FRAME 400
//#define NB_BUFFERS 6
//#define SAMPLES_FRAME 512 // too many missing packets
//#define NB_BUFFERS 60
#define SAMPLES_FRAME 4608 // missing packets with blank samples
#define NB_BUFFERS 21 // threshold 1/3 of buffers
#define MAX_SAMPLES (DEFAULT_SAMPLERATE * 60 * 2) // 2 minutes
#define HEARTBEAT_RATIO 1 // big samples_frame may contains a lot of blank and are sent too late

static const char *jitter_name = "flac encoder";

static size_t encoder_output(encoder_ctx_t *ctx, const unsigned char *buffer, size_t bytes, unsigned samples)
{
	if (bytes == 0)
		return 0;
	size_t length = 0;
	if (ctx->outbuffer == NULL)
	{
		ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
	}

	if (ctx->outbuffer != NULL)
	{
		length = (ctx->out->ctx->size > bytes)? bytes:ctx->out->ctx->size - 1;
		memcpy(ctx->outbuffer, buffer, length);

		encoder_dbg("encoder: flac %lu", length);
		ctx->outbuffer = NULL;

	}
	else
	{
		warn("encoder: jitter out closed");
		length = (size_t)-1;
	}
	return length;
}

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
		write(ctx->dumpfd, buffer, bytes);
#endif
//	if (samples == 0)
//		return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
	size_t orig = bytes / HEARTBEAT_RATIO;
	if (orig == 0)
		orig = 1;
	size_t length;
	while ( (length = encoder_output(ctx, buffer, bytes, samples)) > 0 )
	{
		if ((length == ((size_t) -1)))
			return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
		bytes -= length;
		buffer += length;
		beat_t beat = {0};
#ifdef ENCODER_HEARTBEAT
		if ((bytes % orig) == 0)
		{
			//ctx->heartbeat.ops->unlock(&ctx->heartbeat.ctx);
			beat.samples.nsamples = samples / HEARTBEAT_RATIO;
			//ctx->heartbeat.ops->unlock(&ctx->heartbeat.ctx);
		}
#endif
		ctx->out->ops->push(ctx->out->ctx, length, &beat);
	}
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static int encoder_flac_init(encoder_ctx_t *ctx, jitter_format_t format)
{
	int ret = 0;

	/** reinitialize the encoder **/
	//FLAC__stream_encoder_finish(ctx->encoder);

	ctx->samplesframe = SAMPLES_FRAME;
	ret = FLAC__stream_encoder_set_streamable_subset(ctx->encoder, true);
	if (ret)
		err("encoder: error with flac stream already set");
	FLAC__stream_encoder_set_verify(ctx->encoder, false);
	FLAC__stream_encoder_set_do_exhaustive_model_search(ctx->encoder, true);
	FLAC__stream_encoder_set_do_mid_side_stereo(ctx->encoder, true);
	FLAC__stream_encoder_set_sample_rate(ctx->encoder, ctx->samplerate);
	int samplesize = ctx->samplesize > 3? 24:ctx->samplesize * 8;
	FLAC__stream_encoder_set_bits_per_sample(ctx->encoder, samplesize);
	FLAC__stream_encoder_set_channels(ctx->encoder, ctx->nchannels);
	FLAC__stream_encoder_set_compression_level(ctx->encoder, 5);
	FLAC__stream_encoder_set_blocksize(ctx->encoder, ctx->samplesframe);
	FLAC__stream_encoder_set_max_lpc_order(ctx->encoder, 12);
	FLAC__stream_encoder_set_max_residual_partition_order(ctx->encoder, 8);

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
	//jitter_format_t format = PCM_24bits3_LE_stereo;
	//jitter_format_t format = PCM_16bits_LE_stereo;

	ctx->nchannels = FORMAT_NCHANNELS(format);
	ctx->samplerate = DEFAULT_SAMPLERATE;
	ctx->samplesize = FORMAT_SAMPLESIZE(format) / 8;
	
	// in streaminfg, the number of samples is clearly unknown => 0
	ctx->maxframes = 0;
	// otherwise
	// MAX_SAMPLES / SAMPLES_FRAME

	ctx->encoder = FLAC__stream_encoder_new();

	if (encoder_flac_init(ctx, format) < 0)
	{
		free(ctx);
		err("encoder: DISABLE flac error");
		return NULL;
	}
#ifdef ENCODER_DUMP
	ctx->dumpfd = open("dump.flac", O_RDWR | O_CREAT, 0644);
	err("dump %d", ctx->dumpfd);
#endif
	unsigned long buffsize = ctx->samplesframe * ctx->samplesize * ctx->nchannels;
	warn("encoder FLAC config :\n" \
		"\tbuffer size %lu\n" \
		"\tsamples frame %u\n" \
		"\tsample rate %d\n" \
		"\tsample size %d\n" \
		"\tnchannels %u",
		buffsize,
		ctx->samplesframe,
		ctx->samplerate,
		ctx->samplesize,
		ctx->nchannels);
	jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, NB_BUFFERS, buffsize);
	ctx->in = jitter;
	jitter->format = format;
	jitter->ctx->frequence = 0; // automatic freq
	jitter->ctx->thredhold = NB_BUFFERS / 3;

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
		size_t samplescnt = ctx->in->ops->length(ctx->in->ctx);
		samplescnt /= ctx->samplesize * ctx->nchannels;
		if (samplescnt < ctx->samplesframe)
			warn("encoder: flac frame too small %lu %ld", samplescnt, ctx->in->ctx->size);
		if (ctx->in->ctx->frequence != ctx->samplerate)
		{
#ifdef ENCODER_CHANGE_FRAMERATE
			ctx->samplerate = ctx->in->ctx->frequence;
			encoder_flac_init(ctx, ctx->in->format);

			FLAC__StreamEncoderInitStatus init_status;
			init_status = FLAC__stream_encoder_init_stream(ctx->encoder, _encoder_writecb, NULL, NULL, NULL, ctx);
			if(init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
			{
				err("encoder: flac initializing encoder: %s\n", FLAC__StreamEncoderInitStatusString[init_status]);
				ret = -1;
			}
#else
			/// refuse to change SAMPLE RATE
			err("encoder: impossible to change frame rate use %u", ctx->samplerate);
			player_state(ctx->player, STATE_CHANGE);
			ctx->in->ops->pop(ctx->in->ctx, ctx->in->ctx->size);
#endif

		}
		ctx->framescnt++;
		if (ctx->maxframes && ctx->framescnt > ctx->maxframes)
		{
			warn("encoder: max flac frames");
			encoder_flac_init(ctx, ctx->in->format);

			FLAC__StreamEncoderInitStatus init_status;
			init_status = FLAC__stream_encoder_init_stream(ctx->encoder, _encoder_writecb, NULL, NULL, NULL, ctx);
			if(init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
				err("encoder: flac initializing encoder: %s\n", FLAC__StreamEncoderInitStatusString[init_status]);
				ret = -1;
			}
		}

		if (ctx->inbuffer)
		{
			encoder_dbg("encoder: process %lu/%lu samples", ctx->samplesframe, samplescnt);
			ret = FLAC__stream_encoder_process_interleaved(ctx->encoder,
					(int *)ctx->inbuffer, samplescnt);
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
		err("encoder: flac initializing encoder: %s", FLAC__StreamEncoderInitStatusString[init_status]);
		FLAC__StreamEncoderState state;
		state = FLAC__stream_encoder_get_state(ctx->encoder);
		err("encoder: flac encoder state: %s", FLAC__StreamEncoderStateString[state]);
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

static const char *encoder_mime(encoder_ctx_t *ctx)
{
	return mime_audioflac;
}

static int encoder_samplerate(encoder_ctx_t *ctx)
{
	return ctx->samplerate;
}

static jitter_format_t encoder_format(encoder_ctx_t *ctx)
{
	return FLAC;
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
	.name = "flac",
	.init = encoder_init,
	.type = ES_AUDIO,
	.jitter = encoder_jitter,
	.run = encoder_run,
	.mime = encoder_mime,
	.samplerate = encoder_samplerate,
	.format = encoder_format,
	.destroy = encoder_destroy,
};

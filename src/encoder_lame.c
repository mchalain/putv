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
#include "jitter.h"
#include "heartbeat.h"
#include "media.h"

typedef struct encoder_ops_s encoder_ops_t;
typedef struct encoder_ctx_s encoder_ctx_t;
struct encoder_ctx_s
{
	const encoder_ops_t *ops;
	lame_global_flags *encoder;
	unsigned int samplerate;
	unsigned char nchannels;
	unsigned char samplesize;
	unsigned int samplesframe;
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

static const char *jitter_name = "lame encoder";
void error_report(const char *format, va_list ap)
{
	fprintf(stderr, format, ap);
}

static int encoder_lame_init(encoder_ctx_t *ctx, int samplerate, int samplesize, int nchannels)
{
	if (ctx->encoder)
		lame_close(ctx->encoder);
	ctx->encoder = lame_init();

	lame_set_in_samplerate(ctx->encoder, samplerate);
	ctx->samplerate = samplerate;
	lame_set_num_channels(ctx->encoder, nchannels);
	ctx->nchannels = nchannels;
	ctx->samplesize = samplesize / 8;
	// this value change the complexity and the time of compression
	// nothing else
	lame_set_quality(ctx->encoder, 5);
	lame_set_mode(ctx->encoder, LAME_NCHANNELS);
	lame_set_errorf(ctx->encoder, error_report);

	// for CBR encoding
	// 44100Hz the output buffer is between 1252 and 1254 for brate to 128
	// 48000Hz the output buffer is between 1536 and 1152 for brate to 128
	// 48000Hz the output buffer is between 1008 and 1344 for brate to 112
	lame_set_out_samplerate(ctx->encoder, DEFAULT_SAMPLERATE);
#ifdef ENCODER_VBR
	lame_set_VBR(ctx->encoder, vbr_default);
	lame_set_VBR_q(ctx->encoder, DEFAULT_BITRATE);
#else
	lame_set_VBR(ctx->encoder, vbr_off);
	lame_set_brate(ctx->encoder, DEFAULT_BITRATE);
#endif

	lame_set_disable_reservoir(ctx->encoder, 1);
	
	if (lame_init_params(ctx->encoder) <  0)
	{
		err("encoder: initialization error");
		return -1;
	}
	warn("encoder MP3 config :");
	warn("\tsample rate %d", ctx->samplerate);
	warn("\tsample size %d", ctx->samplesize);
	warn("\tnchannels %u", LAME_NCHANNELS);
#ifdef ENCODER_VBR
	warn("\tvariatic bitrate %d\n", DEFAULT_BITRATE);
#else
	warn("\bitrate %d\n", DEFAULT_BITRATE);
#endif
		
	return 0;
}

static encoder_ctx_t *encoder_init(player_ctx_t *player)
{
	encoder_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ops = encoder_lame;
	ctx->player = player;

	encoder_lame_init(ctx, DEFAULT_SAMPLERATE, FORMAT_SAMPLESIZE(INPUT_FORMAT), FORMAT_NCHANNELS(INPUT_FORMAT));
#if ENCODER_DUMP == 1
	ctx->dumpfd = open("lame_dump.mp3", O_RDWR | O_CREAT, 0644);
#endif
#if ENCODER_DUMP == 2
	ctx->dumpfd = open("lame_dump.wav", O_RDWR | O_CREAT, 0644);
#endif
	/**
	 * set samples frame to 3 framesize to have less than 1500 bytes
	 * but more than 1000 bytes into the output
	 */
	ctx->samplesframe = lame_get_maximum_number_of_samples(ctx->encoder, OUTPUT_AVERAGE);
	unsigned long buffsize = ctx->samplesframe * ctx->samplesize * ctx->nchannels;
	warn("\tsamples frame %u", ctx->samplesframe);
	warn("\tbuffer size %lu", buffsize);
	encoder_dbg("encoder: mp3 ready");
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

static void *lame_thread(void *arg)
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
#ifdef DEBUG
	lame_set_errorf(ctx->encoder, encoder_message);
	lame_set_debugf(ctx->encoder, encoder_message);
	lame_set_msgf(ctx->encoder, encoder_message);
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
		inlength /= ctx->samplesize * ctx->nchannels;
		nsamples += inlength;
		if (inlength < ctx->samplesframe)
			warn("encoder lame: frame too small %d %ld", inlength, ctx->in->ctx->size);
		if (ctx->in->ctx->frequence != ctx->samplerate)
		{
			warn("set lame samplerate %u",ctx->in->ctx->frequence);
			encoder_lame_init(ctx, ctx->in->ctx->frequence, ctx->samplesize * 8, ctx->nchannels);
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
			ret = lame_encode_buffer_interleaved(ctx->encoder,
					(short int *)ctx->inbuffer, inlength,
					ctx->outbuffer, ctx->out->ctx->size);
#if ENCODER_DUMP == 1
			if (ctx->dumpfd > 0 && ret > 0)
				write(ctx->dumpfd, ctx->outbuffer, ret);
#endif
			//ctx->in->ops->pop(ctx->in->ctx, ctx->in->ctx->size);
			ctx->in->ops->pop(ctx->in->ctx, inlength * ctx->samplesize * ctx->nchannels);
		}
		else
		{
			ret = lame_encode_flush_nogap(ctx->encoder, ctx->outbuffer, ctx->out->ctx->size);
			if (ret < 0)
				err("encoder: mp3 encoding error");
			/* TODO : request media data from player to set new ID3 tag */
			lame_init_bitstream(ctx->encoder);
		}
		if (ret > 0)
		{
			encoder_dbg("encoder lame %d", ret);
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
				err("lame error %d, too small buffer %ld", ret, ctx->out->ctx->size);
			else
				err("lame error %d", ret);
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
	config.bitrate = lame_get_brate(ctx->encoder);
	config.ms = jitter->ctx->size * jitter->ctx->count * 8 / config.bitrate;
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
	pthread_create(&ctx->thread, NULL, lame_thread, ctx);
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
	lame_close(ctx->encoder);
#ifdef ENCODER_HEARTBEAT
	ctx->heartbeat.ops->destroy(ctx->heartbeat.ctx);
#endif
	/* release the decoder */
	jitter_destroy(ctx->in);
	free(ctx);
}

const encoder_ops_t *encoder_lame = &(encoder_ops_t)
{
	.name = "lame",
	.init = encoder_init,
	.type = ES_AUDIO,
	.jitter = encoder_jitter,
	.run = encoder_run,
	.mime = encoder_mime,
	.samplerate = encoder_samplerate,
	.format = encoder_format,
	.destroy = encoder_destroy,
};

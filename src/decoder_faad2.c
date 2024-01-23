/*****************************************************************************
 * decoder_faad2.c
 * this file is part of https://github.com/ouistiti-project/putv
 *****************************************************************************
 * Copyright (C) 2022-2024
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
#include <fcntl.h>

#include <neaacdec.h>
#ifdef USE_ID3TAG
#include <id3tag.h>
#endif

#include "player.h"
#include "jitter.h"
#include "heartbeat.h"
#include "filter.h"
typedef struct decoder_s decoder_t;
typedef struct decoder_ops_s decoder_ops_t;
typedef struct decoder_ctx_s decoder_ctx_t;
struct decoder_ctx_s
{
	const decoder_ops_t *ops;
	NeAACDecHandle decoder;
	pthread_t thread;

	jitter_t *in;
	unsigned char *inbuffer;

	jitter_t *out;
	unsigned char *outbuffer;
	size_t outbufferlen;

	filter_t *filter;
	rescale_t rescale;
	player_ctx_t *player;

	uint32_t samplerate;

	heartbeat_t heartbeat;
	unsigned int nloops;
	int bitspersample;

#ifdef DECODER_DUMP
	int dumpfd;
#endif
};

#define DECODER_CTX
#include "decoder.h"
#include "media.h"
#include "event.h"
#include "src.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define decoder_dbg(...)

#ifdef HEARTBEAT_0
#define DECODER_HEARTBEAT
#endif

#define JITTER_TYPE JITTER_TYPE_RING
#define MAX_CHANNELS 6

static jitter_t *_decoder_jitter(decoder_ctx_t *ctx, jitte_t jitte);

#ifdef DEBUG
static clockid_t clockid = CLOCK_REALTIME;
static struct timespec start = {0, 0};
#endif

static uint32_t uint32(char *buff)
{
	return (buff[0] << 21) | (buff[1] << 14) |
		(buff[2] <<  7) | (buff[3] <<  0);
}

static int _faad_output(decoder_ctx_t *ctx, NeAACDecFrameInfo *frameInfo, void *samples)
{
	filter_audio_t audio;

	if (ctx->samplerate == 0)
	{
		ctx->samplerate = frameInfo->samplerate;
		warn("decoder: aac file samplerate %d Hz", ctx->samplerate);
	}
	else if (ctx->samplerate != frameInfo->samplerate)
	{
		ctx->samplerate = frameInfo->samplerate;
		warn("decoder: aac file samplerate %d Hz CHANGING", ctx->samplerate);
	}
	audio.samplerate = frameInfo->samplerate;
	audio.bitspersample = ctx->bitspersample;
	audio.regain = 0;
	/**
	 * faad return interleaved pcm.
	 * The filter needs to manage it as a monochannel stream
	 */
	audio.nchannels = frameInfo->channels;
	audio.nsamples = frameInfo->samples / frameInfo->channels;
	audio.samples[0] = samples;
	audio.mode = AUDIO_MODE_INTERLEAVED;
	decoder_dbg("decoder faad: info %d channels, %lu fps, %lu samples, %d bits", frameInfo->channels, frameInfo->samplerate, frameInfo->samples, audio.bitspersample);

	while (audio.nsamples > 0)
	{
		if (filter_filloutput(ctx->filter, &audio, ctx->out) < 0)
		{
			/**
			 * flush the src jitter to break the stream
			 */
			ctx->in->ops->flush(ctx->in->ctx);
			return -1;
		}
	}
	return 0;
}

static int _faad_parsetags(decoder_ctx_t *ctx)
{
	size_t len = 0;
	ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx, NULL);
	if (ctx->inbuffer == NULL)
		return -1;
	if (!memcmp(ctx->inbuffer, "ID3", 3))
	{
		len = uint32(ctx->inbuffer + 6);
		len += 10;
	}
	while (len > ctx->in->ops->length(ctx->in->ctx))
	{
		ctx->in->ops->pop(ctx->in->ctx, ctx->in->ops->length(ctx->in->ctx));
		len -= ctx->in->ops->length(ctx->in->ctx);
		ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx, NULL);
	}
	ctx->in->ops->pop(ctx->in->ctx, len);
	return 0;
}

static int _faad_initaac(decoder_ctx_t *ctx)
{
	size_t len = ctx->in->ctx->size;

	ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx, NULL);
	len = ctx->in->ops->length(ctx->in->ctx);
	unsigned long samplerate;
	unsigned char channels;
	len = NeAACDecInit(ctx->decoder, ctx->inbuffer,
		len, &samplerate, &channels);
	if (len == -1)
	{
		err("decoder faad: Initialisation error");
		return -1;
	}
	decoder_dbg("decoder faad: samplerate %lu fps, channels %d", samplerate, channels);
	ctx->in->ops->pop(ctx->in->ctx, len);
	return 0;
}

static int _faad_loop(decoder_ctx_t *ctx)
{
	int ret;
	size_t len = ctx->in->ctx->size;

	do
	{
		_faad_parsetags(ctx);
		ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx, NULL);
		if (ctx->inbuffer == NULL)
		{
			decoder_dbg("decoder faad: end of file");
			ret = -1;
			break;
		}
		len = ctx->in->ops->length(ctx->in->ctx);

		NeAACDecFrameInfo frameInfo = {0};
		void *samples = NeAACDecDecode(ctx->decoder, &frameInfo, ctx->inbuffer, len);
		decoder_dbg("decoder faad: decode %ld samples", frameInfo.samples);
		if (frameInfo.error > 0)
		{
			err("decoder faad: error %s", NeAACDecGetErrorMessage(frameInfo.error));
			ret = -1;
			break;
		}
#ifdef DECODER_DUMP
		write(ctx->dumpfd, samples, frameInfo.samples * 4);
#endif
		if (frameInfo.header_type == 2)
			ret = _faad_output(ctx, &frameInfo, samples);
		else
		{
			dbg("frame type %d", frameInfo.header_type);
		}
		ctx->in->ops->pop(ctx->in->ctx, frameInfo.bytesconsumed);
	} while(ret == 0);

	return ret;
}
#define BUFFERSIZE FAAD_MIN_STREAMSIZE*MAX_CHANNELS
/// NBBUFFER must be at least 3 otherwise the decoder block on the end of the source
#define NBUFFER 4

static const char *jitter_name = "faad decoder";

static decoder_ctx_t *_decoder_init(player_ctx_t *player)
{
	decoder_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ops = decoder_mad;
	ctx->player = player;

	ctx->decoder = NeAACDecOpen();
	return ctx;
}

static int _decoder_prepare(decoder_ctx_t *ctx, filter_t *filter, const char *info)
{
	decoder_dbg("decoder: prepare");
	ctx->filter = filter;
	return 0;
}

static jitter_t *_decoder_jitter(decoder_ctx_t *ctx, jitte_t jitte)
{
	if (ctx->in == NULL)
	{
		int factor = jitte;
		int nbbuffer = NBUFFER << factor;
		jitter_t *jitter = jitter_init(JITTER_TYPE, jitter_name, nbbuffer, BUFFERSIZE);
		jitter->format = MPEG4_AAC;
		jitter->ctx->thredhold = nbbuffer / 2;

		ctx->in = jitter;
	}
	return ctx->in;
}

static void *_decoder_thread(void *arg)
{
	int result = 0;
	decoder_ctx_t *ctx = (decoder_ctx_t *)arg;
	/* start decoding */
#ifdef DECODER_HEARTBEAT
	ctx->heartbeat.ops->start(ctx->heartbeat.ctx);
#endif
	dbg("decoder: start running");
#ifdef DECODER_DUMP
	ctx->dumpfd = open("./faad2_dump.wav", O_RDWR | O_CREAT, 0644);
#endif
#ifdef DEBUG
	clockid_t clockid = CLOCK_REALTIME;
	static struct timespec start;
	clock_gettime(clockid, &start);
#endif
	result = _faad_loop(ctx);
#ifdef DEBUG
	static struct timespec now;
	clock_gettime(clockid, &now);
	now.tv_sec -= start.tv_sec;
	if (now.tv_nsec > start.tv_nsec)
	{
		now.tv_nsec -= start.tv_nsec;
	}
	else
	{
		now.tv_nsec -= 1000000000 - start.tv_nsec;
		now.tv_sec -= 1;
	}
	dbg("decoder: during %lu.%09lu", now.tv_sec, now.tv_nsec);
#endif
	/**
	 * push the last buffer to the encoder, otherwise the next
	 * decoder will begins with a pull buffer
	 */
	if (ctx->outbufferlen > 0)
	{
		ctx->out->ops->push(ctx->out->ctx, ctx->outbufferlen, NULL);
	}
	dbg("decoder: stop running");
	player_state(ctx->player, STATE_CHANGE);
#ifdef DECODER_DUMP
	close(ctx->dumpfd);
#endif

	return (void *)(intptr_t)result;
}

static int _decoder_run(decoder_ctx_t *ctx, jitter_t *jitter)
{
	int ret = 0;
	ctx->out = jitter;
	if (ctx->filter)
	{
		rescale_init(&ctx->rescale, 0, jitter->format);
		ctx->filter->ops->set(ctx->filter->ctx, FILTER_SAMPLED, rescale_cb, &ctx->rescale, 0);
		ret = ctx->filter->ops->set(ctx->filter->ctx, FILTER_FORMAT, jitter->format, FILTER_SAMPLERATE, jitter_samplerate(jitter), 0);
	}
	NeAACDecConfigurationPtr conf = NeAACDecGetCurrentConfiguration(ctx->decoder);
	switch (jitter->format)
	{
	case PCM_8bits_mono:
	case PCM_16bits_LE_mono:
	case PCM_16bits_LE_stereo:
		conf->outputFormat = FAAD_FMT_16BIT;
		ctx->bitspersample = 16;
	break;
	case PCM_24bits3_LE_stereo:
		conf->outputFormat = FAAD_FMT_24BIT;
		ctx->bitspersample = 24;
	break;
	case PCM_24bits4_LE_stereo:
	case PCM_32bits_LE_stereo:
		conf->outputFormat = FAAD_FMT_32BIT;
		ctx->bitspersample = 32;
	break;
	}
	conf->defObjectType = 2;
	conf->defSampleRate = jitter_samplerate(jitter);
	NeAACDecSetConfiguration(ctx->decoder, conf);

#ifdef DECODER_HEARTBEAT
	if (heartbeat_samples)
	{
		heartbeat_samples_t config =
		{
			.samplerate = jitter_samplerate(jitter),
			.format = jitter->format,
			.nchannels = 0,
		};
		ctx->heartbeat.ops = heartbeat_samples;
		ctx->heartbeat.ctx = heartbeat_samples->init(&config);
		dbg("set heart %s", jitter->ctx->name);
		jitter->ops->heartbeat(jitter->ctx, &ctx->heartbeat);
	}
#endif
	if (ret == 0)
		ret = _faad_initaac(ctx);
	if (ret == 0)
		pthread_create(&ctx->thread, NULL, _decoder_thread, ctx);
	return ret;
}

static int _decoder_checkin(decoder_ctx_t *ctx, const char *path)
{
	char *ext = strrchr(path, '.');
	if (ext && !strcmp(ext, ".aac"))
		return 1;
#if 0
	if (ext && !strcmp(ext, ".m4a"))
		return 1;
	if (ext && !strcmp(ext, ".mp4"))
		return 1;
#endif
	return 0;
}

static const char *_decoder_mime(decoder_ctx_t *ctx)
{
	return mime_audioaac;
}

static int _decoder_checkout(decoder_ctx_t *ctx, jitter_format_t format)
{
	return (format & JITTER_AUDIO);
}

static void _decoder_destroy(decoder_ctx_t *ctx)
{
	if (ctx->out)
		ctx->out->ops->flush(ctx->out->ctx);
	if (ctx->thread > 0)
		pthread_join(ctx->thread, NULL);
	/* release the decoder */
	NeAACDecClose(ctx->decoder);
#ifdef DECODER_HEARTBEAT
	ctx->heartbeat.ops->destroy(ctx->heartbeat.ctx);
#endif
	if (ctx->filter)
	{
		ctx->filter->ops->destroy(ctx->filter->ctx);
		free(ctx->filter);
	}
	jitter_destroy(ctx->in);
	free(ctx);
}

const decoder_ops_t _decoder_faad2 =
{
	.name = "faad2",
	.checkin = _decoder_checkin,
	.init = _decoder_init,
	.prepare = _decoder_prepare,
	.checkout = _decoder_checkout,
	.jitter = _decoder_jitter,
	.run = _decoder_run,
	.destroy = _decoder_destroy,
	.mime = _decoder_mime,
};

const decoder_ops_t *decoder_faad2 = &_decoder_faad2;

#ifdef DECODER_MODULES
extern const decoder_ops_t decoder_ops __attribute__ ((weak, alias ("_decoder_faad2")));
#endif

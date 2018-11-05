/*****************************************************************************
 * decoder_opus.c
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

#include <FLAC/stream_decoder.h>

#include "player.h"
#include "filter.h"
typedef struct decoder_s decoder_t;
typedef struct decoder_ctx_s decoder_ctx_t;
struct decoder_ctx_s
{
	const decoder_t *ops;
	FLAC__StreamDecoder *decoder;
	int nchannels;
	int samplerate;
	pthread_t thread;
	jitter_t *in;
	unsigned char *inbuffer;
	jitter_t *out;
	unsigned char *outbuffer;
	size_t outbufferlen;
	filter_t filter;
};
#define DECODER_CTX
#include "decoder.h"
#include "jitter.h"
#include "filter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define decoder_dbg(...)

#define BUFFERSIZE 1500

#define NBUFFER 3

static const char *jitter_name = "flac decoder";
static decoder_ctx_t *decoder_init(player_ctx_t *player)
{
	decoder_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ops = decoder_flac;
	ctx->nchannels = 2;
	ctx->samplerate = DEFAULT_SAMPLERATE;

	ctx->decoder = FLAC__stream_decoder_new();
	if (ctx->decoder == NULL)
		err("flac decoder: open error");

	jitter_t *jitter = jitter_ringbuffer_init(jitter_name, NBUFFER, BUFFERSIZE);
	ctx->in = jitter;
	jitter->format = FLAC;

	return ctx;
}

static jitter_t *decoder_jitter(decoder_ctx_t *decoder)
{
	return decoder->in;
}

static FLAC__StreamDecoderReadStatus
input(const FLAC__StreamDecoder *decoder, 
			FLAC__byte buffer[], size_t *bytes,
			void *data)
{
	decoder_ctx_t *ctx = (decoder_ctx_t *)data;
	size_t len = ctx->in->ctx->size;

	ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx);
	if (ctx->inbuffer == NULL)
	{
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	len = ctx->in->ops->length(ctx->in->ctx);
	if (len > *bytes)
		len = *bytes;
	else
		*bytes = len;
	memcpy(buffer, ctx->inbuffer, len);
	ctx->in->ops->pop(ctx->in->ctx, len);

	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus
output(const FLAC__StreamDecoder *decoder,
	const FLAC__Frame *frame, const FLAC__int32 * const buffer[],
	void *data)
{
	decoder_ctx_t *ctx = (decoder_ctx_t *)data;
	filter_audio_t audio;

	/* pcm->samplerate contains the sampling frequency */

	audio.samplerate = FLAC__stream_decoder_get_sample_rate(decoder);
	if (ctx->out->ctx->frequence == 0)
	{
		decoder_dbg("decoder change samplerate to %u", audio.samplerate);
		ctx->out->ctx->frequence = audio.samplerate;
	}
	else if (ctx->out->ctx->frequence != audio.samplerate)
	{
		err("decoder: samplerate %d not supported", ctx->out->ctx->frequence);
	}

	audio.nchannels = FLAC__stream_decoder_get_channels(decoder);
	audio.nsamples = frame->header.blocksize;
	audio.bitspersample = FLAC__stream_decoder_get_bits_per_sample(decoder);
	int i;
	for (i = 0; i < audio.nchannels && i < MAXCHANNELS; i++)
		audio.samples[i] = (signed int *)buffer[i];
	decoder_dbg("decoder: audio frame %d Hz, %d channels, %d samples", audio.samplerate, audio.nchannels, audio.nsamples);

	unsigned int nsamples;
	if (audio.nchannels == 1)
		audio.samples[1] = audio.samples[0];
#ifdef FILTER
	while (audio.nsamples > 0)
	{
		if (ctx->outbuffer == NULL)
		{
			ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
		}

		int len =
			ctx->filter.ops->run(ctx->filter.ctx, &audio,
				ctx->outbuffer + ctx->outbufferlen,
				ctx->out->ctx->size - ctx->outbufferlen);
		ctx->outbufferlen += len;
		if (ctx->outbufferlen >= ctx->out->ctx->size)
		{
			ctx->out->ops->push(ctx->out->ctx, ctx->out->ctx->size, NULL);
			ctx->outbuffer = NULL;
			ctx->outbufferlen = 0;
		}
	}
#else
	for (i = 0; i < audio.nsamples; i++)
	{
		if (ctx->outbuffer == NULL)
		{
			ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
		}
		signed int sample;
		sample = audio.samples[0][i];
		
		*(ctx->outbuffer + ctx->outbufferlen + 0) = (sample >> (audio.bitspersample - 32) ) & 0x00FF;;
		*(ctx->outbuffer + ctx->outbufferlen + 1) = (sample >> (audio.bitspersample - 24) ) & 0x00FF;;
		*(ctx->outbuffer + ctx->outbufferlen + 2) = (sample >> (audio.bitspersample - 16) ) & 0x00FF;
		*(ctx->outbuffer + ctx->outbufferlen + 3) = (sample >> (audio.bitspersample - 8) ) & 0x00FF;
		ctx->outbufferlen += 4;
		if (audio.nchannels > 1)
			sample = audio.samples[1][i];
		*(ctx->outbuffer + ctx->outbufferlen + 0) = (sample >> (audio.bitspersample - 32) ) & 0x00FF;;
		*(ctx->outbuffer + ctx->outbufferlen + 1) = (sample >> (audio.bitspersample - 24) ) & 0x00FF;;
		*(ctx->outbuffer + ctx->outbufferlen + 2) = (sample >> (audio.bitspersample - 16) ) & 0x00FF;
		*(ctx->outbuffer + ctx->outbufferlen + 3) = (sample >> (audio.bitspersample - 8) ) & 0x00FF;
		ctx->outbufferlen += 4;
		if (ctx->outbufferlen >= ctx->out->ctx->size)
		{
			ctx->out->ops->push(ctx->out->ctx, ctx->out->ctx->size, NULL);
			ctx->outbuffer = NULL;
			ctx->outbufferlen = 0;
		}
	}
#endif
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void
metadata(const FLAC__StreamDecoder *decoder,
	const FLAC__StreamMetadata *metadata,
	void *client_data)
{
}

static void
error(const FLAC__StreamDecoder *decoder,
	FLAC__StreamDecoderErrorStatus status,
	void *data)

{
}

static void *decoder_thread(void *arg)
{
	int result = 0;
	decoder_ctx_t *ctx = (decoder_ctx_t *)arg;
	result = FLAC__stream_decoder_init_stream(ctx->decoder,
		input,
		NULL,
		NULL,
		NULL,
		NULL,
		output,
		metadata,
		error,
		ctx);
	result = FLAC__stream_decoder_process_until_end_of_stream(ctx->decoder);
	/**
	 * push the last buffer to the encoder, otherwise the next
	 * decoder will begins with a pull buffer
	 */
	if (ctx->outbufferlen > 0)
	{
		ctx->out->ops->push(ctx->out->ctx, ctx->outbufferlen, NULL);
	}
	ctx->out->ops->flush(ctx->out->ctx);

	return (void *)result;
}

static int decoder_check(char *path)
{
	char *ext = strrchr(path, '.');
	if (ext)
		return strcmp(ext, ".flac");
	return -1;
}

static int decoder_run(decoder_ctx_t *ctx, jitter_t *jitter)
{
	int samplesize = 4;
	int nchannels = 2;
	ctx->out = jitter;
	switch (ctx->out->format)
	{
	case PCM_16bits_LE_mono:
		samplesize = 2;
		nchannels = 1;
	break;
	case PCM_16bits_LE_stereo:
		samplesize = 2;
		nchannels = 2;
	break;
	case PCM_24bits_LE_stereo:
		samplesize = 3;
		nchannels = 2;
	break;
	case PCM_32bits_BE_stereo:
	case PCM_32bits_LE_stereo:
		samplesize = 4;
		nchannels = 2;
	break;
	default:
		err("decoder out format not supported %d", ctx->out->format);
		return -1;
	}
#ifdef FILTER
	ctx->filter.ops = filter_pcm;
	ctx->filter.ctx = ctx->filter.ops->init(jitter->ctx->frequence, samplesize, nchannels);
#endif
	pthread_create(&ctx->thread, NULL, decoder_thread, ctx);
	return 0;
}

static void decoder_destroy(decoder_ctx_t *ctx)
{
	pthread_join(ctx->thread, NULL);
	/* release the decoder */
	FLAC__stream_decoder_delete(ctx->decoder);
#ifdef FILTER
	ctx->filter.ops->destroy(ctx->filter.ctx);
#endif
	jitter_ringbuffer_destroy(ctx->in);
	free(ctx);
}

const decoder_t *decoder_flac = &(decoder_t)
{
	.check = decoder_check,
	.init = decoder_init,
	.jitter = decoder_jitter,
	.run = decoder_run,
	.destroy = decoder_destroy,
	.mime = "audio/flac",
};

#ifndef DECODER_GET
#define DECODER_GET
const decoder_t *decoder_get(decoder_ctx_t *ctx)
{
	return ctx->ops;
}
#endif

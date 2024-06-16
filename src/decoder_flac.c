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
typedef struct decoder_ops_s decoder_ops_t;
typedef struct decoder_ctx_s decoder_ctx_t;
struct decoder_ctx_s
{
	FLAC__StreamDecoder *decoder;
	int nchannels;
	uint32_t samplerate;
	pthread_t thread;
	jitter_t *in;
	unsigned char *inbuffer;
	jitter_t *out;
	unsigned char *outbuffer;
	size_t outbufferlen;
	filter_t *filter;
	player_ctx_t *player;
	uint32_t nsamples;
	uint32_t position;
	uint32_t duration;
	rescale_t rescale;
};
#define DECODER_CTX
#include "decoder.h"
#include "media.h"
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

#define BUFFERSIZE 16500

#define NBUFFER 4

static const char *jitter_name = "flac decoder";
static decoder_ctx_t *_decoder_init(player_ctx_t *player)
{
	decoder_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->nchannels = 2;
	ctx->samplerate = DEFAULT_SAMPLERATE;
	ctx->player = player;

	ctx->decoder = FLAC__stream_decoder_new();
	if (ctx->decoder == NULL)
	{
		err("flac decoder: open error");
		free(ctx);
		ctx = NULL;
	}

	return ctx;
}

static jitter_t *_decoder_jitter(decoder_ctx_t *ctx, jitte_t jitte)
{
	if (ctx->in == NULL)
	{
		int factor = jitte;
		int nbbuffer = NBUFFER << factor;
		jitter_t *jitter = jitter_init(JITTER_TYPE_RING, jitter_name, nbbuffer, BUFFERSIZE);
		jitter->ctx->thredhold = nbbuffer / 2;
		jitter->format = FLAC;
		ctx->in = jitter;
	}
	return ctx->in;
}

static FLAC__StreamDecoderReadStatus
input_cb(const FLAC__StreamDecoder *decoder,
			FLAC__byte buffer[], size_t *bytes,
			void *data)
{
	decoder_ctx_t *ctx = (decoder_ctx_t *)data;
	size_t len = ctx->in->ctx->size;

	ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx, NULL);
	if (ctx->inbuffer == NULL)
	{
		*bytes = 0;
		decoder_dbg("decoder flac: end of file");
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
output_cb(const FLAC__StreamDecoder *decoder,
	const FLAC__Frame *frame, const FLAC__int32 * const buffer[],
	void *data)
{
	decoder_ctx_t *ctx = (decoder_ctx_t *)data;
	filter_audio_t audio;

	/* pcm->samplerate contains the sampling frequency */

	uint32_t samplerate = FLAC__stream_decoder_get_sample_rate(decoder);
	if (ctx->samplerate == 0)
	{
		ctx->samplerate = samplerate;
		warn("decoder: flac file samplerate %d Hz", ctx->samplerate);
	}
	else if (ctx->samplerate != samplerate)
	{
		ctx->samplerate = samplerate;
		warn("decoder: flac file samplerate %d Hz CHANGING", ctx->samplerate);
	}
	audio.samplerate = samplerate;
	audio.nchannels = FLAC__stream_decoder_get_channels(decoder);
	audio.nsamples = frame->header.blocksize;
	audio.bitspersample = FLAC__stream_decoder_get_bits_per_sample(decoder);
	audio.regain = 0;
	audio.mode = 0;
	int i;
	for (i = 0; i < audio.nchannels && i < MAXCHANNELS; i++)
		audio.samples[i] = (sample_t *)buffer[i];
	decoder_dbg("decoder: audio frame %d Hz, %d channels, %d samples size %d bits", audio.samplerate, audio.nchannels, audio.nsamples, audio.bitspersample);

	if (audio.nchannels == 1)
		audio.samples[1] = audio.samples[0];

	ctx->nsamples += audio.nsamples;
	if (ctx->nsamples == ctx->samplerate)
	{
		ctx->position++;
		ctx->nsamples = 0;
	}
	while (audio.nsamples > 0)
	{
		if (filter_filloutput(ctx->filter, &audio, ctx->out) < 0)
		{
			dbg("decoder: end of flac stream");
			/**
			 * flush the src jitter to break the stream
			 */
			ctx->in->ops->flush(ctx->in->ctx);
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
	}

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static FLAC__StreamDecoderLengthStatus
length_cb(const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length, void *client_data)
{
	return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
}

static void
metadata_cb(const FLAC__StreamDecoder *decoder,
	const FLAC__StreamMetadata *metadata,
	void *data)
{
}

static void
error_cb(const FLAC__StreamDecoder *decoder,
	FLAC__StreamDecoderErrorStatus status,
	void *data)
{
}

static void *_decoder_thread(void *arg)
{
	int result = 0;
	decoder_ctx_t *ctx = (decoder_ctx_t *)arg;
	dbg("decoder: start running");
	result = FLAC__stream_decoder_process_until_end_of_stream(ctx->decoder);
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

	return (void *)(intptr_t)result;
}

static int _decoder_checkin(decoder_ctx_t *ctx, const char *path)
{
	char *ext = strrchr(path, '.');
	if (ext)
		return !strcmp(ext, ".flac");
	return 0;
}

static int _decoder_prepare(decoder_ctx_t *ctx, filter_t *filter, const char *info)
{
	decoder_dbg("decoder: prepare");
	ctx->filter = filter;

	int ret;
	FLAC__stream_decoder_set_metadata_ignore(ctx->decoder, FLAC__METADATA_TYPE_PADDING);
	FLAC__stream_decoder_set_metadata_ignore(ctx->decoder, FLAC__METADATA_TYPE_VORBIS_COMMENT);
	FLAC__stream_decoder_set_metadata_ignore(ctx->decoder, FLAC__METADATA_TYPE_CUESHEET);
	FLAC__stream_decoder_set_metadata_ignore(ctx->decoder, FLAC__METADATA_TYPE_PICTURE);
	ret = FLAC__stream_decoder_init_stream(ctx->decoder,
		input_cb,
		NULL,
		NULL,
		length_cb,
		NULL,
		output_cb,
		metadata_cb,
		error_cb,
		ctx);
#ifdef DECODER_FLAC_PROCESS_METADATA
	/**
	 * the metadata are available with a file not with a network stream
	 * The search of metadata on a network stream will be never completed
	 */
	if (ret == FLAC__STREAM_DECODER_INIT_STATUS_OK)
	{
		ret != FLAC__stream_decoder_process_until_end_of_metadata(ctx->decoder);
	}
#endif
	return ret;
}

static int _decoder_run(decoder_ctx_t *ctx, jitter_t *jitter)
{
	int ret = 0;
	ctx->out = jitter;
	/**
	 * Initialization of the filter here.
	 * Because we need the jitter out.
	 */
	if (ctx->filter != NULL)
	{
		rescale_init(&ctx->rescale, 0, jitter->format);
		ctx->filter->ops->set(ctx->filter->ctx, FILTER_SAMPLED, rescale_cb, &ctx->rescale, 0);
		ret = ctx->filter->ops->set(ctx->filter->ctx, FILTER_FORMAT, jitter->format, FILTER_SAMPLERATE, jitter_samplerate(jitter), 0);
	}
	if (ret == 0)
		pthread_create(&ctx->thread, NULL, _decoder_thread, ctx);
	return ret;
}

static const char *_decoder_mime(decoder_ctx_t *ctx)
{
	return mime_audioflac;
}

static int _decoder_checkout(decoder_ctx_t *ctx, jitter_format_t format)
{
	return (format & JITTER_AUDIO);
}

static uint32_t _decoder_position(decoder_ctx_t *ctx)
{
	return ctx->position;
}

static uint32_t _decoder_duration(decoder_ctx_t *ctx)
{
	return ctx->duration;
}

static void _decoder_destroy(decoder_ctx_t *ctx)
{
	if (ctx->out)
		ctx->out->ops->flush(ctx->out->ctx);
	if (ctx->thread > 0)
		pthread_join(ctx->thread, NULL);
	/* release the decoder */
	FLAC__stream_decoder_delete(ctx->decoder);
	jitter_destroy(ctx->in);
	if (ctx->filter)
	{
		ctx->filter->ops->destroy(ctx->filter->ctx);
		free(ctx->filter);
	}
	free(ctx);
}

static const decoder_ops_t _decoder_flac =
{
	.name = "flac" ,
	.checkin = _decoder_checkin,
	.init = _decoder_init,
	.prepare = _decoder_prepare,
	.checkout = _decoder_checkout,
	.jitter = _decoder_jitter,
	.run = _decoder_run,
	.mime = _decoder_mime,
	.position = _decoder_position,
	.duration = _decoder_duration,
	.destroy = _decoder_destroy,
};

const decoder_ops_t *decoder_flac = &_decoder_flac;

#ifdef DECODER_MODULES
extern const decoder_ops_t decoder_ops __attribute__ ((weak, alias ("_decoder_flac")));
#endif

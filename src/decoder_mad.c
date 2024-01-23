/*****************************************************************************
 * decoder_mad.c
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

#include <mad.h>
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
	struct mad_decoder decoder;
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
	beat_samples_t beat;
	mad_timer_t position;
	unsigned int nloops;
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

/**
 * FRACBITS value comes from mad decoder
 */
#define FRACBITS		28
#define JITTER_TYPE JITTER_TYPE_RING

static jitter_t *_decoder_jitter(decoder_ctx_t *ctx, jitte_t jitte);

static
enum mad_flow input(void *data,
		    struct mad_stream *stream)
{
	decoder_ctx_t *ctx = (decoder_ctx_t *)data;
	if (ctx->in == NULL)
	{
		err("decoder mad: input stream error");
		return MAD_FLOW_BREAK;
	}
	size_t len = ctx->in->ctx->size;

	if (stream->next_frame)
		len = stream->next_frame - ctx->inbuffer;
	ctx->in->ops->pop(ctx->in->ctx, len);

	ctx->inbuffer = ctx->in->ops->peer(ctx->in->ctx, NULL);

	len = ctx->in->ops->length(ctx->in->ctx);
	if (ctx->inbuffer == NULL)
	{
		return MAD_FLOW_STOP;
	}
	//input is called for each frame when there is
	// not enought data to decode the "next frame"
	if (stream->next_frame)
		stream->next_frame = ctx->inbuffer;

	decoder_dbg("decoder mad: data len %ld", len);
	mad_stream_buffer(stream, ctx->inbuffer, len);

	return MAD_FLOW_CONTINUE;
}

#ifdef DEBUG
static clockid_t clockid = CLOCK_REALTIME;
static struct timespec start = {0, 0};
#endif

static
enum mad_flow output(void *data,
		     struct mad_header const *header,
		     struct mad_pcm *pcm)
{
	decoder_ctx_t *ctx = (decoder_ctx_t *)data;
	filter_audio_t audio;

	/* pcm->samplerate contains the sampling frequency */

#ifdef DEBUG
	if (start.tv_nsec == 0)
	{
		clock_gettime(clockid, &start);
	}
#endif
#ifdef DEBUG
	unsigned long duration = (header->duration.fraction);
	duration /= (MAD_TIMER_RESOLUTION / 100000);
	decoder_dbg("duration 1 %lu.%02lums", duration / 100, duration % 100);
	duration = pcm->length * 1000;
	duration /= (pcm->samplerate / 100);
	decoder_dbg("duration 2 %lu.%02lums", duration / 100, duration % 100);
#endif

	if (ctx->samplerate == 0)
	{
		ctx->samplerate = pcm->samplerate;
		warn("decoder: mp3 file samplerate %d Hz", ctx->samplerate);
	}
	else if (ctx->samplerate != pcm->samplerate)
	{
		ctx->samplerate = pcm->samplerate;
		warn("decoder: mp3 file samplerate %d Hz CHANGING", ctx->samplerate);
	}
	audio.samplerate = pcm->samplerate;
	audio.nchannels = pcm->channels;
	audio.nsamples = pcm->length;
	audio.bitspersample = FRACBITS;
	audio.mode = 0;
	int i;
	for (i = 0; i < audio.nchannels && i < MAXCHANNELS; i++)
	{
		audio.samples[i] = pcm->samples[i];
	}
	decoder_dbg("decoder mad: audio frame %d Hz, %d channels, %d samples", audio.samplerate, audio.nchannels, audio.nsamples);

	if (audio.nchannels == 1)
		audio.samples[1] = audio.samples[0];

	while (audio.nsamples > 0)
	{
		if (filter_filloutput(ctx->filter, &audio, ctx->out) < 0)
		{
			/**
			 * flush the src jitter to break the stream
			 */
			ctx->in->ops->flush(ctx->in->ctx);
			return MAD_FLOW_STOP;
		}
	}

	return MAD_FLOW_CONTINUE;
}

static
enum mad_flow error(void *data,
		    struct mad_stream *stream,
		    struct mad_frame *frame)
{
	if (MAD_RECOVERABLE(stream->error))
	{
#ifdef USE_ID3TAG
		if (stream->error == MAD_ERROR_LOSTSYNC)
		{
			signed long tagsize;
			tagsize = id3_tag_query(stream->this_frame,
					stream->bufend - stream->this_frame);
			if (tagsize > 0)
			{
#ifdef PROCESS_ID3
				struct id3_tag *tag;

				tag = get_id3(stream, tagsize, decoder);
				if (tag) {
					//process_id3(tag, player);
					id3_tag_delete(tag);
				}
#else
				mad_stream_skip(stream, tagsize);
#endif
			}
		}
		else
#endif
			decoder_dbg("decoder mad: error 0x%04x (%s) at byte offset %p",
				stream->error, mad_stream_errorstr(stream),
				stream->this_frame );
		return MAD_FLOW_CONTINUE;
	}
	else
	{
		err("decoder mad: error 0x%04x (%s) at byte offset %p",
			stream->error, mad_stream_errorstr(stream),
			stream->this_frame );
		return MAD_FLOW_BREAK;
	}
}
enum mad_flow header(void *data, struct mad_header const *header)
{
	decoder_ctx_t *ctx = (decoder_ctx_t *)data;
	decoder_dbg("decoder mad: audio header mpeg1layer%d, flag 0x%x", header->layer, header->flags);
	decoder_dbg("decoder mad: bitrate %d , samplerate %d", header->bitrate, header->samplerate);
	mad_timer_add(&ctx->position, header->duration);
	return MAD_FLOW_CONTINUE;
}

/// MAD_BUFFER_MDLEN is too small on ARM device
#define BUFFERSIZE 2881
//#define BUFFERSIZE MAD_BUFFER_MDLEN

/// NBBUFFER must be at least 3 otherwise the decoder block on the end of the source
#define NBUFFER 4

static const char *jitter_name = "mad decoder";

static decoder_ctx_t *_decoder_init(player_ctx_t *player)
{
	decoder_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->ops = decoder_mad;
	ctx->player = player;

	mad_decoder_init(&ctx->decoder, ctx,
			input, header /* header */, 0 /* filter */, output,
			error, 0 /* message */);
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
		jitter->format = MPEG2_3_MP3;
		jitter->ctx->thredhold = nbbuffer / 2;

		ctx->in = jitter;
	}
	return ctx->in;
}

static void *mad_thread(void *arg)
{
	int result = 0;
	decoder_ctx_t *ctx = (decoder_ctx_t *)arg;
	/* start decoding */
#ifdef DECODER_HEARTBEAT
	ctx->heartbeat.ops->start(ctx->heartbeat.ctx);
#endif
	dbg("decoder: start running");
	result = mad_decoder_run(&ctx->decoder, MAD_DECODER_MODE_SYNC);
#ifdef DEBUG
	clockid_t clockid = CLOCK_REALTIME;
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
	dbg("decoder: end %lu.%09lu", now.tv_sec, now.tv_nsec);
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

	return (void *)(intptr_t)result;
}

static int _decoder_run(decoder_ctx_t *ctx, jitter_t *jitter)
{
	int ret = 0;
	ctx->out = jitter;
	if (ctx->filter)
	{
		if (jitter->format > JITTER_24BITS4_INTERLEAVED)
			rescale_init(&ctx->rescale, 24, 0);
		else
			rescale_init(&ctx->rescale, 0, jitter->format);
		ctx->filter->ops->set(ctx->filter->ctx, FILTER_SAMPLED, rescale_cb, &ctx->rescale, 0);
		ret = ctx->filter->ops->set(ctx->filter->ctx, FILTER_FORMAT, jitter->format, FILTER_SAMPLERATE, jitter_samplerate(jitter), 0);
	}
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
		pthread_create(&ctx->thread, NULL, mad_thread, ctx);
	return ret;
}

static int _decoder_checkin(decoder_ctx_t *ctx, const char *path)
{
	char *ext = strrchr(path, '.');
	if (ext)
		return !strcmp(ext, ".mp3");
	return 0;
}

static const char *_decoder_mime(decoder_ctx_t *ctx)
{
	return mime_audiomp3;
}

static int _decoder_checkout(decoder_ctx_t *ctx, jitter_format_t format)
{
	return (format & JITTER_AUDIO);
}

static uint32_t _decoder_position(decoder_ctx_t *ctx)
{
	uint32_t position = mad_timer_count(ctx->position, 0);
	return position;
}

static uint32_t _decoder_duration(decoder_ctx_t *ctx)
{
	return 0;
}

static void _decoder_destroy(decoder_ctx_t *ctx)
{
	if (ctx->out)
		ctx->out->ops->flush(ctx->out->ctx);
	if (ctx->thread > 0)
		pthread_join(ctx->thread, NULL);
	/* release the decoder */
	mad_decoder_finish(&ctx->decoder);
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

const decoder_ops_t _decoder_mad =
{
	.name = "mad",
	.checkin = _decoder_checkin,
	.init = _decoder_init,
	.prepare = _decoder_prepare,
	.checkout = _decoder_checkout,
	.jitter = _decoder_jitter,
	.run = _decoder_run,
	.position = _decoder_position,
	.duration = _decoder_duration,
	.destroy = _decoder_destroy,
	.mime = _decoder_mime,
};

const decoder_ops_t *decoder_mad = &_decoder_mad;

#ifdef DECODER_MODULES
extern const decoder_ops_t decoder_ops __attribute__ ((weak, alias ("_decoder_mad")));
#endif

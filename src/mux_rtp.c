/*****************************************************************************
 * mux_rtp.c
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
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "player.h"
#include "encoder.h"
#include "rtp.h"
typedef struct mux_s mux_t;
typedef struct mux_ops_s mux_ops_t;
typedef struct mux_ctx_s mux_ctx_t;
typedef struct mux_estream_s
{
	const char *mime;
	jitter_t *in;
	unsigned char pt;
	void *ext;
	uint16_t extlen;
} mux_estream_t;
#define MAX_ESTREAM 2
struct mux_ctx_s
{
	player_ctx_t *ctx;
	mux_estream_t estreams[MAX_ESTREAM];
	jitter_t *out;
	rtpheader_t header;
	pthread_t thread;
};
#define MUX_CTX
#include "mux.h"
#include "media.h"
#include "jitter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define mux_dbg(...)

#define LATENCE_MS 5

static const char *jitter_name = "rtp muxer";
static mux_ctx_t *mux_init(player_ctx_t *player, const char *search)
{
	mux_ctx_t *ctx = calloc(1, sizeof(*ctx));
	int i;
	while (search)
	{
		for (i = 0; i < MAX_ESTREAM && ctx->estreams[i].pt != 0; i++);
		const char *mime = strstr(search, "mime=");
		if (mime)
		{
			mime += 5;
			mime = utils_mime2mime(mime);
		}
		else
			mime = mime_octetstream;
		unsigned char pt = 0;
		const char *ptstr = strstr(search, "pt=");
		if (ptstr)
		{
			ptstr += 3;
			pt = atoi(ptstr);
		}
		if (i < MAX_ESTREAM && pt != 0)
		{
			ctx->estreams[i].pt = pt;
			ctx->estreams[i].mime = mime;
		}
		search = ptstr;
	}

	ctx->header.b.v = 2;
	ctx->header.b.p = 0;
	ctx->header.b.x = 0;
	ctx->header.b.cc = 0;
	ctx->header.b.m = 1;
	ctx->header.b.seqnum = random();
	ctx->header.timestamp = random();
	ctx->header.ssrc = random();

	return ctx;
}

static jitter_t *mux_jitter(mux_ctx_t *ctx, unsigned int index)
{
	if (index < MAX_ESTREAM && ctx->estreams[index].pt != 0)
	{
		return ctx->estreams[index].in;
	}
	return NULL;
}

static int _mux_run(mux_ctx_t *ctx, mux_estream_t *estream)
{
	void *beat = NULL;
	char *inbuffer;
	jitter_t *in = estream->in;
	inbuffer = in->ops->peer(in->ctx, &beat);
	unsigned long inlength = in->ops->length(in->ctx);
	if (inbuffer != NULL)
	{
		int len = sizeof(ctx->header);
		char *outbuffer = ctx->out->ops->pull(ctx->out->ctx);

		mux_dbg("mux: rtp seqnum %d", ctx->header.b.seqnum);
		ctx->header.b.pt = estream->pt;
		// copy header
		memcpy(outbuffer, &ctx->header, len);
		ctx->header.b.m = 0;
		ctx->header.b.seqnum++;
		if (ctx->header.b.seqnum == UINT16_MAX)
		{
			ctx->header.b.seqnum = 0;
			ctx->header.b.m = 1;
		}
		if (estream->extlen > 0 && estream->ext)
		{
			ctx->header.b.x = 1;
			memcpy(outbuffer + len, estream->ext, estream->extlen);
			len += estream->extlen;
		}
#ifdef DEBUG_0
		int i;
		fprintf(stderr, "header: ");
		for (i = 0; i < len; i++)
			fprintf(stderr, "%.2hhx ", outbuffer[i]);
		fprintf(stderr, "\n");
#endif
		// copy payload
		memcpy(outbuffer + len, inbuffer, inlength);
		len += inlength;

		ctx->out->ops->push(ctx->out->ctx, len, beat);
		in->ops->pop(in->ctx, inlength);
	}
	return 1;
}

static void *mux_thread(void *arg)
{
	int result = 0;
	int run = 1;
	mux_ctx_t *ctx = (mux_ctx_t *)arg;
	heartbeat_t *heart = NULL;
	int heartset = 0;
	for (int i = 0; i < MAX_ESTREAM && ctx->estreams[i].in != NULL; i++)
	{
		warn("mux: rtp stream %u %u %s",ctx->header.ssrc, ctx->estreams[i].pt, ctx->estreams[i].mime);
	}
	mux_dbg("mux: rtp thread start");
	while (run)
	{
		run = 0;
		for (int i = 0; i < MAX_ESTREAM && ctx->estreams[i].in != NULL; i++)
		{
			jitter_t *in = ctx->estreams[i].in;
			if (heart == NULL)
				heart = in->ops->heartbeat(in->ctx, NULL);
			if (!heartset && heart != NULL)
			{
				ctx->out->ops->heartbeat(ctx->out->ctx, heart);
				heartset = 1;
			}
			run = _mux_run(ctx, &ctx->estreams[i]);
		}
		if (run == 0)
		{
			sched_yield();
			usleep(LATENCE_MS * 1000);
			run = 1;
		}
	}
	mux_dbg("mux: rtp thread end");
	return (void *)(intptr_t)result;
}

static int mux_run(mux_ctx_t *ctx, jitter_t *sink_jitter)
{
	ctx->out = sink_jitter;
	pthread_create(&ctx->thread, NULL, mux_thread, ctx);
	return 0;
}

static unsigned int mux_attach(mux_ctx_t *ctx, encoder_t * encoder)
{
	const char *mime = encoder->ops->mime(encoder->ctx);
	if (ctx->out == NULL)
		return (unsigned int)-1;
	int i;
	for (i = 0; i < MAX_ESTREAM; i++)
	{
		if (ctx->estreams[i].pt == 0)
			break;
		if (!strcmp(ctx->estreams[i].mime, mime))
			break;
	}
	if ( i < MAX_ESTREAM)
	{
	//	int size = ctx->out->ctx->size - sizeof(ctx->header) - sizeof(uint32_t);
		int size = ctx->out->ctx->size - sizeof(ctx->header);
		unsigned char pt;
		jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, 6, size);
		jitter->ctx->frequence = encoder->ops->samplerate(encoder->ctx);
		jitter->ctx->thredhold = 3;
		jitter->format = encoder->ops->format(encoder->ctx);
		if (mime == mime_audiomp3)
		{
			pt = 14;
		}
		else if (mime == mime_audiopcm)
		{
			pt = 11;
			ctx->estreams[i].extlen = sizeof(rtpext_pcm_t);
			ctx->estreams[i].ext = calloc(1, ctx->estreams[i].extlen);
			rtpext_pcm_t *ext = (rtpext_pcm_t *)ctx->estreams[i].ext;
			ext->format = jitter->format;
			ext->samplerate = jitter->ctx->frequence;
		}
		else if (mime == mime_audioflac)
		{
			pt = 46;
		}
		else
		{
			pt = 99;
		}
		ctx->estreams[i].in = jitter;
		if (ctx->estreams[i].pt == 0)
			ctx->estreams[i].pt = pt;
		ctx->estreams[i].mime = mime;
		warn("mux: rtp attach %s to pt %d", mime, ctx->estreams[i].pt);
	}
	return 0;
}

static const char *mux_mime(mux_ctx_t *ctx, unsigned int index)
{
	if (index < MAX_ESTREAM)

		return ctx->estreams[index].mime;
	return NULL;
}

static void mux_destroy(mux_ctx_t *ctx)
{
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
	for (int i = 0; i < MAX_ESTREAM && ctx->estreams[i].pt != 0; i++)
	{
		if (ctx->estreams[i].ext)
			free(ctx->estreams[i].ext);
		jitter_destroy(ctx->estreams[i].in);
	}
	free(ctx);
}

const mux_ops_t *mux_rtp = &(mux_ops_t)
{
	.init = mux_init,
	.jitter = mux_jitter,
	.run = mux_run,
	.attach = mux_attach,
	.mime = mux_mime,
	.protocol = "rtp",
	.destroy = mux_destroy,
};

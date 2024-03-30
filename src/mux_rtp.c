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
#include <byteswap.h>

#include "player.h"
#include "encoder.h"
#include "heartbeat.h"
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
	void *buffer;
	heartbeat_t heartbeat;
	rtpheader_t header;
	rtpext_putvctrl_t *putvctrl;
	pthread_t thread;
	uint32_t ssrc;
	uint16_t seqnum;
	struct timespec timestamp;
	uint16_t volume;
	int mode;
};
#define MUX_CTX
#include "mux.h"
#include "media.h"
#include "jitter.h"
#include "heartbeat.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define mux_dbg(...)

#define LATENCE_MS 5
#define MUXJITTER_SIZE 8
#define MUX_DOUBLESSRC 0x01

static const char *jitter_name = "rtp muxer";
static void _mux_player_cb(void *arg, event_t event, void *data)
{
	if (event == PLAYER_EVENT_CHANGE)
	{
		mux_ctx_t *ctx = (mux_ctx_t *)arg;
		event_player_state_t *edata = (event_player_state_t *)data;
		if (edata->state == STATE_STOP)
		{
			ctx->putvctrl = calloc(1, sizeof(*ctx->putvctrl));
			ctx->putvctrl->version = PUTVCTRL_VERSION;
			ctx->putvctrl->ncmds = 1;
			ctx->putvctrl->cmd.id = PUTVCTRL_ID_STATE;
			ctx->putvctrl->cmd.len = 2;
			ctx->putvctrl->cmd.data = edata->state;
		}
	}
	if (event == PLAYER_EVENT_VOLUME)
	{
		mux_ctx_t *ctx = (mux_ctx_t *)arg;
		event_player_volume_t *edata = (event_player_volume_t *)data;
		dbg("volume changed %d", edata->volume);
		if (edata->volume != -1)
		{
			ctx->putvctrl = calloc(1, sizeof(*ctx->putvctrl));
			ctx->putvctrl->version = PUTVCTRL_VERSION;
			ctx->putvctrl->ncmds = 1;
			ctx->putvctrl->cmd.id = PUTVCTRL_ID_VOLUME;
			ctx->putvctrl->cmd.len = 2;
			ctx->putvctrl->cmd.data = edata->volume;
			ctx->volume = edata->volume;
		}
		else
			edata->volume = ctx->volume;
	}
}

static mux_ctx_t *mux_init(player_ctx_t *player, const char *search)
{
	mux_ctx_t *ctx = calloc(1, sizeof(*ctx));
	uint32_t ssrc = random();
	ssrc <<= 1; /// set an even number
	if (search)
	{
		const char *string = strstr(search, "ssrc=");
		if (string != NULL)
		{
			string += 5;
			if (sscanf(string, "0x%08x", &ssrc) == 0)
				sscanf(string, "%010x", &ssrc);
			if ((ssrc % 2) == 1)
				warn("demux: rtp src id is odd");
		}
		string = strstr(search, "dupsrc");
		if (string != NULL)
		{
			ctx->mode |= MUX_DOUBLESSRC;
		}
	}
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
	player_eventlistener(player, _mux_player_cb, ctx, jitter_name);

	ctx->header.b.v = 2;
	ctx->header.b.p = 0;
	ctx->header.b.x = 0;
	ctx->header.b.cc = 0;
	ctx->header.b.m = 1;
	ctx->seqnum = random();
	ctx->header.b.seqnum = __bswap_16(ctx->seqnum);
	ctx->header.timestamp = random();
	ctx->ssrc = ssrc;
	ctx->header.ssrc = __bswap_32(ssrc);

	warn("mux: rtp stream ssrc %#04x", ctx->header.ssrc);
	ctx->volume = 20;
	ctx->putvctrl = calloc(1, sizeof(*ctx->putvctrl));
	ctx->putvctrl->version = PUTVCTRL_VERSION;
	ctx->putvctrl->ncmds = 1;
	ctx->putvctrl->cmd.id = PUTVCTRL_ID_VOLUME;
	ctx->putvctrl->cmd.len = 2;
	ctx->putvctrl->cmd.data = ctx->volume;
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
	char *inbuffer;
	jitter_t *in = estream->in;
	size_t len = 0;
	char *outbuffer = ctx->buffer;
	if (!(ctx->mode & MUX_DOUBLESSRC))
		outbuffer = ctx->out->ops->pull(ctx->out->ctx);
	inbuffer = in->ops->peer(in->ctx, NULL);
	if (inbuffer == NULL)
		return 0;
	mux_dbg("mux: rtp seqnum %d", ctx->header.b.seqnum);
	ctx->header.b.pt = estream->pt;
	// copy header
	memcpy(outbuffer, &ctx->header, sizeof(ctx->header));
	ctx->header.b.m = 0;
	ctx->seqnum++;
	if (ctx->seqnum == UINT16_MAX)
	{
		ctx->seqnum = 0;
		ctx->header.b.m = 1;
	}
	ctx->header.b.seqnum = __bswap_16(ctx->seqnum);
	beat_pulse_t beat = {0};
#ifdef MUX_HEARTBEAT
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	if (now.tv_sec > ctx->timestamp.tv_sec ||
		now.tv_nsec > (ctx->timestamp.tv_nsec + RTP_HEARTBEAT_TIMELAPS))
	{
		if (ctx->header.timestamp == UINT32_MAX)
			ctx->header.timestamp = 0;
		else
			ctx->header.timestamp++;
		ctx->timestamp.tv_nsec += RTP_HEARTBEAT_TIMELAPS;
		ctx->timestamp.tv_nsec %= 1000000000;
		ctx->timestamp.tv_sec += (ctx->timestamp.tv_nsec / 1000000000);
		beat.pulses = 1;
	}
#endif
	len += sizeof(ctx->header);

	if (estream->extlen > 0 && estream->ext)
	{
		ctx->header.b.x = 1;
		memcpy(outbuffer + len, estream->ext, estream->extlen);
		len += estream->extlen;
	}
#ifdef DEBUG_0
	fprintf(stderr, "header: ");
	for (int i = 0; i < len; i++)
		fprintf(stderr, "%.2hhx ", outbuffer[i]);
	fprintf(stderr, "\n");
#endif
	while ((len + in->ops->length(in->ctx)) < ctx->out->ctx->size)
	{
		size_t inlength = in->ops->length(in->ctx);
		// copy payload
		memcpy(outbuffer + len, inbuffer, inlength);
		len += inlength;

		in->ops->pop(in->ctx, inlength);
		inbuffer = in->ops->peer(in->ctx, NULL);
		if (inbuffer == NULL)
			return 0;
	}
	in->ops->pop(in->ctx, 0);
	mux_dbg("udp: packet %lu sent", len);
	if (ctx->mode & MUX_DOUBLESSRC)
	{
		outbuffer = ctx->out->ops->pull(ctx->out->ctx);
		memcpy(outbuffer, ctx->buffer, len);
		ctx->out->ops->push(ctx->out->ctx, len, &beat);
		rtpheader_t *header = ctx->buffer;
		header->ssrc = __bswap_32(ctx->ssrc + 1);
		outbuffer = ctx->out->ops->pull(ctx->out->ctx);
		memcpy(outbuffer, ctx->buffer, len);
	}
	ctx->out->ops->push(ctx->out->ctx, len, &beat);

	if (ctx->putvctrl)
	{
		size_t len = sizeof(ctx->header);
		char *outbuffer = ctx->out->ops->pull(ctx->out->ctx);

		// copy header
		memcpy(outbuffer, &ctx->header, len);
		rtpheader_t *header = (rtpheader_t *)outbuffer;
		header->b.pt = PUTVCTRL_PT;
		header->b.x = 1;
		rtpext_t extheader = {PUTVCTRL_PT, sizeof(*ctx->putvctrl)};
		memcpy(outbuffer + len, &extheader, sizeof(extheader));
		len += sizeof(extheader);
		memcpy(outbuffer + len, ctx->putvctrl, sizeof(*ctx->putvctrl));
		len += sizeof(*ctx->putvctrl);
		//len = ctx->out->ops->length(ctx->out->ctx);
		ctx->out->ops->push(ctx->out->ctx, len, NULL);

		free(ctx->putvctrl);
		ctx->putvctrl = NULL;
	}
	return 1;
}

static void *mux_thread(void *arg)
{
	int result = 0;
	int run = 1;
	mux_ctx_t *ctx = (mux_ctx_t *)arg;
	for (int i = 0; i < MAX_ESTREAM && ctx->estreams[i].in != NULL; i++)
	{
		warn("mux: rtp stream %u %u %s",ctx->header.ssrc, ctx->estreams[i].pt, ctx->estreams[i].mime);
	}
	mux_dbg("mux: rtp thread start");
#ifdef RTP_TIMESTAMPS
	clock_gettime(CLOCK_REALTIME, &ctx->timestamp);
#endif
	while (run)
	{
		run = 0;
		for (int i = 0; i < MAX_ESTREAM && ctx->estreams[i].in != NULL; i++)
		{
#ifdef MUX_HEARTBEAT
			if (ctx->out->ops->heartbeat(ctx->out->ctx, NULL) == NULL)
			{
				heartbeat_pulse_t config;
				config.ms = RTP_HEARTBEAT_TIMELAPS / 1000000;
				ctx->heartbeat.ops = heartbeat_pulse;
				ctx->heartbeat.ctx = heartbeat_pulse->init(&config);
				dbg("set heart %s %dms", ctx->out->ctx->name, config.ms);
				ctx->out->ops->heartbeat(ctx->out->ctx, &ctx->heartbeat);
				ctx->heartbeat.ops->start(ctx->heartbeat.ctx);
			}
#endif
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
	if (ctx->buffer)
		free(ctx->buffer);
	ctx->buffer = calloc(1, sink_jitter->ctx->size);
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
		unsigned int jitterdepth = MUXJITTER_SIZE;
		unsigned char pt;
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
			ext->format = encoder->ops->format(encoder->ctx);
			ext->samplerate = encoder->ops->samplerate(encoder->ctx);
		}
		else if (mime == mime_audioflac)
		{
			jitterdepth *= 15;
			pt = 46;
		}
		else
		{
			pt = 99;
		}
		if (ctx->estreams[i].pt == 0)
			ctx->estreams[i].pt = pt;
		ctx->estreams[i].mime = mime;
		jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, jitterdepth, size);
		jitter->ctx->frequence = encoder->ops->samplerate(encoder->ctx);
		jitter->ctx->thredhold = jitterdepth / 2;
		jitter->format = encoder->ops->format(encoder->ctx);
		ctx->estreams[i].in = jitter;
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
	if (ctx->buffer)
		free(ctx->buffer);
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

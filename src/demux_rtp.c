/*****************************************************************************
 * demux_rtp.c
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
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <byteswap.h>

#include <pthread.h>

#include "player.h"
#include "decoder.h"
#include "event.h"
typedef struct src_s demux_t;
typedef struct src_ops_s demux_ops_t;

#define NB_LOOPS 21
#define NB_BUFFERS 8
#define BUFFERSIZE 1500

#ifdef DEMUX_RTP_REORDER
typedef struct demux_reorder_s demux_reorder_t;
struct demux_reorder_s
{
	char buffer[BUFFERSIZE];
	size_t len;
	char id;
	char ready;
};
#endif

typedef struct demux_out_s demux_out_t;
struct demux_out_s
{
	decoder_t *estream;
	uint32_t ssrc;
	jitter_t *jitter;
	char *data;
	const char *mime;
	short cc;
	uint8_t pt;
	demux_out_t *next;
};

typedef struct demux_profile_s demux_profile_t;
struct demux_profile_s
{
	const char *mime;
	demux_profile_t *next;
	uint32_t ssrc;
	char pt;
};

typedef struct demux_ctx_s demux_ctx_t;
typedef struct demux_ctx_s src_ctx_t;
struct demux_ctx_s
{
	demux_out_t *out;
	player_ctx_t *player;
	jitter_t *in;
	jitte_t jitte;
	unsigned short nbbuffers;
	unsigned short seqnum;
	unsigned short seqorig;
	unsigned long missing;
#ifdef DEMUX_RTP_REORDER
	demux_reorder_t *reorder;
#endif
	const char *mime;
	char *url;
	const char *host;
	const char *port;
	uint32_t sessionid;
	pthread_t thread;
	event_listener_t *listener;
	demux_profile_t *profiles;
	demux_profile_t *sessionlist;

#ifdef DEMUX_DUMP
	int dumpfd;
#endif
};
#define SRC_CTX
#define DEMUX_CTX
#include "demux.h"
#include "src.h"
#include "media.h"
#include "jitter.h"
#include "rtp.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define demux_dbg(...)

#define DEMUX_POLICY REALTIME_SCHED
#define DEMUX_PRIORITY 55

static const char *jitter_name = "rtp demux";

static void _demux_player_cb(void *arg, event_t event, void *data)
{
	if (event == PLAYER_EVENT_CHANGE)
	{
		demux_ctx_t *ctx = (demux_ctx_t *)arg;
		event_player_state_t *edata = (event_player_state_t *)data;
		if (edata->state == STATE_STOP)
			ctx->sessionid = 0;
	}
}

static demux_ctx_t *demux_init(player_ctx_t *player, const char *url, const char *mime)
{
	demux_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->player = player;
	ctx->mime = utils_mime2mime(mime);
	demux_profile_t *profile = NULL;
	ctx->nbbuffers = NB_BUFFERS;
#ifdef DEMUX_RTP_REORDER
	ctx->reorder = calloc(ctx->nbbuffers, sizeof(*ctx->reorder));
#endif
	char pt = 20;
	uint32_t ssrc = 0;
	const char *search = NULL;
	ctx->url = utils_parseurl(url, NULL, &ctx->host, &ctx->port, NULL, &search);
	if (search != NULL)
	{
		const char *string = NULL;
		string = strstr(search, "mime=");
		if (string)
		{
			string += 5;
			mime = utils_mime2mime(string);
		}
		string = strstr(search, "pt=");
		if (string != NULL)
		{
			string += 3;
			sscanf(string, "%hhd", &pt);
		}
		string = strstr(search, "ssrc=");
		if (string != NULL)
		{
			string += 5;
			if (sscanf(string, "0x%08x", &ssrc) == 0)
				sscanf(string, "%010u", &ssrc);
		}
		string = strstr(search, "nbuf=");
		if (string != NULL)
		{
			string += 5;
			sscanf(string, "%hd", &ctx->nbbuffers);
		}
	}

	demux_rtp_addprofile(ctx, 14, mime_audiomp3);
	demux_rtp_addprofile(ctx, 11, mime_audiopcm);
	demux_rtp_addprofile(ctx, 46, mime_audioflac);
	warn("demux: add profile %s on %d", mime, pt);
	demux_rtp_addprofile(ctx, pt, mime);

	ctx->sessionid = ssrc;
	player_eventlistener(player, _demux_player_cb, ctx, jitter_name);
#ifdef DEMUX_DUMP
	ctx->dumpfd = open("rtp_dump.rtp", O_RDWR | O_CREAT, 0644);
	err("dump %d", ctx->dumpfd);
#endif

	if (ctx->in == NULL)
	{
		int nbbuffers = ctx->nbbuffers;
		ctx->in = jitter_init(JITTER_TYPE_SG, jitter_name, nbbuffers, BUFFERSIZE);
#ifdef USE_REALTIME
		ctx->in->ops->lock(ctx->in->ctx);
#endif
		ctx->in->format = SINK_BITSSTREAM;
		ctx->in->ctx->thredhold = nbbuffers * 1 / 4;
	}
	return ctx;
}

static jitter_t *demux_jitter(demux_ctx_t *ctx, jitte_t jitte)
{
	if (ctx->jitte < jitte)
	{
		ctx->jitte = jitte;
		ctx->in->ctx->thredhold *= (jitte + 1);
	}
	return ctx->in;
}

static const char *demux_profile(demux_ctx_t *ctx, char pt)
{
	demux_profile_t *profile = ctx->profiles;
	while (profile != NULL && profile->pt != pt) profile = profile->next;
	if (profile == NULL)
		return mime_octetstream;
	return profile->mime;
}

void demux_rtp_addprofile(demux_ctx_t *ctx, char pt, const char *mime)
{
	demux_profile_t *profile = NULL;

	profile = calloc(1, sizeof(*profile));
	profile->pt = pt;
	profile->mime = mime;
	profile->next = ctx->profiles;
	ctx->profiles = profile;
}

static int demux_parseheader(demux_ctx_t *ctx, unsigned char *input, size_t len)
{
	rtpheader_t *header = (rtpheader_t *)input;
	uint16_t seqnum = __bswap_16(header->b.seqnum);
	//dbg("demux: rtp seqnum %#x %#x", header->b.seqnum, seqnum);
	uint32_t ssrc = __bswap_32(header->ssrc);
#ifdef DEBUG_0
	int i;
	fprintf(stderr, "header: ");
	for (i = 0; i < sizeof(*header); i++)
		fprintf(stderr, "%.2x ", input[i]);
	fprintf(stderr, "\n");
	warn("rtp header:");
	warn("\ttimestamp:\t%#x", header->timestamp);
	warn("\tseq:\t%#x", seqnum);
	warn("\tssrc:\t%#x", ssrc);
	warn("\ttype:\t%#x", header->b.pt);
	warn("\tversion:\t%#x", header->b.v);
	warn("\tnb csrc:\t%#x", header->b.cc);
	warn("\tpadding:\t%#x", header->b.p);
#endif
	rtpext_t *extheader = NULL;

	input += sizeof(*header);
	len -= sizeof(*header);
	if (header->b.cc)
	{
		warn("CSCR:");
		input += header->b.cc * sizeof(uint32_t);
		len -= header->b.cc * sizeof(uint32_t);
	}
	if (header->b.x)
	{
		extheader = (rtpext_t *)input;
		input += sizeof(*extheader);
		len -= sizeof(*extheader);
		warn("rtp extension: %x %u", extheader->extid, extheader->extlength);

		input += extheader->extlength;
		len -= extheader->extlength;
	}
	/// player currently support only one session
	if (ctx->sessionid != 0 && ctx->sessionid != ssrc)
	{
		demux_profile_t *it = ctx->sessionlist;
		while(it != NULL)
		{
			if (it->ssrc == ssrc)
				return -1;
			it = it->next;
		}
		it = calloc(1, sizeof(*it));
		it->next = ctx->sessionlist;
		ctx->sessionlist = it;
		it->ssrc = ssrc;
		it->pt = header->b.pt;
		media_t *media = player_media(ctx->player);
		if (media && media->ops->insert)
		{
			char *url;
			if (asprintf(&url, "rtp://%s:%s?ssrc=%#x&pt=%hhd", ctx->host, ctx->port, ssrc, header->b.pt))
			{
				const char * mime = mime_octetstream;
				mime = demux_profile(ctx, header->b.pt);
				media->ops->insert(media->ctx, url, NULL, mime);
				free(url);
			}
		}
		warn("demux: new ssrc %#x", it->ssrc);
		return -1;
	}
	if (header->b.pt == PUTVCTRL_PT && extheader)
	{
		rtpext_putvctrl_t *putvctrl = (rtpext_putvctrl_t *)(((char *)extheader) + sizeof (*extheader));
		if (putvctrl->version != PUTVCTRL_VERSION)
			err("demux: bad ctrl version");
		rtpext_putvctrl_cmd_t *cmd = &putvctrl->cmd;
		dbg("demu: rtp ctl %u %u", cmd->id, cmd->data);
		if (cmd->id == PUTVCTRL_ID_STATE && cmd->data == STATE_STOP)
			ctx->sessionid = 0;
		if (cmd->id == PUTVCTRL_ID_VOLUME)
			player_volume(ctx->player, cmd->data % 100);
		return 0;
	}
	demux_out_t *out;
	out = ctx->out;
	while (out != NULL && out->ssrc != ssrc && out->pt != header->b.pt)
		out = out->next;
	if (out == NULL)
	{
		ctx->sessionid = ssrc;
		const char * mime = mime_octetstream;
		mime = demux_profile(ctx, header->b.pt);
		warn("demux: new rtp substream %d %s(%d)", header->ssrc, mime, header->b.pt);
		if (mime == mime_octetstream)
			return -1;
		out = calloc(1, sizeof(*out));
		out->mime = mime;
		out->ssrc = header->ssrc;
		out->pt = header->b.pt;
		out->cc = header->b.cc;
		out->next = ctx->out;
		ctx->out = out;
		warn("demux: new rtp substream %d %s(%d)", header->ssrc, out->mime, header->b.pt);
		event_listener_t *listener = ctx->listener;
		const char *ext = NULL;
		if (extheader)
			ext = (char *)(extheader + sizeof (*extheader));
		const src_t src = { .ops = demux_rtp, .ctx = ctx, .info = ext };
		event_new_es_t event = {.pid = out->ssrc, .src = &src, .mime = out->mime, .jitte = JITTE_HIGH};
		event_decode_es_t event_decode = {.src = &src};
		while (listener != NULL)
		{
			listener->cb(listener->arg, SRC_EVENT_NEW_ES, (void *)&event);
			event_decode.pid = event.pid;
			event_decode.decoder = event.decoder;
			listener->cb(listener->arg, SRC_EVENT_DECODE_ES, (void *)&event_decode);
			listener = listener->next;
		}
	}
#ifdef DEMUX_RTP_REORDER
	demux_reorder_t *reorder = &ctx->reorder[seqnum % ctx->nbbuffers];
	int id = seqnum % (ctx->nbbuffers * NB_LOOPS);

	if (out->jitter != NULL && reorder->ready && reorder->id != id)
	{
		if (out->data == NULL)
			out->data = out->jitter->ops->pull(out->jitter->ctx);
		char *buffer = reorder->buffer;
		size_t len = reorder->len;
		while (len > out->jitter->ctx->size)
		{
			memcpy(out->data, buffer, out->jitter->ctx->size);
			out->jitter->ops->push(out->jitter->ctx, out->jitter->ctx->size, NULL);
			len -= out->jitter->ctx->size;
			buffer += out->jitter->ctx->size;
		}
		memcpy(out->data, buffer, len);
		out->jitter->ops->push(out->jitter->ctx, len, NULL);
		out->data = NULL;
		reorder->ready = 0;
	}
	if (!reorder->ready)
	{
		if (len > 0)
		{
			memcpy(reorder->buffer, input, len);
			reorder->ready = 1;
			reorder->id = id;
		}
	}
#else
	if (out->jitter != NULL)
	{
		if (ctx->seqnum == 0)
			ctx->seqorig = ctx->seqnum = seqnum - 1;
		ctx->seqnum++;
		unsigned long missing = 0;
		while (ctx->seqnum < seqnum)
		{
#if 0
			/*
			 * generate missing data
			 * this code is not correct:
			 * it is impossible to generate a good stream for the decoder
			 * and if they are missing, they are yet old too,
			 * it is useless to generate them.
			 * it is more efficient to count the missing PCM and
			 * remove from the encoder the same number of PCM, who generated
			 * during previous hole of data.
			 */
			if (out->data == NULL)
				out->data = out->jitter->ops->pull(out->jitter->ctx);
			memset(out->data, 0, out->jitter->ctx->size);
			out->jitter->ops->push(out->jitter->ctx, out->jitter->ctx->size, NULL);
			out->data = NULL;
#endif
			missing++;
			ctx->seqnum++;
		}
		if (missing > 0)
		{
			ctx->missing += missing;
			warn("demux: %lu packet missing %u", missing, ctx->seqnum - ctx->seqorig);
		}
		if (out->data == NULL)
			out->data = out->jitter->ops->pull(out->jitter->ctx);
		if (out->data == NULL)
			return -1;
#ifdef DEMUX_DUMP
		if (ctx->dumpfd > 0)
		{
			write(ctx->dumpfd, input, len);
		}
#endif
		while (len > out->jitter->ctx->size)
		{
			err("demux: udp packet has not to overflow 1500 bytes (%ld)", len);
			memcpy(out->data, input, out->jitter->ctx->size);
			out->jitter->ops->push(out->jitter->ctx, out->jitter->ctx->size, NULL);
			len -= out->jitter->ctx->size;
			input += out->jitter->ctx->size;
			out->data = out->jitter->ops->pull(out->jitter->ctx);
		}
		memcpy(out->data, input, len);
		demux_dbg("demux: push %ld", len);
		out->jitter->ops->push(out->jitter->ctx, len, NULL);
		out->data = NULL;
	}
	else
		len = -1;
#endif
	return len;
}

static void *demux_thread(void *arg)
{
	demux_ctx_t *ctx = (demux_ctx_t *)arg;
	int run = 1;
	demux_dbg("demux: thread start");
	do
	{
		char *input;
		size_t len = 0;
		input = ctx->in->ops->peer(ctx->in->ctx, NULL);
		if (input == NULL)
		{
			run = 0;
		}
		else
		{
			len = ctx->in->ops->length(ctx->in->ctx);
			if ( demux_parseheader(ctx, input, len) < 0)
			{
				demux_dbg("demux: rtp stream unknown");
			}
		}
		ctx->in->ops->pop(ctx->in->ctx, len);
	} while (run);
	demux_dbg("demux: thread end");
	demux_out_t *out = ctx->out;
	while (out != NULL)
	{
		const src_t src = { .ops = demux_rtp, .ctx = ctx};
		event_end_es_t event = {.pid = out->ssrc, .src = &src, .decoder = out->estream};
		event_listener_t *listener = ctx->listener;
		while (listener)
		{
			listener->cb(listener->arg, SRC_EVENT_END_ES, (void *)&event);
			listener = listener->next;
		}
		if (out->data != NULL)
			out->jitter->ops->push(out->jitter->ctx, 0, NULL);
		out = out->next;
	}
	return NULL;
}

static int demux_run(demux_ctx_t *ctx)
{
#ifdef USE_REALTIME
	int ret;

	pthread_attr_t attr;
	struct sched_param params;

	pthread_attr_init(&attr);

	ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	if (ret < 0)
		err("setdetachstate error %s", strerror(errno));
	ret = pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS);
	if (ret < 0)
		err("setscope error %s", strerror(errno));
	ret = pthread_attr_setschedpolicy(&attr, DEMUX_POLICY);
	if (ret < 0)
		err("setschedpolicy error %s", strerror(errno));
	params.sched_priority = DEMUX_PRIORITY;
	ret = pthread_attr_setschedparam(&attr, &params);
	if (ret < 0)
		err("setschedparam error %s", strerror(errno));
	if (getuid() == 0)
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	else
	{
		warn("run server as root to use realtime");
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
	}
	if (ret < 0)
		err("setinheritsched error %s", strerror(errno));
	pthread_create(&ctx->thread, &attr, demux_thread, ctx);
	pthread_attr_destroy(&attr);
#else
	pthread_create(&ctx->thread, NULL, demux_thread, ctx);
#endif
	return 0;
}

static const char *demux_mime(demux_ctx_t *ctx, int index)
{
	demux_out_t *out = ctx->out;
	while (out != NULL && index > 0)
	{
		out = out->next;
		index--;
	}
	const char *mime = ctx->mime;
	if (out != NULL)
		mime = out->mime;
	return mime;
}

static void demux_eventlistener(demux_ctx_t *ctx, event_listener_cb_t cb, void *arg)
{
	event_listener_t *listener = calloc(1, sizeof(*listener));
	listener->cb = cb;
	listener->arg = arg;
	if (ctx->listener == NULL)
		ctx->listener = listener;
	else
	{
		/**
		 * add listener to the end of the list. this allow to call
		 * a new listener with the current event when the function is
		 * called from a callback
		 */
		event_listener_t *previous = ctx->listener;
		while (previous->next != NULL) previous = previous->next;
		previous->next = listener;
	}
}

static int demux_attach(demux_ctx_t *ctx, long index, decoder_t *decoder)
{
	demux_out_t *out = ctx->out;
	while (out != NULL && out->ssrc != (uint32_t)index)
	{
		out = out->next;
	}
	if (out != NULL)
	{
		out->estream = decoder;
		out->jitter = out->estream->ops->jitter(out->estream->ctx, ctx->jitte);
	}
	return 0;
}

static decoder_t *demux_estream(demux_ctx_t *ctx, long index)
{
	demux_out_t *out = ctx->out;
	while (out != NULL)
	{
		if (index == out->ssrc)
			return out->estream;
		out = out->next;
	}
	return NULL;
}

static void demux_destroy(demux_ctx_t *ctx)
{
	demux_out_t *out = ctx->out;
	while (out != NULL)
	{
		demux_out_t *old = out;
		out = out->next;
		if (old->estream != NULL)
			old->estream->ops->destroy(old->estream->ctx);
		free(old);
	}
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		event_listener_t *next = listener->next;
		free(listener);
		listener = next;
	}
	demux_profile_t *profile = ctx->profiles;
	while (profile != NULL)
	{
		demux_profile_t *next = profile->next;
		free(profile);
		profile = next;
	}
	if (ctx->in != NULL)
		ctx->in->destroy(ctx->in);
#ifdef DEMUX_DUMP
	if (ctx->dumpfd > 0)
		close(ctx->dumpfd);
#endif
	free(ctx);
}

const demux_ops_t *demux_rtp = &(const demux_ops_t)
{
	.name = "demux_rtp",
	.protocol = "rtp",
	.init = demux_init,
	.jitter = demux_jitter,
	.run = demux_run,
	.mime = demux_mime,
	.eventlistener = demux_eventlistener,
	.attach = demux_attach,
	.estream = demux_estream,
	.destroy = demux_destroy,
};

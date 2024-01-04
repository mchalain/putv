/*****************************************************************************
 * demux_ts.c
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
#include "decoder.h"
typedef struct src_s demux_t;
typedef struct src_ops_s demux_ops_t;

#define MAX_OUTSTREAM 4

typedef struct demux_out_s demux_out_t;
struct demux_out_s
{
	uint16_t pid;
	decoder_t *estream;
	jitter_t *jitter;
	char *data;
	const char *mime;
	demux_out_t *next;
};

#ifdef LIBDVBPSI
struct ts_pmt_s
{
	int ready;
	unsigned short pid;
	unsigned short number;
	dvbpsi_t *pmt;
	ts_t *stream;
	struct ts_pmt_s *next;
};
typedef struct ts_pmt_s ts_pmt_t;
#endif

typedef struct demux_ctx_s demux_ctx_t;
typedef struct demux_ctx_s src_ctx_t;
struct demux_ctx_s
{
	uint32_t nbpackets;
	uint8_t payloadsize;
	demux_out_t *out;
	jitter_t *in;
	jitte_t jitte;
	const char *mime;
	pthread_t thread;
	event_listener_t *listener;
#ifdef LIBDVBPSI
	dvbpsi_t *dvbpsi_pat;
	ts_pmt_t *dvbpsi_pmts;
#endif
};

#define SRC_CTX
#define DEMUX_CTX
#include "demux.h"
#include "src.h"
#include "media.h"
#include "jitter.h"
#include "dvb.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define demux_dbg(...)

#define BUFFERSIZE  1500
#define NB_BUFFERS 6

#define FULL_PCR
#ifdef FULL_PCR
#define PCR_MAX ((0x1FFFFFFull * 300 ) + 0x1FF)
static unsigned long long _pcr(unsigned char *offset)
{
	unsigned char *pcr_raw = offset;
	unsigned long long int base;
	unsigned char ext;
	base = ((*(pcr_raw)) << 25);
	base += ((*(pcr_raw+1)) << 17);
	base += ((*(pcr_raw+2)) << 9);
	base += ((*(pcr_raw+3)) << 1);
	base += ((*(pcr_raw+4) & 0x80) >> 7);
	ext = *(pcr_raw+5);
	ext += ((*(pcr_raw+4) & 0x01) << 8);
	return base * 300 + ext;
}
#else
#define PCR_MAX (0x32B)
static unsigned long long _pcr(unsigned char *offset)
{
	unsigned char *pcr_raw = offset;
	unsigned long long int base = 0;
	unsigned char ext = 0;
	base += ((*(pcr_raw+4) & 0x80) >> 7);
	ext = *(pcr_raw+5);
	ext += ((*(pcr_raw+4) & 0x01) << 8);
	return base * 300 + ext;
}
#endif
#define PCR_SET(p_pcr, value)	(*p_pcr = value)
#define PCR_DIFF(p_pcr1, p_pcr2) (*p_pcr1 - *p_pcr2)

static int demux_createdecoder(demux_ctx_t *ctx, uint16_t pid, const char *mime, demux_out_t **pout)
{
	event_listener_t *listener = ctx->listener;
	const src_t src = { .ops = demux_rtp, .ctx = ctx };
	event_new_es_t event = {.pid = pid, .src = &src, .mime = mime, .jitte = JITTE_HIGH};
	event_decode_es_t event_decode = {.src = &src};
	while (listener != NULL)
	{
		listener->cb(listener->arg, SRC_EVENT_NEW_ES, (void *)&event);
		event_decode.pid = event.pid;
		event_decode.decoder = event.decoder;
		listener->cb(listener->arg, SRC_EVENT_DECODE_ES, (void *)&event_decode);
		listener = listener->next;
	}
	demux_out_t *out = ctx->out;
	while (out != NULL && out->pid != pid)
		out = out->next;
	if (out== NULL)
		return -1;
	*pout = out;
	return 0;
}

#ifdef LIBDVBPSI
static void _dvbpsi_debug(dvbpsi_t *handle, const dvbpsi_msg_level_t level, const char *msg)
{
	//TSPACKET_LOG_DEBUG("%s",msg);
}

typedef struct type2mime_s
{
	uint8_t type;
	const char *mime;
	uint16_t mimelength;
} type2mime_t;

static const type2mime_t type2mime[] =
{
	{0x03, mime_audiomp3, sizeof(mime_audiomp3) - 1},
	{0x0F, mime_audioaac, sizeof(mime_audioaac) - 1},
	{0x1C, mime_audiopcm, sizeof(mime_audiopcm) - 1},
	{0xC3, mime_audioflac, sizeof(mime_audioflac) - 1},
	{0xC4, mime_audiolac, sizeof(mime_audioalac) - 1},
};

static int dvbpsi_type2mime(uint8_t type, const char **mime)
{
	for (int i = 0; i < (sizeof(type2mime) / sizeof(type2mime_t)); i++)
		if (type == type2mime[i].type)
		{
			*mime = type2mime[i].mime;
			return type2mime[i].mimelength;
		}
	*mime = mime_octetstream;	
	return sizeof(mime_octetstream) - 1;
}

static void _dvbpsi_pmt(void *p_cb_data, dvbpsi_pmt_t *p_new_pmt)
{
	demux_ctx_t *ctx = (demux_ctx_t *)p_cb_data;
	dbg("PMT programm es pid 0x%02X with pcr", p_new_pmt->i_pcr_pid);
	dvbpsi_pmt_es_t *dvbi_es = p_new_pmt->p_first_es;
	while (dvbi_es != NULL)
	{
		int i = 0;
		dbg("PMT es %d pid 0x%02X", dvbi_es->i_type, dvbi_es->i_pid);
		demux_out_t *out = ctx->out;
		while (out != NULL &&out->pid != pid)
			out = out->next;
		if (out == NULL)
		{
			const char *mime = NULL;
			dvbpsi_type2mime(dvbpsi_es->i_type, &mime);
			demux_createdecoder(ctx, pid, mime, &out);
		}
		dvbi_es = dvbi_es->p_next;
	}
	dvbpsi_descriptor_t *descriptor = p_new_pmt->p_first_descriptor;
	while (descriptor != NULL)
	{
		char string[188];
		memcpy(string, descriptor->p_data, (descriptor->i_length > 187)? 187:descriptor->i_length);
		dbg("PMT es %d %s", descriptor->i_tag, string);
		descriptor = descriptor->p_next;
	}
	dbg("PMT end");
	pmt->ready = 1;
}
void _dvbpsi_pat(void *p_cb_data, dvbpsi_pat_t *p_new_pat)
{
	demux_ctx_t *ctx = (demux_ctx_t *)p_cb_data;
	dbg("PAT programm tid 0x%02X", p_new_pat->i_ts_id);
	dvbpsi_pat_program_t *program = p_new_pat->p_first_program;
	while (program != NULL)
	{
		ts_pmt_t *pmt = ctx->dvbpsi_pmts;
		while (pmt != NULL && pmt->pid != program->i_pid) pmt = pmt->next;
		if (pmt == NULL)
		{
			pmt = calloc(1,sizeof(*ctx->pmts));
			pmt->pid = program->i_pid;
			pmt->number = program->i_number;
			pmt->next = ctx->dvbpsi_pmts;
			ctx->dvbpsi_pmts = pmt;
		}
		dbg("PAT programm %d pid 0x%02X",program->i_number, program->i_pid);
		pmt->pmt = dvbpsi_new(_dvbpsi_debug, DVBPSI_MSG_NONE);
		dvbpsi_pmt_attach(pmt->pmt, program->i_number, _dvbpsi_pmt, (void *)ctx);
		program = program->p_next;
	}
}
#endif
#if 0
int ts_synchronize(ts_t *stream, char *buffer, int max)
{
	if (stream->occurance >= 5)
	{
		stream->occurance = 0;
		return 1;
	}
	char *data = buffer;
	for (; (data - buffer) < max; data++)
	{
		ts_packet_header_t *header = (ts_packet_header_t *)data;
		if (header->decoded.sync == 0x47)
		{
			stream->occurance++;
			dbg("TS packet found %d", stream->occurance);
			data += stream->packetsize - 1;
		}
		else
		{
			warn("TS stream not synchronized");
			stream->occurance = 0;
		}
	}
	if (stream->occurance >= 5)
	{
		stream->occurance = 0;
		return 0;
	}
	if (data != buffer + max)
	{
		err("TS sync %d", data - buffer - max);
		return data - buffer - max;
	}
	else
		return 1;
}
#endif
static int demux_parsecontent(demux_ctx_t *ctx, char *input, size_t len, demux_out_t *out)
{
	if (out == NULL)
		return 0;
	if (len < ctx->payloadsize)
		return -1;
	if (out->data == NULL)
		out->data = out->jitter->ops->pull(out->jitter->ctx);
	len = ctx->payloadsize;
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
	return ctx->payloadsize;
}

static int demux_parseheader(demux_ctx_t *ctx, char *buffer, size_t length, demux_out_t **pout)
{
	int ret = 0;
	char *offset = buffer;

	ts_packet_header_t *header = (ts_packet_header_t *)offset;
	if (length < sizeof(*header))
		return -1;

	int16_t pid = ((header->decoded.hpid & 0x1F) << 8) + header->decoded.lpid;

	ctx->nbpackets++;
	if (header->decoded.sync != 0x47)
	{
		err("TS buffer is not a packet");
		return -1;
	}
#ifdef DEBUG
	else
	{
		fprintf(stderr, "pid: 0x%02X ", pid);
		fprintf(stderr, "counter %u ", header->decoded.counter);
		if (header->decoded.payload)
			fprintf(stderr, "payload ");
		if (header->decoded.scrambling)
			fprintf(stderr, "scrambling ");
		if (header->decoded.adaption)
			fprintf(stderr, "adaption ");
		fprintf(stderr, "\n");
	}
#endif
	//short pid = ntohs((short)header->decoded.pid);

	uint64_t pcr = 0;
	demux_out_t *out = ctx->out;
	while (out != NULL && (out->pid != pid))
		out = out->next;
#ifdef LIBDVBPSI
	if (ctx->dvbpsi_pat != NULL)
	{
		dvbpsi_packet_push(ctx->dvbpsi_pat, buffer);
	}
	ts_pmt_t *pmt = ctx->dvbpsi_pmts;
	while(pmt != NULL)
	{
		if (!pmt->ready)
			dvbpsi_packet_push(pmt->pmt, buffer);
		pmt = pmt->next;
	}
#else
	if (pid == 0x39 && out == NULL && demux_createdecoder(ctx, pid, mime_audiomp3, &out) < 0)
			return -1;
	if (pid == 0x40 && out == NULL && demux_createdecoder(ctx, pid, mime_audioflac, &out) < 0)
			return -1;
	if (pid == 0x41 && out == NULL && demux_createdecoder(ctx, pid, mime_audioaac, &out) < 0)
			return -1;
	if (pid == 0x42 && out == NULL && demux_createdecoder(ctx, pid, mime_audiopcm, &out) < 0)
			return -1;
#endif
	if (header->decoded.adaption)
	{
		offset += sizeof(*header);
		ts_packet_adaption_t *adaption = (ts_packet_adaption_t *)offset;
		offset +=  sizeof(*adaption);
		if (adaption->decoded.pcr)
		{
			pcr = _pcr(offset);
			dbg("pid: 0x%02X adaption pcr %lu ", pid, pcr);
			offset += 6;
		}
		if (adaption->decoded.opcr)
		{
			unsigned long int opcr = _pcr(offset);
			dbg("opcr %06lX ", opcr);
			offset += 6;
		}
		if (adaption->decoded.splicing)
		{
			unsigned char splicing = *offset;
			dbg("splicing %d ", splicing);
			offset += sizeof(splicing);
		}
		if (adaption->decoded.private)
		{
			unsigned char privatelength = *offset;
			dbg("privatelength %d ", privatelength);
			offset += sizeof(privatelength);
			offset += privatelength;
		}
	}
	*pout = out;
	return offset - buffer;
}

static const char *jitter_name = "dvb demux";

static demux_ctx_t *demux_init(player_ctx_t *player, const char *url, const char *mime)
{
	demux_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->mime = utils_mime2mime(mime);
	ctx->payloadsize = 184;
#ifdef LIBDVBPSI
	ctx->dvbpsi_pat = dvbpsi_new(_dvbpsi_debug, DVBPSI_MSG_DEBUG);
	dvbpsi_pat_attach(ctx->dvbpsi_pat, _dvbpsi_pat, (void *)ctx);
#endif
	return ctx;
}

static jitter_t *demux_jitter(demux_ctx_t *ctx, jitte_t jitte)
{
	if (ctx->in == NULL)
	{
		int nbbuffers = NB_BUFFERS << jitte;
		ctx->in = jitter_init(JITTER_TYPE_SG, jitter_name, nbbuffers, BUFFERSIZE);
#ifdef USE_REALTIME
		ctx->in->ops->lock(ctx->in->ctx);
#endif
		ctx->in->format = SINK_BITSSTREAM;
		ctx->in->ctx->thredhold = nbbuffers * 3 / 4;
		ctx->jitte = jitte;
	}
	return ctx->in;
}

static void *demux_thread(void *arg)
{
	demux_ctx_t *ctx = (demux_ctx_t *)arg;
	int run = 1;
	do
	{
		char *input;
		input = ctx->in->ops->peer(ctx->in->ctx, NULL);
		if (input == NULL)
		{
			run = 0;
			break;
		}

		int len;
		len = ctx->in->ops->length(ctx->in->ctx);
		demux_out_t *out = NULL;
		int headerlen = 0;
		if (len > 0)
			headerlen = demux_parseheader(ctx, input, len, &out);
		else
			warn("demux: empty buffer %p", input);
		if (headerlen > 0)
		{
			demux_parsecontent(ctx, input + headerlen, len - headerlen, out);
		}
		ctx->in->ops->pop(ctx->in->ctx, len);
	} while (run);
	return NULL;
}

static int demux_run(demux_ctx_t *ctx)
{
	pthread_create(&ctx->thread, NULL, demux_thread, ctx);
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
	if (out != NULL)
		return out->mime;
	return ctx->mime;
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
	while (out != NULL && out->pid != index) out = out->next;
	if (out == NULL)
	{
		out = calloc(1, sizeof(*out));
		out->next = ctx->out;
		ctx->out = out;
		out->pid = index;
	}
	out->estream = decoder;
	out->jitter = decoder->ops->jitter(out->estream->ctx, ctx->jitte);
	return 0;
}

static decoder_t *demux_estream(demux_ctx_t *ctx, long index)
{
	demux_out_t *out = ctx->out;
	while (out != NULL)
	{
		if (index == out->pid)
			return out->estream;
		out = out->next;
	}
	return NULL;
}

static void demux_destroy(demux_ctx_t *ctx)
{
#ifdef LIBDVBPSI
	if (ctx->dvbpsi_pat != NULL)
	{
		dvbpsi_pat_detach(ctx->dvbpsi_pat);
		dvbpsi_delete(ctx->dvbpsi_pat);
		ctx->dvbpsi_pat = NULL;
	}
	if (ctx->dvbpsi_pmts != NULL)
	{
		ts_pmt_t *pmt = ctx->dvbpsi_pmts;
		while (pmt != NULL)
		{
			ts_pmt_t *next = pmt->next;
			dvbpsi_pmt_detach(pmt->pmt);
			dvbpsi_delete(pmt->pmt);
			free(pmt);
			pmt = next;
		}
		ctx->pmts = NULL;
	}
#endif
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		event_listener_t *next = listener->next;
		free(listener);
		listener = next;
	}
	free(ctx);
}

const demux_ops_t *demux_dvb = &(demux_ops_t)
{
	.name = "demux_dvb",
	.protocol = "dvb",
	.init = demux_init,
	.jitter = demux_jitter,
	.run = demux_run,
	.mime = demux_mime,
	.eventlistener = demux_eventlistener,
	.attach = demux_attach,
	.estream = demux_estream,
	.destroy = demux_destroy,
};

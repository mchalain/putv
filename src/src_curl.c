/*****************************************************************************
 * src_file.c
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
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define __USE_GNU
#include <pthread.h>

#include <curl/curl.h>

#include "player.h"
#include "event.h"
typedef struct src_ops_s src_ops_t;
typedef struct src_ctx_s src_ctx_t;
struct src_ctx_s
{
	const src_ops_t *ops;
	int dumpfd;
	player_ctx_t *player;
	jitter_t *out;
	unsigned char *outbuffer;
	size_t outlen;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	enum
	{
		SRC_STOP,
		SRC_RUN,
		SRC_END,
	} state;
	CURL *curl;
	const char *mime;
	decoder_t *estream;
	long pid;
	event_listener_t *listener;
};
#define SRC_CTX
#include "src.h"
#include "jitter.h"
#include "media.h"
#include "decoder.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define src_dbg(...)

#ifdef DEBUG
#define SRC_CURL_VERBOSE 1L
#else
#define SRC_CURL_VERBOSE 0L
#endif

static uint write_cb(char *in, uint size, uint nmemb, src_ctx_t *ctx)
{
	size_t writelen = 0;
	size_t len = ctx->out->ctx->size;

	nmemb *= size;
	if (ctx->state == SRC_STOP)
		return nmemb;
	while (nmemb > 0)
	{
		ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
		if (ctx->outbuffer == NULL)
		{
			dbg("src: out buffer stop");
			return 0;
		}
		len = ctx->out->ctx->size;
		if (len > nmemb)
			len = nmemb;
		memcpy(ctx->outbuffer, in + writelen, len);
#ifdef CURL_DUMP
		if (ctx->dumpfd > 0 && len > 0)
		{
			write(ctx->dumpfd, in + writelen, len);
		}
#endif
		writelen += len;
		nmemb -= len;
		ctx->out->ops->push(ctx->out->ctx, len, NULL);
		sched_yield();
	}
	src_dbg("src: curl read %ld", writelen);
	return writelen;
}

static int xferinfo(void *arg,
		curl_off_t dltotal, curl_off_t dlnow,
		curl_off_t ultotal, curl_off_t ulnow)
{
	src_ctx_t *ctx = (src_ctx_t *)arg;
	if (ctx->state == SRC_END)
		return 1;
	return 0;
}

static src_ctx_t *_src_init(player_ctx_t *player, const char * arg, const char *mime)
{
	src_ctx_t *ctx;
	CURL *curl;
	curl = curl_easy_init();
	if (curl)
	{
		ctx = calloc(1, sizeof(*ctx));
		ctx->ops = src_curl;
		ctx->curl = curl;
		ctx->player = player;
		ctx->mime = mime;
		pthread_mutex_init(&ctx->mutex, NULL);
		pthread_cond_init(&ctx->cond, NULL);

		curl_easy_setopt(curl, CURLOPT_URL, arg);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_VERBOSE, SRC_CURL_VERBOSE);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
		//curl_easy_setopt(curl, CURLOPT_USERPWD, "user:password");
		//curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);

#ifdef CURL_DUMP
		ctx->dumpfd = open("curl_dump.mp3", O_RDWR | O_CREAT, 0644);
#endif
		dbg("src: %s", src_curl->name);
	}
	return ctx;
}

static void *_src_thread(void *arg)
{
	src_ctx_t *ctx = (src_ctx_t *)arg;
	src_dbg("src: curl running");
	int ret = curl_easy_perform(ctx->curl);
	if ( ret != CURLE_OK)
	{
		err("src curl error %d on %p", ret, ctx->curl);
		return 0;
	}
	dbg("src: end of stream");
	ctx->out->ops->flush(ctx->out->ctx);
	const src_t src = { .ops = src_curl, .ctx = ctx};
	event_end_es_t event = {.pid = ctx->pid, .src = &src, .decoder = ctx->estream};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		listener->cb(listener->arg, SRC_EVENT_END_ES, (void *)&event);
		listener = listener->next;
	}
	return 0;
}

static int _src_prepare(src_ctx_t *ctx, const char *info)
{
	src_dbg("src: prepare");
	/**
	 * decoder may need data to prepare the stream.
	 * The thread starts for the preparation and
	 * will be waiting until the running state
	 */
	int ret = pthread_create(&ctx->thread, NULL, _src_thread, ctx);
	pthread_mutex_lock(&ctx->mutex);
	ctx->state = SRC_RUN;
	pthread_cond_broadcast(&ctx->cond);
	pthread_mutex_unlock(&ctx->mutex);

	const src_t src = { .ops = src_curl, .ctx = ctx};
	event_new_es_t event = {.pid = ctx->pid, .src = &src, .mime = ctx->mime, .jitte = JITTE_HIGH};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		listener->cb(listener->arg, SRC_EVENT_NEW_ES, (void *)&event);
		listener = listener->next;
	}
	pthread_mutex_lock(&ctx->mutex);
	ctx->state = SRC_STOP;
	pthread_cond_broadcast(&ctx->cond);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

static int _src_run(src_ctx_t *ctx)
{
	src_dbg("src: running");
	pthread_mutex_lock(&ctx->mutex);
	ctx->state = SRC_RUN;
	pthread_cond_broadcast(&ctx->cond);
	pthread_mutex_unlock(&ctx->mutex);
	const src_t src = { .ops = src_curl, .ctx = ctx};
	event_decode_es_t event = {.pid = ctx->pid, .src = &src, .decoder = ctx->estream};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		listener->cb(listener->arg, SRC_EVENT_DECODE_ES, (void *)&event);
		listener = listener->next;
	}
	return 0;
}

static const char *_src_mime(src_ctx_t *ctx, int index)
{
	if (index > 0)
		return NULL;
	const char *mime = ctx->mime;
	if (ctx->mime == NULL)
	{
		CURLcode ret = curl_easy_getinfo(ctx->curl, CURLINFO_CONTENT_TYPE, &mime);
		if (ret == CURLE_OK)
			ctx->mime = utils_mime2mime(mime);
	}
	return mime;
}

static void _src_eventlistener(src_ctx_t *ctx, event_listener_cb_t cb, void *arg)
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

static int _src_attach(src_ctx_t *ctx, long index, decoder_t *decoder)
{
	if (index > 0)
		return -1;
	ctx->estream = decoder;
	ctx->out = ctx->estream->ops->jitter(ctx->estream->ctx, JITTE_HIGH);
	curl_easy_setopt(ctx->curl, CURLOPT_BUFFERSIZE, ctx->out->ctx->size);
	curl_easy_setopt(ctx->curl, CURLOPT_MAX_RECV_SPEED_LARGE,
				(curl_off_t)ctx->out->ctx->size * ctx->out->ctx->count);
}

static decoder_t *_src_estream(src_ctx_t *ctx, long index)
{
	return ctx->estream;
}

static void _src_destroy(src_ctx_t *ctx)
{
	dbg("src: destroy");
	if (ctx->out != NULL)
	{
		ctx->out->ops->flush(ctx->out->ctx);
	}
	if (ctx->thread)
	{
		pthread_mutex_lock(&ctx->mutex);
		ctx->state = SRC_END;
		pthread_cond_broadcast(&ctx->cond);
		pthread_mutex_unlock(&ctx->mutex);
		pthread_join(ctx->thread, NULL);
	}
	if (ctx->estream != NULL)
		ctx->estream->ops->destroy(ctx->estream->ctx);
#ifdef CURL_DUMP
	if (ctx->dumpfd > 0)
		close(ctx->dumpfd);
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

static const char *_src_medium()
{
	return mime_directory;
}

const src_ops_t *src_curl = &(src_ops_t)
{
	.name = "curl",
	.protocol = "http://|https://|file://",
	.medium = _src_medium,
	.init = _src_init,
	.prepare = _src_prepare,
	.run = _src_run,
	.mime = _src_mime,
	.eventlistener = _src_eventlistener,
	.attach = _src_attach,
	.estream = _src_estream,
	.destroy = _src_destroy,
};

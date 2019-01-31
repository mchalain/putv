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
	CURL *curl;
};
#define SRC_CTX
#include "src.h"
#include "jitter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define src_dbg(...)

static uint write_cb(char *in, uint size, uint nmemb, src_ctx_t *ctx)
{
	size_t writelen = 0;
	size_t len = ctx->out->ctx->size;

	nmemb *= size;
	while (nmemb > 0)
	{
		if (player_waiton(ctx->player, STATE_PAUSE) < 0)
		{
			ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
			ctx->out->ops->push(ctx->out->ctx, 0, NULL);
			return 0;
		}

		ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
		if (ctx->outbuffer == NULL)
			return 0;
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
		pthread_yield();
	}

	return writelen;
}

static src_ctx_t *src_init(player_ctx_t *player, const char * arg)
{
	src_ctx_t *ctx;
	CURL *curl;
	curl = curl_easy_init();
	if (curl)
	{
		curl_easy_setopt(curl, CURLOPT_URL, arg);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
		//curl_easy_setopt(curl, CURLOPT_USERPWD, "user:password");
		//curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
		curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)31415);

		ctx = calloc(1, sizeof(*ctx));
		ctx->ops = src_curl;
		ctx->curl = curl;
		ctx->player = player;

		curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
#ifdef CURL_DUMP
		ctx->dumpfd = open("curl_dump.mp3", O_RDWR | O_CREAT);
#endif
	}
	return ctx;
}

static void *src_thread(void *arg)
{
	src_ctx_t *ctx = (src_ctx_t *)arg;
	int ret = curl_easy_perform(ctx->curl);
	if ( ret != CURLE_OK)
	{
		dbg("src curl error %d on %p", ret, ctx->curl);
	}
	ctx->outbuffer = ctx->out->ops->pull(ctx->out->ctx);
	ctx->out->ops->push(ctx->out->ctx, 0, NULL);
	ctx->out->ops->flush(ctx->out->ctx);
	return 0;
}

static int src_run(src_ctx_t *ctx, jitter_t *out)
{
	int ret;
	ctx->out = out;
	ret = curl_easy_setopt(ctx->curl, CURLOPT_BUFFERSIZE, ctx->out->ctx->size);
	//ret = curl_easy_perform(ctx->curl);
	ret = pthread_create(&ctx->thread, NULL, src_thread, ctx);
	return ret;
}

static void src_destroy(src_ctx_t *ctx)
{
	pthread_join(ctx->thread, NULL);
#ifdef CURL_DUMP
	if (ctx->dumpfd > 0)
		close(ctx->dumpfd);
#endif
	curl_easy_cleanup(ctx->curl);
	free(ctx);
}

const src_ops_t *src_curl = &(src_ops_t)
{
	.protocol = "http://|file://",
	.init = src_init,
	.run = src_run,
	.destroy = src_destroy,
};

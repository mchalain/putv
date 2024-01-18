/*****************************************************************************
 * src_unix.c
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
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <pwd.h>

#include "player.h"
#include "jitter.h"
#include "event.h"
typedef struct src_ops_s src_ops_t;
typedef struct src_ctx_s src_ctx_t;
struct src_ctx_s
{
	player_ctx_t *player;
	const char *mime;
	int handle;
	state_t state;
	pthread_t thread;
	jitter_t *out;
	unsigned int samplerate;
	decoder_t *estream;
	event_listener_t *listener;
};
#define SRC_CTX
#include "src.h"
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

static const char *jitter_name = "unix socket";
static src_ctx_t *_src_init(player_ctx_t *player, const char *url, const char *mime)
{
	int count = 2;
	int ret;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;

	const char *protocol = NULL;
	const char *path = NULL;
	void *value = utils_parseurl(url, &protocol, NULL, NULL, &path, NULL);
	if (protocol && strcmp(protocol, "unix") != 0)
	{
		free(value);
		return NULL;
	}
	if (path != NULL)
	{
		if (path[0] == '~')
		{
			struct passwd *pw = NULL;
			pw = getpwuid(geteuid());
			if (chdir(pw->pw_dir) == 0)
				path++;
			if (path[0] == '/')
				path++;
		}
		struct stat filestat;
		ret = stat(path, &filestat);
		if (ret != 0 || !S_ISSOCK(filestat.st_mode))
		{
			err("error: %s is not a socket", path);
			free(value);
			return NULL;
		}
		strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	}
	else
	{
		free(value);
		return NULL;
	}
	free(value);

	int handle = socket(AF_UNIX, SOCK_STREAM, 0);
	if (handle == 0)
	{
		err("connection error on socket %s", path);
		return NULL;
	}

	ret = connect(handle, (struct sockaddr *) &addr, sizeof(addr));
	if (ret < 0)
	{
		err("connection error on socket %s", path);
		close(handle);
		return NULL;
	}

	src_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->player = player;
	ctx->handle = handle;
	ctx->mime = mime;
	if (ctx->mime == mime_octetstream)
	{
#ifdef DECODER_LAME
		ctx->mime = mime_audiomp3;
#elif defined(DECODER_FLAC)
		ctx->mime = mime_audioflac;
#endif
	}
	dbg("src: %s", src_unix->name);
	return ctx;
}

static void *_src_thread(void *arg)
{
	src_ctx_t *ctx = (src_ctx_t *)arg;
	int ret;
	while (ctx->state != STATE_ERROR)
	{
		unsigned char *buff = ctx->out->ops->pull(ctx->out->ctx);
		if (buff == NULL)
		{
			ctx->state = STATE_ERROR;
			break;
		}
		ret = recv(ctx->handle, buff, ctx->out->ctx->size, MSG_NOSIGNAL);
		if (ret == -EPIPE)
		{
		}
		if (ret < 0)
		{
			ctx->state = STATE_ERROR;
			err("sink: error write pcm %d", ret);
		}
		else if (ret == 0)
		{
			ctx->out->ops->push(ctx->out->ctx, 0, NULL);
			ctx->out->ops->flush(ctx->out->ctx);
			break;
		}
		else
		{
			src_dbg("sink: play %d", ret);
			ctx->out->ops->push(ctx->out->ctx, ret, NULL);
		}
	}
	dbg("sink: thread end");
	ctx->out->ops->flush(ctx->out->ctx);
	const src_t src = { .ops = src_unix, .ctx = ctx};
	event_end_es_t event = {.pid = 0, .src = &src, .decoder = ctx->estream};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		listener->cb(listener->arg, SRC_EVENT_END_ES, (void *)&event);
		listener = listener->next;
	}
	return NULL;
}

static int _src_run(src_ctx_t *ctx)
{
	const src_t src = { .ops = src_unix, .ctx = ctx};
	event_new_es_t event = {.pid = 0, .src = &src, .mime = ctx->mime, .jitte = JITTE_MID};
	event_decode_es_t event_decode = {.src = &src};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		listener->cb(listener->arg, SRC_EVENT_NEW_ES, (void *)&event);
		event_decode.pid = event.pid;
		event_decode.decoder = event.decoder;
		listener->cb(listener->arg, SRC_EVENT_DECODE_ES, (void *)&event_decode);
		listener = listener->next;
	}
	pthread_create(&ctx->thread, NULL, _src_thread, ctx);
	return 0;
}

static const char *_src_mime(src_ctx_t *ctx, int index)
{
	if (index > 0)
		return NULL;
	return ctx->mime;
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
	ctx->out = ctx->estream->ops->jitter(ctx->estream->ctx, JITTE_MID);
}

static decoder_t *_src_estream(src_ctx_t *ctx, long index)
{
	return ctx->estream;
}

static void _src_destroy(src_ctx_t *ctx)
{
	if (ctx->estream != NULL)
		ctx->estream->ops->destroy(ctx->estream->ctx);
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		event_listener_t *next = listener->next;
		free(listener);
		listener = next;
	}
	close(ctx->handle);
	free(ctx);
}

static const char *_src_medium()
{
	return mime_octetstream;
}

const src_ops_t *src_unix = &(src_ops_t)
{
	.name = "unix",
	.protocol = "unix://|file://",
	.medium = _src_medium,
	.init = _src_init,
	.run = _src_run,
	.mime = _src_mime,
	.eventlistener = _src_eventlistener,
	.attach = _src_attach,
	.estream = _src_estream,
	.destroy = _src_destroy,
};

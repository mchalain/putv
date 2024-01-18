/*****************************************************************************
 * src_alsa.c
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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <pwd.h>

#include "player.h"
#include "decoder.h"
#include "media.h"
#include "src.h"
#include "demux.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define src_dbg(...)

#define MAX_SRCOPS 10

static void src_init(void) __attribute__((constructor));
const src_ops_t *src_list[MAX_SRCOPS];

static int _src_ops_register(const src_ops_t *srcops, const src_ops_t **list)
{
	for (int i = 0; i < MAX_SRCOPS; i++)
	{
		if (list[i] == NULL || list[i] == srcops)
		{
			list[i] = srcops;
			return i;
		}
	}
	return -1;
}

int src_ops_register(const src_ops_t *srcops)
{
	return _src_ops_register(srcops, src_list);
}

void src_init(void)
{
#ifdef SRC_FILE
	src_ops_register(src_file);
#endif
#ifdef SRC_UNIX
	src_ops_register(src_unix);
#endif
#ifdef SRC_CURL
	src_ops_register(src_curl);
#endif
#ifdef SRC_ALSA
	src_ops_register(src_alsa);
#endif
#ifdef SRC_UDP
	src_ops_register(src_udp);
#endif
}

static int _src_find(const src_ops_t *const src_list[],
					const char *url)
{
	int i = 0;
	while (src_list[i] != NULL)
	{
		src_dbg("src: check %s", src_list[i]->name);
		const char *protocol = src_list[i]->protocol;
		int len = strlen(protocol);
		while (protocol != NULL)
		{
			const char *next = strchr(protocol,'|');
			if (next != NULL)
				len = next - protocol;
			src_dbg("src: check protocol %.*s", len, protocol);
			if (!(strncmp(url, protocol, len)))
			{
				return i;
			}
			protocol = next;
			if (protocol)
			{
				protocol++;
				len = strlen(protocol);
			}
		}
		i++;
	}
	return -1;
}
static src_t *_src_build(const src_ops_t *const src_list[],
		player_ctx_t *player, const char *url,
		const char *mime, const char *info)
{
	const src_ops_t *src_ops = NULL;
	src_ctx_t *src_ctx = NULL;
	src_dbg("src: source %s", url);
	int src_id = _src_find(src_list, url);
	if (src_id < 0)
	{
		err("src not found %s", url);
		return NULL;
	}
	src_ctx = src_list[src_id]->init(player, url, mime);
	src_ops = src_list[src_id];
	src_t *src = calloc(1, sizeof(*src));
	src->ops = src_ops;
	src->ctx = src_ctx;
	src->mediaid = -1;
	if (info)
		src->info = strdup(info);

	return src;
}

src_t *src_build(player_ctx_t *player, const char *url, const char *mime, int id, const char *info)
{
	src_t *src = _src_build(src_list, player, url, mime, info);
	if (src != NULL)
		src->mediaid = id;
	return src;
}

void src_destroy(src_t *src)
{
	src->ops->destroy(src->ctx);
	if (src->info != NULL)
		free(src->info);
	free(src);
}

const char *_src_mime(const char *protocol, const src_ops_t *const *ops)
{
	int src_id = _src_find(ops, protocol);
	if (src_id < 0)
		return NULL;
	return ops[src_id]->medium;
}

const char *src_mime(const char *protocol)
{
	return _src_mime(protocol, src_list);
}

#ifdef DEMUX_PASSTHROUGH
static void demux_init(void) __attribute__((constructor));
const src_ops_t *demux_list[MAX_SRCOPS];

int demux_ops_register(const src_ops_t *srcops)
{
	return _src_ops_register(srcops, demux_list);
}

static void demux_init()
{
	demux_ops_register(demux_passthrough);
#ifdef DEMUX_RTP
	demux_ops_register(demux_rtp);
#endif
#ifdef DEMUX_DVB
	demux_ops_register(demux_dvb);
#endif
}

const char *demux_mime(const char *protocol)
{
	return _src_mime(protocol, demux_list);
}

demux_t *demux_build(player_ctx_t *player, const char *url, const char *mime)
{
	return _src_build(demux_list, player, url, mime, NULL);
}
#endif

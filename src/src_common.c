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
#include "src.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define src_dbg(...)

src_t *src_build(player_ctx_t *player, const char *url, const char *mime)
{
	const src_ops_t *const src_list[] = {
	#ifdef SRC_FILE
		src_file,
	#endif
	#ifdef SRC_UNIX
		src_unix,
	#endif
	#ifdef SRC_CURL
		src_curl,
	#endif
		NULL
	};

	const src_ops_t *src_default = NULL;
	#if defined(SRC_FILE)
	src_default = src_file;
	#elif defined(SRC_UNIX)
	src_default = src_unix;
	#elif defined(SRC_CURL)
	src_default = src_curl;
	#endif

	int i = 0;
	src_ctx_t *src_ctx = NULL;
	while (src_list[i] != NULL)
	{
		const char *protocol = src_list[i]->protocol;
		int len = strlen(protocol);
		while (protocol != NULL)
		{
			const char *next = strchr(protocol,'|');
			if (next != NULL)
				len = next - protocol;
			if (!(strncmp(url, protocol, len)))
			{
				src_ctx = src_list[i]->init(player, url);
				src_default = src_list[i];
				break;
			}
			protocol = next;
			if (protocol)
			{
				protocol++;
				len = strlen(protocol);
			}
		}
		if (src_ctx != NULL)
			break;
		i++;
	}

	if (src_ctx == NULL && src_default != NULL)
	{
		src_ctx = src_default->init(player, url);
	}
	if (src_ctx == NULL)
	{
		err("src not found %s", url);
		return NULL;
	}
	src_t *src = calloc(1, sizeof(*src));
	src->ops = src_default;
	src->ctx = src_ctx;

	src->audio[0] = decoder_build(player, mime);
	return src;
}

/*****************************************************************************
 * media_common.c
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
#include <stdlib.h>
#include <string.h>

#include "media.h"
#include "decoder.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

const char const *mime_octetstream = "octet/stream";

const char *utils_getmime(const char *path)
{
#ifdef DECODER_MAD
	if (!decoder_mad->check(path))
		return decoder_mad->mime;
#endif
#ifdef DECODER_FLAC
	if (!decoder_flac->check(path))
		return decoder_flac->mime;
#endif
	return mime_octetstream;
}

const char *utils_getpath(const char *url, const char *proto)
{
	const char *path = strstr(url, proto);
	if (path == NULL)
	{
		if (strstr(url, "://"))
		{
			return NULL;
		}
		path = url;
	}
	else
		path += strlen(proto);
	return path;
}

media_t *media_build(const char *url)
{
	const media_ops_t *const media_list[] = {
	#ifdef MEDIA_DIR
		media_dir,
	#endif
	#ifdef MEDIA_SQLITE
		media_sqlite,
	#endif
	#ifdef MEDIA_FILE
		media_file,
	#endif
		NULL
	};

	int i = 0;
	media_ctx_t *media_ctx = NULL;
	while (media_list[i] != NULL)
	{
		media_ctx = media_list[i]->init(url);
		if (media_ctx != NULL)
			break;
		i++;
	}
	if (media_ctx == NULL)
	{
		err("media not found %s", url);
		return NULL;
	}
	media_t *media = calloc(1, sizeof(*media));
	media->ops = media_list[i];
	media->ctx = media_ctx;

	return media;
}

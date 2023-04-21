/*****************************************************************************
 * decoder_mad.c
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>

#ifdef DECODER_MODULES
#include <dlfcn.h>
#include <dirent.h>
#endif

#include "player.h"
#include "decoder.h"
#include "filter.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define decoder_dbg(...)

static const decoder_ops_t * decoderslist [10];

#ifdef DECODER_MODULES
static const decoder_ops_t * decoder_load_module(const char *root, const char *name)
{
	const decoder_ops_t *ops = NULL;
	if (name != NULL)
	{
		char *file = NULL;
		if (!strncmp("decoder_", name, 7) &&
			asprintf(&file, "%s/%s", root, name) > 0)
		{
			void *dh = dlopen(file, RTLD_NOW | RTLD_DEEPBIND | RTLD_GLOBAL);
			if (dh == NULL)
			{
				err("ERROR: No such output library: '%s %s'", file, dlerror());
			}
			else
			{
				ops = dlsym(dh, "decoder_ops");
				dbg("new decoder %p", ops);
			}
			free(file);
		}
	}
	return ops;
}
#endif

decoder_t *decoder_build(player_ctx_t *player, const char *mime)
{
	decoder_t *decoder = NULL;
	decoder_ctx_t *ctx = NULL;
	const decoder_ops_t *ops = NULL;
	int i = 0;
	if (mime)
	{
		while (decoderslist[i] != NULL)
		{
			if (!strcmp(mime, decoderslist[i]->mime(NULL)))
			{
				ops = decoderslist[i];
				break;
			}
			i++;
		}
	}

	if (ops != NULL)
	{
		ctx = ops->init(player);
	}
	if (ctx != NULL)
	{
		warn("decoder: new %s for %s", ops->name, ops->mime(NULL));
		decoder = calloc(1, sizeof(*decoder));
		decoder->ops = ops;
		decoder->ctx = ctx;
	}
	return decoder;
}

const char *decoder_mimelist(int first)
{
	const char *mime = NULL;
	static int i = 0;
	if (first)
		i = 0;
	if (decoderslist[i] != NULL)
	{
		mime = decoderslist[i]->mime(NULL);
		i++;
	}
	else
		i = 0;
	return mime;
}

const char *decoder_mime(const char *path)
{
	int i = 0;
	const decoder_ops_t *ops = decoderslist[i];
	while (ops != NULL)
	{
		if (ops->checkin(NULL, path))
		{
			return ops->mime(NULL);
			break;
		}
		i++;
		ops = decoderslist[i];
	}
	return NULL;
}

static void _decoder_init(void) __attribute__((constructor));

static void _decoder_init(void)
{
	const decoder_ops_t *decoders[] = {
#ifndef DECODER_MODULES
#ifdef DECODER_MAD
		decoder_mad,
#endif
#ifdef DECODER_FLAC
		decoder_flac,
#endif
#ifdef DECODER_FAAD2
		decoder_faad2,
#endif
#endif
#ifdef DECODER_PASSTHROUGH
		decoder_passthrough,
#endif
		NULL
	};

	int i;
	for (i = 0; i < 10 && decoders[i] != NULL; i++)
	{
		decoderslist[i] = decoders[i];
	}

#ifdef DECODER_MODULES
	struct dirent **namelist;
	int n;

	n = scandir(PKGLIBDIR, &namelist, NULL, alphasort);
	while (n > 0)
	{
		n--;
		if (namelist[n]->d_name[0] != '.')
		{
			const decoder_ops_t *ops = decoder_load_module(PKGLIBDIR, namelist[n]->d_name);
			if (ops != NULL)
				decoderslist[i++] = ops;
			if (i >= 10)
				break;
		}
	}
#endif
}

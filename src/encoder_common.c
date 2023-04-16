/*****************************************************************************
 * encoder_common.c
 * this file is part of https://github.com/ouistiti-project/putv
 *****************************************************************************
 * Copyright (C) 2022-2024
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
 */
#include <stdio.h>
#include <string.h>

#include "encoder.h"
#include "media.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

const encoder_ops_t *encoder_check(const char *path)
{
	const encoder_ops_t *encoder = ENCODER;
	const char *pathend = strchr(path, '?');
	if (pathend == NULL)
		pathend = strchr(path, '&');
	int len = pathend - path;
	if (pathend == NULL)
		len = strlen(path);
	const char *ext = strrchr(path, '.');
	if (ext != NULL)
	{
#ifdef ENCODER_LAME
		if (!strncmp(ext, ".mp3", len))
			encoder = encoder_lame;
#endif
#ifdef ENCODER_FLAC
		if (!strncmp(ext, ".flac", len))
			encoder = encoder_flac;
#endif
	}
	else
	{
#ifdef ENCODER_LAME
		if (!strncmp(path, mime_audiomp3, len))
			encoder = encoder_lame;
#endif
#ifdef ENCODER_FLAC
		if (!strncmp(path, mime_audioflac, len))
			encoder = encoder_flac;
#endif
	}
	return encoder;
}

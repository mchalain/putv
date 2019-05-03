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
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

#include "media.h"
#include "decoder.h"
#include "player.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

const char const *mime_octetstream = "octet/stream";
const char const *mime_audiomp3 = "audio/mp3";
const char const *mime_audioflac = "audio/flac";
const char const *mime_audioalac = "audio/alac";
const char const *mime_audiopcm = "audio/pcm";

void utils_srandom()
{
	unsigned int seed;
	if (!access(RANDOM_DEVICE, R_OK))
	{
		int fd = open(RANDOM_DEVICE, O_RDONLY);
		sched_yield();
		read(fd, &seed, sizeof(seed));
		close(fd);
	}
	else
	{
		seed = time(NULL);
	}
	srandom(seed);
}

const char *utils_getmime(const char *path)
{
#ifdef DECODER_MAD
	if (!decoder_mad->check(path))
		return decoder_mad->mime(NULL);
#endif
#ifdef DECODER_FLAC
	if (!decoder_flac->check(path))
		return decoder_flac->mime(NULL);
#endif
#ifdef DECODER_PASSTHROUGH
	if (!decoder_passthrough->check(path))
		return decoder_passthrough->mime(NULL);
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

char *utils_parseurl(const char *url, char **protocol, char **host, char **port, char **path, char **search)
{
	char *turl = malloc(strlen(url) + 1 + 1);
	strcpy(turl, url);

	char *str_protocol = turl;
	char *str_host = strstr(turl, "://");
	if (str_host == NULL)
	{
		if (protocol)
			*protocol = NULL;
		if (host)
			*host = NULL;
		if (path)
			*path = turl;
		return turl;
	}
	*str_host = '\0';
	str_host += 3;
	char *str_port = strchr(str_host, ':');
	char *str_path = strchr(str_host, '/');
	char *str_search = strchr(str_host, '?');

	if (str_port != NULL)
	{
		if (str_path && str_path < str_port)
		{
			str_port = NULL;
		}
		else if (str_search && str_search < str_port)
		{
			str_port = NULL;
		}
		else
		{
			*str_port = '\0';
			str_port += 1;
		}
	}
	if (str_path != NULL)
	{
		if (str_search && str_search < str_path)
		{
			str_path = NULL;
		}
		else
		{
			memmove(str_path + 1, str_path, strlen(str_path) + 1);
			*str_path = '\0';
			str_path += 1;
		}
	}
	if (str_search != NULL)
	{
		*str_search = '\0';
		str_search += 1;
	}
	if (protocol)
		*protocol = str_protocol;
	if (host)
		*host = str_host;
	if (port)
		*port = str_port;
	if (path)
		*path = str_path;
	if (search)
		*search = str_search;
	return turl;
}

const char *utils_mime2mime(const char *mime)
{
	const char *mime2 = NULL;
	if (mime != NULL)
	{
		if (!strcmp(mime, mime_audiomp3))
			mime2 = mime_audiomp3;
		if (!strcmp(mime, mime_audioflac))
			mime2 = mime_audioflac;
		if (!strcmp(mime, mime_audiopcm))
			mime2 = mime_audiopcm;
	}
	return mime2;
}

const char *utils_format2mime(jitter_format_t format)
{
	switch (format)
	{
	case PCM_16bits_LE_mono:
	case PCM_16bits_LE_stereo:
	case PCM_24bits3_LE_stereo:
	case PCM_24bits4_LE_stereo:
	case PCM_32bits_LE_stereo:
	case PCM_32bits_BE_stereo:
		return mime_audiopcm;
	case MPEG2_3_MP3:
		return mime_audiomp3;
	case FLAC:
		return mime_audioflac;
	case MPEG2_1:
	case MPEG2_2:
		return mime_octetstream;
	case DVB_frame:
	case SINK_BITSSTREAM:
	default:
		break;
	}
	return mime_octetstream;
}

static char *current_path;
media_t *media_build(player_ctx_t *player, const char *url)
{
	if (url == NULL)
		return NULL;

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

	char *oldpath = current_path;
	current_path = strdup(url);

	int i = 0;
	media_ctx_t *media_ctx = NULL;
	while (media_list[i] != NULL)
	{
		media_ctx = media_list[i]->init(player, current_path);
		if (media_ctx != NULL)
			break;
		i++;
	}
	if (media_ctx == NULL)
	{
		free(current_path);
		current_path = oldpath;
		err("media not found %s", url);
		return NULL;
	}
	media_t *media = calloc(1, sizeof(*media));
	media->ops = media_list[i];
	media->ctx = media_ctx;
	if (oldpath)
		free(oldpath);

	return media;
}

const char *media_path()
{
	return current_path;
}

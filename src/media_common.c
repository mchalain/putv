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

#ifdef USE_ID3TAG
#include <id3tag.h>
#include <jansson.h>
#define N_(string) string
#endif

#ifdef USE_OGGMETADDATA
#include <FLAC/metadata.h>
#endif

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
const char const *mime_audiopcm = "audio/pcm";

const char const *str_title = "Title";
const char const *str_artist = "Artist";
const char const *str_album = "Album";
const char const *str_track = "Track";
const char const *str_year = "Year";
const char const *str_genre = "Genre";
const char const *str_date = "Date";

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

#ifdef USE_ID3TAG
int media_parseid3tag(const char *path, json_t *object)
{
	struct
	{
		char const *id;
		char const *label;
	} const labels[] =
	{
	{ ID3_FRAME_TITLE,  N_(str_title)     },
	{ ID3_FRAME_ARTIST, N_(str_artist)    },
	{ ID3_FRAME_ALBUM,  N_(str_album)     },
	{ ID3_FRAME_TRACK,  N_(str_track)     },
	{ ID3_FRAME_YEAR,   N_(str_year)      },
	{ ID3_FRAME_GENRE,  N_(str_genre)     },
	};
	struct id3_file *fd = id3_file_open(path, ID3_FILE_MODE_READONLY);
	if (fd == NULL)
		return -1;
	struct id3_tag *tag = id3_file_tag(fd);

	int i;
	for (i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
	{
		struct id3_frame const *frame;
		frame = id3_tag_findframe(tag, labels[i].id, 0);
		if (frame)
		{
			union id3_field const *field;
			id3_ucs4_t const *ucs4;
			field    = id3_frame_field(frame, 1);
			ucs4 = id3_field_getstrings(field, 0);
			if (labels[i].id == ID3_FRAME_GENRE && ucs4 != NULL)
				ucs4 = id3_genre_name(ucs4);
			json_t *value;
			if (ucs4 != NULL)
			{
				char *latin1 = id3_ucs4_utf8duplicate(ucs4);
				value = json_string(latin1);
				free(latin1);
			}
			else
				value = json_null();
			json_object_set(object, labels[i].label, value);
		}
	}
	id3_file_close(fd);
	return 0;
}
#endif

#ifdef USE_OGGMETADDATA
int media_parseoggmetadata(const char *path, json_t *object)
{
	struct
	{
		char const *id;
		char const *label;
		int const length;
		enum {
			label_string,
			label_integer,
		} type;
	} const labels[] =
	{
		{str_title, str_title, 5, label_string},
		{str_album, str_album, 5, label_string},
		{str_artist, str_artist, 6, label_string},
		{str_year, str_year, 4, label_integer},
		{str_genre, str_genre, 5, label_string},
		{str_date, str_year, 4, label_integer},
		{"TRACKNUMBER", str_track, 11, label_integer},
	};
	FLAC__StreamMetadata *vorbiscomment;
	FLAC__metadata_get_tags(path, &vorbiscomment);
	FLAC__StreamMetadata_VorbisComment *data;
	data = &vorbiscomment->data.vorbis_comment;
	int n;
	for (n = 0; n < data->num_comments; n++)
	{
		FLAC__StreamMetadata_VorbisComment_Entry *comments;
		comments = &data->comments[n];
		json_t *value;
		int i;
		for (i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
		{
			const char *svalue = comments->entry + labels[i].length;
			if (!strncasecmp(comments->entry, labels[i].id, labels[i].length) &&
				svalue[0] == '=')
			{
				svalue++;
				switch(labels[i].type)
				{
				case label_string:
					value = json_string(svalue);
				break;
				case label_integer:
					value = json_integer(atoi(svalue));
				break;
				}
				json_object_set(object, labels[i].label, value);
			}
		}
	}
	FLAC__metadata_object_delete(vorbiscomment);
	return 0;
}
#endif

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

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
#include <limits.h>
#include <stdint.h>

#ifdef USE_ID3TAG
#include <id3tag.h>
#define N_(string) string
#endif

#ifdef USE_OGGMETADDATA
#include <FLAC/metadata.h>
#endif

#include <jansson.h>

#include "media.h"
#include "decoder.h"
#include "src.h"
#include "player.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define media_dbg(...)

const char* const mime_octetstream = "octet/stream";
const char* const mime_audiomp3 = "audio/mp3";
const char* const mime_audioflac = "audio/flac";
const char* const mime_audioalac = "audio/alac";
const char* const mime_audioaac = "audio/aac";
const char* const mime_audiopcm = "audio/pcm";
const char* const mime_imagejpg = "image/jpg";
const char* const mime_imagepng = "image/png";
const char* const mime_directory = "inode/directory";

const char* const str_title = "title";
const char* const str_artist = "artist";
const char* const str_album = "album";
const char* const str_track = "track";
const char* const str_year = "year";
const char* const str_genre = "genre";
const char* const str_date = "date";
const char* const str_comment = "comment";
const char* const str_cover = "cover";
const char* const str_regain = "replaygain";
const char* const str_duration = "duration";
const char* const str_likes = "likes";

void utils_srandom()
{
	unsigned int seed = 0;
	if (!access(RANDOM_DEVICE, R_OK))
	{
		int fd = open(RANDOM_DEVICE, O_RDONLY);
		sched_yield();
		int ret = read(fd, &seed, sizeof(seed));
		if (ret != sizeof(seed))
			seed = time(NULL);
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
	const char *mime = decoder_mime(path);
	if (mime != NULL)
		return mime;
	mime = src_mime(path);
	if (mime != NULL)
		return mime;
	struct stat statinfo;
	if (stat(path, &statinfo) == 0 && S_ISDIR(statinfo.st_mode))
		return mime_directory;
	return mime_octetstream;
}

char *utils_getpath(const char *url, const char *proto, char **query, int strict)
{
	char *newpath = NULL;
	const char *path = strstr(url, proto);
	if (path == NULL)
	{
		if (strict || strstr(url, "://") != NULL)
		{
			return NULL;
		}
		path = url;
	}
	else
		path += strlen(proto);
	if (!strncmp(path,"://", 3))
		path+=3;
	int length = strlen(path);
	if (query != NULL)
	{
		*query = strchr(path, '?');
		if (*query != NULL)
		{
			length -= strlen(*query);
			*query += 1;
		}
	}
	if (path[0] == '~')
	{
		path++;
		if (path[0] == '/')
			path++;

		char *home = getenv("HOME");
		length += strlen(home);
		newpath = malloc(length + 2);
		snprintf(newpath, length + 2, "%s/%s", home, path);
	}
	else
	{
		/*
		 * TO CHECK
		 * on Buildroot:
		 * if the calloc allocate (length + 1) as necessary,
		 * for an absolute path the next call to calloc will crash.
		 * for a relative path, the application runs fine.
		 * if the calloc allocate (length + 2)
		 * the application runs fine in any cases.
		 * For the HOME with an absolute path (see under),
		 * the application runs fine in any cases.
		 */
		newpath = calloc(1, length + 2);
		strncpy(newpath, path, length + 1);
		newpath[length] = '\0';
	}
	newpath[length + 1] = 0;
	return newpath;
}

void *utils_parseurl(const char *url, const char **protocol, const char **host, const char **port, const char **path, const char **search)
{
	char *turl = malloc(strlen(url) + 1 + 1);
	strcpy(turl, url);

	char *str_protocol = turl;
	char *str_host = strchr(turl, ':');
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
	str_host += 1;
	if (!strncmp(str_host, "//", 2))
		str_host += 2;
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
	if (!strncmp(str_protocol, "file", 4))
		str_path = str_host;
	if (str_path != NULL)
	{
		if (str_search && str_search < str_path)
		{
			str_path = NULL;
		}
		else
		{
			/// We want to keep the first / of the path
			memmove(str_path + 1, str_path, strlen(str_path) + 1);
			*str_path = '\0';
			str_path += 1;
			str_search = strchr(str_path, '?');
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
	int length;
	if (mime != NULL)
	{
		length = strlen(mime_audiomp3);
		if (!strncmp(mime, mime_audiomp3, length))
			return mime_audiomp3;
		length = strlen(mime_audioflac);
		if (!strncmp(mime, mime_audioflac, length))
			return mime_audioflac;
		length = strlen(mime_audioalac);
		if (!strncmp(mime, mime_audioalac, length))
			return mime_audioalac;
		length = strlen(mime_audiopcm);
		if (!strncmp(mime, mime_audiopcm, length))
			return mime_audiopcm;
		length = strlen(mime_audioaac);
		if (!strncmp(mime, mime_audioaac, length))
			return mime_audioaac;
		length = strlen(mime_imagejpg);
		if (!strncmp(mime, mime_imagejpg, length))
			return mime_imagejpg;
		length = strlen(mime_imagepng);
		if (!strncmp(mime, mime_imagepng, length))
			return mime_imagepng;
	}
	return mime_octetstream;
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

static const char *media_regfile(const char *path, const char *mime, const unsigned char *data, unsigned long length)
{
	int fd = -1;
	char *ext = strrchr(path, '.');
	if (!strcmp(mime, mime_imagepng))
		strcpy(ext, ".png");
	else if (!strcmp(mime, mime_imagejpg) ||
		!strcmp(mime, "image/jpeg"))
		strcpy(ext, ".jpg");
	if (access(path, R_OK) != 0)
	{
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd > 0)
		{
			int ret = write(fd, data, length);
			if (ret != length)
				err("media: write image file error");
			close(fd);
		}
	}
	return path;
}

static char *media_tmpfile(char *path, const char *mime, const unsigned char *data, unsigned long length)
{
	static int fd = -1;
	if (fd > 0)
		close(fd);
	char *ext = strrchr(path, '.');
	if (!strcmp(mime, mime_imagepng))
		strcpy(ext, "XXXXXX.png");
	else if (!strcmp(mime, mime_imagejpg) ||
		!strcmp(mime, "image/jpeg"))
		strcpy(ext, "XXXXXX.jpg");
	fd = mkstemps(path, 4);
	if (fd > 0)
	{
		write(fd, data, length);
	}
	return path;
}

#ifdef USE_ID3TAG
#define ID3MAXFIELDS 5
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
	{ ID3_FRAME_COMMENT,N_(str_comment)   },
	{ "APIC",           N_(str_cover)   },
	{ "RGAD",           N_(str_regain)   },
	{ "TLEN",           N_(str_duration)   },
	};
	struct id3_file *fd = id3_file_open(path, ID3_FILE_MODE_READONLY);
	if (fd == NULL)
		return -1;
	struct id3_tag *tag = id3_file_tag(fd);

	int i;
#if 0
	for (i = 0; i < tag->nframes; i++)
	{
		struct id3_frame const *frame = NULL;
		frame = tag->frames[i];
		warn("frame %s", frame->id);
		media_dbg("field %d type %d", i, frame->type);
	}
#endif
	for (i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
	{
		json_t *value = NULL;
		struct id3_frame const *frame = NULL;
		int j = 0;
		frame = id3_tag_findframe(tag, labels[i].id, j);
		while (frame != NULL)
		{
			union id3_field const *field;
			const char *mimetype = "image/png";

			for (int fieldid = 0; (field = id3_frame_field(frame, fieldid)) != NULL && fieldid < ID3MAXFIELDS; fieldid++)
			{
				media_dbg("field[%s][%d] %s %d", frame->id, fieldid, labels[i].label, id3_field_type(field));
				switch (id3_field_type(field))
				{
				case ID3_FIELD_TYPE_TEXTENCODING:
				{
					enum id3_field_textencoding encoding = ID3_FIELD_TEXTENCODING_UTF_8;
					// the encoding is useless as we want utf8 for json string
					encoding = id3_field_gettextencoding(field);
					media_dbg("field encoding: %d", encoding);
				}
				break;
				case ID3_FIELD_TYPE_FRAMEID:
				{
					const char *frid = id3_field_getframeid(field);
					media_dbg("frameid %s", frid);
				}
				break;
				case ID3_FIELD_TYPE_STRINGLIST:
				{
					int nb = 0;
					int k = 0;
					nb = id3_field_getnstrings(field);
					if (nb > 1)
					{
						value = json_array();
						for (k = 0; k < nb; k++)
						{
							json_t *fieldvalue;
							id3_ucs4_t const *ucs4 = NULL;
							ucs4 = id3_field_getstrings(field, k);
							if (labels[i].id == ID3_FRAME_GENRE)
								ucs4 = id3_genre_name(ucs4);
							id3_utf8_t *utf8 = id3_ucs4_utf8duplicate(ucs4);
							fieldvalue = json_string(utf8);
							json_array_append(value, fieldvalue);
							free(utf8);
						}
					}
					else if (nb == 1)
					{
						id3_ucs4_t const *ucs4 = NULL;
						ucs4 = id3_field_getstrings(field, k);
						if (labels[i].id == ID3_FRAME_GENRE)
							ucs4 = id3_genre_name(ucs4);
						id3_utf8_t *utf8 = id3_ucs4_utf8duplicate(ucs4);
						if (labels[i].id == ID3_FRAME_YEAR ||
							labels[i].id == ID3_FRAME_TRACK ||
							labels[i].id == "RGAD" ||
							labels[i].id == "TLEN")
							value = json_integer(atoi((char *)utf8));
						else
							value = json_string(utf8);
						free(utf8);
					}
				}
				break;
				case ID3_FIELD_TYPE_INT8:
				case ID3_FIELD_TYPE_INT16:
				case ID3_FIELD_TYPE_INT24:
				case ID3_FIELD_TYPE_INT32:
				{
					unsigned long integer = id3_field_getint(field);
					value = json_integer(integer);
				}
				break;
				case ID3_FIELD_TYPE_STRING:
				{
					id3_ucs4_t const *ucs4 = NULL;
					ucs4 = id3_field_getstring(field);
					id3_utf8_t *utf8 = id3_ucs4_utf8duplicate(ucs4);
					if (labels[i].id == ID3_FRAME_YEAR ||
						labels[i].id == ID3_FRAME_TRACK ||
						labels[i].id == "RGAD" ||
						labels[i].id == "TLEN")
						value = json_integer(atoi((char *)utf8));
					else
						value = json_string(utf8);
					free(utf8);
				}
				break;
				case ID3_FIELD_TYPE_STRINGFULL:
				{
					id3_ucs4_t const *ucs4 = NULL;
					ucs4 = id3_field_getfullstring(field);
					id3_utf8_t *utf8 = id3_ucs4_utf8duplicate(ucs4);
					value = json_string(utf8);
					free(utf8);
				}
				break;
				case ID3_FIELD_TYPE_LATIN1:
				{
					const char *latin1 = id3_field_getlatin1(field);
					id3_ucs4_t *ucs4 = id3_latin1_ucs4duplicate(latin1);
					id3_utf8_t *utf8 = id3_ucs4_utf8duplicate(ucs4);
					if (labels[i].id == "APIC")
						mimetype = latin1;
					else
						value = json_string(utf8);
					free(utf8);
					free(ucs4);
				}
				break;
				case ID3_FIELD_TYPE_LATIN1FULL:
				{
					const char *latin1 = id3_field_getfulllatin1(field);
					value = json_string(latin1);
				}
				break;
				case ID3_FIELD_TYPE_DATE:
				{

				}
				break;
				case ID3_FIELD_TYPE_BINARYDATA:
				{
					char coverpath[PATH_MAX];
					const unsigned char *data = NULL;
					unsigned long length = 0;

					data = id3_field_getbinarydata(field, &length);
					strcpy(coverpath, path);
					char *name = strrchr(coverpath, '/');
					if (name != NULL)
						name++;
					else
						name = coverpath;
					strcpy(name, "cover.XXX");
					const char *latin1 = media_regfile(coverpath, mimetype, data, length);
					value = json_string(latin1);
				}
				break;
				}
			}
			if (value == NULL)
				value = json_null();

			json_object_set(object, labels[i].label, value);

			j++;
			frame = id3_tag_findframe(tag, labels[i].id, j);
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
	FLAC__StreamMetadata *vorbiscomment = NULL;
	if (!FLAC__metadata_get_tags(path, &vorbiscomment))
	{
		dbg("media: no Vorbis Comment for %s", path);
		return -1;
	}
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

	/**
	 * picture
	 */
	FLAC__StreamMetadata *vorbispicture;
	FLAC__metadata_get_picture(path, &vorbispicture, FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER, NULL, NULL, -1, -1, -1, -1);
	if (vorbispicture != NULL)
	{
		FLAC__StreamMetadata_Picture *picture;
		picture = &vorbispicture->data.picture;

		char coverpath[PATH_MAX];
		strncpy(coverpath, path, PATH_MAX - 10);
		char *name = strrchr(coverpath, '/');
		if (name != NULL)
			name++;
		else
			name = coverpath;
		strcpy(name, "cover.XXX");

		json_t *value;
		value = json_string(media_regfile(coverpath, picture->mime_type, picture->data, picture->data_length));
		json_object_set(object, str_cover, value);

		FLAC__metadata_object_delete(vorbispicture);
	}
	return 0;
}
#endif

static const char *_last_info = NULL;
static json_t *_last_jinfo = NULL;
const char *media_parseinfo(const char *info, const char *key)
{
	const char *value = NULL;
	json_error_t error;
	if (_last_info != NULL && info != _last_info)
	{
		json_decref(_last_jinfo);
		_last_jinfo = NULL;
	}
	if (_last_jinfo == NULL)
	{
		_last_jinfo = json_loads(info, 0, &error);
		_last_info = info;
	}
	if (json_is_object(_last_jinfo))
	{
		json_t *jvalue;
		jvalue = json_object_get(_last_jinfo, key);
		if (jvalue != NULL && json_is_string(jvalue))
			value = json_string_value(jvalue);
	}
	return value;
}

unsigned int media_boost(const char *info)
{
	const char *sboost = media_parseinfo(info, str_regain);
	unsigned int boost = 0;
	if (sboost != NULL)
		boost = strtol(sboost, NULL, 10);
	return boost;
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

	media_ctx_t *media_ctx = NULL;
	const media_ops_t *media_ops = NULL;
	for (int i = 0; media_list[i] != NULL; i++)
	{
		dbg("media: try %s",media_list[i]->name);
		media_ctx = media_list[i]->init(player, current_path);
		if (media_ctx != NULL)
		{
			media_ops = media_list[i];
			break;
		}
	}
	if (media_ctx == NULL)
	{
		free(current_path);
		current_path = oldpath;
		err("media: %s not found", url);
		return NULL;
	}
	media_t *media = calloc(1, sizeof(*media));
	media->ops = media_ops;
	media->ctx = media_ctx;
	warn("media: %s new library", url);
	if (oldpath)
		free(oldpath);

	return media;
}

char *media_fillinfo(const char *url, const char *mime)
{
	int ret = 0;
	char *info = NULL;
	json_t *object = NULL;
	const char *path = url;

	if (strncmp(path, "file://", 7) == 0)
		path += 7;
	else
		return NULL;
	object = json_object();
#ifdef USE_ID3TAG
	if (mime && !strcmp(mime, mime_audiomp3))
	{
		ret = media_parseid3tag(path, object);
	}
#endif
#ifdef USE_OGGMETADDATA
	if (mime && !strcmp(mime, mime_audioflac))
	{
		ret = media_parseoggmetadata(path, object);
	}
#endif
	char coverpath[PATH_MAX];
	strcpy(coverpath, path);
	if (strlen(coverpath) >= 8)
	{
		char *dname = strrchr(coverpath, '/');
		if (dname == NULL)
			dname = coverpath;
		else
			dname++;

		strcpy(dname, "cover.jpg");
		if (!access(coverpath, R_OK))
		{
			json_t *value;
			value = json_string(coverpath);
			json_object_set(object, str_cover, value);
		}

		strcpy(dname, "cover.png");
		if (!access(coverpath, R_OK))
		{
			json_t *value;
			value = json_string(coverpath);
			json_object_set(object, str_cover, value);
		}

		strcpy(dname, "Notes.nfo");
		if (!access(coverpath, R_OK))
		{
			json_t *value;
			value = json_string(coverpath);
			json_object_set(object, str_comment, value);
		}
	}
#ifdef DEBUG
	info = json_dumps(object, JSON_INDENT(2));
#else
	info = json_dumps(object, 0);
#endif
	media_dbg("media info: %s", info);
	json_decref(object);
	return info;
}

const char *media_path()
{
	return current_path;
}

int media_parse_info(json_t *jinfo, char **ptitle, char **partist,
		char **palbum, char **pgenre, char **pcover, char **pcomment,
		int *plikes)
{
	if (json_is_object(jinfo))
	{
		json_t *value;
		value = json_object_get(jinfo, str_title);
		if (value != NULL && json_is_string(value))
		{
			*ptitle = strdup(json_string_value(value));
			json_object_del(jinfo, str_title);
		}
		value = json_object_get(jinfo, str_artist);
		if (value != NULL && json_is_string(value))
		{
			*partist = strdup(json_string_value(value));
			json_object_del(jinfo, str_artist);
		}
		value = json_object_get(jinfo, str_album);
		if (value != NULL && json_is_string(value))
		{
			*palbum = strdup(json_string_value(value));
			json_object_del(jinfo, str_album);
		}
		value = json_object_get(jinfo, str_genre);
		if (value != NULL && json_is_string(value))
		{
			*pgenre = strdup(json_string_value(value));
			json_object_del(jinfo, str_genre);
		}
		value = json_object_get(jinfo, str_cover);
		if (value != NULL && json_is_string(value))
		{
			*pcover = strdup(json_string_value(value));
			json_object_del(jinfo, str_cover);
		}
		value = json_object_get(jinfo, str_comment);
		if (value != NULL && json_is_string(value))
		{
			*pcomment = strdup(json_string_value(value));
			json_object_del(jinfo, str_comment);
		}
		value = json_object_get(jinfo, str_likes);
		if (value != NULL && json_is_integer(value))
		{
			*plikes = json_integer_value(value);
			json_object_del(jinfo, str_likes);
		}
	}
	return 0;
}

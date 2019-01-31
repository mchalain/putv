/*****************************************************************************
 * media_sqlite.c
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
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <sqlite3.h>

#include "player.h"
#include "media.h"

struct media_ctx_s
{
	sqlite3 *db;
	int mediaid;
	unsigned int options;
};

#define OPTION_LOOP 0x0001
#define OPTION_RANDOM 0x0002

#define PROTOCOLNAME "file://"
#define PROTOCOLNAME_LENGTH 7

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#ifdef DEBUG
#define SQLITE3_CHECK(ret, value, sql) \
		if (ret != SQLITE_OK) {\
			err("%s(%d) %s", __FUNCTION__, __LINE__, sql); \
			return value; \
		}
#else
#define SQLITE3_CHECK(...)
#endif

static int media_count(media_ctx_t *ctx);
static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
static int media_find(media_ctx_t *ctx, int id,  media_parse_t cb, void *data);
static int media_list(media_ctx_t *ctx, media_parse_t cb, void *data);
static int media_play(media_ctx_t *ctx, media_parse_t cb, void *data);
static int media_next(media_ctx_t *ctx);
static int media_end(media_ctx_t *ctx);

static const char *str_key_title = "Title";
static const char *str_key_artist = "Artist";
static const char *str_key_album = "Album";
static const char *str_key_genre = "Genre";

static int _execute(sqlite3_stmt *statement)
{
	int id = -1;
	int ret;

	ret = sqlite3_step(statement);
	while (ret == SQLITE_ROW)
	{
		int i = 0, nbColumns = sqlite3_column_count(statement);
		if (i < nbColumns)
		{
			//const char *key = sqlite3_column_name(statement, i);
			if (sqlite3_column_type(statement, i) == SQLITE_INTEGER)
			{
				id = sqlite3_column_int(statement, i);
			}
		}
		ret = sqlite3_step(statement);
	}
	return id;
}

static int media_count(media_ctx_t *ctx)
{
	sqlite3 *db = ctx->db;
	int count = 0;
	sqlite3_stmt *statement;
#ifndef MEDIA_SQLITE_EXT
	char *sql = "select \"id\" from \"media\" ";
#else
	char *sql = "select \"id\" from \"opus\" ";
#endif
	int ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, -1, sql);

	ret = sqlite3_step(statement);
	if (ret != SQLITE_ERROR)
	{
		do
		{
			count++;
			ret = sqlite3_step(statement);
		} while (ret == SQLITE_ROW);
	}
	sqlite3_finalize(statement);

	return count;
}

static int findmedia(sqlite3 *db, const char *path)
{
	sqlite3_stmt *statement;
#ifndef MEDIA_SQLITE_EXT
	char *sql = "select \"id\" from \"media\" where \"url\"=@PATH";
#else
	char *sql = "select \"opusid\" from \"media\" where \"url\"=@PATH";
#endif
	int ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, -1, sql);

	ret = sqlite3_bind_text(statement, sqlite3_bind_parameter_index(statement, "@PATH"), path, -1, SQLITE_STATIC);
	SQLITE3_CHECK(ret, -1, sql);

	int id = _execute(statement);

#ifdef MEDIA_SQLITE_EXT
	if (id == -1)
	{
		char *sql = "select \"id\" from \"word\" where \"name\"=@NAME";
		sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		/** set the default value of @FIELDS **/
		sqlite3_bind_text(statement, sqlite3_bind_parameter_index(statement, "@NAME"), path, -1, SQLITE_STATIC);

		int wordid = _execute(statement);
		if (wordid != -1)
		{
			const char *queries[] = {
				"select \"id\" from \"opus\" where \"titleid\"=@ID",
				"select \"id\" from \"opus\" inner join artist on artist.id=opus.artistid  where artist.wordid=@ID",
				"select \"id\" from \"opus\" inner join album on album.id=opus.albumid  where album.wordid=@ID",
				NULL
			};
			int i = 0;
			while (id == -1 && queries[i] != NULL)
			{
				sqlite3_prepare_v2(db, queries[i], -1, &statement, NULL);
				/** set the default value of @FIELDS **/
				sqlite3_bind_int(statement, sqlite3_bind_parameter_index(statement, "@ID"), wordid);
				id = _execute(statement);
				i++;
			}
		}
	}
#endif
	sqlite3_finalize(statement);
	return id;
}

static int media_remove(media_ctx_t *ctx, int id, const char *path)
{
	int ret;
	sqlite3 *db = ctx->db;
	if (path != NULL)
	{
		id = findmedia(db, path);
	}
	if (id > 0)
	{
		sqlite3_stmt *statement;
#ifndef MEDIA_SQLITE_EXT
		char *sql = "delete from \"media\" where \"id\"=@ID";
#else
		char *sql = "delete from \"media\" where \"opusid\"=@ID";
#endif
		ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		int index = sqlite3_bind_parameter_index(statement, "@ID");
		ret = sqlite3_bind_int(statement, index, id);
		SQLITE3_CHECK(ret, -1, sql);

		ret = sqlite3_step(statement);
		if (ret != SQLITE_DONE)
			ret = -1;
		else
		{
			dbg("putv: remove media %s", path);
		}
		sqlite3_finalize(statement);
	}
	return ret;
}

#ifdef MEDIA_SQLITE_EXT
static int opus_insert_word(media_ctx_t *ctx, const char *word, int *exist)
{
	sqlite3 *db = ctx->db;
	int ret;
	char *wordselect = "select \"id\" from \"word\" where \"name\" = @WORD";
	
	sqlite3_stmt *st_select;
	ret = sqlite3_prepare_v2(db, wordselect, -1, &st_select, NULL);
	SQLITE3_CHECK(ret, -1, wordselect);

	int index;
	index = sqlite3_bind_parameter_index(st_select, "@WORD");

	int id = -1;
	ret = sqlite3_bind_text(st_select, index, word, -1, SQLITE_STATIC);
	SQLITE3_CHECK(ret, -1, wordselect);

	ret = sqlite3_step(st_select);
	if (ret != SQLITE_ROW)
	{
		char *wordinsert = "insert into \"word\" (\"name\") values (@WORD)";
		sqlite3_stmt *st_insert;
		ret = sqlite3_prepare_v2(db, wordinsert, -1, &st_insert, NULL);
		SQLITE3_CHECK(ret, -1, wordinsert);

		ret = sqlite3_bind_text(st_insert, index, word, -1, SQLITE_STATIC);
		SQLITE3_CHECK(ret, -1, wordinsert);

		ret = sqlite3_step(st_insert);
		id = sqlite3_last_insert_rowid(db);
		if (exist != NULL)
			*exist = 0;
		sqlite3_finalize(st_insert);
	}
	else
	{
		int type;
		type = sqlite3_column_type(st_select, 0);
		if (type == SQLITE_INTEGER)
			id = sqlite3_column_int(st_select, 0);
	}
	sqlite3_finalize(st_select);
	return id;
}

static int opus_insert_info(media_ctx_t *ctx, const char *table, int wordid)
{
	sqlite3 *db = ctx->db;
	int ret;
	char *wordselect = "select \"id\" from \"%s\" where \"wordid\" = @WORDID";
	char *wordinsert = "insert into \"%s\" (\"wordid\") values (@WORDID)";

	char sql[48 + 20 + 1];
	snprintf(sql, 69, wordselect, table);

	sqlite3_stmt *st_select;
	ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
	SQLITE3_CHECK(ret, -1, sql);

	int index;
	index = sqlite3_bind_parameter_index(st_select, "@WORDID");

	int id = -1;
	ret = sqlite3_bind_int(st_select, index, wordid);
	SQLITE3_CHECK(ret, -1, sql);
	ret = sqlite3_step(st_select);
	if (ret != SQLITE_ROW)
	{
		snprintf(sql, 69, wordinsert, table);

		sqlite3_stmt *st_insert;
		ret = sqlite3_prepare_v2(db, sql, -1, &st_insert, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		ret = sqlite3_bind_int(st_insert, index, wordid);
		SQLITE3_CHECK(ret, -1, sql);

		ret = sqlite3_step(st_insert);
		sqlite3_finalize(st_insert);
		/**
		 * sqlite3_last_insert_rowid must after the statement finialize
		 */
		id = sqlite3_last_insert_rowid(db);
	}
	else
	{
		int type;
		type = sqlite3_column_type(st_select, 0);
		if (type == SQLITE_INTEGER)
			id = sqlite3_column_int(st_select, 0);
	}
	sqlite3_finalize(st_select);
	return id;
}

#include <jansson.h>

static int opus_parse_info(const char *info, char **ptitle, char **partist, char **palbum, char **pgenre)
{
	json_error_t error;
	json_t *jinfo = json_loads(info, 0, &error);
	json_unpack(jinfo, "{s:s,s:s,s:s,s:s}", str_key_title, ptitle, str_key_artist, partist, str_key_album, palbum, str_key_genre, pgenre);
	if (*ptitle)
		*ptitle = strdup(*ptitle);
	if (*partist)
		*partist = strdup(*partist);
	if (*palbum)
		*palbum = strdup(*palbum);
	if (*pgenre)
		*pgenre = strdup(*pgenre);
	json_decref(jinfo);
	return 0;
}

static json_t *opus_getjson(media_ctx_t *ctx, int opusid)
{
	json_t *json_info = json_object();

	sqlite3 *db = ctx->db;
	char *sql = "select titleid, artistid, albumid, genreid from opus where id=@ID";
	sqlite3_stmt *st_select;
	int ret;
	ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
	SQLITE3_CHECK(ret, NULL, sql);

	int index;

	index = sqlite3_bind_parameter_index(st_select, "@ID");
	ret = sqlite3_bind_int(st_select, index, opusid);
	SQLITE3_CHECK(ret, NULL, sql);

	ret = sqlite3_step(st_select);
	if (ret == SQLITE_ROW)
	{
		int type;
		int wordid = -1;
		type = sqlite3_column_type(st_select, 0);
		if (type == SQLITE_INTEGER)
		{
			wordid = sqlite3_column_int(st_select, 0);
			char *sql = "select name from word where id=@ID";
			sqlite3_stmt *st_select;
			ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
			SQLITE3_CHECK(ret, NULL, sql);

			int index;

			index = sqlite3_bind_parameter_index(st_select, "@ID");
			ret = sqlite3_bind_int(st_select, index, wordid);
			SQLITE3_CHECK(ret, NULL, sql);

			ret = sqlite3_step(st_select);
			if (ret == SQLITE_ROW)
			{
				int type;
				type = sqlite3_column_type(st_select, 0);
				if (type == SQLITE_TEXT)
				{
					const char *string = sqlite3_column_text(st_select, 0);
					json_t *jstring = json_string(string);
					json_object_set_new(json_info, str_key_title, jstring);
				}
			}
			sqlite3_finalize(st_select);
		}
		type = sqlite3_column_type(st_select, 1);
		if (type == SQLITE_INTEGER)
		{
			wordid = sqlite3_column_int(st_select, 1);
			char *sql = "select name from word inner join artist on word.id=artist.wordid where artist.id=@ID";
			sqlite3_stmt *st_select;
			ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
			SQLITE3_CHECK(ret, NULL, sql);

			int index;

			index = sqlite3_bind_parameter_index(st_select, "@ID");
			ret = sqlite3_bind_int(st_select, index, wordid);
			SQLITE3_CHECK(ret, NULL, sql);

			ret = sqlite3_step(st_select);
			if (ret == SQLITE_ROW)
			{
				int type;
				type = sqlite3_column_type(st_select, 0);
				if (type == SQLITE_TEXT)
				{
					const char *string = sqlite3_column_text(st_select, 0);
					json_t *jstring = json_string(string);
					json_object_set_new(json_info, str_key_artist, jstring);
				}
			}
			sqlite3_finalize(st_select);
		}
		type = sqlite3_column_type(st_select, 2);
		if (type == SQLITE_INTEGER)
		{
			wordid = sqlite3_column_int(st_select, 2);
			char *sql = "select name from word inner join album on word.id=album.wordid where album.id=@ID";
			sqlite3_stmt *st_select;
			ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
			SQLITE3_CHECK(ret, NULL, sql);

			int index;

			index = sqlite3_bind_parameter_index(st_select, "@ID");
			ret = sqlite3_bind_int(st_select, index, wordid);
			SQLITE3_CHECK(ret, NULL, sql);

			ret = sqlite3_step(st_select);
			if (ret == SQLITE_ROW)
			{
				int type;
				type = sqlite3_column_type(st_select, 0);
				if (type == SQLITE_TEXT)
				{
					const char *string = sqlite3_column_text(st_select, 0);
					json_t *jstring = json_string(string);
					json_object_set_new(json_info, str_key_album, jstring);
				}
			}
			sqlite3_finalize(st_select);
		}
		type = sqlite3_column_type(st_select, 3);
		if (type == SQLITE_INTEGER)
		{
			wordid = sqlite3_column_int(st_select, 3);
			char *sql = "select name from word inner join genre on word.id=genre.wordid where genre.id=@ID";
			sqlite3_stmt *st_select;
			ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
			SQLITE3_CHECK(ret, NULL, sql);

			int index;

			index = sqlite3_bind_parameter_index(st_select, "@ID");
			ret = sqlite3_bind_int(st_select, index, wordid);
			SQLITE3_CHECK(ret, NULL, sql);

			ret = sqlite3_step(st_select);
			if (ret == SQLITE_ROW)
			{
				int type;
				type = sqlite3_column_type(st_select, 0);
				if (type == SQLITE_TEXT)
				{
					const char *string = sqlite3_column_text(st_select, 0);
					json_t *jstring = json_string(string);
					json_object_set_new(json_info, str_key_genre, jstring);
				}
			}
			sqlite3_finalize(st_select);
		}
	}
	sqlite3_finalize(st_select);

	return json_info;
}


static char *opus_get(media_ctx_t *ctx, int opusid)
{
	char *info;
	json_t *jinfo = opus_getjson(ctx, opusid);
	info = json_dumps(jinfo, JSON_INDENT(2));
	return info;
}

static int opus_insert(media_ctx_t *ctx, const char *info, int mediaid)
{
	sqlite3 *db = ctx->db;
	char *title = NULL;
	int titleid = -1;
	char *artist = NULL;
	int artistid = -1;
	char *genre = NULL;
	int genreid = -1;
	char *album = NULL;
	int albumid = -1;
	int exist = 1;

	opus_parse_info(info, &title, &artist, &album, &genre);

	if (title != NULL)
	{
		titleid = opus_insert_word(ctx, title, &exist);
		free(title);
	}
	if (artist != NULL)
	{
		artistid = opus_insert_word(ctx, artist, &exist);
		if (artistid > -1)
			artistid = opus_insert_info(ctx, "artist", artistid);
		free(artist);
	}
	if (album != NULL)
	{
		albumid = opus_insert_word(ctx, album, &exist);
		if (albumid > -1)
			albumid = opus_insert_info(ctx, "album", albumid);
		free(album);
	}
	if (genre != NULL)
	{
		genreid = opus_insert_word(ctx, genre, NULL);
		if (genreid > -1)
			genreid = opus_insert_info(ctx, "genre", genreid);
		free(genre);
	}

	int opusid = -1;

	int ret;
	char *select = "select id from opus where titleid=@TITLEID and artistid=@ARTISTID and albumid=@ALBUMID";

	sqlite3_stmt *st_select;
	ret = sqlite3_prepare_v2(db, select, -1, &st_select, NULL);
	SQLITE3_CHECK(ret, -1, select);

	int index;

	index = sqlite3_bind_parameter_index(st_select, "@TITLEID");
	ret = sqlite3_bind_int(st_select, index, titleid);
	SQLITE3_CHECK(ret, -1, select);
	index = sqlite3_bind_parameter_index(st_select, "@ARTISTID");
	ret = sqlite3_bind_int(st_select, index, artistid);
	SQLITE3_CHECK(ret, -1, select);
	index = sqlite3_bind_parameter_index(st_select, "@ALBUMID");
	ret = sqlite3_bind_int(st_select, index, albumid);
	SQLITE3_CHECK(ret, -1, select);
	ret = sqlite3_step(st_select);
	if (ret != SQLITE_ROW)
	{
		char *insert = "insert into \"opus\" (\"titleid\",\"artistid\",\"albumid\",\"genreid\") values (@TITLEID,@ARTISTID,@ALBUMID,@GENREID)";
		sqlite3_stmt *st_insert;
		ret = sqlite3_prepare_v2(db, insert, -1, &st_insert, NULL);
		SQLITE3_CHECK(ret, -1, insert);

		int index;

		index = sqlite3_bind_parameter_index(st_insert, "@TITLEID");
		ret = sqlite3_bind_int(st_insert, index, titleid);
		SQLITE3_CHECK(ret, -1, select);
		index = sqlite3_bind_parameter_index(st_insert, "@ARTISTID");
		ret = sqlite3_bind_int(st_insert, index, artistid);
		SQLITE3_CHECK(ret, -1, select);
		index = sqlite3_bind_parameter_index(st_insert, "@ALBUMID");
		ret = sqlite3_bind_int(st_insert, index, albumid);
		SQLITE3_CHECK(ret, -1, select);
		index = sqlite3_bind_parameter_index(st_insert, "@GENREID");
		ret = sqlite3_bind_int(st_insert, index, genreid);
		SQLITE3_CHECK(ret, -1, select);
		ret = sqlite3_step(st_insert);
		if (ret != SQLITE_DONE)
		{
			err("media sqlite: error on insert %d", ret);
			opusid = -1;
		}
		else
		{
			opusid = sqlite3_last_insert_rowid(db);
		}
		sqlite3_finalize(st_insert);
	}
	else
	{
		int type;
		type = sqlite3_column_type(st_select, 0);
		if (type == SQLITE_INTEGER)
			opusid = sqlite3_column_int(st_select, 0);
	}
	sqlite3_finalize(st_select);
	return opusid;
}
#endif

static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	sqlite3 *db = ctx->db;

	if (path == NULL)
		return -1;

	int id = findmedia(db, path);
	if (id != -1)
		return 0;

#ifdef MEDIA_SQLITE_EXT
	int opusid = opus_insert(ctx, info, id);
#endif

	int ret = 0;
	sqlite3_stmt *statement;
#ifndef MEDIA_SQLITE_EXT
	char *sql = "insert into \"media\" (\"url\", \"mime\", \"info\") values(@PATH , @MIME , @INFO);";
#else
	char *sql = "insert into \"media\" (\"url\", \"mime\", \"opusid\") values(@PATH , @MIME, @OPUSID );";
#endif

	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, -1, sql);

	int index;
	char *tpath = NULL;
	if (strstr(path, "://"))
	{
		tpath = strdup(path);
	}
	else
	{
		int len = PROTOCOLNAME_LENGTH + strlen(path) + 1;
		tpath = malloc(len);
		snprintf(tpath, len, PROTOCOLNAME"%s", path);
	}
	index = sqlite3_bind_parameter_index(statement, "@PATH");
	ret = sqlite3_bind_text(statement, index, tpath, -1, SQLITE_STATIC);
	SQLITE3_CHECK(ret, -1, sql);

#ifndef MEDIA_SQLITE_EXT
	index = sqlite3_bind_parameter_index(statement, "@INFO");
	if (info != NULL)
		ret = sqlite3_bind_text(statement, index, info, -1, SQLITE_STATIC);
	else
		ret = sqlite3_bind_null(statement, index);
#else
	index = sqlite3_bind_parameter_index(statement, "@OPUSID");
	ret = sqlite3_bind_int(statement, index, opusid);
#endif
	SQLITE3_CHECK(ret, -1, sql);
	index = sqlite3_bind_parameter_index(statement, "@MIME");
	if (mime == NULL)
		mime = utils_getmime(path);
	if (mime)
		ret = sqlite3_bind_text(statement, index, mime, -1, SQLITE_STATIC);
	else
		ret = sqlite3_bind_null(statement, index);
	SQLITE3_CHECK(ret, -1, sql);

	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
	{
		err("media sqlite: error on insert %d", ret);
		ret = -1;
	}
	else
	{
		int id = sqlite3_last_insert_rowid(db);
		dbg("putv: new media[%d] %s", id, path);

		sqlite3_stmt *statement;
		char *sql = "insert into \"playlist\" (\"id\") values(@ID);";

		ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		index = sqlite3_bind_parameter_index(statement, "@ID");
#ifndef MEDIA_SQLITE_EXT
		ret = sqlite3_bind_int(statement, index, id);
#else
		ret = sqlite3_bind_int(statement, index, opusid);
#endif
		SQLITE3_CHECK(ret, -1, sql);
		ret = sqlite3_step(statement);
		if (ret != SQLITE_DONE)
		{
			err("media sqlite: error on insert %d", ret);
			ret = -1;
		}
	}
	free(tpath);
	sqlite3_finalize(statement);

	return ret;
}

static int _media_execute(media_ctx_t *ctx, sqlite3_stmt *statement, media_parse_t cb, void *data)
{
	int count = 0;
	int ret = sqlite3_step(statement);
	while (ret == SQLITE_ROW)
	{
		count ++;
		const char *url = NULL;
		const void *info = NULL;
		const char *mime = NULL;
		int id = -1;
		int index = 0;
		int type;

		index = 0;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_TEXT)
			url = sqlite3_column_text(statement, index);

		index = 1;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_TEXT)
			mime = sqlite3_column_text(statement, index);

#ifndef MEDIA_SQLITE_EXT
		index = 2;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_INTEGER)
			id = sqlite3_column_int(statement, index);

		index = 3;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_TEXT)
			info = sqlite3_column_blob(statement, index);
#else
		index = 2;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_INTEGER)
			id = sqlite3_column_int(statement, index);
		if (id != -1)
		{
			info = opus_get(ctx, id);
		}
#endif

		dbg("media: %d %s", id, url);
		if (cb != NULL)
		{
			int ret;
			ret = cb(data, id, url, (const char *)info, mime);
			if (ret != 0)
				break;
		}
#ifdef MEDIA_SQLITE_EXT
		if (id != -1)
		{
			free(info);
		}
#endif

		ret = sqlite3_step(statement);
	}
	return count;
}

static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *data)
{
	int count;
	int ret;
	sqlite3_stmt *statement;
#ifndef MEDIA_SQLITE_EXT
	char *sql = "select \"url\", \"mime\", \"id\", \"info\" from \"media\" where id = @ID";
#else
	char *sql = "select \"url\", \"mime\", \"opusid\" from \"media\" where opusid = @ID";
#endif
	ret = sqlite3_prepare_v2(ctx->db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, -1, sql);

	int index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(ret, -1, sql);

	count = _media_execute(ctx, statement, cb, data);
	sqlite3_finalize(statement);
	return count;
}

static int media_list(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	int ret = 0;
	sqlite3 *db = ctx->db;
	int count = 0;
	sqlite3_stmt *statement;
#ifndef MEDIA_SQLITE_EXT
	char *sql = "select \"url\", \"mime\", \"id\", \"info\" from \"media\" ";
#else
	char *sql = "select \"url\", \"mime\", \"opusid\" from \"media\"";
#endif
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, -1, sql);

	count = _media_execute(ctx, statement, cb, data);
	sqlite3_finalize(statement);

	return count;
}

static int media_play(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	media_find(ctx, ctx->mediaid, cb, data);
	return ctx->mediaid;
}

static int media_next(media_ctx_t *ctx)
{
	int ret;
	sqlite3_stmt *statement;
	char *sql[] = {
		"select \"id\" from \"playlist\" where id > @ID limit 1",
		"select \"id\" from \"playlist\" limit 1",
		"select \"id\" from \"playlist\" order by random() limit 1",
		};
	if (ctx->options & OPTION_RANDOM)
	{
		ret = sqlite3_prepare_v2(ctx->db, sql[2], -1, &statement, NULL);
		SQLITE3_CHECK(ret, -1, sql[2]);
	}
	else if (ctx->mediaid != 0)
	{
		ret = sqlite3_prepare_v2(ctx->db, sql[0], -1, &statement, NULL);
		SQLITE3_CHECK(ret, -1, sql[0]);

		int index = sqlite3_bind_parameter_index(statement, "@ID");
		ret = sqlite3_bind_int(statement, index, ctx->mediaid);
		SQLITE3_CHECK(ret, -1, sql[0]);
	}
	else
	{
		ret = sqlite3_prepare_v2(ctx->db, sql[1], -1, &statement, NULL);
		SQLITE3_CHECK(ret, -1, sql[1]);
	}

	ret = sqlite3_step(statement);
	if (ret == SQLITE_ROW)
	{
		ctx->mediaid = sqlite3_column_int(statement, 0);
	}
	else
		ctx->mediaid = -1;

	if ((ctx->options & OPTION_LOOP) && (ctx->mediaid == -1))
	{
		media_next(ctx);
	}
	sqlite3_finalize(statement);
	return ctx->mediaid;
}

static int media_end(media_ctx_t *ctx)
{
	ctx->mediaid = -1;
	return 0;
}

/**
 * If the current media is the last one,
 * the loop requires to restart the player.
 */
static void media_loop(media_ctx_t *ctx, int enable)
{
	if (enable)
		ctx->options |= OPTION_LOOP;
	else
		ctx->options &= ~OPTION_LOOP;
}

static void media_random(media_ctx_t *ctx, int enable)
{
	if (enable)
		ctx->options |= OPTION_RANDOM;
	else
		ctx->options &= ~OPTION_RANDOM;
}

static int media_options(media_ctx_t *ctx, media_options_t option, int enable)
{
	int ret = 0;
	if (option == MEDIA_LOOP)
	{
		media_loop(ctx, enable);
		ret = (ctx->options & OPTION_LOOP) == OPTION_LOOP;
	}
	else if (option == MEDIA_RANDOM)
	{
		media_random(ctx, enable);
		ret = (ctx->options & OPTION_RANDOM) == OPTION_RANDOM;
	}
	return ret;
}

static int _media_opendb(sqlite3 **db, const char *dbpath, const char *dbname)
{
	int ret;
	struct stat dbstat;

	ret = stat(dbpath, &dbstat);
	if ((ret == 0)  && S_ISREG(dbstat.st_mode))
	{
		ret = sqlite3_open_v2(dbpath, db, SQLITE_OPEN_READWRITE, NULL);
		if (ret == SQLITE_ERROR)
		{
			ret = sqlite3_open_v2(dbpath, db, SQLITE_OPEN_READONLY, NULL);
		}
	}
	else if ((ret == 0)  && S_ISDIR(dbstat.st_mode))
	{
		char *path = malloc(strlen(dbpath) + 1 + strlen(dbname) + 3 + 1);
		sprintf(path, "%s/%s.db", dbpath, dbname);

		ret = stat(path, &dbstat);
		if ((ret == 0)  && S_ISREG(dbstat.st_mode))
		{
			ret = sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE, NULL);
			if (ret == SQLITE_ERROR)
			{
				ret = sqlite3_open_v2(path, db, SQLITE_OPEN_READONLY, NULL);
			}
		}
		else
		{
			ret = sqlite3_open_v2(path, db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
			ret = SQLITE_CORRUPT;
		}
		free(path);
	}
	else
	{
		ret = sqlite3_open_v2(dbpath, db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
		ret = SQLITE_CORRUPT;
	}
	return ret;
}

static int _media_initdb(sqlite3 *db, const char *query[])
{
	char *error = NULL;
	int i = 0;
	int ret = SQLITE_OK;

	while (query[i] != NULL)
	{
		if (ret != SQLITE_OK)
		{
			err("media prepare error %d", ret);
			break;
		}
		ret = sqlite3_exec(db, query[i], NULL, NULL, &error);
		i++;
	}
	return ret;
}

static media_ctx_t *media_init(const char *url, ...)
{
	media_ctx_t *ctx = NULL;
	sqlite3 *db = NULL;
	int ret = SQLITE_ERROR;

	if (strstr(url, "db://") == NULL)
		return NULL;
	const char *dbpath = utils_getpath(url, "db://");
	if (dbpath)
	{
		ret = _media_opendb(&db, dbpath, "putv");
	}
	if (db)
	{
		if (ret == SQLITE_CORRUPT)
		{
#ifndef MEDIA_SQLITE_EXT
			const char *query[] = {
"create table media (\"id\" INTEGER PRIMARY KEY, \"url\" TEXT UNIQUE NOT NULL, \"mime\" TEXT, \"info\" BLOB, \"opusid\" INTEGER);",
"create table playlist (\"id\" INTEGER, FOREIGN KEY (id) REFERENCES media(id) ON UPDATE SET NULL);",
				NULL,
			};
			ret = _media_initdb(db, query);
#else
			const char *query[] = {
"create table media (id INTEGER PRIMARY KEY, url TEXT UNIQUE NOT NULL, mime TEXT, info BLOB, opusid INTEGER, comment BLOB, FOREIGN KEY (opusid) REFERENCES opus(id) ON UPDATE SET NULL);",
"create table opus (id INTEGER PRIMARY KEY,  titleid INTEGER UNIQUE NOT NULL, artistid INTEGER, otherid INTEGER, albumid INTEGER, genreid INTEGER, comment BLOB, FOREIGN KEY (titleid) REFERENCES word(id), FOREIGN KEY (artistid) REFERENCES artist(id) ON UPDATE SET NULL, FOREIGN KEY (albumid) REFERENCES album(id) ON UPDATE SET NULL, FOREIGN KEY (genreid) REFERENCES word(id) ON UPDATE SET NULL);",
"create table album (id INTEGER PRIMARY KEY, wordid INTEGER UNIQUE NOT NULL, artistid INTEGER, genreid INTEGER, comment BLOB, FOREIGN KEY (wordid) REFERENCES word(id), FOREIGN KEY (artistid) REFERENCES artist(id) ON UPDATE SET NULL, FOREIGN KEY (genreid) REFERENCES word(id) ON UPDATE SET NULL);",
"create table artist (id INTEGER PRIMARY KEY, wordid INTEGER UNIQUE NOT NULL, comment BLOB, FOREIGN KEY (wordid) REFERENCES word(id));",
"create table genre (id INTEGER PRIMARY KEY, wordid INTEGER, FOREIGN KEY (wordid) REFERENCES word(id));",
"create table playlist (id INTEGER, FOREIGN KEY (id) REFERENCES opus(id) ON UPDATE SET NULL);",
"create table word (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);",
				NULL,
			};
			ret = _media_initdb(db, query);
#endif
		}
		if (ret == SQLITE_OK)
		{
			dbg("open db %s", dbpath);
			ctx = calloc(1, sizeof(*ctx));
			ctx->db = db;
			ctx->mediaid = 0;
		}
		else
		{
			err("media db open error %d", ret);
			sqlite3_close_v2(db);
		}
	}
	return ctx;
}

static void media_destroy(media_ctx_t *ctx)
{
	if (ctx->db)
		sqlite3_close_v2(ctx->db);
	free(ctx);
}

const media_ops_t *media_sqlite = &(const media_ops_t)
{
	.init = media_init,
	.destroy = media_destroy,
	.next = media_next,
	.play = media_play,
	.list = media_list,
	.find = media_find,
	.insert = media_insert,
	.remove = media_remove,
	.count = media_count,
	.end = media_end,
	.options = media_options,
};

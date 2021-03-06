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

#include <pwd.h>

#include <sqlite3.h>

#include "player.h"
#include "media.h"

struct media_ctx_s
{
	sqlite3 *db;
	char *path;
	char *query;
	int mediaid;
	unsigned int options;
	int listid;
	int oldlistid;
	int fill;
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

#define media_dbg(...)

#ifdef DEBUG
#define SQLITE3_CHECK(ret, value, sql) \
		if (ret != SQLITE_OK) {\
			err("%s(%d) => %d %s", __FUNCTION__, __LINE__, ret, sql); \
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
static option_state_t media_loop(media_ctx_t *ctx, option_state_t enable);
static option_state_t media_random(media_ctx_t *ctx, option_state_t enable);
static void media_destroy(media_ctx_t *ctx);

static const char str_mediasqlite[] = "sqlite DB";

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
	char *sql = "select count(*) from \"playlist\" where listid=@LISTID";
	int ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, -1, sql);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	if (index > 0)
	{
		ret = sqlite3_bind_int(statement, index, ctx->listid);
		SQLITE3_CHECK(ret, -1, sql);
	}

	ret = sqlite3_step(statement);
	if (ret == SQLITE_ROW)
	{
		count = sqlite3_column_int(statement, 0);
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
	int force = 0;
	sqlite3 *db = ctx->db;
	if (path != NULL)
	{
		id = findmedia(db, path);
		force = 1;
	}

	if (id > 0)
	{
		sqlite3_stmt *statement;
		char *sql = NULL;
		if (force)
		{
#ifndef MEDIA_SQLITE_EXT
			sql = "delete from \"media\" where \"id\"=@ID";
#else
			sql = "delete from \"media\" where \"opusid\"=@ID";
#endif
		}
		else
		{
			sql = "delete from \"playlist\" where \"id\"=@ID and \"listid\"=@LISTID";
		}
		ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		int index = sqlite3_bind_parameter_index(statement, "@ID");
		ret = sqlite3_bind_int(statement, index, id);
		SQLITE3_CHECK(ret, -1, sql);

		index = sqlite3_bind_parameter_index(statement, "@LISTID");
		if (index > 0)
		{
			ret = sqlite3_bind_int(statement, index, ctx->listid);
			SQLITE3_CHECK(ret, -1, sql);
		}

		ret = sqlite3_step(statement);
		if (ret != SQLITE_DONE)
			ret = -1;
		else if (force)
			media_remove(ctx, id, NULL);
		else
		{
			ret = 0;
			media_dbg("putv: remove media %s", path);
		}
		sqlite3_finalize(statement);
	}
	return ret;
}

static int table_insert_word(media_ctx_t *ctx, const char *table, const char *word, int *exist)
{
	sqlite3 *db = ctx->db;
	int ret;
	char *wordselect = "select id from %s where \"name\" = @WORD";

	char sql[256];
	snprintf(sql, 256, wordselect, table);

	sqlite3_stmt *st_select;
	ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
	SQLITE3_CHECK(ret, -1, wordselect);

	int index;
	index = sqlite3_bind_parameter_index(st_select, "@WORD");

	int id = -1;
	ret = sqlite3_bind_text(st_select, index, word, -1, SQLITE_STATIC);
	SQLITE3_CHECK(ret, -1, wordselect);

	ret = sqlite3_step(st_select);
	if (ret != SQLITE_ROW)
	{
		char *wordinsert = "insert into %s (\"name\") values (@WORD)";

		char sql[256];
		snprintf(sql, 256, wordinsert, table);

		sqlite3_stmt *st_insert;
		ret = sqlite3_prepare_v2(db, sql, -1, &st_insert, NULL);
		SQLITE3_CHECK(ret, -1, wordinsert);

		index = sqlite3_bind_parameter_index(st_insert, "@WORD");
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

#ifdef MEDIA_SQLITE_EXT
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

static int opus_insertalbum(media_ctx_t *ctx, int albumid, int artistid, int coverid, int genreid)
{
	sqlite3 *db = ctx->db;
	albumid = opus_insert_info(ctx, "album", albumid);
	if (coverid != -1)
	{
		int ret;
		char *sql = "update \"album\" set \"coverid\"=@COVERID where id=@ALBUMID";
		sqlite3_stmt *st_update;
		ret = sqlite3_prepare_v2(db, sql, -1, &st_update, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		int index;
		index = sqlite3_bind_parameter_index(st_update, "@ALBUMID");
		ret = sqlite3_bind_int(st_update, index, albumid);
		SQLITE3_CHECK(ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_update, "@COVERID");
		ret = sqlite3_bind_int(st_update, index, coverid);
		SQLITE3_CHECK(ret, -1, sql);
		ret = sqlite3_step(st_update);
		sqlite3_finalize(st_update);
	}
	if (artistid != -1)
	{
		int ret;
		char *sql = "update \"album\" set \"artistid\"=@ARTISTID where id=@ALBUMID";
		sqlite3_stmt *st_update;
		ret = sqlite3_prepare_v2(db, sql, -1, &st_update, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		int index;
		index = sqlite3_bind_parameter_index(st_update, "@ALBUMID");
		ret = sqlite3_bind_int(st_update, index, albumid);
		SQLITE3_CHECK(ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_update, "@ARTISTID");
		ret = sqlite3_bind_int(st_update, index, coverid);
		SQLITE3_CHECK(ret, -1, sql);
		ret = sqlite3_step(st_update);
		sqlite3_finalize(st_update);
	}
	if (genreid != -1)
	{
		int ret;
		char *sql = "update \"album\" set \"genreid\"=@GENREID where id=@ALBUMID";
		sqlite3_stmt *st_update;
		ret = sqlite3_prepare_v2(db, sql, -1, &st_update, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		int index;
		index = sqlite3_bind_parameter_index(st_update, "@ALBUMID");
		ret = sqlite3_bind_int(st_update, index, albumid);
		SQLITE3_CHECK(ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_update, "@GENREID");
		ret = sqlite3_bind_int(st_update, index, coverid);
		SQLITE3_CHECK(ret, -1, sql);
		ret = sqlite3_step(st_update);
		sqlite3_finalize(st_update);
	}
	return albumid;
}

#include <jansson.h>

static int opus_parse_info(const char *info, char **ptitle, char **partist, char **palbum, char **pgenre, char **pcover)
{
	json_error_t error;
	json_t *jinfo = json_loads(info, 0, &error);
	if (json_is_object(jinfo))
	{
		json_t *value;
		value = json_object_get(jinfo, str_title);
		if (value != NULL && json_is_string(value))
			*ptitle = strdup(json_string_value(value));
		value = json_object_get(jinfo, str_artist);
		if (value != NULL && json_is_string(value))
			*partist = strdup(json_string_value(value));
		value = json_object_get(jinfo, str_album);
		if (value != NULL && json_is_string(value))
			*palbum = strdup(json_string_value(value));
		value = json_object_get(jinfo, str_genre);
		if (value != NULL && json_is_string(value))
			*pgenre = strdup(json_string_value(value));
		value = json_object_get(jinfo, str_cover);
		if (value != NULL && json_is_string(value))
			*pcover = strdup(json_string_value(value));
	}
	json_decref(jinfo);
	return 0;
}

char *opus_getcover(media_ctx_t *ctx, int coverid)
{
	sqlite3 *db = ctx->db;
	int ret;
	char *cover = NULL;

	char *sql = "select name from cover where id=@ID";
	sqlite3_stmt *st_select;
	ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
	SQLITE3_CHECK(ret, NULL, sql);

	int index;

	index = sqlite3_bind_parameter_index(st_select, "@ID");
	ret = sqlite3_bind_int(st_select, index, coverid);
	SQLITE3_CHECK(ret, NULL, sql);

	ret = sqlite3_step(st_select);
	if (ret == SQLITE_ROW)
	{
		int type;
		type = sqlite3_column_type(st_select, 0);
		if (type == SQLITE_TEXT)
		{
			const char *string = sqlite3_column_text(st_select, 0);
			if (string != NULL)
				cover = strdup(string);
		}
	}
	sqlite3_finalize(st_select);
	return cover;
}

static json_t *opus_getjson(media_ctx_t *ctx, int opusid, int coverid)
{
	json_t *json_info = json_object();

	sqlite3 *db = ctx->db;
	char *sql = "select titleid, artistid, albumid, genreid, coverid from opus where id=@ID";
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
					json_object_set_new(json_info, str_title, jstring);
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
					json_object_set_new(json_info, str_artist, jstring);
				}
			}
			sqlite3_finalize(st_select);
		}
		type = sqlite3_column_type(st_select, 2);
		if (type == SQLITE_INTEGER)
		{
			wordid = sqlite3_column_int(st_select, 2);
			//char *sql = "select name from word inner join album on word.id=album.wordid where album.id=@ID";
			char *sql = "select word.name, album.coverid from word inner join album on word.id=album.wordid where album.id=@ID";
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
					json_object_set_new(json_info, str_album, jstring);
				}
				type = sqlite3_column_type(st_select, 1);
				if (type == SQLITE_INTEGER)
				{
					coverid = sqlite3_column_int(st_select, 1);
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
					json_object_set_new(json_info, str_genre, jstring);
				}
			}
			sqlite3_finalize(st_select);
		}
		if (coverid == -1)
		{
			type = sqlite3_column_type(st_select, 4);
			if (type == SQLITE_INTEGER)
			{
				coverid = sqlite3_column_int(st_select, 4);
			}
		}
		if(coverid != -1)
		{
			char *cover = opus_getcover(ctx, coverid);
			if (cover != NULL)
			{
				json_t *jstring = json_string(cover);
				json_object_set_new(json_info, str_cover, jstring);
				free(cover);
			}
		}
	}
	sqlite3_finalize(st_select);

	return json_info;
}


static char *opus_get(media_ctx_t *ctx, int opusid, int coverid)
{
	char *info;
	json_t *jinfo = opus_getjson(ctx, opusid, coverid);
	info = json_dumps(jinfo, JSON_INDENT(2));
	return info;
}

static int opus_insert(media_ctx_t *ctx, const char *info, int *palbumid, const char *filename)
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
	char *cover = NULL;
	int coverid = -1;
	int exist = 1;

	opus_parse_info(info, &title, &artist, &album, &genre, &cover);

	warn("%s , %s , %s", title, album, artist);
	if (title != NULL)
	{
		titleid = table_insert_word(ctx, "word", title, &exist);
		free(title);
	}
	else
	{
		titleid = table_insert_word(ctx, "word", filename, &exist);
	}
	if (artist != NULL)
	{
		artistid = table_insert_word(ctx, "word", artist, &exist);
		if (artistid > -1)
			artistid = opus_insert_info(ctx, "artist", artistid);
		free(artist);
	}
	else
	{
		//default wordid is unknown
		artistid = 2;
	}
	if (cover != NULL)
	{
		coverid = table_insert_word(ctx, "cover", cover, &exist);
		free(cover);
	}
	if (album != NULL)
	{
		albumid = table_insert_word(ctx, "word", album, &exist);
		free(album);
	}
	else
	{
		albumid = 2;
	}
	*palbumid = opus_insertalbum(ctx, albumid, artistid, coverid, genreid);

	if (genre != NULL)
	{
		genreid = table_insert_word(ctx, "word", genre, NULL);
		if (genreid > -1)
			genreid = opus_insert_info(ctx, "genre", genreid);
		free(genre);
	}
	else
	{
		//default wordid is unknown
		genreid = 2;
	}

	int opusid = -1;

	int ret;
	char *select = "select id coverid from opus where titleid=@TITLEID and artistid=@ARTISTID";

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
	ret = sqlite3_step(st_select);
	if (ret != SQLITE_ROW)
	{
		char *sql = "insert into \"opus\" (\"titleid\",\"artistid\",\"albumid\",\"genreid\",\"coverid\",\"like\") values (@TITLEID,@ARTISTID,@ALBUMID,@GENREID,@COVERID, 5)";
		sqlite3_stmt *st_insert;
		ret = sqlite3_prepare_v2(db, sql, -1, &st_insert, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		int index;

		index = sqlite3_bind_parameter_index(st_insert, "@TITLEID");
		ret = sqlite3_bind_int(st_insert, index, titleid);
		SQLITE3_CHECK(ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_insert, "@ARTISTID");
		ret = sqlite3_bind_int(st_insert, index, artistid);
		SQLITE3_CHECK(ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_insert, "@ALBUMID");
		ret = sqlite3_bind_int(st_insert, index, *palbumid);
		SQLITE3_CHECK(ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_insert, "@GENREID");
		ret = sqlite3_bind_int(st_insert, index, genreid);
		SQLITE3_CHECK(ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_insert, "@COVERID");
		ret = sqlite3_bind_int(st_insert, index, coverid);
		SQLITE3_CHECK(ret, -1, sql);
		ret = sqlite3_step(st_insert);
		if (ret != SQLITE_DONE)
		{
			err("media sqlite: error on insert %d %s", ret, sqlite3_errmsg(db));
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
		if (genreid != -1)
		{
			int type;
			type = sqlite3_column_type(st_select, 0);
			if (type == SQLITE_INTEGER)
				opusid = sqlite3_column_int(st_select, 0);

			char *sql = "update \"opus\" set \"genreid\"=@GENREID where id=@OPUSID";
			sqlite3_stmt *st_update;
			ret = sqlite3_prepare_v2(db, sql, -1, &st_update, NULL);
			SQLITE3_CHECK(ret, -1, sql);

			int index;
			index = sqlite3_bind_parameter_index(st_update, "@OPUSID");
			ret = sqlite3_bind_int(st_update, index, opusid);
			SQLITE3_CHECK(ret, -1, sql);
			index = sqlite3_bind_parameter_index(st_update, "@GENREID");
			ret = sqlite3_bind_int(st_update, index, genreid);
			SQLITE3_CHECK(ret, -1, sql);
			ret = sqlite3_step(st_update);
			sqlite3_finalize(st_update);
		}

		if (coverid != -1)
		{
			int type;
			type = sqlite3_column_type(st_select, 0);
			if (type == SQLITE_INTEGER)
				opusid = sqlite3_column_int(st_select, 0);
			type = sqlite3_column_type(st_select, 1);
			if (type == SQLITE_INTEGER)
				coverid = sqlite3_column_int(st_select, 1);

			if (coverid != -1)
			{
				char *sql = "update \"opus\" set \"coverid\"=@COVERID where id=@OPUSID";
				sqlite3_stmt *st_update;
				ret = sqlite3_prepare_v2(db, sql, -1, &st_update, NULL);
				SQLITE3_CHECK(ret, -1, sql);

				int index;
				index = sqlite3_bind_parameter_index(st_update, "@OPUSID");
				ret = sqlite3_bind_int(st_update, index, opusid);
				SQLITE3_CHECK(ret, -1, sql);
				index = sqlite3_bind_parameter_index(st_update, "@COVERID");
				ret = sqlite3_bind_int(st_update, index, coverid);
				SQLITE3_CHECK(ret, -1, sql);
				ret = sqlite3_step(st_update);
				sqlite3_finalize(st_update);
			}
		}
	}
	sqlite3_finalize(st_select);
	return opusid;
}
#endif

static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	int id;
	int opusid = -1;
	int ret = 0;
	sqlite3 *db = ctx->db;

#ifdef MEDIA_SQLITE_EXT
	int albumid = -1;
	const char *filename = strrchr(path, '/');
	if (filename != NULL)
		filename += 1;
	else
		filename = path;
	if (info == NULL)
		info = 	media_fillinfo(path, mime);

	opusid = opus_insert(ctx, info, &albumid, filename);
	if (opusid == -1)
		err("opusid error");
	if (path == NULL)
		return opusid;
#else
	if (path == NULL)
		return -1;
#endif

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

	id = findmedia(db, tpath);
	if (id == -1)
	{
		sqlite3_stmt *statement;
#ifndef MEDIA_SQLITE_EXT
		char *sql = "insert into \"media\" (\"url\", \"mimeid\", \"info\") values(@PATH , @MIMEID , @INFO);";
#else
		char *sql = "insert into \"media\" (\"url\", \"mimeid\", \"opusid\", \"albumid\") values(@PATH , @MIMEID, @OPUSID, @ALBUMID );";
#endif

		ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		int index;
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
		if (albumid != -1)
		{
			index = sqlite3_bind_parameter_index(statement, "@ALBUMID");
			ret = sqlite3_bind_int(statement, index, albumid);
		}
		else
		{
			index = sqlite3_bind_parameter_index(statement, "@ALBUMID");
			ret = sqlite3_bind_null(statement, index);
		}
#endif
		SQLITE3_CHECK(ret, -1, sql);
		index = sqlite3_bind_parameter_index(statement, "@MIMEID");
		int mimeid = 0;
		int exist;
		if (mime == NULL)
			mime = utils_getmime(path);
		mimeid = table_insert_word(ctx, "mimes", mime, &exist);
		ret = sqlite3_bind_int(statement, index, mimeid);
		SQLITE3_CHECK(ret, -1, sql);

		ret = sqlite3_step(statement);
		if (ret != SQLITE_DONE)
		{
			err("media sqlite: error %d on insert of %s\n\t%s", ret, path, sqlite3_errmsg(db));
			ret = -1;
		}
		else
		{
			id = sqlite3_last_insert_rowid(db);
			media_dbg("putv: new media[%d] %s", id, path);
		}
		sqlite3_finalize(statement);
		opusid = id;
	}
#ifdef MEDIA_SQLITE_EXT
	else
	{
		int index;
		sqlite3_stmt *statement;
		char *sql = "update \"media\" set \"opusid\"=@OPUSID where id = @ID;";

		ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		index = sqlite3_bind_parameter_index(statement, "@OPUSID");
		ret = sqlite3_bind_int(statement, index, opusid);
		SQLITE3_CHECK(ret, -1, sql);

		index = sqlite3_bind_parameter_index(statement, "@ID");
		ret = sqlite3_bind_int(statement, index, id);
		SQLITE3_CHECK(ret, -1, sql);

		sqlite3_finalize(statement);
	}
#endif
	free(tpath);
	if (opusid != -1)
	{
		int index;
		sqlite3_stmt *statement;
		char *sql = "insert into \"playlist\" (\"id\", \"listid\") values(@ID,@LISTID);";

		ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		SQLITE3_CHECK(ret, -1, sql);

		index = sqlite3_bind_parameter_index(statement, "@ID");
		ret = sqlite3_bind_int(statement, index, opusid);
		index = sqlite3_bind_parameter_index(statement, "@LISTID");
		ret = sqlite3_bind_int(statement, index, ctx->listid);

		SQLITE3_CHECK(ret, -1, sql);
		ret = sqlite3_step(statement);
		if (ret != SQLITE_DONE)
		{
			err("media sqlite: error on insert %d", ret);
			ret = -1;
		}
		sqlite3_finalize(statement);
	}

	return opusid;
}

static const char *_media_getmime(media_ctx_t *ctx, int mimeid)
{
	switch(mimeid)
	{
	case 0:
		return mime_octetstream;
	case 1:
		return mime_audiomp3;
	case 2:
		return mime_audioflac;
	case 3:
		return mime_audioalac;
	case 4:
		return mime_audiopcm;
	default:
	    return "";
	}
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
		const char *coverurl = NULL;
		int id = -1;
		int index = 0;
		int type;

		/**
		 * retreive media url
		 */
		index = 0;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_TEXT)
			url = sqlite3_column_text(statement, index);

		/**
		 * retreive mime
		 */
		index++;
		type = sqlite3_column_type(statement, index);
		int mimeid = 0;
		if (type == SQLITE_INTEGER)
			mimeid = sqlite3_column_int(statement, index);
		mime = _media_getmime(ctx, mimeid);

#ifndef MEDIA_SQLITE_EXT
		/**
		 * retreive id
		 */
		index++;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_INTEGER)
			id = sqlite3_column_int(statement, index);

		/**
		 * retreive info
		 */
		index++;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_TEXT)
			info = sqlite3_column_blob(statement, index);
#else
		/**
		 * retreive opusid and info
		 */
		index++;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_INTEGER)
			id = sqlite3_column_int(statement, index);

		/**
		 * retreive cover if requested
		 */
		int coverid = -1;
		index++;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_INTEGER)
			coverid = sqlite3_column_int(statement, index);
		if (id != -1)
		{
			info = opus_get(ctx, id, coverid);
		}
#endif
		media_dbg("media: %d %s", id, url);
		if (cb != NULL && id > -1)
		{
			int ret;
			ret = cb(data, id, url, (const char *)info, mime);
			if (ret != 0)
				break;
		}
#ifdef MEDIA_SQLITE_EXT
		if (info != NULL)
		{
			free((char *)info);
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
	char *sql = "select \"url\", \"mimeid\", \"id\", \"info\" from \"media\"  where id = @ID";
#else
	char *sql = "select \"url\", \"mimeid\", \"opusid\", \"coverid\" from \"media\" inner join \"album\" on album.id=media.albumid where opusid = @ID";
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
	int index;

#ifndef MEDIA_SQLITE_EXT
	char *sql = "select \"url\", \"mimeid\", \"id\", \"info\" from \"media\" inner join playlist on media.id=playlist.id where playlist.listid=@LISTID;";
#else
	char *sql = "select \"url\", \"mimeid\", \"opusid\", \"coverid\" from \"media\" inner join playlist on media.id=playlist.id, \"album\" on album.id=media.albumid where playlist.listid=@LISTID;";
#endif
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, -1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, ctx->listid);
	SQLITE3_CHECK(ret, 1, sql);

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
		"select \"id\" from \"playlist\" where listid == @LISTID and id > @ID limit 1",
		"select \"id\" from \"playlist\" where listid == @LISTID limit 1",
		"select \"id\" from \"playlist\" where listid == @LISTID order by random() limit 1",
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
	int index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, ctx->listid);
	SQLITE3_CHECK(ret, -1, sql[0]);

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
static option_state_t media_loop(media_ctx_t *ctx, option_state_t enable)
{
	if (enable == OPTION_ENABLE)
		ctx->options |= OPTION_LOOP;
	else if (enable == OPTION_DISABLE)
		ctx->options &= ~OPTION_LOOP;
	return (ctx->options & OPTION_LOOP)? OPTION_ENABLE: OPTION_DISABLE;
}

static option_state_t media_random(media_ctx_t *ctx, option_state_t enable)
{
	if (enable == OPTION_ENABLE)
		ctx->options |= OPTION_RANDOM;
	else if (enable == OPTION_DISABLE)
		ctx->options &= ~OPTION_RANDOM;
	return (ctx->options & OPTION_RANDOM)? OPTION_ENABLE: OPTION_DISABLE;
}

static int _media_setlist(void *arg, int id, const char *url, const char *info, const char *mime)
{
	media_ctx_t *ctx = (media_ctx_t *)arg;
	int ret;
	sqlite3 *db = ctx->db;
	sqlite3_stmt *statement;
	int index;
	char *sql;

	/** insert the new entry into the playlist **/
	sql = "select id from \"playlist\" where id=@ID and listid=@LISTID;";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, 1, sql);

	index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(ret, 1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, ctx->listid);
	SQLITE3_CHECK(ret, 1, sql);

	ret = sqlite3_step(statement);

	sqlite3_finalize(statement);
	if (ret == SQLITE_ROW)
	{
		return 0;
	}

	/** insert the new entry into the playlist **/
	sql = "insert into playlist (\"id\", \"listid\") values (@ID, @LISTID);";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, 1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, ctx->listid);
	SQLITE3_CHECK(ret, 1, sql);

	index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(ret, 1, sql);

	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
	{
		err("media sqlite: error on insert %d", ret);
	}
	else
		ret = 0;
	sqlite3_finalize(statement);
	return ret;
}

static int _media_createlist(media_ctx_t *ctx, char *playlist, int fill)
{
	int ret;
	int listid = 1;
	sqlite3 *db = ctx->db;

#ifndef MEDIA_SQLITE_EXT
	char *sql = "select id from listname where name=@NAME";
#else
	char *sql = "select listname.id from listname inner join word on word.id=listname.wordid where word.name=@NAME";
#endif
	sqlite3_stmt *st_select;
	ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
	SQLITE3_CHECK(ret, 1, sql);

	int index;

	index = sqlite3_bind_parameter_index(st_select, "@NAME");
	ret = sqlite3_bind_text(st_select, index, playlist, -1 , SQLITE_STATIC);
	SQLITE3_CHECK(ret, 1, sql);

	ret = sqlite3_step(st_select);
	if (ret == SQLITE_ROW)
	{
		listid = sqlite3_column_int(st_select, 0);
	}
	else
	{
#ifndef MEDIA_SQLITE_EXT
		char *sql = "insert into listname (\"name\") values (@NAME);";
#else
		char *sql = "insert into word (\"name\") values (@NAME);";
#endif
		sqlite3_stmt *st_insert;
		ret = sqlite3_prepare_v2(db, sql, -1, &st_insert, NULL);
		SQLITE3_CHECK(ret, 1, sql);

		int index;

		index = sqlite3_bind_parameter_index(st_insert, "@NAME");
		ret = sqlite3_bind_text(st_insert, index, playlist, -1 , SQLITE_STATIC);
		SQLITE3_CHECK(ret, 1, sql);

		ret = sqlite3_step(st_insert);
		if (ret != SQLITE_DONE)
		{
			err("media sqlite: error on insert %d", ret);
			listid = 1;
		}
		else
		{
			listid = sqlite3_last_insert_rowid(db);
		}

		sqlite3_finalize(st_insert);
#ifndef MEDIA_SQLITE_EXT
#else
		sql = "insert into listname (\"wordid\") values (@ID);";
		ret = sqlite3_prepare_v2(db, sql, -1, &st_insert, NULL);
		SQLITE3_CHECK(ret, 1, sql);

		index = sqlite3_bind_parameter_index(st_insert, "@ID");
		ret = sqlite3_bind_int(st_insert, index, listid);
		SQLITE3_CHECK(ret, 1, sql);

		ret = sqlite3_step(st_insert);
		if (ret != SQLITE_DONE)
		{
			err("media sqlite: error on insert %d", ret);
		}
		else
		{
			listid = sqlite3_last_insert_rowid(db);
			dbg("new playlist %s %d", playlist, listid);
			int tempolist = ctx->listid;
			ctx->listid = listid;
			if (fill == 1)
				media_list(ctx, _media_setlist, ctx);
			ctx->listid = tempolist;
		}
		sqlite3_finalize(st_insert);
#endif
	}
	sqlite3_finalize(st_select);
	return listid;
}

#ifndef MEDIA_SQLITE_EXT
#define media_filter(...) NULL
#else
#define TABLE_ALBUM 0
#define TABLE_ARTIST 1
#define TABLE_SPEED 2
#define TABLE_TITLE 3
#define TABLE_GENRE 4
static int _media_filter(media_ctx_t *ctx, int table, const char *word)
{
	int ret = 0;
	sqlite3 *db = ctx->db;
	sqlite3_stmt *statement;
	int count = 0;
	int index;
	char *sql[] = {
		"select \"url\", \"mimeid\", \"opusid\", \"coverid\" from \"media\" "
		"inner join opus on opus.id = media.opusid "
		"where opus.albumid in ("
			"select album.id from album "
				"inner join "
					"word on word.id=album.wordid "
				"where LOWER(word.name) like LOWER(@NAME)"
			");",
		"select \"url\", \"mimeid\", \"opusid\", \"coverid\" from \"media\" "
		"inner join opus on opus.id = media.opusid "
		"where opus.artistid in ("
			"select artist.id from artist "
				"inner join "
					"word on word.id=artist.wordid "
				"where LOWER(word.name) like LOWER(@NAME)"
			");",
		"select \"url\", \"mimeid\", \"opusid\", \"coverid\" from \"media\" "
		"inner join opus on opus.id = media.opusid "
		"where opus.speedid in ("
			"select speed.id from speed "
				"inner join "
					"word on word.id=speed.wordid "
				"where LOWER(word.name) like LOWER(@NAME)"
			");",
		"select \"url\", \"mimeid\", \"opusid\", \"coverid\" from \"media\" "
		"inner join opus on opus.id = media.opusid "
		"where titleid in ("
			"select word.id from word "
				"where LOWER(word.name) like LOWER(@NAME)"
			");",
		"select \"url\", \"mimeid\", \"opusid\", \"coverid\" from \"media\" "
		"where genreid in ("
			"select word.id from word "
				"where LOWER(word.name) like LOWER(@NAME)"
			");",
	};
	ret = sqlite3_prepare_v2(db, sql[table], -1, &statement, NULL);
	SQLITE3_CHECK(ret, -1, sql[table]);

	index = sqlite3_bind_parameter_index(statement, "@NAME");
	ret = sqlite3_bind_text(statement, index, word, -1 , SQLITE_STATIC);
	SQLITE3_CHECK(ret, 1, sql[table]);

	count = _media_execute(ctx, statement, _media_setlist, ctx);
	sqlite3_finalize(statement);
	return count;
}

static int media_filter(media_ctx_t *ctx, media_filter_t *filter)
{
	int listid = _media_createlist(ctx, "filter", 0);
	if (ctx->listid != listid)
		ctx->oldlistid = ctx->listid;
	ctx->listid = listid;

	int ret = 0;
	sqlite3 *db = ctx->db;
	int count = 0;
	sqlite3_stmt *statement;
	char *sql;
	int index;

	/** free the current filter **/
	sql = "delete from playlist "
			"where listid=@LISTID;";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(ret, -1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, ctx->listid);
	SQLITE3_CHECK(ret, 1, sql);

	ret = sqlite3_step(statement);
	sqlite3_finalize(statement);
	if (ret != SQLITE_DONE)
	{
		err("media sqlite: error on delete %d", ret);
		return 0;
	}
	if (filter == NULL)
	{
		ctx->listid = ctx->oldlistid;
		return 0;
	}
	/** fill the new filter **/
	if (filter->album != NULL)
	{
		count += _media_filter(ctx, TABLE_ALBUM, filter->album);
	}
	if (filter->artist != NULL)
	{
		count += _media_filter(ctx, TABLE_ARTIST, filter->artist);
	}
	if (filter->title != NULL)
	{
		count += _media_filter(ctx, TABLE_TITLE, filter->title);
	}
	if (filter->speed != NULL)
	{
		count += _media_filter(ctx, TABLE_SPEED, filter->speed);
	}
	if (filter->genre != NULL)
	{
		count += _media_filter(ctx, TABLE_GENRE, filter->genre);
	}
	if (filter->keyword != NULL)
	{
		count += _media_filter(ctx, TABLE_ALBUM, filter->keyword);
		count += _media_filter(ctx, TABLE_ARTIST, filter->keyword);
		count += _media_filter(ctx, TABLE_TITLE, filter->keyword);
		count += _media_filter(ctx, TABLE_GENRE, filter->keyword);
	}
	return count;
}
#endif

static int _media_changelist(media_ctx_t *ctx, char *playlist)
{
	int listid = _media_createlist(ctx, playlist, ctx->fill);
	return listid;
}

static int _media_opendb(media_ctx_t *ctx, const char *url)
{
	int ret;
	struct stat dbstat;
	/**
	 * store path to keep query all the time
	 */
	ctx->path = utils_getpath(url, "db://", &ctx->query);

	if (ctx->path == NULL)
		return -1;
	ret = stat(ctx->path, &dbstat);
	if ((ret == 0) && S_ISREG(dbstat.st_mode))
	{
		ret = sqlite3_open_v2(ctx->path, &ctx->db, SQLITE_OPEN_READWRITE, NULL);
		if (ret == SQLITE_ERROR)
		{
			ret = sqlite3_open_v2(ctx->path, &ctx->db, SQLITE_OPEN_READONLY, NULL);
		}
	}
	else
	{
		ret = sqlite3_open_v2(ctx->path, &ctx->db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
		ret = SQLITE_CORRUPT;
	}
	ctx->listid = 1;
	if (ctx->query != NULL)
	{
		char *fill = strstr(ctx->query, "fill=true");
		if (fill != NULL)
		{
			ctx->fill = 1;
		}
		char *playlist = strstr(ctx->query, "playlist=");
		if (playlist != NULL)
		{
			playlist += 9;
			char tempo[64];
			strncpy(tempo, playlist, sizeof(tempo));
			char *end = strchr(tempo, ',');
			if (end != NULL)
				*end = '\0';
			ctx->listid = _media_changelist(ctx, tempo);
		}
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
			err("media prepare error %d query[%d]", ret, i);
			break;
		}
		media_dbg("query %d",i);
		media_dbg("query %s", query[i]);
		ret = sqlite3_exec(db, query[i], NULL, NULL, &error);
		i++;
	}
	return ret;
}

static media_ctx_t *media_init(player_ctx_t *player, const char *url, ...)
{
	media_ctx_t *ctx = NULL;
	int ret = SQLITE_ERROR;

	ctx = calloc(1, sizeof(*ctx));
	ret = _media_opendb(ctx, url);
	if (ret != -1 && ctx->db)
	{
		char *error = NULL;
		if (sqlite3_exec(ctx->db, "PRAGMA encoding=\"UTF-8\";", NULL, NULL, &error))
			warn("sqlite pragma error: %s", error);
		if (ret == SQLITE_CORRUPT)
		{
#ifndef MEDIA_SQLITE_EXT
			const char *query[] = {
"create table mimes (\"id\" INTEGER PRIMARY KEY, \"name\" TEXT UNIQUE NOT NULL);",
"insert into mimes (id, name) values (1, \"audio/mp3\");",
"insert into mimes (id, name) values (2, \"audio/flac\");",
"insert into mimes (id, name) values (3, \"audio/alac\");",
"create table media (\"id\" INTEGER PRIMARY KEY, \"url\" TEXT UNIQUE NOT NULL, \"mimeid\" INTEGER, \"info\" BLOB, \"opusid\" INTEGER," \
	"FOREIGN KEY (mimeid) REFERENCES mimes(id) ON UPDATE SET NULL);",
"create table listname (\"id\" INTEGER PRIMARY KEY, \"name\" TEXT UNIQUE NOT NULL);",
"create table playlist (\"id\" INTEGER, \"listid\" INTEGER, " \
	"FOREIGN KEY (id) REFERENCES media(id) ON UPDATE SET NULL, " \
	"FOREIGN KEY (listid) REFERENCES listname(id) ON UPDATE SET NULL);",
"insert into listname (id, name) values (1, \"default\");",
				NULL,
			};
#else
/**
 * Database format:
 * |   Table   |   Field   |  Comments               |
 * ---------------------------------------------------
 * |  media    |    url    | URL or file path to the |
 * |           |           | data                    |
 * |           |   mime    | mime string about the   |
 * |           |           | type of data (ref mime) |
 * |           |  opusid   | (ref opus)              |
 * |           |  albumid  | opus may be on several  |
 * |           |           | albums, media comes from|
 * |           |           | one and only one album  |
 * |           |  comment  | specific test about the |
 * |           |           | data (radio, story...)  |
 * ---------------------------------------------------
 * |   opus    | titleid   | title of the opus       |
 * |           |           | (ref word)              |
 * |           |  albumid  | opus is first available |
 * |           |           | from one album          |
 * |           | artistid  | name of the artist      |
 * |           |           | (ref artist => word)    |
 * |           | genreid   | genre of the opus       |
 * |           |           | (ref word)              |
 * |           | coverid   | url to the cover image  |
 * |           |           | for the opus (ref cover)|
 * |           |   like    | note to the opus        |
 * |           |   speed   | speed of the music      |
 * |           |           | (ref speed)             |
 * |           |  introid  | link the opeing opus of |
 * |           |           | this one (ref opus)     |
 * |           |  comment  | text about the opus     |
 * ---------------------------------------------------
 * |   album   |  wordid   | name of the album       |
 * |           |           | (ref word)              |
 * |           | artistid  | artist of the album may |
 * |           |           | be different of the opus|
 * |           |           | (ref artist)            |
 * |           |  genreid  | genre of the opus       |
 * |           |           | (ref word)              |
 * |           |  coverid  | url to the cover image  |
 * |           |           | for the album(ref cover)|
 * |           |  comment  | text about the album    |
 * ---------------------------------------------------
 */
			const char *query[] = {
"create table mimes (\"id\" INTEGER PRIMARY KEY, \"name\" TEXT UNIQUE NOT NULL);",
"insert into mimes (id, name) values (0, \"octet/stream\");",
"insert into mimes (id, name) values (1, \"audio/mp3\");",
"insert into mimes (id, name) values (2, \"audio/flac\");",
"insert into mimes (id, name) values (3, \"audio/alac\");",
"insert into mimes (id, name) values (4, \"audio/pcm\");",
"create table media (id INTEGER PRIMARY KEY, url TEXT UNIQUE NOT NULL, mimeid INTEGER, info BLOB, " \
	"opusid INTEGER, albumid INTEGER, comment BLOB, " \
	"FOREIGN KEY (mimeid) REFERENCES mimes(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (opusid) REFERENCES opus(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (albumid) REFERENCES album(id) ON UPDATE SET NULL);",
"create table opus (id INTEGER PRIMARY KEY,  titleid INTEGER UNIQUE NOT NULL, " \
	"artistid INTEGER, otherid INTEGER, albumid INTEGER, " \
	"genreid INTEGER, coverid INTEGER, like INTEGER, speedid INTEGER DEFAULT(2), introid INTEGER, comment BLOB, " \
	"FOREIGN KEY (titleid) REFERENCES word(id), " \
	"FOREIGN KEY (introid) REFERENCES opus(id), " \
	"FOREIGN KEY (artistid) REFERENCES artist(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (albumid) REFERENCES album(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (genreid) REFERENCES word(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (speedid) REFERENCES speed(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (coverid) REFERENCES cover(id) ON UPDATE SET NULL);",
"create table album (id INTEGER PRIMARY KEY, wordid INTEGER UNIQUE NOT NULL, artistid INTEGER, " \
	"genreid INTEGER, coverid INTEGER, comment BLOB, " \
	"FOREIGN KEY (wordid) REFERENCES word(id), " \
	"FOREIGN KEY (artistid) REFERENCES artist(id) ON UPDATE SET NULL, " \
	"FOREIGN KEY (genreid) REFERENCES word(id) ON UPDATE SET NULL, " \
	"FOREIGN KEY (coverid) REFERENCES cover(id) ON UPDATE SET NULL);",
"create table artist (id INTEGER PRIMARY KEY, wordid INTEGER UNIQUE NOT NULL, comment BLOB, " \
	"FOREIGN KEY (wordid) REFERENCES word(id));",
"create table genre (id INTEGER PRIMARY KEY, wordid INTEGER, " \
	"FOREIGN KEY (wordid) REFERENCES word(id));",
"create table speed (id INTEGER PRIMARY KEY, wordid INTEGER, " \
	"FOREIGN KEY (wordid) REFERENCES word(id));",
"insert into speed (id, wordid) values (1, 3);",
"insert into speed (id, wordid) values (2, 4);",
"insert into speed (id, wordid) values (3, 5);",
"create table cover (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);",
"create table playlist (id INTEGER, listid INTEGER, " \
	"FOREIGN KEY (id) REFERENCES media(id) ON UPDATE SET NULL, " \
	"FOREIGN KEY (listid) REFERENCES listname(id) ON UPDATE SET NULL);",
"create table word (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);",
"insert into word (id, name) values (1, \"default\");",
"insert into word (id, name) values (2, \"unknown\");",
"insert into word (id, name) values (3, \"cool\");",
"insert into word (id, name) values (4, \"ambiant\");",
"insert into word (id, name) values (5, \"dance\");",
"create table listname (id INTEGER PRIMARY KEY, wordid INTEGER, " \
	"FOREIGN KEY (wordid) REFERENCES word(id));",
"insert into listname (id, wordid) values (1, 1);",
				NULL,
			};
#endif
			ret = _media_initdb(ctx->db, query);
		}
		if (ret == SQLITE_OK)
		{
			dbg("open db %s", url);
		}
		else
		{
			err("media db open error %d", ret);
			media_destroy(ctx);
			ctx = NULL;
		}
	}
	else
	{
		free(ctx);
		ctx = NULL;
	}
	return ctx;
}

static void media_destroy(media_ctx_t *ctx)
{
	if (ctx->db)
		sqlite3_close_v2(ctx->db);
	free(ctx->path);
	free(ctx);
}

const media_ops_t *media_sqlite = &(const media_ops_t)
{
	.name = str_mediasqlite,
	.init = media_init,
	.destroy = media_destroy,
	.next = media_next,
	.play = media_play,
	.list = media_list,
	.find = media_find,
	.filter = media_filter,
	.insert = media_insert,
	.append = media_insert,
	.remove = media_remove,
	.count = media_count,
	.end = media_end,
	.random = media_random,
	.loop = media_loop,
};

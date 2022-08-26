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
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <pwd.h>

#include <sqlite3.h>
#include <jansson.h>

#include "player.h"
#include "media.h"

struct media_ctx_s
{
	sqlite3 *db;
	sqlite3 *dbplaylist;
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
#define media_dbgsql(statement, line) do { \
	char *string = sqlite3_expanded_sql(statement); \
	media_dbg("media db: sql %s (%d)", string, line); \
	sqlite3_free(string); \
} while(0)
#else
#define media_dbgsql(...)
#endif

#ifdef DEBUG
#define SQLITE3_CHECK(db, ret, value, sql) \
		if (ret != SQLITE_OK) {\
			err("%s(%d) => %d %s", __FUNCTION__, __LINE__, ret, sql); \
			err("\t%s", sqlite3_errmsg(db)); \
			return value; \
		}
#else
#define SQLITE3_CHECK(db, ...)
#endif

static int media_count(media_ctx_t *ctx);
static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
static int media_find(media_ctx_t *ctx, int id,  media_parse_t cb, void *data);
static int media_list(media_ctx_t *ctx, media_parse_t cb, void *data);
static int media_play(media_ctx_t *ctx, int id, media_parse_t cb, void *data);
static int media_next(media_ctx_t *ctx);
static int media_end(media_ctx_t *ctx);
static option_state_t media_loop(media_ctx_t *ctx, option_state_t enable);
static option_state_t media_random(media_ctx_t *ctx, option_state_t enable);
static void media_destroy(media_ctx_t *ctx);

#define TABLE_NONE 0
#define TABLE_ALBUM 1
#define TABLE_ARTIST 2
#define TABLE_SPEED 3
#define TABLE_TITLE 4
#define TABLE_GENRE 5
static int _media_filter(media_ctx_t *ctx, int table, const char *word);

static int playlist_create(media_ctx_t *ctx, char *playlist, int fill);
static int playlist_destroy(media_ctx_t *ctx, int listid);
static int playlist_find(media_ctx_t *ctx, char *playlist);
static int playlist_count(media_ctx_t *ctx, int listid);
static int playlist_has(media_ctx_t *ctx, int listid, int mediaid);
static int playlist_append(media_ctx_t *ctx, int listid, int mediaid, int likes);
static int playlist_likes(media_ctx_t *ctx, int listid, int mediaid, int likes);
static int playlist_remove(media_ctx_t *ctx, int listid, int id);

static const char str_mediasqlite[] = "sqlite DB";

static int media_count(media_ctx_t *ctx)
{
	return playlist_count(ctx, ctx->listid);
}

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

/**
 * returns media id from a path or a title or artist or album
 */
static int _media_find(media_ctx_t *ctx, const char *path)
{
	sqlite3 *db = ctx->db;
	sqlite3_stmt *statement;
	const char sql[] = "SELECT id FROM media WHERE url=@PATH";
	int ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	ret = sqlite3_bind_text(statement, sqlite3_bind_parameter_index(statement, "@PATH"), path, -1, SQLITE_STATIC);
	SQLITE3_CHECK(db, ret, -1, sql);

	int id = _execute(statement);
	sqlite3_finalize(statement);

	if (id == -1)
	{
		const char sql[] = "SELECT id FROM word WHERE \"name\"=@NAME";
		sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		/** set the default value of @FIELDS **/
		sqlite3_bind_text(statement, sqlite3_bind_parameter_index(statement, "@NAME"), path, -1, SQLITE_STATIC);

		media_dbgsql(statement, __LINE__);
		int wordid = _execute(statement);
		sqlite3_finalize(statement);
		if (wordid != -1)
		{
			const char *queries[] = {
				"SELECT id FROM opus WHERE titleid=@ID",
				"SELECT id FROM opus INNER JOIN artist ON artist.id=opus.artistid  WHERE artist.wordid=@ID",
				"SELECT id FROM opus INNER JOIN album ON album.id=opus.albumid  WHERE album.wordid=@ID",
				NULL
			};
			int i = 0;
			while (id == -1 && queries[i] != NULL)
			{
				sqlite3_prepare_v2(db, queries[i], -1, &statement, NULL);
				/** set the default value of @FIELDS **/
				sqlite3_bind_int(statement, sqlite3_bind_parameter_index(statement, "@ID"), wordid);
				media_dbgsql(statement, __LINE__);
				id = _execute(statement);
				sqlite3_finalize(statement);
				i++;
			}
			if (id != -1)
			{
				const char sql[] = "SELECT id FROM media WHERE opusid=@ID";
				sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
				/** set the default value of @FIELDS **/
				sqlite3_bind_int(statement, sqlite3_bind_parameter_index(statement, "@ID"), id);
				media_dbgsql(statement, __LINE__);
				id = _execute(statement);
				sqlite3_finalize(statement);
			}
		}
	}
	return id;
}

static int wordtable_insert(media_ctx_t *ctx, const char *table, const char *word)
{
	sqlite3 *db = ctx->db;
	int ret;
	const char query[] = "INSERT INTO %s (\"name\") VALUES (@WORD)";

	char sql[sizeof(query) + 20];
	snprintf(sql, sizeof(query) + 20, query, table);

	sqlite3_stmt *statement;
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, query);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@WORD");
	ret = sqlite3_bind_text(statement, index, word, -1, SQLITE_STATIC);
	SQLITE3_CHECK(db, ret, -1, query);

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	int id = -1;
	if (ret == SQLITE_DONE)
		id = sqlite3_last_insert_rowid(db);
	else
	{
		err("media:insert error %s", sqlite3_errmsg(db));
	}
	sqlite3_finalize(statement);
	return id;
}

static int wordtable_find(media_ctx_t *ctx, const char *table, const char *word)
{
	sqlite3 *db = ctx->db;
	int ret;
	const char query[] = "SELECT id FROM %s WHERE name=@WORD COLLATE NOCASE";

	char sql[sizeof(query) + 20];
	snprintf(sql, sizeof(query) + 20, query, table);

	sqlite3_stmt *statement;
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, query);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@WORD");
	ret = sqlite3_bind_text(statement, index, word, -1, SQLITE_STATIC);
	SQLITE3_CHECK(db, ret, -1, query);

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	int id = -1;
	if (ret == SQLITE_ROW)
	{
		int type;
		type = sqlite3_column_type(statement, 0);
		if (type == SQLITE_INTEGER)
			id = sqlite3_column_int(statement, 0);
	}
	sqlite3_finalize(statement);
	return id;
}

static int opus_insert_info(media_ctx_t *ctx, const char *table, int wordid)
{
	sqlite3 *db = ctx->db;
	int ret;
	const char wordselect[] = "SELECT id FROM \"%s\" WHERE \"wordid\" = @WORDID";
	const char wordinsert[] = "INSERT INTO \"%s\" (\"wordid\") VALUES (@WORDID)";

	char sql[48 + 20 + 1];
	snprintf(sql, 69, wordselect, table);

	sqlite3_stmt *st_select;
	ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	int index;
	index = sqlite3_bind_parameter_index(st_select, "@WORDID");

	int id = -1;
	ret = sqlite3_bind_int(st_select, index, wordid);
	SQLITE3_CHECK(db, ret, -1, sql);

	media_dbgsql(st_select, __LINE__);
	ret = sqlite3_step(st_select);
	if (ret != SQLITE_ROW)
	{
		snprintf(sql, 69, wordinsert, table);

		sqlite3_stmt *st_insert;
		ret = sqlite3_prepare_v2(db, sql, -1, &st_insert, NULL);
		SQLITE3_CHECK(db, ret, -1, sql);

		ret = sqlite3_bind_int(st_insert, index, wordid);
		SQLITE3_CHECK(db, ret, -1, sql);

		media_dbgsql(st_insert, __LINE__);
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

static int album_insertfield(media_ctx_t *ctx, int albumid, char *field, int fieldid)
{
	sqlite3 *db = ctx->db;
	int ret;
	const char query[] = "UPDATE album SET %s=@ID WHERE id=@ALBUMID";
	char *sql = malloc(strlen(query) + strlen(field) + 1);
	sprintf(sql, query, field);

	sqlite3_stmt *statement;
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@ALBUMID");
	ret = sqlite3_bind_int(statement, index, albumid);
	SQLITE3_CHECK(db, ret, -1, sql);
	index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, fieldid);
	SQLITE3_CHECK(db, ret, -1, sql);

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	sqlite3_finalize(statement);
	free(sql);
	if (ret != SQLITE_DONE)
		return -1;
	return 0;
}

static int album_insert(media_ctx_t *ctx, char *album, int artistid, int coverid, int genreid)
{
	sqlite3 *db = ctx->db;
	int albumid;
	if (album != NULL)
	{
		int exist;
		albumid = wordtable_find(ctx, "word", album);
		if (albumid == -1)
			albumid = wordtable_insert(ctx, "word", album);
		free(album);
	}

	albumid = opus_insert_info(ctx, "album", albumid);
	if (coverid != -1)
	{
		album_insertfield(ctx, albumid, "coverid", coverid);
	}
	if (artistid != -1)
	{
		album_insertfield(ctx, albumid, "artistid", artistid);
	}
	if (genreid != -1)
	{
		album_insertfield(ctx, albumid, "genreid", genreid);
	}
	return albumid;
}

static int opus_populateinfo(media_ctx_t *ctx, json_t *jinfo, int *ptitleid, int *partistid,
		char **album, int *pgenreid, int *pcoverid, char **pcomment, int *plikes)
{
	char *title = NULL;
	char *artist = NULL;
	char *genre = NULL;
	char *cover = NULL;
	int exist = 1;

	media_parse_info(jinfo, &title, &artist, album, &genre, &cover, pcomment, plikes);

	media_dbg("%s , %s , %s", title?title:"", *album?*album:"", artist?artist:"");
	dbg("%s , %s , %s", title?title:"", *album?*album:"", artist?artist:"");
	if (title != NULL)
	{
		*ptitleid = wordtable_find(ctx, "word", title);
		if (*ptitleid == -1)
			*ptitleid = wordtable_insert(ctx, "word", title);
		free(title);
	}
	else
	{
		*ptitleid = -1;
	}
	if (artist != NULL)
	{
		*partistid = wordtable_find(ctx, "word", artist);
		if (*partistid == -1)
			*partistid = wordtable_insert(ctx, "word", artist);
		free(artist);
	}
	if (cover != NULL)
	{
		*pcoverid = wordtable_find(ctx, "cover", cover);
		if (*pcoverid == -1)
			*pcoverid = wordtable_find(ctx, "cover", cover);
		free(cover);
	}

	if (genre != NULL)
	{
		*pgenreid = wordtable_find(ctx, "word", genre);
		if (*pgenreid == -1)
			*pgenreid = wordtable_insert(ctx, "word", genre);
		free(genre);
	}

	if (*partistid > -1)
		*partistid = opus_insert_info(ctx, "artist", *partistid);
	if (*pgenreid > -1)
		*pgenreid = opus_insert_info(ctx, "genre", *pgenreid);

	return 0;
}

char *opus_getcover(media_ctx_t *ctx, int coverid)
{
	sqlite3 *db = ctx->db;
	int ret;
	char *cover = NULL;

	const char *sql = "select name from cover where id=@ID";
	sqlite3_stmt *st_select;
	ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
	SQLITE3_CHECK(db, ret, NULL, sql);

	int index;

	index = sqlite3_bind_parameter_index(st_select, "@ID");
	ret = sqlite3_bind_int(st_select, index, coverid);
	SQLITE3_CHECK(db, ret, NULL, sql);

	media_dbgsql(st_select, __LINE__);
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

	sqlite3 *db = ctx->db;
	const char sql[] = "SELECT titleid, artistid, albumid, genreid, coverid FROM opus WHERE id=@ID";
	sqlite3_stmt *st_select;
	int ret;
	ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
	SQLITE3_CHECK(db, ret, NULL, sql);

	int index;
	json_t *json_info = json_object();

	index = sqlite3_bind_parameter_index(st_select, "@ID");
	ret = sqlite3_bind_int(st_select, index, opusid);
	SQLITE3_CHECK(db, ret, NULL, sql);

	media_dbgsql(st_select, __LINE__);
	ret = sqlite3_step(st_select);
	if (ret == SQLITE_ROW)
	{
		int type;
		int wordid = -1;
		type = sqlite3_column_type(st_select, 0);
		if (type == SQLITE_INTEGER)
		{
			wordid = sqlite3_column_int(st_select, 0);
			const char sql[] = "SELECT name FROM word WHERE id=@ID";
			sqlite3_stmt *st_select;
			ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
			SQLITE3_CHECK(db, ret, NULL, sql);

			int index;

			index = sqlite3_bind_parameter_index(st_select, "@ID");
			ret = sqlite3_bind_int(st_select, index, wordid);
			SQLITE3_CHECK(db, ret, NULL, sql);

			media_dbgsql(st_select, __LINE__);
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
			const char sql[] = "SELECT name FROM word INNER JOIN artist ON word.id=artist.wordid WHERE artist.id=@ID";
			sqlite3_stmt *st_select;
			ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
			SQLITE3_CHECK(db, ret, NULL, sql);

			int index;

			index = sqlite3_bind_parameter_index(st_select, "@ID");
			ret = sqlite3_bind_int(st_select, index, wordid);
			SQLITE3_CHECK(db, ret, NULL, sql);

			media_dbgsql(st_select, __LINE__);
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
			const char sql[] = "SELECT word.name, album.coverid FROM word INNER JOIN album ON word.id=album.wordid WHERE album.id=@ID";
			sqlite3_stmt *st_select;
			ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
			SQLITE3_CHECK(db, ret, NULL, sql);

			int index;

			index = sqlite3_bind_parameter_index(st_select, "@ID");
			ret = sqlite3_bind_int(st_select, index, wordid);
			SQLITE3_CHECK(db, ret, NULL, sql);

			media_dbgsql(st_select, __LINE__);
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
			const char sql[] = "SELECT name FROM word INNER JOIN genre ON word.id=genre.wordid WHERE genre.id=@ID";
			sqlite3_stmt *st_select;
			ret = sqlite3_prepare_v2(db, sql, -1, &st_select, NULL);
			SQLITE3_CHECK(db, ret, NULL, sql);

			int index;

			index = sqlite3_bind_parameter_index(st_select, "@ID");
			ret = sqlite3_bind_int(st_select, index, wordid);
			SQLITE3_CHECK(db, ret, NULL, sql);

			media_dbgsql(st_select, __LINE__);
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


static char *opus_get(media_ctx_t *ctx, int opusid, int coverid, const char *info)
{
	char *newinfo = NULL;
	json_t *jnewinfo = opus_getjson(ctx, opusid, coverid);
	json_error_t error;
	json_t *jinfo = json_loads(info, 0, &error);
	json_object_update_missing(jnewinfo, jinfo);
	newinfo = json_dumps(jnewinfo, JSON_INDENT(2));
	json_decref(jinfo);
	json_decref(jnewinfo);
	return newinfo;
}

static int opus_updatefield(media_ctx_t *ctx, int opusid, const char *field, int fieldid)
{
	const char query[] = "UPDATE opus SET \"%s\"=@FIELDID WHERE id=@OPUSID";
	char sql[sizeof(query) + 20];
	snprintf(sql, sizeof(query) + 20, query, field);

	int ret;
	sqlite3 *db = ctx->db;
	sqlite3_stmt *statement;
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@OPUSID");
	ret = sqlite3_bind_int(statement, index, opusid);
	SQLITE3_CHECK(db, ret, -1, sql);
	index = sqlite3_bind_parameter_index(statement, "@FIELDID");
	ret = sqlite3_bind_int(statement, index, fieldid);
	SQLITE3_CHECK(db, ret, -1, sql);
	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	sqlite3_finalize(statement);

	return ret;
}

static int opus_insert(media_ctx_t *ctx, json_t *jinfo, int *palbumid,
		const char *filename, int *plikes)
{
	int titleid = -1;
	int artistid = -1;
	int genreid = -1;
	int coverid = -1;
	char *comment = NULL;
	char *album = NULL;

	opus_populateinfo(ctx, jinfo, &titleid, &artistid, &album, &genreid, &coverid, &comment, plikes);

	if (titleid == -1)
	{
		err("media: title must be present");
		return -1;
	}
	if (album != NULL)
		*palbumid = album_insert(ctx, album, artistid, coverid, genreid);

	int opusid = -1;

	int ret;
	sqlite3 *db = ctx->db;
	const char select[] = "SELECT id FROM opus WHERE titleid=@TITLEID AND artistid=@ARTISTID";
	sqlite3_stmt *st_select;
	ret = sqlite3_prepare_v2(db, select, -1, &st_select, NULL);
	SQLITE3_CHECK(db, ret, -1, select);

	int index;

	index = sqlite3_bind_parameter_index(st_select, "@TITLEID");
	ret = sqlite3_bind_int(st_select, index, titleid);
	SQLITE3_CHECK(db, ret, -1, select);
	index = sqlite3_bind_parameter_index(st_select, "@ARTISTID");
	ret = sqlite3_bind_int(st_select, index, artistid);
	SQLITE3_CHECK(db, ret, -1, select);
	media_dbgsql(st_select, __LINE__);
	ret = sqlite3_step(st_select);
	if (ret != SQLITE_ROW)
	{
		const char sql[] = "INSERT INTO opus (titleid,artistid,albumid,genreid,coverid,comment) " \
					"VALUES (@TITLEID,@ARTISTID,@ALBUMID,@GENREID,@COVERID, @COMMENT)";
		sqlite3_stmt *st_insert;
		ret = sqlite3_prepare_v2(db, sql, -1, &st_insert, NULL);
		SQLITE3_CHECK(db, ret, -1, sql);

		int index;

		index = sqlite3_bind_parameter_index(st_insert, "@TITLEID");
		ret = sqlite3_bind_int(st_insert, index, titleid);
		SQLITE3_CHECK(db, ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_insert, "@ARTISTID");
		ret = sqlite3_bind_int(st_insert, index, artistid);
		SQLITE3_CHECK(db, ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_insert, "@ALBUMID");
		ret = sqlite3_bind_int(st_insert, index, *palbumid);
		SQLITE3_CHECK(db, ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_insert, "@GENREID");
		ret = sqlite3_bind_int(st_insert, index, genreid);
		SQLITE3_CHECK(db, ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_insert, "@COVERID");
		ret = sqlite3_bind_int(st_insert, index, coverid);
		SQLITE3_CHECK(db, ret, -1, sql);
		index = sqlite3_bind_parameter_index(st_insert, "@COMMENT");
		ret = sqlite3_bind_text(st_insert, index, comment, -1, SQLITE_STATIC);
		SQLITE3_CHECK(db, ret, -1, sql);
		media_dbgsql(st_insert, __LINE__);
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
		int type;
		type = sqlite3_column_type(st_select, 0);
		if (type == SQLITE_INTEGER)
			opusid = sqlite3_column_int(st_select, 0);
		if (genreid != -1)
		{
			opus_updatefield(ctx, opusid, "genreid", genreid);
		}
		if (coverid != -1)
		{
			opus_updatefield(ctx, opusid, "coverid", coverid);
		}
	}
	sqlite3_finalize(st_select);
	return opusid;
}
static int _media_updateopusid(media_ctx_t *ctx, int id, int opusid)
{
	int ret = 0;
	sqlite3 *db = ctx->db;
	int index;
	sqlite3_stmt *statement;
	const char sql[] = "UPDATE media SET opusid=@OPUSID WHERE id = @ID;";

	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	index = sqlite3_bind_parameter_index(statement, "@OPUSID");
	ret = sqlite3_bind_int(statement, index, opusid);
	SQLITE3_CHECK(db, ret, -1, sql);

	index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(db, ret, -1, sql);

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
	{
		err("media sqlite: error %d on update of %d\n\t%s", ret, id, sqlite3_errmsg(db));
		ret = -1;
	}
	sqlite3_finalize(statement);
	return ret;
}

static int _media_updateinfo(media_ctx_t *ctx, int id, const char *info, int likes)
{
	int ret = 0;
	sqlite3 *db = ctx->db;
	int index;
	sqlite3_stmt *statement;
	const char *sqllist[] = {
			"UPDATE media SET info=@INFO WHERE id = @ID;",
			"UPDATE media SET likes=@LIKES WHERE id = @ID;"
		};
	const char *sql = NULL;
	if (info != NULL)
	{
		sql = sqllist[0];
	}
	else if (likes > 0)
	{
		sql = sqllist[1];
	}
	if (sql == NULL)
		return -1;
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	index = sqlite3_bind_parameter_index(statement, "@INFO");
	if (index != -1)
	{
		ret = sqlite3_bind_text(statement, index, info, -1, SQLITE_STATIC);
		SQLITE3_CHECK(db, ret, -1, sql);
	}

	index = sqlite3_bind_parameter_index(statement, "@LIKES");
	if (index != -1)
	{
		ret = sqlite3_bind_int(statement, index, likes);
		SQLITE3_CHECK(db, ret, -1, sql);
	}

	index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(db, ret, -1, sql);

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
	{
		err("media sqlite: error %d on update of %d\n\t%s", ret, id, sqlite3_errmsg(db));
		ret = -1;
	}
	sqlite3_finalize(statement);
	return (ret != SQLITE_DONE);
}

static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	int id;
	int ret = 0;
	sqlite3 *db = ctx->db;

	dbg("media: inserting media into DB");
	int mimeid = 0;
	if (mime == NULL)
		mime = utils_getmime(path);
	mimeid = wordtable_find(ctx, "mimes", mime);
	if (mimeid == -1)
		mimeid = wordtable_insert(ctx, "mimes", mime);

	int opusid = -1;
	int albumid = -1;
	const char *filename = strrchr(path, '/');
	if (filename != NULL)
		filename += 1;
	else
		filename = path;
	if (info == NULL)
		info = 	media_fillinfo(path, mime);

	json_error_t error;

	json_t *jinfo = json_loads(info, 0, &error);
	int likes;
	opusid = opus_insert(ctx, jinfo, &albumid, filename, &likes);
	info = json_dumps(jinfo, 0);
	json_decref(jinfo);
	if (opusid == -1)
	{
		err("media: opusid error");
		return -1;
	}
	if (path == NULL)
		return opusid;

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

	id = _media_find(ctx, tpath);
	if (id == -1)
	{
		sqlite3_stmt *statement;
		const char sql[] = "INSERT INTO media (url, mimeid, opusid, albumid, info, likes) VALUES(@PATH , @MIMEID, @OPUSID, @ALBUMID, @INFO, @LIKES);";

		ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		SQLITE3_CHECK(db, ret, -1, sql);

		int index;
		index = sqlite3_bind_parameter_index(statement, "@PATH");
		ret = sqlite3_bind_text(statement, index, tpath, -1, SQLITE_STATIC);
		SQLITE3_CHECK(db, ret, -1, sql);

		index = sqlite3_bind_parameter_index(statement, "@INFO");
		if (info != NULL && index > 0)
			ret = sqlite3_bind_text(statement, index, info, -1, SQLITE_STATIC);
		else
			ret = sqlite3_bind_null(statement, index);
		SQLITE3_CHECK(db, ret, -1, sql);
		index = sqlite3_bind_parameter_index(statement, "@OPUSID");
		ret = sqlite3_bind_int(statement, index, opusid);
		SQLITE3_CHECK(db, ret, -1, sql);

		index = sqlite3_bind_parameter_index(statement, "@ALBUMID");
		ret = sqlite3_bind_int(statement, index, albumid);
		SQLITE3_CHECK(db, ret, -1, sql);

		index = sqlite3_bind_parameter_index(statement, "@MIMEID");
		ret = sqlite3_bind_int(statement, index, mimeid);
		SQLITE3_CHECK(db, ret, -1, sql);

		index = sqlite3_bind_parameter_index(statement, "@LIKES");
		if (likes > 0 )
			ret = sqlite3_bind_int(statement, index, likes);
		else
			ret = sqlite3_bind_int(statement, index, 1);
		SQLITE3_CHECK(db, ret, -1, sql);

		media_dbgsql(statement, __LINE__);
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
		playlist_append(ctx, ctx->listid, id, likes);
	}
	else
	{
		if (info != NULL)
			_media_updateinfo(ctx, id, info, 0);
		if (likes > 0)
			_media_updateinfo(ctx, id, NULL, likes);
		_media_updateopusid(ctx, id, opusid);
	}
	free(tpath);

	free((char *)info);

	return opusid;
}


static int media_modify(media_ctx_t *ctx, int opusid, const char *info)
{
	int ret = -1;

	if (opusid != -1)
	{
		int likes = -1;

		sqlite3 *db = ctx->db;
		const char sql[] = "SELECT id, titleid, artistid, genreid, coverid, albumid FROM opus WHERE id=@ID";
		sqlite3_stmt *statememt;
		ret = sqlite3_prepare_v2(db, sql, -1, &statememt, NULL);
		SQLITE3_CHECK(db, ret, -1, sql);

		int index;

		index = sqlite3_bind_parameter_index(statememt, "@ID");
		ret = sqlite3_bind_int(statememt, index, opusid);
		SQLITE3_CHECK(db, ret, -1, sql);

		json_t *jinfo = NULL;
		ret = sqlite3_step(statememt);
		if (ret == SQLITE_ROW)
		{
			int titleid = -1;
			int artistid = -1;
			int genreid = -1;
			char *album = NULL;
			int coverid = -1;
			char *comment = NULL;
			json_error_t error;

			jinfo = json_loads(info, 0, &error);
			opus_populateinfo(ctx, jinfo, &titleid, &artistid, &album, &genreid, &coverid, &comment, &likes);

			opusid = sqlite3_column_int(statememt, 0);
			if (titleid != -1)
				opus_updatefield(ctx, opusid, "titleid", titleid);
			else
				titleid = sqlite3_column_int(statememt, 1);
			if (artistid != -1)
				opus_updatefield(ctx, opusid, "artistid", artistid);
			else
				artistid = sqlite3_column_int(statememt, 2);
			if (genreid != -1)
				opus_updatefield(ctx, opusid, "genreid", genreid);
			else
				genreid = sqlite3_column_int(statememt, 3);
			if (coverid != -1)
				opus_updatefield(ctx, opusid, "coverid", coverid);
			else
				coverid = sqlite3_column_int(statememt, 4);
			if (album != NULL)
			{
				int albumid = album_insert(ctx, album, artistid, coverid, genreid);
				opus_updatefield(ctx, opusid, "albumid", albumid);
			}
			if (comment != NULL)
			{

			}
			sqlite3_finalize(statememt);
			ret = 0;
		}
		if (ret == 0)
		{
			sqlite3 *db = ctx->db;
			const char sql[] = "SELECT id FROM media WHERE opusid=@ID";
			sqlite3_stmt *statememt;
			ret = sqlite3_prepare_v2(db, sql, -1, &statememt, NULL);
			SQLITE3_CHECK(db, ret, -1, sql);

			int index;

			index = sqlite3_bind_parameter_index(statememt, "@ID");
			ret = sqlite3_bind_int(statememt, index, opusid);
			SQLITE3_CHECK(db, ret, -1, sql);

			json_t *jinfo = NULL;
			ret = sqlite3_step(statememt);
			if (ret == SQLITE_ROW)
			{
				char *info = json_dumps(jinfo, 0);
				int id = sqlite3_column_int(statememt, 0);
				sqlite3_finalize(statememt);
				if (strlen(info) > 0)
					ret = _media_updateinfo(ctx, id, info, 0);
				free(info);

				if (likes > 0)
					ret = _media_updateinfo(ctx, id, NULL, likes);
			}
		}
		if (jinfo)
			json_decref(jinfo);
	}
	return ret;
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
		if (type == SQLITE_TEXT)
			mime = sqlite3_column_text(statement, index);

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

		index++;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_TEXT)
			info = sqlite3_column_blob(statement, index);
		if (id != -1)
		{
			info = opus_get(ctx, id, coverid, info);
		}

		media_dbg("media: %d %s", id, url);
		if (cb != NULL && id > -1)
		{
			int ret;
			ret = cb(data, id, url, (const char *)info, mime);
			if (ret != 0)
				break;
		}

		if (info != NULL)
		{
			free((char *)info);
		}

		ret = sqlite3_step(statement);
	}
	return count;
}

static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *data)
{
	int ret = 0;
	sqlite3 *db = ctx->db;
	sqlite3_stmt *statement;
	if (id == -1)
		return 0;

	int count;
	const char sql[] = "SELECT url, mimes.name, opusid, album.coverid, \"info\" FROM media " \
			"INNER JOIN album ON album.id=media.albumid, mimes ON media.mimeid=mimes.id " \
			"WHERE opusid=@ID";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	int index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(db, ret, -1, sql);

	media_dbgsql(statement, __LINE__);
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

	const char sql[] = "SELECT url, mimes.name, opusid, album.coverid, info FROM media " \
			"INNER JOIN playlist ON media.id=playlist.id, album ON album.id=media.albumid, " \
			"mimes ON media.mimeid=mimes.id " \
			"WHERE playlist.listid=@LISTID;";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, ctx->listid);
	SQLITE3_CHECK(db, ret, 1, sql);

	media_dbgsql(statement, __LINE__);
	count = _media_execute(ctx, statement, cb, data);
	sqlite3_finalize(statement);

	return count;
}

static int media_play(media_ctx_t *ctx, int id, media_parse_t cb, void *data)
{
	ctx->mediaid = id;
	media_find(ctx, ctx->mediaid, cb, data);
	return ctx->mediaid;
}

static int media_next(media_ctx_t *ctx)
{
	int ret;
#ifdef MEDIA_DB_PLAYLIST_MEMORY
	sqlite3 *db = ctx->dbplaylist;
#else
	sqlite3 *db = ctx->db;
#endif
	sqlite3_stmt *statement;

	const char *sql = NULL;
	if (ctx->options & OPTION_RANDOM)
	{
		sql = "SELECT id, likes FROM playlist WHERE listid=@LISTID AND likes > 0 ORDER BY RANDOM() LIMIT 1";
	}
	else if (ctx->mediaid > -1)
	{
		sql = "SELECT id, likes FROM playlist WHERE listid=@LISTID AND id > @ID AND likes > 0 LIMIT 1";
	}
	else
	{
		sql = "SELECT id, likes FROM playlist WHERE listid=@LISTID AND likes > 0 LIMIT 1";
	}
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@ID");
	if (index > 0)
	{
		ret = sqlite3_bind_int(statement, index, ctx->mediaid);
		SQLITE3_CHECK(db, ret, -1, sql);
	}

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, ctx->listid);
	SQLITE3_CHECK(db, ret, -1, sql);

	int likes;
	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	if (ret == SQLITE_ROW)
	{
		ctx->mediaid = sqlite3_column_int(statement, 0);
		likes = sqlite3_column_int(statement, 1);
	}
	else
		ctx->mediaid = -1;
	sqlite3_finalize(statement);

	if (ctx->mediaid == -1)
	{
		_media_filter(ctx, TABLE_NONE, NULL);
		if (ctx->options & OPTION_LOOP)
			media_next(ctx);
	}
	else
	{
		playlist_likes(ctx, ctx->listid, ctx->mediaid, likes - 1);
	}
	return ctx->mediaid;
}

static int media_end(media_ctx_t *ctx)
{
	ctx->mediaid = -1;
	return 0;
}

static int _media_remove(media_ctx_t *ctx, int id)
{
	int ret;
	sqlite3 *db = ctx->db;
	sqlite3_stmt *statement;
	const char sql[] = "DELETE FROM media WHERE opusid=@ID";

	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	int index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(db, ret, -1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	if (index > 0)
	{
		ret = sqlite3_bind_int(statement, index, ctx->listid);
		SQLITE3_CHECK(db, ret, -1, sql);
	}

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
		ret = -1;
	else
		playlist_remove(ctx, ctx->listid, id);
	sqlite3_finalize(statement);

	return ret;
}

static int media_remove(media_ctx_t *ctx, int id, const char *path)
{
	int ret = -1;
	sqlite3 *db = ctx->db;
	if (id > 0)
		return playlist_remove(ctx, ctx->listid, id);

	if (path != NULL)
	{
		id = _media_find(ctx, path);
	}

	if (id > 0)
	{
		ret =_media_remove(ctx, id);
	}
	return ret;
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

static int _media_setlist(void *arg, int id, int likes)
{
	media_ctx_t *ctx = (media_ctx_t *)arg;

	if (playlist_has(ctx, ctx->listid, id) > 0)
		return playlist_likes(ctx, ctx->listid, id, likes);
	return playlist_append(ctx, ctx->listid, id, likes);
}

static int playlist_create(media_ctx_t *ctx, char *playlist, int fill)
{
	int ret;
	int listid = 1;
	listid = playlist_find(ctx, playlist);

	if (listid == -1)
	{
		listid = wordtable_insert(ctx, "word", playlist);
		if (listid == -1)
			listid = 1;

		sqlite3 *db = ctx->dbplaylist;

		const char sql[] = "INSERT INTO listname (wordid) VALUES (@ID);";
		sqlite3_stmt *statement;
		ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
		SQLITE3_CHECK(db, ret, 1, sql);

		int index;
		index = sqlite3_bind_parameter_index(statement, "@ID");
		ret = sqlite3_bind_int(statement, index, listid);
		SQLITE3_CHECK(db, ret, 1, sql);

		media_dbgsql(statement, __LINE__);
		ret = sqlite3_step(statement);
		if (ret != SQLITE_DONE)
		{
			SQLITE3_CHECK(db, ret, 1, sql);
		}
		else
		{
			listid = sqlite3_last_insert_rowid(db);
			dbg("new playlist %s %d", playlist, listid);
			int tempolist = ctx->listid;
			ctx->listid = listid;
			if (fill == 1)
				_media_filter(ctx, TABLE_NONE, NULL);
			ctx->listid = tempolist;
		}
		sqlite3_finalize(statement);
	}
	return listid;
}

static int playlist_find(media_ctx_t *ctx, char *playlist)
{
	int ret;
	int listid = -1;
#ifdef MEDIA_DB_PLAYLIST_MEMORY
	sqlite3 *db = ctx->dbplaylist;
#else
	sqlite3 *db = ctx->db;
#endif

	const char sql[] = "SELECT listname.id FROM listname INNER JOIN word ON word.id=listname.wordid WHERE word.name=@NAME";
	sqlite3_stmt *statement;
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, 1, sql);

	int index;

	index = sqlite3_bind_parameter_index(statement, "@NAME");
	ret = sqlite3_bind_text(statement, index, playlist, -1 , SQLITE_STATIC);
	SQLITE3_CHECK(db, ret, 1, sql);

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	if (ret == SQLITE_ROW)
	{
		listid = sqlite3_column_int(statement, 0);
	}
	sqlite3_finalize(statement);
	return listid;
}

static int playlist_count(media_ctx_t *ctx, int listid)
{
	int ret;
#ifdef MEDIA_DB_PLAYLIST_MEMORY
	sqlite3 *db = ctx->dbplaylist;
#else
	sqlite3 *db = ctx->db;
#endif
	int count = 0;

	sqlite3_stmt *statement;
	const char sql[] = "SELECT COUNT(*) FROM playlist WHERE listid=@LISTID";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	if (index > 0)
	{
		ret = sqlite3_bind_int(statement, index, listid);
		SQLITE3_CHECK(db, ret, -1, sql);
	}

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	if (ret == SQLITE_ROW)
	{
		count = sqlite3_column_int(statement, 0);
	}
	sqlite3_finalize(statement);

	return count;
}

static int playlist_has(media_ctx_t *ctx, int listid, int id)
{
	int ret;
#ifdef MEDIA_DB_PLAYLIST_MEMORY
	sqlite3 *db = ctx->dbplaylist;
#else
	sqlite3 *db = ctx->db;
#endif
	int count = 0;

	sqlite3_stmt *statement;
	const char sql[] = "SELECT COUNT(*) FROM playlist WHERE id=@ID AND listid=@LISTID;";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, 1, sql);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(db, ret, 1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, listid);
	SQLITE3_CHECK(db, ret, 1, sql);

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	if (ret == SQLITE_ROW)
	{
		count = sqlite3_column_int(statement, 0);
	}
	sqlite3_finalize(statement);

	return count;
}

static int playlist_append(media_ctx_t *ctx, int listid, int id, int likes)
{
	int ret;
#ifdef MEDIA_DB_PLAYLIST_MEMORY
	sqlite3 *db = ctx->dbplaylist;
#else
	sqlite3 *db = ctx->db;
#endif

	sqlite3_stmt *statement;
	const char sql[] = "INSERT INTO playlist (id, listid, likes) VALUES (@ID, @LISTID, @LIKES);";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, 1, sql);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, listid);
	SQLITE3_CHECK(db, ret, 1, sql);

	index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(db, ret, 1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LIKES");
	ret = sqlite3_bind_int(statement, index, likes);
	SQLITE3_CHECK(db, ret, 1, sql);

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
	{
		SQLITE3_CHECK(db, ret, 1, sql);
	}
	else
		ret = 0;
	sqlite3_finalize(statement);
	return ret;
}

static int playlist_likes(media_ctx_t *ctx, int listid, int id, int likes)
{
	int ret;
#ifdef MEDIA_DB_PLAYLIST_MEMORY
	sqlite3 *db = ctx->dbplaylist;
#else
	sqlite3 *db = ctx->db;
#endif

	sqlite3_stmt *statement;
	const char sql[] = "UPDATE playlist SET likes=@LIKES WHERE id=@ID AND listid=@LISTID;";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, 1, sql);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, listid);
	SQLITE3_CHECK(db, ret, 1, sql);

	index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(db, ret, 1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LIKES");
	ret = sqlite3_bind_int(statement, index, likes);
	SQLITE3_CHECK(db, ret, 1, sql);

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
	{
		SQLITE3_CHECK(db, ret, 1, sql);
	}
	else
		ret = 0;
	sqlite3_finalize(statement);
	return ret;
}

static int playlist_remove(media_ctx_t *ctx, int listid, int id)
{
#ifdef MEDIA_DB_PLAYLIST_MEMORY
	sqlite3 *db = ctx->dbplaylist;
#else
	sqlite3 *db = ctx->db;
#endif
	int ret = -1;
	sqlite3_stmt *statement;
	const char sql[] = "DELETE FROM playlist WHERE id=@ID and listid=@LISTID";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	int index;
	index = sqlite3_bind_parameter_index(statement, "@ID");
	ret = sqlite3_bind_int(statement, index, id);
	SQLITE3_CHECK(db, ret, -1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	if (index > 0)
	{
		ret = sqlite3_bind_int(statement, index, ctx->listid);
		SQLITE3_CHECK(db, ret, -1, sql);
	}

	media_dbgsql(statement, __LINE__);
	ret = sqlite3_step(statement);
	return ret;
}

static int playlist_destroy(media_ctx_t *ctx, int listid)
{
	int ret = 0;
#ifdef MEDIA_DB_PLAYLIST_MEMORY
	sqlite3 *db = ctx->dbplaylist;
#else
	sqlite3 *db = ctx->db;
#endif
	sqlite3_stmt *statement;
	int index;

	/** free the current filter **/
	const char sql[] = "DELETE FROM playlist WHERE listid=@LISTID;";
	ret = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql);

	index = sqlite3_bind_parameter_index(statement, "@LISTID");
	ret = sqlite3_bind_int(statement, index, listid);
	SQLITE3_CHECK(db, ret, 1, sql);

	ret = sqlite3_step(statement);
	sqlite3_finalize(statement);
	if (ret != SQLITE_DONE)
	{
		err("media sqlite: error on delete %d", ret);
		return -1;
	}
	return 0;
}

static int _media_filter(media_ctx_t *ctx, int table, const char *word)
{
	int ret = 0;
	sqlite3 *db = ctx->db;
	sqlite3_stmt *statement;
	int count = 0;
	int index;
	const char *sql[] = {
		"SELECT opusid, likes FROM media;",
		"SELECT opusid, likes FROM media "
		"INNER JOIN opus ON opus.id = media.opusid "
		"WHERE opus.albumid IN ("
			"SELECT album.id FROM album "
				"INNER JOIN word ON word.id=album.wordid "
				"WHERE LOWER(word.name) LIKE LOWER(@NAME)"
			");",
		"SELECT opusid, likes FROM media "
		"INNER JOIN opus ON opus.id = media.opusid "
		"WHERE opus.artistid IN ("
			"SELECT artist.id FROM artist "
				"INNER JOIN word ON word.id=artist.wordid "
				"WHERE LOWER(word.name) LIKE LOWER(@NAME)"
			");",
		"SELECT opusid, likes FROM media "
		"INNER JOIN opus ON opus.id = media.opusid"
		"WHERE opus.speedid in ("
			"SELECT speed.id FROM speed "
				"INNER JOIN word ON word.id=speed.wordid "
				"WHERE LOWER(word.name) LIKE LOWER(@NAME)"
			");",
		"SELECT opusid, likes FROM media "
		"INNER JOIN opus ON opus.id = media.opusid "
		"WHERE titleid IN ("
			"SELECT word.id FROM word "
				"WHERE LOWER(word.name) LIKE LOWER(@NAME)"
			");",
		"SELECT opusid, likes FROM media "
		"INNER JOIN opus ON opus.id = media.opusid "
		"WHERE genreid IN ("
			"SELECT word.id FROM word "
				"WHERE LOWER(word.name) LIKE LOWER(@NAME)"
			");",
	};
	ret = sqlite3_prepare_v2(db, sql[table], -1, &statement, NULL);
	SQLITE3_CHECK(db, ret, -1, sql[table]);

	index = sqlite3_bind_parameter_index(statement, "@NAME");
	if (index > 0)
	{
		ret = sqlite3_bind_text(statement, index, word, -1 , SQLITE_STATIC);
		SQLITE3_CHECK(db, ret, 1, sql[table]);
	}
	ret = sqlite3_step(statement);

	while (ret == SQLITE_ROW)
	{
		int id = sqlite3_column_int(statement, 0);
		int likes = sqlite3_column_int(statement, 1);
		_media_setlist(ctx, id, likes);
		ret = sqlite3_step(statement);
	}
	sqlite3_finalize(statement);
	return count;
}

static int media_filter(media_ctx_t *ctx, media_filter_t *filter)
{
	int listid = playlist_create(ctx, "filter", 0);
	if (ctx->listid != listid)
		ctx->oldlistid = ctx->listid;
	ctx->listid = listid;

	if (playlist_destroy(ctx, listid) != 0)
	{
		return 0;
	}
	if (filter == NULL)
	{
		ctx->listid = ctx->oldlistid;
		return 0;
	}

	int count = 0;
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

static int _media_changelist(media_ctx_t *ctx, char *playlist)
{
	int listid = playlist_create(ctx, playlist, ctx->fill);
	return listid;
}

static int _media_opendb(media_ctx_t *ctx, const char *url)
{
	int ret;
	struct stat dbstat;
	/**
	 * store path to keep query all the time
	 */
	ctx->path = utils_getpath(url, "db://", &ctx->query, 1);

	if (ctx->path == NULL)
		return -1;
	ret = stat(ctx->path, &dbstat);
	if ((ret == 0) && S_ISREG(dbstat.st_mode))
	{
		char *readwrite = NULL;
		if (ctx->query != NULL)
			readwrite = strstr(ctx->query, "readwrite");
		if (readwrite != NULL)
			ret = sqlite3_open_v2(ctx->path, &ctx->db, SQLITE_OPEN_READWRITE, NULL);
		else
			ret = SQLITE_ERROR;
		if (ret != SQLITE_OK)
		{
			err("Open database in read only");
			ret = sqlite3_open_v2(ctx->path, &ctx->db, SQLITE_OPEN_READONLY, NULL);
		}
	}
	else
	{
		ret = sqlite3_open_v2(ctx->path, &ctx->db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
		ret = SQLITE_CORRUPT;
	}
	ctx->listid = 1;
	return ret;
}

#ifdef MEDIA_SQLITE_INITDB
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
 * |           |   speed   | speed of the music      |
 * |           |           | (ref speed)             |
 * |           |  comment  | text about the album    |
 * ---------------------------------------------------
 */
static const char *query[] = {
"CREATE TABLE mimes (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);",
"INSERT INTO mimes (id, name) VALUES (0, \"octet/stream\");",
"INSERT INTO mimes (id, name) VALUES (1, \"audio/mp3\");",
"INSERT INTO mimes (id, name) VALUES (2, \"audio/flac\");",
"INSERT INTO mimes (id, name) VALUES (3, \"audio/alac\");",
"INSERT INTO mimes (id, name) VALUES (4, \"audio/pcm\");",
"CREATE TABLE media (id INTEGER PRIMARY KEY, url TEXT UNIQUE NOT NULL, "\
	"mimeid INTEGER, info BLOB, opusid INTEGER, " \
	"albumid INTEGER NOT NULL DEFAULT(1), likes INTEGER DEFAULT(1), " \
	"FOREIGN KEY (mimeid) REFERENCES mimes(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (opusid) REFERENCES opus(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (albumid) REFERENCES album(id) ON UPDATE SET NULL);",
"CREATE TABLE opus (id INTEGER PRIMARY KEY,  titleid INTEGER, " \
	"artistid INTEGER, otherid INTEGER, albumid INTEGER DEFAULT(1), " \
	"genreid INTEGER DEFAULT(0), coverid INTEGER, " \
	"speedid INTEGER DEFAULT(0), introid INTEGER, comment BLOB, " \
	"FOREIGN KEY (titleid) REFERENCES word(id), " \
	"FOREIGN KEY (introid) REFERENCES opus(id), " \
	"FOREIGN KEY (artistid) REFERENCES artist(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (albumid) REFERENCES album(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (genreid) REFERENCES word(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (speedid) REFERENCES speed(id) ON UPDATE SET NULL," \
	"FOREIGN KEY (coverid) REFERENCES cover(id) ON UPDATE SET NULL);",
"CREATE TABLE album (id INTEGER PRIMARY KEY, wordid INTEGER UNIQUE NOT NULL, " \
	"artistid INTEGER, genreid INTEGER DEFAULT(0), " \
	"speedid INTEGER DEFAULT(0), coverid INTEGER, comment BLOB, " \
	"FOREIGN KEY (wordid) REFERENCES word(id), " \
	"FOREIGN KEY (artistid) REFERENCES artist(id) ON UPDATE SET NULL, " \
	"FOREIGN KEY (genreid) REFERENCES word(id) ON UPDATE SET NULL, " \
	"FOREIGN KEY (coverid) REFERENCES cover(id) ON UPDATE SET NULL);",
"INSERT INTO album (id, wordid) VALUES (1, 2);",
"CREATE TABLE artist (id INTEGER PRIMARY KEY, " \
	"wordid INTEGER UNIQUE NOT NULL, comment BLOB, " \
	"FOREIGN KEY (wordid) REFERENCES word(id));",
"CREATE TABLE genre (id INTEGER PRIMARY KEY, wordid INTEGER NOT NULL, " \
	"FOREIGN KEY (wordid) REFERENCES word(id));",
"INSERT INTO genre (id, wordid) VALUES (0, 2);",
"INSERT INTO genre (id, wordid) VALUES (1, 8);",
"INSERT INTO genre (id, wordid) VALUES (2, 9);",
"INSERT INTO genre (id, wordid) VALUES (3, 10);",
"INSERT INTO genre (id, wordid) VALUES (4, 11);",
"INSERT INTO genre (id, wordid) VALUES (5, 5);",
"CREATE TABLE speed (id INTEGER PRIMARY KEY, wordid INTEGER NOT NULL, " \
	"FOREIGN KEY (wordid) REFERENCES word(id));",
"INSERT INTO speed (id, wordid) VALUES (0, 2);",
"INSERT INTO speed (id, wordid) VALUES (1, 3);",
"INSERT INTO speed (id, wordid) VALUES (2, 4);",
"INSERT INTO speed (id, wordid) VALUES (3, 5);",
"INSERT INTO speed (id, wordid) VALUES (4, 6);",
"CREATE TABLE cover (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);",
"CREATE TABLE word (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);",
"INSERT INTO word (id, name) VALUES (1, \"default\");",
"INSERT INTO word (id, name) VALUES (2, \"unknown\");",
"INSERT INTO word (id, name) VALUES (3, \"cool\");",
"INSERT INTO word (id, name) VALUES (4, \"ambiant\");",
"INSERT INTO word (id, name) VALUES (5, \"dance\");",
"INSERT INTO word (id, name) VALUES (6, \"live\");",
"INSERT INTO word (id, name) VALUES (7, \"radio\");",
"INSERT INTO word (id, name) VALUES (8, \"pop\");",
"INSERT INTO word (id, name) VALUES (9, \"rock\");",
"INSERT INTO word (id, name) VALUES (10, \"jazz\");",
"INSERT INTO word (id, name) VALUES (11, \"classic\");",
				NULL,
			};
#endif

static const char *playlistquery[] = {
"CREATE TABLE playlist (id INTEGER, listid INTEGER DEFAULT(1), likes INTEGER DEFAULT(1), " \
	"FOREIGN KEY (id) REFERENCES media(id) ON UPDATE SET NULL, " \
	"FOREIGN KEY (listid) REFERENCES listname(id) ON UPDATE SET NULL);",
"CREATE TABLE listname (id INTEGER PRIMARY KEY, wordid INTEGER, " \
	"FOREIGN KEY (wordid) REFERENCES word(id));",
"INSERT INTO listname (id, wordid) VALUES (1, 1);",
				NULL,
			};

static int _media_initdb(sqlite3 *db, const char *query[])
{
	char *error = NULL;
	int i = 0;
	int ret = SQLITE_OK;

	while (query[i] != NULL)
	{
		media_dbg("query %d",i);
		media_dbg("query %s", query[i]);
		ret = sqlite3_exec(db, query[i], NULL, NULL, &error);
		if (ret != SQLITE_OK)
		{
			err("media: prepare error %d query[%d]", ret, i);
			err("\t%s", sqlite3_errmsg(db));
			dbg("media: query %s", query[i]);
			break;
		}
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
			ret = _media_initdb(ctx->db, query);
			if (ret == SQLITE_OK)
			{
				ret = _media_initdb(ctx->db, playlistquery);
			}
		}
#ifdef MEDIA_DB_PLAYLIST_MEMORY
		if (ret == SQLITE_OK)
		{
			ret = sqlite3_open_v2(":memory:", &ctx->dbplaylist, SQLITE_OPEN_CREATE |SQLITE_OPEN_READWRITE, NULL);
			if (ret == SQLITE_OK)
			{
				ret = _media_initdb(ctx->dbplaylist, playlistquery);
			}
		}
#endif
		if (ret == SQLITE_OK && ctx->query != NULL)
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
		if (ret == SQLITE_OK)
		{
			warn("media: db %s", url);
			_media_filter(ctx, TABLE_NONE, NULL);
		}
		else
		{
			err("media: db open error %s %s", sqlite3_errstr(ret), strerror(errno));
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
	{
		int ret = sqlite3_close_v2(ctx->db);
		if (ret != SQLITE_OK)
			err("media: DB close error %s", sqlite3_errstr(ret));
	}
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
	.modify = media_modify,
	.remove = media_remove,
	.count = media_count,
	.end = media_end,
	.random = media_random,
	.loop = media_loop,
};

#ifdef GENERATEDBSCRIPT
#include "decoder.h"
const decoder_ops_t *decoder_check(const char *path)
{
	return NULL;
}

int main()
{
	for (int i = 0; query[i] != NULL; i++)
	{
		printf("%s\n", query[i]);
	}
	return 0;
}
#endif

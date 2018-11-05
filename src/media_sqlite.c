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

#include <sqlite3.h>

#include "player.h"
#include "media.h"
#include "decoder.h"

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

static int media_count(media_ctx_t *ctx);
static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
static int media_find(media_ctx_t *ctx, int id,  media_parse_t cb, void *data);
static int media_current(media_ctx_t *ctx, media_parse_t cb, void *data);
static int media_list(media_ctx_t *ctx, media_parse_t cb, void *data);
static int media_play(media_ctx_t *ctx, media_parse_t cb, void *data);
static int media_next(media_ctx_t *ctx);
static int media_end(media_ctx_t *ctx);

static const char *utils_getmime(const char *path)
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
	int size = 256;
	char *sql = sqlite3_malloc(size);
	snprintf(sql, size, "select \"id\" from \"media\" ");
	sqlite3_prepare_v2(db, sql, size, &statement, NULL);

	int ret = sqlite3_step(statement);
	if (ret != SQLITE_ERROR)
	{
		do
		{
			count++;
			ret = sqlite3_step(statement);
		} while (ret == SQLITE_ROW);
	}
	sqlite3_finalize(statement);
	sqlite3_free(sql);

	return count;
}

static int findmedia(sqlite3 *db, const char *path)
{
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	snprintf(sql, size, "select \"id\" from \"media\" where \"url\"=@PATH");
	sqlite3_prepare_v2(db, sql, size, &statement, NULL);
	/** set the default value of @FIELDS **/
	sqlite3_bind_text(statement, sqlite3_bind_parameter_index(statement, "@PATH"), path, -1, SQLITE_STATIC);

	int id = _execute(statement);

	sqlite3_finalize(statement);
	sqlite3_free(sql);
	return id;
}

static int media_remove(media_ctx_t *ctx, int id, const char *path)
{
	int ret;
	sqlite3 *db = ctx->db;
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	if (path != NULL)
	{
		snprintf(sql, size, "delete from \"media\" where \"url\"=@PATH");
		ret = sqlite3_prepare_v2(db, sql, size, &statement, NULL);
		/** set the default value of @FIELDS **/
		int index = sqlite3_bind_parameter_index(statement, "@PATH");
		ret = sqlite3_bind_text(statement, index, path, -1, SQLITE_STATIC);
	}
	else if (id > 0)
	{
		snprintf(sql, size, "delete from \"media\" where \"id\"=@ID");
		ret = sqlite3_prepare_v2(db, sql, size, &statement, NULL);
		/** set the default value of @FIELDS **/
		int index = sqlite3_bind_parameter_index(statement, "@ID");
		ret = sqlite3_bind_int(statement, index, id);
	}
	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
		ret = -1;
	else
	{
		dbg("putv: remove media %s", path);
	}
	sqlite3_finalize(statement);
	sqlite3_free(sql);
	return ret;
}

static int media_insert(media_ctx_t *ctx, const char *path, const char *info, const char *mime)
{
	sqlite3 *db = ctx->db;

	if (path == NULL)
		return -1;

	int id = findmedia(db, path);
	if (id != -1)
		return 0;

	int ret = 0;
	sqlite3_stmt *statement;
	int size = 1024;
	char *sql;
	sql = sqlite3_malloc(size);
	snprintf(sql, size, "insert into \"media\" (\"url\", \"mime\", \"info\") values(@PATH , @MIME , @INFO);");

	ret = sqlite3_prepare_v2(db, sql, size, &statement, NULL);
	if (ret != SQLITE_OK)
		return -1;

	int index;
	char *tpath = NULL;
	if (strstr(path, "://"))
	{
		tpath = malloc(strlen(path)+1);
		strcpy(tpath, path);
	}
	else
	{
		int len = PROTOCOLNAME_LENGTH + strlen(path) + 1;
		tpath = malloc(len);
		snprintf(tpath, len, PROTOCOLNAME"%s", path);
	}
	index = sqlite3_bind_parameter_index(statement, "@PATH");
	ret = sqlite3_bind_text(statement, index, tpath, -1, SQLITE_STATIC);
	free(tpath);

	index = sqlite3_bind_parameter_index(statement, "@INFO");
	if (info != NULL)
		ret = sqlite3_bind_text(statement, index, info, -1, SQLITE_STATIC);
	else
		ret = sqlite3_bind_null(statement, index);
	index = sqlite3_bind_parameter_index(statement, "@MIME");
	if (mime == NULL)
		mime = utils_getmime(path);
	if (mime)
		ret = sqlite3_bind_text(statement, index, mime, -1, SQLITE_STATIC);
	else
		ret = sqlite3_bind_null(statement, index);

	ret = sqlite3_step(statement);
	if (ret != SQLITE_DONE)
	{
		err("media sqlite: error on insert %s", ret);
		ret = -1;
	}
	else
	{
		dbg("putv: new media %s", path);
	}
	sqlite3_finalize(statement);
	sqlite3_free(sql);

	return ret;
}

static int _media_execute(sqlite3_stmt *statement, media_parse_t cb, void *data)
{
	int count = 0;
	int ret = sqlite3_step(statement);
	while (ret == SQLITE_ROW)
	{
		count ++;
		const char *url = NULL;
		const void *info = NULL;
		const char *mime = NULL;
		int id;
		int index = 0;
		int type;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_TEXT)
			url = sqlite3_column_text(statement, index);
		index = 1;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_TEXT)
			info = sqlite3_column_blob(statement, index);
		index = 2;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_TEXT)
			mime = sqlite3_column_text(statement, index);
		index = 3;
		type = sqlite3_column_type(statement, index);
		if (type == SQLITE_INTEGER)
			id = sqlite3_column_int(statement, index);
		dbg("media: %d %s", id, url);
		if (cb != NULL)
			cb(data, url, (const char *)info, mime);

		ret = sqlite3_step(statement);
	}
	return count;
}

static int media_find(media_ctx_t *ctx, int id, media_parse_t cb, void *data)
{
	int count;
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	snprintf(sql, size, "select \"url\", \"info\", \"mime\", \"id\" from \"media\" where id = @ID");
	sqlite3_prepare_v2(ctx->db, sql, size, &statement, NULL);

	int index = sqlite3_bind_parameter_index(statement, "@ID");
	sqlite3_bind_int(statement, index, id);

	count = _media_execute(statement, cb, data);
	sqlite3_finalize(statement);
	sqlite3_free(sql);
	return count;
}

static int media_current(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	int ret = media_find(ctx, ctx->mediaid, cb, data);
	return ret;
}

static int media_list(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	int ret = 0;
	sqlite3 *db = ctx->db;
	int count = 0;
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	snprintf(sql, size, "select \"url\", \"info\", \"mime\", \"id\" from \"media\" ");
	ret = sqlite3_prepare_v2(db, sql, size, &statement, NULL);

	count = _media_execute(statement, cb, data);
	sqlite3_finalize(statement);
	sqlite3_free(sql);

	return count;
}

static int media_play(media_ctx_t *ctx, media_parse_t cb, void *data)
{
	media_current(ctx, cb, data);
	return ctx->mediaid;
}

static int media_next(media_ctx_t *ctx)
{
	sqlite3_stmt *statement;
	int size = 256;
	char *sql = sqlite3_malloc(size);
	if (ctx->mediaid != 0)
	{
		snprintf(sql, size, "select \"id\" from \"media\" where id > @ID");
		sqlite3_prepare_v2(ctx->db, sql, size, &statement, NULL);

		int index = sqlite3_bind_parameter_index(statement, "@ID");
		sqlite3_bind_int(statement, index, ctx->mediaid);
	}
	else
	{
		snprintf(sql, size, "select \"id\" from \"media\"");
		sqlite3_prepare_v2(ctx->db, sql, size, &statement, NULL);
	}

	int ret = sqlite3_step(statement);
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
	sqlite3_free(sql);
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

static media_ctx_t *media_init(const char *dbpath)
{
	media_ctx_t *ctx = NULL;
	sqlite3 *db = NULL;
	int ret = SQLITE_ERROR;

	if (dbpath)
	{
#ifndef MEDIA_EXT
		if (!access(dbpath, R_OK|W_OK))
		{
			ret = sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READWRITE, NULL);
		}
		else if (!access(dbpath, R_OK))
		{
			ret = sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL);
		}
		else
		{
			ret = sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
			ret = SQLITE_CORRUPT;
		}
#else
#endif
	}
	if (db)
	{
		if (ret == SQLITE_CORRUPT)
		{
			const char *query[] = {
#ifndef MEDIA_EXT
				"create table media (\"id\" INTEGER PRIMARY KEY, \"url\" TEXT UNIQUE NOT NULL, \"mime\" TEXT, \"info\" BLOB, \"opusid\" INTEGER);",
#else
				"create table media (\"id\" INTEGER PRIMARY KEY, \"url\" TEXT UNIQUE NOT NULL, \"mime\" TEXT, \"info\" BLOB, \"opusid\" INTEGER, FOREIGN KEY (opusid) REFERENCES opus(id) ON UPDATE SET NULL);",
#endif
				NULL,
			};
			char *error = NULL;
			int i = 0;
			ret = SQLITE_OK;
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

media_ops_t *media_sqlite = &(media_ops_t)
{
	.init = media_init,
	.destroy = media_destroy,
	.next = media_next,
	.play = media_play,
	.list = media_list,
	.current = media_current,
	.find = media_find,
	.insert = media_insert,
	.remove = media_remove,
	.count = media_count,
	.end = media_end,
	.options = media_options,
};

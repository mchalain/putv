/*****************************************************************************
 * cmds_json.c
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
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>

#include <pthread.h>
#include <jansson.h>

#include "unix_server.h"
#include "player.h"
#include "jsonrpc.h"
typedef struct json_request_list_s json_request_list_t;
struct json_request_list_s
{
	json_request_list_t *next;
	json_t *request;
	thread_info_t *info;
	int id;
};

typedef enum eventsmask_e eventsmask_t;
enum eventsmask_e
{
	ONCHANGE = 0x01,
};

#define ONCHANGE_MEDIA 0x01
#define ONCHANGE_SOURCE 0x02
#define ONCHANGE_VOLUME 0x04
typedef struct cmds_ctx_s cmds_ctx_t;
struct cmds_ctx_s
{
	player_ctx_t *player;
	sink_t *sink;
	const char *socketpath;
	pthread_t threadrecv;
	pthread_t threadsend;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	thread_info_t *info;
	json_request_list_t *requests;
	unsigned int eventsmask;
	int run;
	int onchangeid;
	int onchangemask;
};
#define CMDS_CTX
#include "cmds.h"
#include "media.h"
#include "decoder.h"
#include "src.h"
#include "sink.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define cmds_dbg(...)

#ifdef DEBUG
#define JSONRPC_DEBUG_FORMAT JSON_INDENT(2)
#else
#define JSONRPC_DEBUG_FORMAT 0
#endif

static struct jsonrpc_method_entry_t method_table[];

static const char const *str_stop = "stop";
static const char const *str_play = "play";
static const char const *str_pause = "pause";
static const char const *str_next = "next";

static int _print_entry(void *arg, int id, const char *url,
		const char *info, const char *mime)
{
	if (url == NULL)
		return -1;

	json_t *object = (json_t*)arg;
	json_t *json_info;
	if (info != NULL)
	{
		json_error_t error;
		json_info = json_loads(info, 0, &error);
		json_object_set(object, "info", json_info);
	}

	json_t *json_sources = json_array();
	int i;
	for (i = 0; i < 1; i++)
	{
		json_t *json_source = json_object();
		json_t *json_url = json_string(url);
		json_object_set(json_source, "url", json_url);

		if (mime != NULL)
		{
			json_t *json_mime = json_string(mime);
			json_object_set(json_source, "mime", json_mime);
		}
		json_array_append_new(json_sources, json_source);
	}
	json_object_set(object, "sources", json_sources);

	return 0;
}

#define MAX_ITEMS 10
typedef struct entry_s
{
	json_t *list;
	int max;
	int first;
	int count;
} entry_t;

static int _append_entry(void *arg, int id, const char *url,
		const char *info, const char *mime)
{
	int ret = 0;
	entry_t *entry = (entry_t*)arg;

	if ((id >= entry->first) && entry->max)
	{
		json_t *object = json_object();
		if (id >= 0 &&_print_entry(object, id, url, info, mime) == 0)
		{
			json_t *index = json_integer(id);
			json_object_set(object, "id", index);
			json_array_append_new(entry->list, object);
			entry->max--;
			entry->count++;
			if (entry->max == 0)
				ret = -1;
		}
		else
			json_decref(object);
	}
	return ret;
}

static int method_list(json_t *json_params, json_t **result, void *userdata)
{
	int ret;
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	int count = media->ops->count(media->ctx);
	cmds_dbg("cmds: list");

	if (media->ops->list == NULL)
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
		return -1;
	}

	entry_t entry;
	entry.list = json_array();

	json_t *maxitems_js = json_object_get(json_params, "maxitems");
	if (maxitems_js)
	{
		int maxitems = json_integer_value(maxitems_js);
		if (maxitems > -1)
			entry.max = (maxitems < count)?maxitems:count;
		else
			entry.max = count;
	}
	else
		entry.max = MAX_ITEMS;

	json_t *first = json_object_get(json_params, "first");
	if (first)
		entry.first = json_integer_value(first);
	else
		entry.first = 0;

	entry.count = 0;
	media->ops->list(media->ctx, _append_entry, (void *)&entry);
	*result = json_pack("{s:i,s:i,s:o}",
		"count", count, "nbitems", entry.count, "playlist", entry.list);

	return 0;
}

static int method_info(json_t *json_params, json_t **result, void *userdata)
{
	int ret;
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	cmds_dbg("cmds: info");

	if (media->ops->find == NULL)
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
		return -1;
	}

	json_t *id_js = json_object_get(json_params, "id");
	if (id_js && json_is_integer(id_js))
	{
		int id = json_integer_value(id_js);
		*result = json_object();
		media->ops->find(media->ctx, id, _print_entry, (void *)*result);
		json_object_set(*result, "id", id_js);
	}
	else
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "id not found", json_null());
	}

	return 0;
}

typedef struct
{
	media_t *media;
	const char *info;
	json_t *result;
} _setinfo_t;
static int _setinfo(void *arg, int id, const char *url,
		const char *info, const char *mime)
{
	if (url == NULL)
		return -1;
	_setinfo_t *data = (_setinfo_t *)arg;
	media_t *media = data->media;
	const char *newinfo = data->info;
	if (newinfo && info && strcmp(newinfo, info))
	{
		if (media->ops->modify(media->ctx, id, newinfo) != 0)
		{
			json_object_set(data->result, "status", json_string("KO"));
			json_object_set(data->result, "message", json_string("storage error"));
			return -2;
		}
	}
	json_object_set(data->result, "status", json_string("OK"));
	return 0;
}

static int method_setinfo(json_t *json_params, json_t **result, void *userdata)
{
	int ret;
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	cmds_dbg("cmds: setinfo");

	if (media->ops->modify == NULL)
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
		return -1;
	}

	json_t *id_js = json_object_get(json_params, "id");
	if (id_js && json_is_integer(id_js))
	{
		int id = json_integer_value(id_js);
		char * info = json_dumps(json_object_get(json_params, "info"), 0);
		*result = json_object();
		_setinfo_t data = {
			.media = media,
			.info = info,
			.result = *result,
		};
		media->ops->find(media->ctx, id, _setinfo, &data);
		free(info);
		json_object_set(*result, "id", id_js);
	}
	else
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "id not found", json_null());
	}

	return 0;
}

static int method_filter(json_t *json_params, json_t **result, void *userdata)
{
	int ret;
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	media_filter_t filter = {0};
	media_filter_t *pfilter = NULL;
	cmds_dbg("cmds: filter");

	if (media->ops->filter == NULL)
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
		return -1;
	}

	json_t *value;
	value = json_object_get(json_params, "keyword");
	if (json_is_string(value) && json_string_length(value) > 0)
	{
		filter.keyword = json_string_value(value);
		pfilter = &filter;
	}
	value = json_object_get(json_params, "title");
	if (json_is_string(value) && json_string_length(value) > 0)
	{
		filter.title = json_string_value(value);
		pfilter = &filter;
	}
	value = json_object_get(json_params, "artist");
	if (json_is_string(value) && json_string_length(value) > 0)
	{
		filter.artist = json_string_value(value);
		pfilter = &filter;
	}
	value = json_object_get(json_params, "album");
	if (json_is_string(value) && json_string_length(value) > 0)
	{
		filter.album = json_string_value(value);
		pfilter = &filter;
	}
	value = json_object_get(json_params, "genre");
	if (json_is_string(value) && json_string_length(value) > 0)
	{
		filter.genre = json_string_value(value);
		pfilter = &filter;
	}

	int count;
	count = media->ops->filter(media->ctx, pfilter);
	*result = json_pack("{s:i}", "count", count);

	return 0;
}

static int method_remove(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	int ret = -1;
	cmds_dbg("cmds: remove");

	if (media->ops->remove == NULL)
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
		return -1;
	}
	int id = -1;

	if (json_is_array(json_params))
	{
		size_t index;
		json_t *value;
		json_array_foreach(json_params, index, value)
		{
			if (json_is_string(value))
			{
				const char *str = json_string_value(value);
				ret = media->ops->remove(media->ctx, 0, str);
			}
			else if (json_is_integer(value))
			{
				int id = json_integer_value(value);
				ret = media->ops->remove(media->ctx, id, NULL);
			}
		}
	}
	else if (json_is_object(json_params))
	{
		json_t *value;
		value = json_object_get(json_params, "id");
		if (json_is_integer(value))
		{
			id = json_integer_value(value);
			ret = media->ops->remove(media->ctx, id, NULL);
		}
		value = json_object_get(json_params, "url");
		if (json_is_string(value))
		{
			const char *str = json_string_value(value);
			ret = media->ops->remove(media->ctx, 0, str);
		}
	}
	if (ret == -1)
	{
		*result = jsonrpc_error_object(-12345,
			"remove error",
			json_string("media could not be removed into the playlist"));
	}
	else
	{
		*result = json_pack("{s:s,s:s}", "status", "DONE", "message", "media removed");
		if (id != -1)
			json_object_set(*result, "id", json_integer(id));
		ret = 0;
	}
	return ret;
}

typedef int (*append_cb_t)(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
static int method_appendsource(media_t *media, json_t * path, json_t * info, json_t * mime)
{
	append_cb_t append_cb = NULL;
	if (media->ops->append != NULL)
		append_cb = media->ops->append;
	else if (media->ops->insert != NULL)
		append_cb = media->ops->insert;
	else
	{
		return -2;
	}

	const char *str = NULL;
	char *infostr = NULL;
	const char *mimestr = NULL;
	str = json_string_value(path);
	mimestr = json_string_value(mime);
	cmds_dbg("cmds: append %s", str);
	if (info == NULL)
	{
		infostr = strdup("");
	}
	else if (json_is_string(info))
	{
		infostr = strdup(json_string_value(info));
	}
	else if (json_is_object(info))
	{
		infostr = json_dumps(info, 0);
	}
	int ret = append_cb(media->ctx, str, infostr, mimestr);
	free(infostr);
	return ret;
}

static int method_append(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	cmds_dbg("cmds: append");

	int ret = -1;
	if (json_is_array(json_params))
	{
		size_t i;
		json_t *value;
		json_array_foreach(json_params, i, value)
		{
			if (json_is_string(value))
			{
				const char *str = NULL;
				str = json_string_value(value);
				cmds_dbg("cmds: append %s", str);
				ret = method_appendsource(media, value, NULL, NULL);
			}
			else if (json_is_object(value))
			{
				json_t * info = json_object_get(value, "info");
				json_t * sources = json_object_get(value, "sources");
				json_t * source = json_object_get(source, "url");
				size_t j;
				if (json_is_array(sources))
				{
					json_array_foreach(sources, j, source)
					{
						json_t * path = json_object_get(source, "url");
						json_t * mime = json_object_get(value, "mime");
						ret = method_appendsource(media, path, info, mime);
						if (ret < 0)
							break;
					}
				}
				else if (json_is_string(source))
				{
					ret = method_appendsource(media, source, info, NULL);
				}
			}
			else
			{
				err("cmds: json badly formatted");
			}
			if (ret == -2)
			{
				*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
				return -1;
			}

			if (ret == -1)
			{
				char *valuestr = json_dumps(value, 0);
				err("cmds: %s could not be inserted into the playlist", valuestr);
				free(valuestr);
				*result = jsonrpc_error_object(-12345,
					"append error",
					json_string("media could not be inserted into the playlist"));
				return -1;
			}
		}
		*result = json_pack("{s:s,s:s,s:i}", "status", "DONE", "message", "media append", "id", ret);
		ret = 0;
	}
	else
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));

	return ret;
}

static int method_play(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	int ret = -1;
	cmds_dbg("cmds: play");

	switch (player_state(ctx->player, STATE_PLAY))
	{
	case STATE_STOP:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_stop));
	break;
	case STATE_CHANGE:
	case STATE_PLAY:
		*result = json_pack("{ss}", "state", str_play);
		ret = 0;
	break;
	case STATE_PAUSE:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_pause));
	break;
	default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	}
	return ret;
}

static int method_pause(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	int ret = -1;
	cmds_dbg("cmds: pause");

	switch (player_state(ctx->player, STATE_PAUSE))
	{
	case STATE_STOP:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_stop));
	break;
	case STATE_PLAY:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_play));
	break;
	case STATE_PAUSE:
		*result = json_pack("{ss}", "state", str_pause);
		ret = 0;
	break;
	default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	}
	return ret;
}

static int method_stop(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	int ret = -1;
	cmds_dbg("cmds: stop");

	switch (player_state(ctx->player, STATE_STOP))
	{
	case STATE_STOP:
		*result = json_pack("{ss}", "state", str_stop);
		ret = 0;
	break;
	case STATE_PLAY:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_play));
	break;
	case STATE_PAUSE:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_pause));
	break;
	default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	}
	return ret;
}

static int method_next(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	int ret = -1;
	cmds_dbg("cmds: next");

	int id = player_next(ctx->player, 1);
	switch (player_state(ctx->player, STATE_UNKNOWN))
	{
	case STATE_STOP:
		*result = json_pack("{ss}", "state", str_stop);
		ret = 0;
	break;
	case STATE_PLAY:
	case STATE_CHANGE:
		*result = json_pack("{ss,s:i}", "state", str_play, "id", id);
		ret = 0;
	break;
	case STATE_PAUSE:
		*result = json_pack("{ss,s:i}", "state", str_pause, "id", id);
		ret = 0;
	break;
	default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	}
	return ret;
}

static int method_setnext(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	int ret = -1;
	int id = 0;

	if (json_is_object(json_params))
	{
		id = json_integer_value(json_object_get(json_params, "id"));
	}
	cmds_dbg("cmds: setnext id %d", id);
	ret = player_play(ctx->player, id);
	if (ret == 0)
	{
		*result = json_pack("{si}", "next", id);
		ret = 0;
	}
	else
	{
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	}
	return ret;
}

static int method_getposition(json_t *json_params, json_t **result, void *userdata)
{
	int ret = -1;
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	const src_t *src = player_source(ctx->player);
	decoder_t *decoder = NULL;
	if (src != NULL)
	{
		decoder = src->ops->estream(src->ctx, 0);
	}
	if (decoder != NULL)
	{
		*result = json_pack("{s:i,s:i}",
			"position", decoder->ops->position(decoder->ctx),
			"duration", decoder->ops->duration(decoder->ctx));
		ret = 0;
	}
	else
	{
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	}
	return ret;
}

typedef struct _display_ctx_s _display_ctx_t;
struct _display_ctx_s
{
	cmds_ctx_t *ctx;
	json_t *result;
};

static int _display(void *arg, int id, const char *url, const char *info, const char *mime)
{
	_display_ctx_t *display =(_display_ctx_t *)arg;
	cmds_ctx_t *ctx = display->ctx;
	json_t *result = display->result;
	json_t *object;

	if (result == NULL)
		display->result = json_array();
	if (json_is_object(result))
		object = result;
	else if (json_is_array(result))
		object = json_object();

	_print_entry(object, id, url, info, mime);
	json_t *index = json_integer(id);
	json_object_set(object, "id", index);

	if (json_is_array(result))
		json_array_append_new(result, object);
	return 0;
}

static int method_change(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	json_t *value;
	int ret = -1;
	cmds_dbg("cmds: change");
	dbg("cmds: change %s", json_dumps(json_params,JSONRPC_DEBUG_FORMAT));

	if (json_is_object(json_params))
	{
		int now = 0;
		int loop = 0;
		int random = 0;
		value = json_object_get(json_params, "options");
		if (json_is_array(value))
		{
			int index;
			json_t *option = NULL;
			json_array_foreach(value, index, option)
			{
				if (json_is_string(option) && !strcmp(json_string_value(option), "loop"))
					loop = 1;
				if (json_is_string(option) && !strcmp(json_string_value(option), "random"))
					random = 1;
				if (json_is_string(option) && !strcmp(json_string_value(option), "autostart"))
					now = 1;
			}
		}
		value = json_object_get(json_params, "media");
		if (json_is_string(value))
		{
			const char *str = json_string_value(value);
			dbg("cmds: try change %s", str);
			ret = player_change(ctx->player, str, random, loop, now);
			if (ret >= 0)
			{
				*result = json_pack("{s:s,s:s}", "media", "changed", "state", str_stop);
			}
			else
			{
				*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("media refused"));
			}
		}
		value = json_object_get(json_params, "id");
		if (*result == NULL && json_is_integer(value))
		{
			_display_ctx_t display = {
				.ctx = ctx,
				.result = json_object(),
			};
			media_t *media = player_media(ctx->player);
			int id = json_integer_value(value);
			if (media->ops->find(media->ctx, id, _display, &display) == 1)
			{
				*result = display.result;
				ret = 0;
			}
		}
	}
	return ret;
}

static int method_onchange(json_t *json_params, json_t **result, void *userdata)
{
	cmds_dbg("cmds: onchange");
	int ret;
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);

	*result = json_object();
	if (media != NULL && (ctx->onchangemask & ONCHANGE_SOURCE))
	{
		_display_ctx_t display = {
			.ctx = ctx,
			.result = *result,
		};
		int id = player_mediaid(ctx->player);
		ret = media->ops->find(media->ctx, id, _display, &display);
		int next = player_next(ctx->player, 0);
		json_object_set(*result, "next", json_integer(next));

		int count = media->ops->count(media->ctx);
		json_object_set(*result, "count", json_integer(count));

		const char *mediapath = media_path();
		json_object_set(*result, "media", json_string(mediapath));

		json_t *options = json_array();
		if (media->ops->loop && media->ops->loop(media->ctx, OPTION_REQUEST) == OPTION_ENABLE)
		{
			json_array_append(options, json_string("loop"));
		}
		if (media->ops->random && media->ops->random(media->ctx, OPTION_REQUEST) == OPTION_ENABLE)
		{
			json_array_append(options, json_string("random"));
		}
		json_object_set(*result, "options", options);
		ctx->onchangemask &= ~ONCHANGE_SOURCE;
		ctx->onchangemask &= ~ONCHANGE_MEDIA;
	}

	json_t *json_state;
	state_t state = player_state(ctx->player, STATE_UNKNOWN);

	switch (state)
	{
	case STATE_CHANGE:
	case STATE_PLAY:
		json_state = json_string(str_play);
	break;
	case STATE_PAUSE:
		json_state = json_string(str_pause);
	break;
	default:
		json_state = json_string(str_stop);
	}
	json_object_set(*result, "state", json_state);

	if (ctx->sink && ctx->sink->ops->getvolume != NULL && (ctx->onchangemask & ONCHANGE_VOLUME))
	{
		unsigned int volume = ctx->sink->ops->getvolume(ctx->sink->ctx);
		json_object_set(*result, "volume", json_integer(volume));
		ctx->onchangemask &= ~ONCHANGE_VOLUME;
	}
	return 0;
}

static int method_status(json_t *json_params, json_t **result, void *userdata)
{
	cmds_dbg("cmds: status");
	return method_onchange(json_params, result, userdata);
}

static int method_options(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	int ret = -1;
	media_t *media = player_media(ctx->player);
	int loopstate = -1;
	int randomstate = -1;

	if (json_is_object(json_params))
	{
		json_t *value = NULL;
		int state = -1;

		value = json_object_get(json_params, "loop");
		if (json_is_boolean(value))
		{
			state = json_boolean_value(value);
			if (media->ops->loop)
			{
				loopstate = media->ops->loop(media->ctx, state);
				ret = 0;
			}

		}
		value = json_object_get(json_params, "random");
		if (json_is_boolean(value))
		{
			state = json_boolean_value(value);
			if (media->ops->random)
			{
				randomstate = media->ops->random(media->ctx, state);
				ret = 0;
				if (randomstate == OPTION_DISABLE && media->ops->find != NULL)
				{
					int id = player_mediaid(ctx->player);
					id += 1;
					ret = player_play(ctx->player, id);
				}
			}
		}
		if (ret == 0)
		{
			*result = json_object();
			state = media->ops->loop(media->ctx, OPTION_REQUEST);
			value = json_boolean(state);
			json_object_set(*result, "loop", value);
			state = media->ops->random(media->ctx, OPTION_REQUEST);
			value = json_boolean(state);
			json_object_set(*result, "random", value);
		}
		else
		{
			*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
			return -1;
		}
	}
	return 0;
}

static int method_volume(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	int ret = -1;

	if (ctx->sink == NULL || ctx->sink->ops->getvolume == NULL)
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
		return -1;
	}
	json_t *value = NULL;
	unsigned int volume = ctx->sink->ops->getvolume(ctx->sink->ctx);
	if (json_is_object(json_params))
	{
		value = json_object_get(json_params, "level");
		if (value && json_is_integer(value))
		{
			if (ctx->sink->ops->setvolume == NULL)
			{
				*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
				return -1;
			}
			int volume = json_boolean_value(value);
			if (volume > 100)
				volume = 100;
			if (volume < 0)
				volume = 0;
			ctx->sink->ops->setvolume(ctx->sink->ctx, volume);
		}
		value = json_object_get(json_params, "step");
		if (value && json_is_integer(value))
		{
			if (ctx->sink->ops->setvolume == NULL)
			{
				*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
				return -1;
			}
			volume += json_integer_value(value);
			if (volume > 100)
				volume = 100;
			if (volume < 0)
				volume = 0;
			ctx->sink->ops->setvolume(ctx->sink->ctx, volume);
		}
	}
	*result = json_object();
	json_object_set(*result, "level", json_integer(volume));
	return 0;
}

static int method_capabilities(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	int ret;
	media_t *media = player_media(ctx->player);
	json_t *value;
	json_t *params;

	*result = json_object();
	json_t *events;
	events = json_array();

	json_t *event;
	event = json_object();
	value = json_string("onchange");
	json_object_set(event, "method", value);
	params = json_object();
	json_object_set(event, "params", params);
	json_array_append(events, event);
	json_object_set(*result, "events", events);

	json_t *actions;
	actions = json_array();

	json_t *action;

	action = json_object();
	value = json_string("change");
	json_object_set(action, "method", value);
	params = json_array();
	value = json_string("id");
	json_array_append(params, value);
	value = json_string("media");
	json_array_append(params, value);
	value = json_string("name");
	json_array_append(params, value);
	json_object_set(action, "params", params);
	json_array_append(actions, action);

	action = json_object();
	value = json_string("play");
	json_object_set(action, "method", value);
	params = json_null();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	action = json_object();
	value = json_string("pause");
	json_object_set(action, "method", value);
	params = json_null();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	action = json_object();
	value = json_string("stop");
	json_object_set(action, "method", value);
	params = json_null();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	if (media->ops->next != NULL)
	{
		action = json_object();
		value = json_string("next");
		json_object_set(action, "method", value);
		params = json_null();
		json_object_set(action, "params", params);
		json_array_append(actions, action);
	}
	action = json_object();
	value = json_string("status");
	json_object_set(action, "method", value);
	params = json_null();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	action = json_object();
	value = json_string("info");
	json_object_set(action, "method", value);
	params = json_array();
	value = json_string("id");
	json_array_append(params, value);
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	if (media->ops->list != NULL)
	{
		action = json_object();
		value = json_string("list");
		json_object_set(action, "method", value);
		params = json_array();
		value = json_string("maxitems");
		json_array_append(params, value);
		value = json_string("first");
		json_array_append(params, value);
		json_object_set(action, "params", params);
		json_array_append(actions, action);
	}
	if (media->ops->filter != NULL)
	{
		action = json_object();
		value = json_string("filter");
		json_object_set(action, "method", value);
		params = json_array();
		value = json_string("keyword");
		json_array_append(params, value);
		value = json_string("title");
		json_array_append(params, value);
		value = json_string("artist");
		json_array_append(params, value);
		value = json_string("album");
		json_array_append(params, value);
		value = json_string("genre");
		json_array_append(params, value);
		value = json_string("speed");
		json_array_append(params, value);
		json_object_set(action, "params", params);
		json_array_append(actions, action);
	}
	if (media->ops->find != NULL)
	{
		action = json_object();
		value = json_string("setnext");
		json_object_set(action, "method", value);
		params = json_array();
		value = json_string("id");
		json_array_append(params, value);
		json_object_set(action, "params", params);
		json_array_append(actions, action);
	}
	if (media->ops->insert != NULL)
	{
		action = json_object();
		value = json_string("append");
		json_object_set(action, "method", value);
		params = json_array();
		value = json_string("url");
		json_array_append(params, value);
		value = json_string("id");
		json_array_append(params, value);
		json_object_set(action, "params", params);
		json_array_append(actions, action);
	}
	if (media->ops->remove != NULL)
	{
		action = json_object();
		value = json_string("remove");
		json_object_set(action, "method", value);
		params = json_array();
		value = json_string("id");
		json_array_append(params, value);
		json_object_set(action, "params", params);
		json_array_append(actions, action);
	}
	action = NULL;
	if (media->ops->random != NULL)
	{
		if (action == NULL)
		{
			action = json_object();
			value = json_string("options");
			json_object_set(action, "method", value);
			params = json_array();
		}
		value = json_string("random");
		json_array_append(params, value);
	}
	if (media->ops->loop != NULL)
	{
		if (action == NULL)
		{
			action = json_object();
			value = json_string("options");
			json_object_set(action, "method", value);
			params = json_array();
		}
		value = json_string("loop");
		json_array_append(params, value);
	}
	if (action != NULL)
	{
		json_object_set(action, "params", params);
		json_array_append(actions, action);
	}
	if (ctx->sink && ctx->sink->ops->getvolume != NULL)
	{
		action = json_object();
		value = json_string("volume");
		json_object_set(action, "method", value);
		params = json_array();
		value = json_string("level");
		json_array_append(params, value);
		value = json_string("step");
		json_array_append(params, value);
		json_object_set(action, "params", params);
		json_array_append(actions, action);
	}
	const src_t *src = player_source(ctx->player);
	decoder_t *decoder = NULL;
	if (src != NULL)
	{
		decoder = src->ops->estream(src->ctx, 0);
	}
	if (decoder != NULL && decoder->ops->position != NULL)
	{
		action = json_object();
		value = json_string("getposition");
		json_object_set(action, "method", value);
		params = json_array();
		json_object_set(action, "params", params);
		json_array_append(actions, action);
	}
	json_object_set(*result, "actions", actions);

	json_t *input;
	input = json_object();
	json_t *codec;
	codec = json_array();
	const char *mime = decoder_mimelist(1);
	while (mime != NULL)
	{
		value = json_string(mime);
		json_array_append(codec, value);
		mime = decoder_mimelist(0);
	}
	json_object_set(input, "codec", codec);
	json_t *aprotocol;
	aprotocol = json_array();
	const char *protocol;
	char* off;
#ifdef SRC_DIR
	protocol = src_dir->protocol;
	while ((off = strchr(protocol, ',')) > 0)
	{
		value = json_stringn(protocol, off - protocol);
		protocol = off + 1;
		json_array_append(aprotocol, value);
	}
	value = json_string(protocol);
	json_array_append(aprotocol, value);
#endif
#ifdef SRC_FILE
	protocol = src_file->protocol;
	while ((off = strchr(protocol, ',')) > 0)
	{
		value = json_stringn(protocol, off - protocol);
		protocol = off + 1;
		json_array_append(aprotocol, value);
	}
	value = json_string(protocol);
	json_array_append(aprotocol, value);
#endif
#ifdef SRC_CURL
	protocol = src_curl->protocol;
	while ((off = strchr(protocol, ',')) > 0)
	{
		value = json_stringn(protocol, off - protocol);
		protocol = off + 1;
		json_array_append(aprotocol, value);
	}
	value = json_string(protocol);
	json_array_append(aprotocol, value);
#endif
#ifdef SRC_UNIX
	protocol = src_unix->protocol;
	while ((off = strchr(protocol, ',')) > 0)
	{
		value = json_stringn(protocol, off - protocol);
		protocol = off + 1;
		json_array_append(aprotocol, value);
	}
	value = json_string(protocol);
	json_array_append(aprotocol, value);
#endif
#ifdef SRC_UDP
	protocol = src_udp->protocol;
	while ((off = strchr(protocol, ',')) > 0)
	{
		value = json_stringn(protocol, off - protocol);
		protocol = off + 1;
		json_array_append(aprotocol, value);
	}
	value = json_string(protocol);
	json_array_append(aprotocol, value);
#endif
	json_object_set(input, "protocol", aprotocol);
	json_object_set(*result, "input", input);
	return 0;
}


static struct jsonrpc_method_entry_t method_table[] = {
	{ 'r', "capabilities", method_capabilities, "o" },
	{ 'r', "play", method_play, "" },
	{ 'r', "pause", method_pause, "" },
	{ 'r', "stop", method_stop, "" },
	{ 'r', "next", method_next, "" },
	{ 'r', "setnext", method_setnext, "o" },
	{ 'r', "list", method_list, "o" },
	{ 'r', "info", method_info, "o" },
	{ 'r', "setinfo", method_setinfo, "o" },
	{ 'r', "filter", method_filter, "o" },
	{ 'r', "append", method_append, "[]" },
	{ 'r', "remove", method_remove, "o" },
	{ 'r', "status", method_status, "" },
	{ 'r', "change", method_change, "o" },
	{ 'n', "onchange", method_onchange, "o" },
	{ 'r', "options", method_options, "o" },
	{ 'r', "volume", method_volume, "o" },
	{ 'r', "getposition", method_getposition, "" },
	{ 0, NULL },
};

#ifdef JSONRPC_USE_DUMPCALLBACK
static int _cmds_send(const char *buff, size_t size, void *userctx)
{
	thread_info_t *info = (thread_info_t *)userctx;
	cmds_ctx_t *ctx = info->userctx;
	int sock = info->sock;
	int ret = size;
	/**
	 * this code allow to send the message small part by small part.
	 */
	while (size > 0)
	{
		ret = send(sock, buff, size, MSG_NOSIGNAL);
		if (ret < 0)
			break;
		size -= ret;
		buff += ret;
	}
	if (ret < 0)
		err("cmd: json send error %s", strerror(errno));
	return (ret >= 0)?0:-1;
}
#endif

static void _cmds_json_removeinfo(cmds_ctx_t *ctx, thread_info_t *info)
{
	thread_info_t *it = ctx->info;
	if (ctx->info == info)
	{
		ctx->info = ctx->info->next;
	}
	else
	{
		while (it != NULL && it->next != NULL && it->next != info)
		{
			it = it->next;
		}
		if (it != NULL)
			it->next = info->next;
	}
	info->sock = -1;
}

static size_t _cmds_recv(void *buff, size_t size, void *userctx)
{
	thread_info_t *info = (thread_info_t *)userctx;
	cmds_ctx_t *ctx = info->userctx;
	int sock = info->sock;

	ssize_t ret = recv(sock,
		buff, size, MSG_PEEK | MSG_DONTWAIT | MSG_NOSIGNAL);
	dbg("cmds: receive message %ld", ret);
	if (ret <= 0)
	{
		err("cmds: json recv error %s", strerror(errno));
		pthread_mutex_lock(&ctx->mutex);
		_cmds_json_removeinfo(ctx, info);
		pthread_mutex_unlock(&ctx->mutex);
		return (size_t)ret;
	}

	ssize_t length = strlen(((char *)buff)) + 1;
	if (length < ret)
	{
		size = length;
	}
	size = recv(sock,
		buff, size, MSG_DONTWAIT | MSG_NOSIGNAL);
	((char *)buff)[size] = '\0';

	cmds_dbg("cmds: recv data %.*s", (int)size, (char *)buff);
	return size;
}

static void jsonrpc_onchange(void * userctx, event_t event, void *eventarg)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userctx;

	switch (event)
	{
		case PLAYER_EVENT_CHANGE:
			pthread_mutex_lock(&ctx->mutex);
			event_player_state_t *eventval = (event_player_state_t *)eventarg;
			if (eventval->state != STATE_CHANGE)
				ctx->eventsmask |= ONCHANGE;
			else
				ctx->onchangemask |= ONCHANGE_SOURCE;
			pthread_mutex_unlock(&ctx->mutex);
			pthread_cond_broadcast(&ctx->cond);
		break;
		case SINK_EVENT_VOLUME:
			pthread_mutex_lock(&ctx->mutex);
			ctx->onchangemask |= ONCHANGE_VOLUME;
			ctx->eventsmask |= ONCHANGE;
			pthread_mutex_unlock(&ctx->mutex);
			pthread_cond_broadcast(&ctx->cond);
		break;
	}
}

static int jsonrpc_sendevent(cmds_ctx_t *ctx, thread_info_t *info, const char *event)
{
	int ret = 0;

	json_t *notification = jsonrpc_jrequest(event, method_table, (void *)ctx, NULL);
	if (notification)
	{
		char *message = json_dumps(notification, JSONRPC_DEBUG_FORMAT);
		int length = strlen(message);
		int sock = info->sock;
		cmds_dbg("cmds: send notification %d %*s", length, length, message);
		ret = send(sock, message, length, MSG_DONTWAIT | MSG_NOSIGNAL);
		dbg("cmds: send notification %d", ret);
		fsync(sock);
		free(message);
		json_decref(notification);
	}
	else
	{
		err("cmds: unkonwn event %s", event);
	}
	if (ret < 0)
	{
		err("cmd: json send error %s", strerror(errno));
		if (errno != EPIPE)
			ret = 0;
	}
	return ret;
}

static int _jsonrpc_sendresponse(thread_info_t *info, json_t *request)
{
	int ret = 0;
	int sock = info->sock;
	cmds_ctx_t *ctx = info->userctx;

	json_t *response = jsonrpc_jresponse(request, method_table, ctx);

	if (response != NULL)
	{
		char *buff = json_dumps(response, JSONRPC_DEBUG_FORMAT );
		int length = strlen(buff);
		cmds_dbg("cmds: send response %d %*s", length, length, buff);
		ret = send(sock, buff, length, MSG_DONTWAIT | MSG_NOSIGNAL);
		dbg("cmds: send response %d", ret);
		fsync(sock);
		free(buff);
	}
	else
	{
		err("cmds: no response for %s", json_dumps(request, JSONRPC_DEBUG_FORMAT ));
	}
	json_decref(response);
	return ret;
}

/**
 * this is the main loop for the sending
 * There is only one lopp for all clients
 */
static void *_cmds_json_pthreadsend(void *arg)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)arg;

	pthread_mutex_lock(&ctx->mutex);
	ctx->run = 1;
	pthread_mutex_unlock(&ctx->mutex);
	pthread_cond_broadcast(&ctx->cond);
	while (ctx->run)
	{
		int wait = 0;
		pthread_mutex_lock(&ctx->mutex);
		while (ctx->requests == NULL && ctx->eventsmask == 0)
		{
			pthread_cond_wait(&ctx->cond, &ctx->mutex);
			cmds_dbg("cmds: send new message");
		}
		pthread_mutex_unlock(&ctx->mutex);
		while (ctx->requests != NULL)
		{
			cmds_dbg("cmds: send response");
			json_request_list_t *request = ctx->requests;
			ctx->requests = ctx->requests->next;
			int ret = -1;
			/** run only request not disabled by a previous request **/
			if (request->info != NULL)
			{
				if (wait) sched_yield();
				ret = _jsonrpc_sendresponse(request->info, request->request);
				wait = 1;
			}
			if (ret < 0)
			{
				err("cmds: send response error %d", ret);
				/**
				 * remove all requests on this client threads
				 */
				json_request_list_t *it = ctx->requests;
				for (; it != NULL; it = it->next)
				{
					if (request->info == it->info)
					{
						/** disabled requests on this client **/
						it->info = NULL;
					}
				}

				/** free the client **/
				if (request->info != NULL)
				{
					pthread_mutex_lock(&ctx->mutex);
					_cmds_json_removeinfo(ctx, request->info);
					pthread_mutex_unlock(&ctx->mutex);
				}
				/** free the request **/
				json_decref(request->request);
				free(request);
				continue;
			}
			json_decref(request->request);
			free(request);
		}
		while (ctx->eventsmask != 0)
		{
			cmds_dbg("cmds: send event");
			if ((ctx->eventsmask & ONCHANGE) == ONCHANGE)
			{
				thread_info_t *info = ctx->info;
				while (info)
				{
					thread_info_t *next = info->next;
					if (wait) sched_yield();
					int ret = jsonrpc_sendevent(ctx, info, "onchange");
					wait = 1;
					if (ret < 0)
					{
						pthread_mutex_lock(&ctx->mutex);
						err("cmds: send event error %d", ret);
						_cmds_json_removeinfo(ctx, info);
						pthread_mutex_unlock(&ctx->mutex);
					}
					info = next;
				}
				ctx->eventsmask &= ~ONCHANGE;
			}
		}
	}
	warn("cmds: leave thread send");
	return NULL;
}

/**
 * this is the main loop of the client socket
 * There is one loop for each client
 */
static int jsonrpc_command(thread_info_t *info)
{
	int ret = 0;
	int sock = info->sock;
	cmds_ctx_t *ctx = info->userctx;
	if (ctx->info == NULL)
		ctx->info = info;

	/**
	 * wait that the sending loop is ready
	 */
	pthread_mutex_lock(&ctx->mutex);
	while (ctx->run == 0)
	{
		pthread_cond_wait(&ctx->cond, &ctx->mutex);
	}
	pthread_mutex_unlock(&ctx->mutex);

	warn("cmds: json socket connection");
	event_player_state_t event = {.playerctx = ctx->player};
	event.state = player_state(ctx->player, STATE_UNKNOWN);
	jsonrpc_onchange(ctx, PLAYER_EVENT_CHANGE, &event);
	errno = 0;
	int run = 1;

	struct pollfd poll_set[1];
	memset(poll_set, 0, sizeof(poll_set));
	int numfds = 0;
	poll_set[0].fd = sock;
	poll_set[0].events = POLLIN | POLLHUP;
	numfds++;
	while (run)
	{
		if (ctx->info == NULL)
			break;

		cmds_dbg("cmds: recv wait");
		ret = poll(poll_set, numfds, -1);
		if (poll_set[0].revents & POLLHUP)
		{
			err("cmds: client hangup");
			run = 0;
			break;
		}
		cmds_dbg("cmds: recv ready");
		poll_set[0].revents = 0;

		json_t *request = NULL;
		json_error_t error;
		int flags = JSON_DISABLE_EOF_CHECK;
		//int flags = 0;
		request = json_load_callback(_cmds_recv, info, flags, &error);
		if (request != NULL)
		{
			cmds_dbg("cmds: receive request %s", json_dumps(request, JSONRPC_DEBUG_FORMAT ));
			json_request_list_t *entry = calloc(1, sizeof(*entry));
			entry->info = info;
			entry->request = request;
			pthread_mutex_lock(&ctx->mutex);
			if (ctx->requests == NULL)
				ctx->requests = entry;
			else
			{
				json_request_list_t *it = ctx->requests;
				while (it->next != NULL) it = it->next;
				it->next = entry;
			}
			pthread_mutex_unlock(&ctx->mutex);
			pthread_cond_broadcast(&ctx->cond);
		}
		else
		{
			err("cmds: json error %s", error.text);
			if (info->sock == -1)
				run = 0;
		}
	}
	pthread_mutex_lock(&ctx->mutex);
	warn("cmds: json socket %d leave", info->sock);
	_cmds_json_removeinfo(ctx, info);
	pthread_mutex_unlock(&ctx->mutex);
	unixserver_remove(info);

	return ret;
}

static cmds_ctx_t *cmds_json_init(player_ctx_t *player, void *arg)
{
	cmds_ctx_t *ctx = NULL;
	ctx = calloc(1, sizeof(*ctx));
	ctx->player = player;
	ctx->socketpath = (const char *)arg;
	pthread_cond_init(&ctx->cond, NULL);
	pthread_mutex_init(&ctx->mutex, NULL);
	ctx->onchangeid = player_eventlistener(ctx->player, jsonrpc_onchange, (void *)ctx, "jsonrpc");
	ctx->onchangemask = 0;
	return ctx;
}

static void *_cmds_json_pthreadrecv(void *arg)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)arg;

	unixserver_run(jsonrpc_command, (void *)ctx, ctx->socketpath);
	warn("cmds: leave thread recv");
	return NULL;
}

static int cmds_json_run(cmds_ctx_t *ctx, sink_t *sink)
{
	ctx->sink = sink;
	if (sink->ops->eventlistener)
		sink->ops->eventlistener(sink->ctx, jsonrpc_onchange, (void *)ctx);

	pthread_create(&ctx->threadrecv, NULL, _cmds_json_pthreadrecv, (void *)ctx);
	pthread_create(&ctx->threadsend, NULL, _cmds_json_pthreadsend, (void *)ctx);
	return 0;
}

static void cmds_json_destroy(cmds_ctx_t *ctx)
{
	if (ctx->info)
		unixserver_kill(ctx->info);
	player_removeevent(ctx->player, ctx->onchangeid);
	ctx->onchangeid = 0;
	ctx->info = NULL;
	pthread_join(ctx->threadrecv, NULL);
	ctx->run = 0;
	pthread_cond_broadcast(&ctx->cond);
	pthread_join(ctx->threadsend, NULL);
	pthread_cond_destroy(&ctx->cond);
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
}

cmds_ops_t *cmds_json = &(cmds_ops_t)
{
	.init = cmds_json_init,
	.run = cmds_json_run,
	.destroy = cmds_json_destroy,
};

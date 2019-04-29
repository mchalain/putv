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

#include <pthread.h>
#include <jansson.h>

#include "unix_server.h"
#include "player.h"
#include "jsonrpc.h"
#define DATA_LENGTH 1200
typedef struct cmds_ctx_s cmds_ctx_t;
struct cmds_ctx_s
{
	player_ctx_t *player;
	sink_t *sink;
	const char *socketpath;
	pthread_t thread;
	pthread_mutex_t mutex;
	thread_info_t *info;
	struct
	{
		char data[DATA_LENGTH];
		int length;
	} buff_snd;
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

static int _print_entry(void *arg, const char *url,
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
		if (id >= 0 &&_print_entry(object, url, info, mime) == 0)
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
	int nbitems = MAX_ITEMS;
	nbitems = (count < nbitems)? count:nbitems;

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
		entry.max = (maxitems < nbitems)?maxitems:nbitems;
	}
	else
		entry.max = nbitems;

	json_t *first = json_object_get(json_params, "first");
	if (first)
		entry.first = json_integer_value(first);
	else
		entry.first = 0;

	entry.count = 0;
	media->ops->list(media->ctx, _append_entry, (void *)&entry);
	*result = json_pack("{s:i,s:i,s:o}", "count", count, "nbitems", entry.count, "playlist", entry.list);

	return 0;
}

static int method_remove(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	int ret = -1;

	if (media->ops->remove == NULL)
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
		return -1;
	}

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
			int id = json_integer_value(value);
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
		ret = 0;
	}
	return ret;
}

static int method_append(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);

	if (media->ops->insert == NULL)
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
		return -1;
	}

	int ret = -1;
	if (json_is_array(json_params)) {
		size_t index;
		json_t *value;
		json_array_foreach(json_params, index, value)
		{
			if (json_is_string(value))
			{
				const char *str = json_string_value(value);
				ret = media->ops->insert(media->ctx, str, "", NULL);
			}
			else if (json_is_object(value))
			{
				json_t * path = json_object_get(value, "url");
				json_t * info = json_object_get(value, "info");
				json_t * mime = json_object_get(value, "mime");
				if (json_is_string(info))
				{
					ret = media->ops->insert(media->ctx,
							json_string_value(path),
							json_string_value(info),
							json_string_value(mime));
				}
				else if (json_is_object(info))
				{
					ret = media->ops->insert(media->ctx,
							json_string_value(path),
							json_dumps(info, 0),
							json_string_value(mime));
				}
			}
			if (ret == -1)
				*result = jsonrpc_error_object(-12345,
					"append error",
					json_string("media could not be inserted into the playlist"));
			else
			{
				*result = json_pack("{s:s,s:s,s:i}", "status", "DONE", "message", "media append", "id", ret);
				ret = 0;
			}
		}
	}
	else
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));

	return ret;
}

static int method_play(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);

	if (media->ops->count(media->ctx) > 0)
		player_state(ctx->player, STATE_PLAY);
	else
	{
		player_state(ctx->player, STATE_STOP);
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss,ss}", "state", str_stop, "message", "no item found"));
		return -1;
	}
	switch (player_state(ctx->player, STATE_UNKNOWN))
	{
	case STATE_STOP:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_stop));
	return -1;
	case STATE_PLAY:
		*result = json_pack("{ss}", "state", str_play);
	return 0;
	case STATE_PAUSE:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_pause));
	return -1;
	default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	return -1;
	}
	return 0;
}

static int method_pause(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;

	switch (player_state(ctx->player, STATE_PAUSE))
	{
	case STATE_STOP:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_stop));
	return -1;
	case STATE_PLAY:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_play));
	return -1;
	case STATE_PAUSE:
		*result = json_pack("{ss}", "state", str_pause);
	return 0;
	default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	return -1;
	}
	return 0;
}

static int method_stop(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	switch (player_state(ctx->player, STATE_STOP))
	{
	case STATE_STOP:
		*result = json_pack("{ss}", "state", str_stop);
	return 0;
	case STATE_PLAY:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_play));
	return -1;
	case STATE_PAUSE:
		*result = jsonrpc_error_object_predefined(JSONRPC_INTERNAL_ERROR, json_pack("{ss}", "state", str_pause));
	return -1;
	default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	return -1;
	}
}

static int method_next(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	player_next(ctx->player);
	const char *state = NULL;
	switch (player_state(ctx->player, STATE_UNKNOWN))
	{
	case STATE_STOP:
		state = str_stop;
		*result = json_pack("{ss}", "state", state);
	return 0;
	break;
	case STATE_PLAY:
	case STATE_CHANGE:
		state = str_play;
		*result = json_pack("{ss}", "state", state);
	return 0;
	break;
	case STATE_PAUSE:
		state = str_pause;
		*result = json_pack("{ss}", "state", state);
	return 0;
	break;
	default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	return -1;
	break;
	}
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

	json_t *json_state;
	state_t state = player_state(ctx->player, STATE_UNKNOWN);

	switch (state)
	{
	case STATE_PLAY:
		json_state = json_string(str_play);
	break;
	case STATE_PAUSE:
		json_state = json_string(str_pause);
	break;
	default:
		json_state = json_string(str_stop);
	}
	json_object_set(object, "state", json_state);
	_print_entry(object, url, info, mime);
	json_t *index = json_integer(id);
	json_object_set(object, "id", index);

	if (json_is_array(result))
		json_array_append_new(result, object);
	return 1;
}

static int method_change(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	_display_ctx_t display = {
		.ctx = ctx,
		.result = json_object(),
	};
	json_t *value;
	if (json_is_object(json_params))
	{
		value = json_object_get(json_params, "id");
		if (json_is_integer(value))
		{
			int id = json_integer_value(value);
			if (media->ops->find(media->ctx, id, _display, &display) == 1)
			{
				*result = display.result;
			}
		}
		value = json_object_get(json_params, "media");
		if (json_is_string(value))
		{
			const char *media = json_string_value(value);
			if (player_change(ctx->player, media, 0, 0) == 0)
			{
				*result = json_pack("{s:s,s:s}", "media", "changed", "state", str_stop);
			}
		}
	}
	if (*result == NULL);
	{
		*result = json_pack("{s:s}", "state", str_stop);
	}
	return 0;
}

static int method_onchange(json_t *json_params, json_t **result, void *userdata)
{
	int ret;
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	_display_ctx_t display = {
		.ctx = ctx,
		.result = json_object(),
	};
	int id = player_mediaid(ctx->player);
	ret = media->ops->find(media->ctx, id, _display, &display);
	if (ret == 1)
	{
		*result = display.result;
	}
	else
	{
		json_decref(display.result);
	}
	if (*result == NULL)
	{
		*result = json_pack("{s:s}", "state", str_stop);
	}

	int next = media->ops->play(media->ctx, NULL, NULL);
	json_object_set(*result, "next", json_integer(next));

	int count = media->ops->count(media->ctx);
	json_object_set(*result, "count", json_integer(count));

	const char *mediapath = media_path();
	json_object_set(*result, "media", json_string(mediapath));

	json_t *options = json_array();
	if (media->ops->loop(media->ctx, OPTION_REQUEST) == OPTION_ENABLE)
		json_array_append(options, json_string("loop"));
	if (media->ops->random(media->ctx, OPTION_REQUEST) == OPTION_ENABLE)
		json_array_append(options, json_string("random"));
	json_object_set(*result, "options", options);

	if (ctx->sink->ops->getvolume != NULL)
	{
		unsigned int volume = ctx->sink->ops->getvolume(ctx->sink->ctx);
		json_object_set(*result, "volume", json_integer(volume));
	}
	return 0;
}

static int method_status(json_t *json_params, json_t **result, void *userdata)
{
	return method_onchange(json_params, result, userdata);
}

static int method_options(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	int ret = -1;
	media_t *media = player_media(ctx->player);

	json_t *value = NULL;
	json_t *loop_value = NULL;
	json_t *random_value = NULL;
	if (json_is_object(json_params))
	{
		value = json_object_get(json_params, "loop");
		if (json_is_boolean(value))
		{
			int state = json_boolean_value(value);
			if (media->ops->loop)
			{
				media->ops->loop(media->ctx, state);
				ret = 0;
			}
			else
			{
				*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
				return -1;
			}

			loop_value = json_boolean(state);
		}
		value = json_object_get(json_params, "random");
		if (json_is_boolean(value))
		{
			int state = json_boolean_value(value);
			if (media->ops->random)
			{
				media->ops->random(media->ctx, state);
				ret = 0;
			}
			else
			{
				*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
				return -1;
			}

			random_value = json_boolean(state);
		}
		*result = json_object();
		if (loop_value != NULL)
			json_object_set(*result, "loop", loop_value);
		if (random_value != NULL)
			json_object_set(*result, "random", random_value);
	}
	return 0;
}

static int method_volume(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	int ret = -1;

	if (ctx->sink->ops->getvolume == NULL)
	{
		*result = jsonrpc_error_object(JSONRPC_INVALID_REQUEST, "Method not available", json_null());
		return -1;
	}
	json_t *value = NULL;
	if (json_is_object(json_params))
	{
		value = json_object_get(json_params, "level");
		if (json_is_integer(value))
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
		int volume = ctx->sink->ops->getvolume(ctx->sink->ctx);
		value = json_object_get(json_params, "step");
		if (json_is_integer(value))
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
		*result = json_object();
		json_object_set(*result, "level", json_integer(volume));
	}
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
	if (ctx->sink->ops->getvolume != NULL)
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
	json_object_set(*result, "actions", actions);

	json_t *input;
	input = json_object();
	json_t *codec;
	codec = json_array();
#ifdef DECODER_MAD
	value = json_string(decoder_mad->mime(NULL));
	json_array_append(codec, value);
#endif
#ifdef DECODER_FLAC
	value = json_string(decoder_flac->mime(NULL));
	json_array_append(codec, value);
#endif
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
	{ 'r', "list", method_list, "o" },
	{ 'r', "append", method_append, "[]" },
	{ 'r', "remove", method_remove, "o" },
	{ 'r', "status", method_status, "" },
	{ 'r', "change", method_change, "o" },
	{ 'n', "onchange", method_onchange, "o" },
	{ 'r', "options", method_options, "o" },
	{ 'r', "volume", method_volume, "o" },
	{ 0, NULL },
};

#ifdef JSONRPC_LARGEPACKET
static int _cmds_send(const char *buff, size_t size, void *userctx)
{
	thread_info_t *info = (thread_info_t *)userctx;
	cmds_ctx_t *ctx = info->userctx;
	int sock = info->sock;
	int ret = size;

	if (ctx->buff_snd.length + size + 1 > sizeof(ctx->buff_snd.data))
	{
		ret = send(sock, ctx->buff_snd.data, ctx->buff_snd.length, MSG_DONTWAIT | MSG_NOSIGNAL);
		cmds_dbg("send %d/%d: %s", ret, ctx->buff_snd.length, ctx->buff_snd.data);
		ctx->buff_snd.length = 0;
	}
	memcpy(ctx->buff_snd.data + ctx->buff_snd.length, buff, size);
	ctx->buff_snd.length += size;
	ctx->buff_snd.data[ctx->buff_snd.length] = 0;
	return (ret > 0)?0:-1;
}
#endif

static void jsonrpc_onchange(void * userctx, player_ctx_t *player, state_t state)
{
	thread_info_t *info = (thread_info_t *)userctx;
	cmds_ctx_t *ctx = info->userctx;

	pthread_mutex_lock(&ctx->mutex);
#ifdef JSONRPC_LARGEPACKET
	json_t *notification = jsonrpc_jrequest("onchange", method_table, (void *)ctx, NULL);
	if (notification)
	{
		json_dump_callback(notification, _cmds_send, info, JSONRPC_DEBUG_FORMAT);
		int sock = info->sock;
		int ret = send(sock, ctx->buff_snd.data, ctx->buff_snd.length + 1, MSG_DONTWAIT | MSG_NOSIGNAL);
		cmds_dbg("send %d/%d: %s", ret, ctx->buff_snd.length + 1, ctx->buff_snd.data);
		ctx->buff_snd.length = 0;
		json_decref(notification);
	}
#else
	char* notification = jsonrpc_request("onchange", sizeof("onchange"), method_table, (void *)ctx, NULL);
	if (notification)
	{
		int length = strlen(notification);
		int sock = info->sock;
		if (send(sock, notification, length, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		{
			err("cmd: json event error on send");
			//TODO remove notification from player
		}
	}
#endif
	pthread_mutex_unlock(&ctx->mutex);
}

static int jsonrpc_command(thread_info_t *info)
{
	int ret = 0;
	int sock = info->sock;
	cmds_ctx_t *ctx = info->userctx;
	ctx->info = info;

	warn("json socket connection");
	int onchangeid = player_onchange(ctx->player, jsonrpc_onchange, (void *)info);
	jsonrpc_onchange(info, ctx->player, player_state(ctx->player, STATE_UNKNOWN));

	while (sock > 0)
	{
		if (ctx->info == NULL)
			break;
		fd_set rfds;
		struct timeval timeout = {1, 0};
		int maxfd = sock;

		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		ret = select(maxfd + 1, &rfds, NULL, NULL, &timeout);
		if (ret > 0 && FD_ISSET(sock, &rfds))
		{
			char buffer[1500];
			ret = recv(sock, buffer, 1500, MSG_NOSIGNAL);
			if (ret > 0)
			{
				cmds_dbg("recv %d: %s", ret, buffer);
				json_error_t error;
				json_t *request = json_loads(buffer, 0, &error);
				if (request != NULL)
				{
					json_t *response = jsonrpc_jresponse(request, method_table, ctx);
					/**
					 * The json callback may send an event before the answer.
					 * Use lock before jresonse may generate double lock
					 */
					pthread_mutex_lock(&ctx->mutex);
					if (response != NULL)
					{
#ifdef JSONRPC_LARGEPACKET
						json_dump_callback(response, _cmds_send, info, JSONRPC_DEBUG_FORMAT);
						if (ctx->buff_snd.length > 0)
						{
							int ret = send(sock, ctx->buff_snd.data, ctx->buff_snd.length + 1, MSG_DONTWAIT | MSG_NOSIGNAL);
							cmds_dbg("send %d/%d: %s", ret, ctx->buff_snd.length + 1, ctx->buff_snd.data);
						}
						ctx->buff_snd.length = 0;
#else
						char *buff = json_dumps(response, JSONRPC_DEBUG_FORMAT );
						ret = send(sock, buff, strlen(buff), MSG_NOSIGNAL);
						ret = send(sock, "", 1, MSG_DONTWAIT | MSG_NOSIGNAL);
#endif
						json_decref(response);
					}
					json_decref(request);
					pthread_mutex_unlock(&ctx->mutex);
				}
			}
			if (ret == 0)
			{
				warn("json socket closed");
				unixserver_remove(info);
				sock = 0;
			}
		}
		if (ret < 0)
		{
			if (errno != EAGAIN)
			{
				unixserver_remove(info);
				sock = 0;
			}
		}
	}
	player_removeevent(ctx->player, onchangeid);
	return ret;
}

static cmds_ctx_t *cmds_json_init(player_ctx_t *player, sink_t *sink, void *arg)
{
	cmds_ctx_t *ctx = NULL;
	ctx = calloc(1, sizeof(*ctx));
	ctx->player = player;
	ctx->sink = sink;
	ctx->socketpath = (const char *)arg;
	pthread_mutex_init(&ctx->mutex, NULL);
	return ctx;
}

static void *_cmds_json_pthread(void *arg)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)arg;

	unixserver_run(jsonrpc_command, (void *)ctx, ctx->socketpath);

	return NULL;
}

static int cmds_json_run(cmds_ctx_t *ctx)
{
	pthread_create(&ctx->thread, NULL, _cmds_json_pthread, (void *)ctx);
	return 0;
}

static void cmds_json_destroy(cmds_ctx_t *ctx)
{
	unixserver_remove(ctx->info);
	ctx->info = NULL;
	pthread_join(ctx->thread, NULL);
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
}

cmds_ops_t *cmds_json = &(cmds_ops_t)
{
	.init = cmds_json_init,
	.run = cmds_json_run,
	.destroy = cmds_json_destroy,
};

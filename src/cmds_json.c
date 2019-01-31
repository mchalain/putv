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
typedef struct cmds_ctx_s cmds_ctx_t;
struct cmds_ctx_s
{
	player_ctx_t *player;
	const char *socketpath;
	pthread_t thread;
	pthread_mutex_t mutex;
	thread_info_t *info;
};
#define CMDS_CTX
#include "cmds.h"
#include "media.h"
#include "decoder.h"
#include "src.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

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
	int index;
} entry_t;

static int _append_entry(void *arg, int id, const char *url,
		const char *info, const char *mime)
{
	entry_t *entry = (entry_t*)arg;

	entry->index++;
	if ((entry->index > entry->first) && (entry->index <= (entry->first + entry->max)))
	{
		json_t *object = json_object();
		if (_print_entry(object, url, info, mime) == 0)
		{
			json_t *index = json_integer(id);
			json_object_set(object, "id", index);
			json_array_append_new(entry->list, object);
		}
		else
			json_decref(object);
	}
	if (entry->index > (entry->first + entry->max))
	{
		return -1;
	}
	return 0;
}

static int method_list(json_t *json_params, json_t **result, void *userdata)
{
	int ret;
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;

	media_t *media = player_media(ctx->player);
	int count = media->ops->count(media->ctx);
	int nbitems = MAX_ITEMS;
	nbitems = (count < nbitems)? count:nbitems;

	entry_t entry;
	entry.list = json_array();

	json_t *maxitems = json_object_get(json_params, "maxitems");
	if (maxitems)
		entry.max = (json_integer_value(maxitems) < nbitems)?json_integer_value(maxitems):nbitems;
	else
		entry.max = nbitems;

	json_t *first = json_object_get(json_params, "first");
	if (first)
		entry.first = json_integer_value(first);
	else
		entry.first = 0;

	entry.index = 0;
	media->ops->list(media->ctx, _append_entry, (void *)&entry);
	*result = json_pack("{s:i,s:i,s:o}", "count", count, "nbitems", nbitems, "playlist", entry.list);

	return 0;
}

static int method_remove(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	int ret;

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
		value = json_object_get(value, "id");
		if (json_is_integer(value))
		{
			int id = json_integer_value(value);
			ret = media->ops->remove(media->ctx, id, NULL);
		}
		value = json_object_get(value, "url");
		if (json_is_string(value))
		{
			const char *str = json_string_value(value);
			ret = media->ops->remove(media->ctx, 0, str);
		}
	}
	if (ret == -1)
		*result = jsonrpc_error_object(-12345,
			"append error",
			json_string("media could not be insert into the playlist"));
	else
		*result = json_pack("{s:s,s:s}", "status", "DONE", "message", "media append");
	return 0;
}

static int method_append(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);

	if (json_is_array(json_params)) {
		int ret;
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
				ret = media->ops->insert(media->ctx,
						json_string_value(path),
						json_string_value(info),
						json_string_value(mime));
			}
			if (ret == -1)
				*result = jsonrpc_error_object(-12345,
					"append error",
					json_string("media could not be insert into the playlist"));
			else
				*result = json_pack("{s:s,s:s}", "status", "DONE", "message", "media append");
		}
	}
	return 0;
}

static int method_play(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);

	if (media->ops->count(media->ctx) > 0)
		player_state(ctx->player, STATE_PLAY);
	else
		player_state(ctx->player, STATE_STOP);
	switch (player_state(ctx->player, STATE_UNKNOWN))
	{
	case STATE_STOP:
		*result = json_pack("{ss}", "state", str_stop);
	return -1;
	case STATE_PLAY:
		*result = json_pack("{ss}", "state", str_play);
	return 0;
	case STATE_PAUSE:
		*result = json_pack("{ss}", "state", str_pause);
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

	if (player_state(ctx->player, 0) == STATE_PLAY)
	{
		switch (player_state(ctx->player, STATE_PAUSE))
		{
		case STATE_STOP:
			*result = json_pack("{ss}", "state", str_stop);
		return -1;
		case STATE_PLAY:
			*result = json_pack("{ss}", "state", str_play);
		return -1;
		case STATE_PAUSE:
			*result = json_pack("{ss}", "state", str_pause);
		return 0;
		default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
		return -1;
		}
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
		*result = json_pack("{ss}", "state", str_play);
	return -1;
	case STATE_PAUSE:
		*result = json_pack("{ss}", "state", str_pause);
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
	break;
	case STATE_PLAY:
	case STATE_CHANGE:
		state = str_play;
		*result = json_pack("{ss}", "state", state);
	break;
	case STATE_PAUSE:
		state = str_pause;
		*result = json_pack("{ss}", "state", state);
	break;
	default:
		*result = jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, json_string("player state error"));
	break;
	}
	return 0;
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
	if (*result == NULL)
	{
		*result = json_pack("{s:s}", "state", str_stop);
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
	int ret;
	media_t *media = player_media(ctx->player);

	*result = json_object();
	json_t *value;
	if (json_is_object(json_params))
	{
		value = json_object_get(json_params, "loop");
		if (json_is_boolean(value))
		{
			int state = json_boolean_value(value);
			ret = media->ops->options(media->ctx, MEDIA_LOOP, state);
			value = json_boolean(ret);
			json_object_set(*result, "loop", value);
		}
		value = json_object_get(json_params, "random");
		if (json_is_boolean(value))
		{
			int state = json_boolean_value(value);
			ret = media->ops->options(media->ctx, MEDIA_RANDOM, state);
			value = json_boolean(ret);
			json_object_set(*result, "random", value);
		}
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
	value = json_string("play");
	json_object_set(action, "method", value);
	params = json_object();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	action = json_object();
	value = json_string("pause");
	json_object_set(action, "method", value);
	params = json_object();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	action = json_object();
	value = json_string("stop");
	json_object_set(action, "method", value);
	params = json_object();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	action = json_object();
	value = json_string("next");
	json_object_set(action, "method", value);
	params = json_object();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	action = json_object();
	value = json_string("status");
	json_object_set(action, "method", value);
	params = json_object();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	value = json_string("list");
	json_object_set(action, "method", value);
	params = json_object();
	json_object_set(action, "params", params);
	json_array_append(actions, action);
	json_object_set(*result, "actions", actions);

	json_t *input;
	input = json_object();
	json_t *codec;
	codec = json_array();
#ifdef DECODER_MAD
	value = json_string(decoder_mad->mime);
	json_array_append(codec, value);
#endif
#ifdef DECODER_FLAC
	value = json_string(decoder_flac->mime);
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
	{ 'r', "remove", method_remove, "[]" },
	{ 'r', "status", method_status, "" },
	{ 'n', "onchange", method_onchange, "o" },
	{ 'r', "options", method_options, "o" },
	{ 0, NULL },
};

#ifdef JSONRPC_LARGEPACKET
static int _cmds_send(const char *buff, size_t size, void *userctx)
{
	thread_info_t *info = (thread_info_t *)userctx;
	cmds_ctx_t *ctx = info->userctx;
	int sock = info->sock;
	int ret;
	ret = send(sock, buff, size, MSG_NOSIGNAL);
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
		send(info->sock, "\r\n", 2, MSG_DONTWAIT | MSG_NOSIGNAL);
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

	player_onchange(ctx->player, jsonrpc_onchange, (void *)info);
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
				json_error_t error;
				json_t *request = json_loads(buffer, 0, &error);
				if (request != NULL)
				{
					json_t *response = jsonrpc_jresponse(request, method_table, ctx);
					if (response != NULL)
					{
#ifdef JSONRPC_LARGEPACKET
						pthread_mutex_lock(&ctx->mutex);
						json_dump_callback(response, _cmds_send, info, JSONRPC_DEBUG_FORMAT);
						ret = send(sock, "\r\n", 2, MSG_DONTWAIT | MSG_NOSIGNAL);
						pthread_mutex_unlock(&ctx->mutex);
#else
						pthread_mutex_lock(&ctx->mutex);
						char *buff = json_dumps(response, JSONRPC_DEBUG_FORMAT );
						ret = send(sock, buff, strlen(buff) + 1, MSG_NOSIGNAL);
						ret = send(sock, "\r\n", 2, MSG_DONTWAIT | MSG_NOSIGNAL);
						pthread_mutex_unlock(&ctx->mutex);
#endif
						json_decref(response);
					}
					json_decref(request);
				}
			}
			if (ret == 0)
			{
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
	return ret;
}

static cmds_ctx_t *cmds_json_init(player_ctx_t *player, void *arg)
{
	cmds_ctx_t *ctx = NULL;
	ctx = calloc(1, sizeof(*ctx));
	ctx->player = player;
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

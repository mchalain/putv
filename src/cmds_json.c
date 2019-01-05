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
	thread_info_t *info;
};
#define CMDS_CTX
#include "cmds.h"
#include "media.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static struct jsonrpc_method_entry_t method_table[];

static const char const *str_stop = "stop";
static const char const *str_play = "play";
static const char const *str_pause = "pause";

static int _print_entry(void *arg, const char *url,
		const char *info, const char *mime)
{
	if (url == NULL)
		return -1;

	json_t *object = (json_t*)arg;
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

	json_t *json_info;
	if (info != NULL)
	{
		json_error_t error;
		json_info = json_loads(info, 0, &error);
		json_object_set(object, "info", json_info);
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
	if (player_state(ctx->player, STATE_UNKNOWN) == STATE_PLAY)
		*result = json_pack("{s:s,s:s}", "status", "DONE", "message", "media append");
	else
		*result = jsonrpc_error_object(-12345,
					"play error",
					json_string("empty playlist"));
	return 0;
}

static int method_pause(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;

	if (player_state(ctx->player, 0) == STATE_PLAY)
		player_state(ctx->player, STATE_PAUSE);
	return 0;
}

static int method_stop(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	player_state(ctx->player, STATE_STOP);
	return 0;
}

static int method_next(json_t *json_params, json_t **result, void *userdata)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userdata;
	media_t *media = player_media(ctx->player);
	media->ops->next(media->ctx);
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
	return 0;
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
			char *media = json_string_value(value);
			if (player_change(ctx->player, media, 0, 0) == 0)
			{
				*result = json_pack("{s:s}", "media", "changed");
			}
		}
	}
	if (*result == NULL);
	{
		*result = json_pack("{s:s}", "state", "stop");
	}
	return 0;
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


static struct jsonrpc_method_entry_t method_table[] = {
	{ 'r', "play", method_play, "" },
	{ 'r', "pause", method_pause, "" },
	{ 'r', "stop", method_stop, "" },
	{ 'r', "next", method_next, "" },
	{ 'r', "list", method_list, "o" },
	{ 'r', "append", method_append, "[]" },
	{ 'r', "remove", method_remove, "[]" },
	{ 'n', "change", method_change, "o" },
	{ 'r', "options", method_options, "o" },
	{ 0, NULL },
};

static void jsonrpc_onchange(void * userctx, player_ctx_t *ctx, state_t state)
{
	thread_info_t *info = (thread_info_t *)userctx;

	char* notification = jsonrpc_request("change", sizeof("change"), method_table, (void *)ctx, NULL);
	int length = strlen(notification);
	int sock = info->sock;
	if (send(sock, notification, length, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
	{
		//TODO remove notification from player
	}
}

struct _cmds_send_s
{
	int sock;
};
static int _cmds_send(const char *buff, size_t size, void *arg)
{
	struct _cmds_send_s *data = (struct _cmds_send_s *)arg;
	int sock = data->sock;
	int ret;
	ret = send(sock, buff, size, MSG_DONTWAIT | MSG_NOSIGNAL);
	return (ret == size)?0:-1;
}

static int jsonrpc_command(thread_info_t *info)
{
	int ret = 0;
	int sock = info->sock;
	cmds_ctx_t *ctx = info->userctx;
	ctx->info = info;
	player_onchange(ctx->player, jsonrpc_onchange, (void *)info);

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
				json_t *request = json_loadb(buffer, ret, 0, &error);
				if (request != NULL)
				{
					json_t *response = jsonrpc_jresponse(request, method_table, ctx);
					if (response != NULL)
					{
						struct _cmds_send_s data = {sock = sock};
						json_dump_callback(response, _cmds_send, &data, JSON_INDENT(2));
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
	free(ctx);
}

cmds_ops_t *cmds_json = &(cmds_ops_t)
{
	.init = cmds_json_init,
	.run = cmds_json_run,
	.destroy = cmds_json_destroy,
};

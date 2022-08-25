/*****************************************************************************
 * input.c
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
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include <pthread.h>
#include <signal.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef USE_INOTIFY
#include <sys/inotify.h>
#endif

#include <libgen.h>
#include <jansson.h>
#include <linux/input.h>
#ifdef USE_LIBINPUT
#include <libinput.h>
#endif

#include "client_json.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

typedef void *(*__start_routine_t) (void *);

typedef struct ctx_s ctx_t;
struct ctx_s
{
	const char *root;
	const char *name;
	const char *cmdline_path;
	json_t *media;
	int media_id;
	int inputfd;
	char *socketpath;
	client_data_t *client;
	char run;
	enum
	{
		STATE_UNKNOWN,
		STATE_PLAY,
		STATE_PAUSE,
		STATE_STOP,
	} state;
};

typedef int (*method_t)(ctx_t *ctx, const char *arg);

static int method_append(ctx_t *ctx, const char *arg);
static int method_update(ctx_t *ctx, const char *arg);
static int method_remove(ctx_t *ctx, const char *arg);
static int method_list(ctx_t *ctx, const char *arg);
static int method_filter(ctx_t *ctx, const char *arg);
static int method_media(ctx_t *ctx, const char *arg);
static int method_export(ctx_t *ctx, const char *arg);
static int method_import(ctx_t *ctx, const char *arg);
static int method_search(ctx_t *ctx, const char *arg);
static int method_info(ctx_t *ctx, const char *arg);
static int method_play(ctx_t *ctx, const char *arg);
static int method_pause(ctx_t *ctx, const char *arg);
static int method_stop(ctx_t *ctx, const char *arg);
static int method_next(ctx_t *ctx, const char *arg);
static int method_volume(ctx_t *ctx, const char *arg);
static int method_repeat(ctx_t *ctx, const char *arg);
static int method_shuffle(ctx_t *ctx, const char *arg);
static int method_wait(ctx_t *ctx, const char *arg);
static int method_quit(ctx_t *ctx, const char *arg);
static int method_help(ctx_t *ctx, const char *arg);

struct cmd_s {
	const char shortkey;
	const char *name;
	const char *help;
	method_t method;
};
static const struct cmd_s cmds[] = {{
		.shortkey = 0,
		.name = "append",
		.method = method_append,
		.help = "add an opus into the media\n" \
			"        <json media> {\"sources\":[{\"url\":\"https://example.com/stream.mp3\"}],\"info\":{\"title\": \"test\",\"artist\":\"John Doe\",\"album\":\"white\"}}",
	},{
		.shortkey = 0,
		.name = "update",
		.method = method_update,
		.help = "",
	},{
		.shortkey = 0,
		.name = "remove",
		.method = method_remove,
		.help = "",
	},{
		.shortkey = 0,
		.name = "list",
		.method = method_list,
		.help = "display the opus from the media\n"\
			"        <first opus id> <max number of opus>",
	},{
		.shortkey = 0,
		.name = "filter",
		.method = method_filter,
		.help = "",
	},{
		.shortkey = 0,
		.name = "media",
		.method = method_media,
		.help = "request to change the media\n" \
			"        <media url>",
	},{
		.shortkey = 0,
		.name = "export",
		.method = method_export,
		.help = "export the opus from the media into a json file\n" \
			"        <file path>",
	},{
		.shortkey = 0,
		.name = "import",
		.method = method_import,
		.help = "import opus from a json file into the media\n" \
			"        <file path>",
	},{
		.shortkey = 0,
		.name = "search",
		.method = method_search,
		.help = "",
	},{
		.shortkey = 0,
		.name = "info",
		.method = method_info,
		.help = "display an opus from the media\n" \
			"        <opus id>",
	},{
		.shortkey = 'p',
		.name = "play",
		.method = method_play,
		.help = "start the stream\n" \
			"        [media id]",
	},{
		.shortkey = 0,
		.name = "pause",
		.method = method_pause,
		.help = "suspend the stream",
	},{
		.shortkey = 0,
		.name = "stop",
		.method = method_stop,
		.help = "stop the stream",
	},{
		.shortkey = 'n',
		.name = "next",
		.method = method_next,
		.help = "request the next opus",
	},{
		.shortkey = 0,
		.name = "volume",
		.method = method_volume,
		.help = "request to change the level of volume\n" \
			"        <0..100>",
	},{
		.shortkey = 0,
		.name = "repeat",
		.method = method_repeat,
		.help = "change the repeat mode\n" \
			"        <on|off>",
	},{
		.shortkey = 0,
		.name = "shuffle",
		.method = method_shuffle,
		.help = "change the shuffle mode\n" \
			"        <on|off>",
	},{
		.shortkey = 0,
		.name = "wait",
		.method = method_wait,
		.help = "ait a number of media changing\n        <0..100>",
	},{
		.shortkey = 0,
		.name = "quit",
		.method = method_quit,
		.help = "quit the command line application",
	},{
		.shortkey = 'h',
		.name = "help",
		.method = method_help,
		.help = "display this help",
	}, {
		.shortkey = 0,
		.name = NULL,
		.method = NULL,
		.help = NULL,
	}
};

int cmdline_checkstate(void *data, json_t *params)
{
	ctx_t *ctx = (ctx_t *)data;
	const char *state;
	json_unpack(params, "{ss}", "state", &state);
	if (!strcmp(state, "play"))
		ctx->state = STATE_PLAY;
	else if (!strcmp(state, "pause"))
		ctx->state = STATE_PAUSE;
	else if (!strcmp(state, "stop"))
		ctx->state = STATE_STOP;
	else
		ctx->state = STATE_UNKNOWN;
	return 0;
}

static int method_next(ctx_t *ctx, const char *arg)
{
	return client_next(ctx->client, cmdline_checkstate, ctx);
}

static int method_play(ctx_t *ctx, const char *arg)
{
	int ret = 0;
	int id = -1;
	if (arg)
		ret = sscanf(arg, "%d", &id);
	if (ret == 0)
		return client_play(ctx->client, cmdline_checkstate, ctx);
	else
	{
		return client_setnext(ctx->client, (client_event_prototype_t)method_next, ctx, id);
	}
	return -1;
}

static int method_pause(ctx_t *ctx, const char *arg)
{
	return client_pause(ctx->client, cmdline_checkstate, ctx);
}

static int method_stop(ctx_t *ctx, const char *arg)
{
	return client_stop(ctx->client, cmdline_checkstate, ctx);
}

static int method_volume(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	unsigned int volume = atoi(arg);
	if (volume != -1)
	{
		ret = client_volume(ctx->client, NULL, ctx, volume);
	}
	return ret;
}

static int method_media(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	json_error_t error;
	json_t *media = json_loads(arg, 0, &error);
	ret = media_change(ctx->client, NULL, ctx, media);
	return ret;
}

static int method_repeat(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	if (! strcmp(arg, "on"))
		ret = media_options(ctx->client, NULL, ctx, -1, 1);
	else
		ret = media_options(ctx->client, NULL, ctx, -1, 0);
	return ret;
}

static int method_shuffle(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	if (! strcmp(arg, "on"))
		ret = media_options(ctx->client, NULL, ctx, 1, -1);
	else
		ret = media_options(ctx->client, NULL, ctx, 0, -1);
	return ret;
}

static int method_append(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	json_error_t error;
	json_t *media = json_loads(arg, 0, &error);
	if (media != NULL)
	{
		json_t *params;
		params = json_array();
		json_array_append(params, media);
		ret = media_insert(ctx->client, NULL, ctx, params);
	}
	return ret;
}

static int method_update(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	int id;
	char *info = NULL;
	if (arg)
		ret = sscanf(arg, "%d %1024mc", &id, &info);
	if (ret == 2 && info)
	{
		json_error_t error;
		json_t *jinfo = json_loads(info, 0, &error);
		ret = media_setinfo(ctx->client, NULL, ctx, id, jinfo);
	}
	return ret;
}

static int method_remove(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	int id;
	json_t *params = json_object();
	if (arg)
		ret = sscanf(arg, "%d", &id);
	if (ret == 1)
		json_object_set(params, "id", json_integer(id));
	ret = media_remove(ctx->client, NULL, ctx, params);
	return ret;
}

static int display_info(ctx_t *ctx, json_t *info)
{
	const unsigned char *key = NULL;
	json_t *value;
	json_object_foreach(info, key, value)
	{
		if (json_is_string(value))
		{
			const char *string = json_string_value(value);
			if (string != NULL)
			{
				fprintf(stdout, "  %s: %s\n", key, string);
			}
		}
		if (json_is_integer(value))
		{
			fprintf(stdout, "  %s: %lld\n", key, json_integer_value(value));
		}
		if (json_is_null(value))
		{
			fprintf(stdout, "  %s: empty\n", key);
		}
	}

	return 0;
}

static int display_media(ctx_t *ctx, json_t *media)
{
	json_t *info = json_object_get(media, "info");
	int id = json_integer_value(json_object_get(media, "id"));
	fprintf(stdout, "media: %d\n", id);
	display_info(ctx, info);
	return 0;
}

static int display_list(ctx_t *ctx, json_t *params)
{
	const unsigned char *key = NULL;
	json_t *value;
	int count = json_integer_value(json_object_get(params, "count"));
	int nbitems = json_integer_value(json_object_get(params, "nbitems"));
	fprintf(stdout, "nb media: %d\n", count);
	json_t *playlist = json_object_get(params, "playlist");
	int i;
	json_t *media;
	json_array_foreach(playlist, i, media)
	{
		display_media(ctx, media);
	}
	return 0;
}

static int method_list(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	int first = 0;
	int max = 5;
	if (arg)
		ret = sscanf(arg, "%d %d", &first, &max);
	if (ret >= 0)
		ret = media_list(ctx->client, (client_event_prototype_t)display_list, ctx, first, max);
	else
		fprintf(stdout, "error on parameter %s\n", strerror(errno));
	return ret;
}

static int method_filter(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	return ret;
}

struct export_list_s
{
	ctx_t *ctx;
	FILE *outfile;
	signed long nbitems;
};

static int export_file(void *arg, json_t *list)
{
	int ret = -1;
	struct export_list_s *data = (struct export_list_s *) arg;
	ctx_t *ctx = data->ctx;

	if (data->nbitems == -1)
	{
		data->nbitems = json_integer_value(json_object_get(list, "count"));
	}
	json_t *playlist = json_object_get(list, "playlist");
	int nbitems = json_integer_value(json_object_get(list, "nbitems"));
	//warn("list size %ld %d", json_array_size(playlist), nbitems);
	if (nbitems == 0)
	{
		data->nbitems = 0;
		return 0;
	}
	int index;
	json_t *item;
	json_array_foreach(playlist, index, item)
	{
		ret = json_dumpf(item, data->outfile, JSON_INDENT(2));
		if (nbitems > index + 1)
			fprintf(data->outfile, ",");
	}
	fflush(data->outfile);
	data->nbitems -= nbitems;
	return ret;
}

#define PLAYLIST_CHUNK 20
static int method_export(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	static struct export_list_s data;
	data.ctx = ctx;
	data.outfile = fopen(arg, "w");
	if (data.outfile == NULL)
	{
		fprintf(stdout, "error on file %s\n", strerror(errno));
		return -1;
	}
	fprintf(data.outfile, "[");
	data.nbitems = -1;
	client_async(ctx->client, 0);
	int index = 0;
	do
	{
		ret = media_list(ctx->client, export_file, &data, index, PLAYLIST_CHUNK);
		if (ret < 0)
			break;
		index += PLAYLIST_CHUNK;
	}
	while (data.nbitems > 0);
	fprintf(data.outfile, "]");
	fclose(data.outfile);
	client_async(ctx->client, 1);
	return ret;
}

static int method_import(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	json_error_t error;
	json_t *media = json_load_file(arg, 0, &error);
	if (media != NULL)
	{
		json_t *params;
		if (json_is_array(media))
			params = media;
		else
		{
			params = json_array();
			json_array_append(params, media);
		}
		ret = media_insert(ctx->client, NULL, ctx, params);
	}
	json_decref(media);
	return ret;
}

static int method_search(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	return ret;
}

static int method_info(ctx_t *ctx, const char *arg)
{
	int ret = -1;
	int id;
	if (arg)
		ret = sscanf(arg, "%d", &id);
	if (ret == 1)
		ret = media_info(ctx->client, (client_event_prototype_t)display_media, ctx, id);
	return ret;
}

static int method_quit(ctx_t *ctx, const char *arg)
{
	client_disconnect(ctx->client);
	ctx->run = 0;
	return 0;
}

static int method_wait(ctx_t *ctx, const char *arg)
{
	int nb = atoi(arg);
	media_wait(ctx->client,nb);
	return 0;
}

static int method_help(ctx_t *ctx, const char *arg)
{
	fprintf(stdout, "putv commands:\n");
	for (int i = 0; cmds[i].name != NULL; i++)
	{
		fprintf(stdout, " %s : %s\n", cmds[i].name, cmds[i].help);
	}
	return 0;
}

int printevent(ctx_t *ctx, json_t *json_params)
{
	char *state;
	int id;
	json_t *info = json_object();

	json_unpack(json_params, "{ss,si,so}", "state", &state,"id", &id, "info", &info);
	fprintf(stdout, "\n%s\n", state);
	display_info(ctx, info);
	fprintf(stdout, "> ");
	fflush(stdout);
	return 0;
}

int run_shell(ctx_t *ctx)
{
	int fd = 0;
	ctx->run = 1;
	while (ctx->run)
	{
		int ret;
		fd_set rfds;
		struct timeval timeout = {1, 0};
		struct timeval *ptimeout = NULL;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		int maxfd = fd;
		fprintf (stdout, "> ");
		fflush(stdout);
		ret = select(maxfd + 1, &rfds, NULL, NULL, ptimeout);
		char buffer[1024];
		if (ret > 0 && FD_ISSET(fd, &rfds))
		{
			int length;
			int start;
			ret = ioctl(fd, FIONREAD, &length);
			if (length > sizeof(buffer))
			{
				err("string too long");
				continue;
			}
			ret = read(fd, buffer, length);
			if (ret <= 0)
			{
				ctx->run = 0;
				continue;
			}
			method_t method = NULL;
			for (int i = 0; cmds[i].name != NULL; i++)
			{
				start = strlen(cmds[i].name);
				if (!strncmp(buffer, cmds[i].name, start))
				{
					method = cmds[i].method;
					break;
				}
			}
			for (int i = 0; cmds[i].name != NULL; i++)
			{
				if ( cmds[i].shortkey == 0)
					continue;
				if (buffer[0] == cmds[i].shortkey && buffer[1] == '\n')
				{
					method = cmds[i].method;
					start = 1;
					break;
				}
			}
			const char *arg = NULL;
			for (int i  = start; i < length; i++)
			{
				if (buffer[i] == ' ' || buffer[i] == '\t')
					continue;
				if (buffer[i] == '\n')
					break;
				if (method != NULL)
				{
					arg = buffer + i;
					break;
				}
			}
			if (method)
			{
				char *end = NULL;
				if (arg)
					end = strchr(arg, '\n');
				if (end != NULL)
					*end = '\0';
				ret = method(ctx, arg);
				if (ret < 0)
					ctx->run = 0;
			}
			else
				fprintf(stdout, " command not found\n");
		}
	}
	return 0;
}

int run_client(void *arg)
{
	ctx_t *ctx = (ctx_t *)arg;

	client_data_t data = {0};
	client_unix(ctx->socketpath, &data);
	client_async(&data, 1);
	ctx->client = &data;

	client_eventlistener(&data, "onchange", (client_event_prototype_t)printevent, ctx);
	client_loop(&data);

	client_disconnect(&data);
	return 0;
}

#ifdef USE_INOTIFY
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

static void *_check_socket(void *arg)
{
	ctx_t *ctx = (ctx_t *)arg;
	int inotifyfd;

	inotifyfd = inotify_init();
	int dirfd = inotify_add_watch(inotifyfd, ctx->root,
					IN_MODIFY | IN_CREATE | IN_DELETE);
	int run = 1;
	while (run)
	{
		if (!access(ctx->socketpath, R_OK | W_OK))
		{
			run_client((void *)ctx);
		}

		char buffer[BUF_LEN];
		int length;
		length = read(inotifyfd, buffer, BUF_LEN);

		if (length < 0)
		{
			err("read");
			run = 0;
			continue;
		}

		int i = 0;
		while (i < length)
		{
			struct inotify_event *event =
				(struct inotify_event *) &buffer[i];
			if (event->len)
			{
				if (event->mask & IN_CREATE)
				{
					sleep(1);
				}
#if 0
				else if (event->mask & IN_DELETE)
				{
				}
				else if (event->mask & IN_MODIFY)
				{
					dbg("The file %s was modified.", event->name);
				}
#endif
			}
			i += EVENT_SIZE + event->len;
		}
	}
	close(inotifyfd);
}
#endif

int main(int argc, char **argv)
{
	int mode = 0;
	ctx_t data = {
		.root = "/tmp",
		.name = "putv",
	};
	const char *media_path;

	int opt;
	do
	{
		opt = getopt(argc, argv, "R:n:m:h");
		switch (opt)
		{
			case 'R':
				data.root = optarg;
			break;
			case 'n':
				data.name = optarg;
			break;
			case 'm':
				media_path = optarg;
			break;
			case 'h':
				fprintf(stderr, "cmdline -R <dir> -n <socketname> [-m <jsonfile>]");
				fprintf(stderr, "cmdline for putv applications\n");
				fprintf(stderr, " -R <DIR>   change the socket directory directory");
				fprintf(stderr, " -n <NAME>  change the socket name");
				fprintf(stderr, " -m <FILE>  load Json file to mqnqge media");
				return -1;
			break;
		}
	} while(opt != -1);

	json_error_t error;
	json_t *media;
	media = json_load_file(media_path, 0, &error);
	if (json_is_object(media))
	{
		media = json_object_get(media, "media");
	}
	if (! json_is_array(data.media))
	{
		json_decref(data.media);
		data.media = NULL;
	}
	data.media = media;
	data.socketpath = malloc(strlen(data.root) + 1 + strlen(data.name) + 1);
	sprintf(data.socketpath, "%s/%s", data.root, data.name);

	pthread_t thread;
#ifdef USE_INOTIFY
	pthread_create(&thread, NULL, (__start_routine_t)_check_socket, (void *)&data);
#else
	pthread_create(&thread, NULL, (__start_routine_t)run_client, (void *)&data);
#endif
	run_shell(&data);

	pthread_join(thread, NULL);

	free(data.socketpath);
	json_decref(data.media);
	return 0;
}

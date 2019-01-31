/*****************************************************************************
 * main.c
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

#include <libgen.h>
#include <jansson.h>

#include <pthread.h>

#ifdef USE_INOTIFY
#include <sys/inotify.h>
#endif

#include "client_json.h"
#include "../version.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif


typedef int (*print_t)(void *data, char *string);

typedef struct display_ctx_s display_ctx_t;
struct display_ctx_s
{
	int inotifyfd;
	int dirfd;
	int linelength;
	print_t print;
	void *print_data;
	const char *root;
	const char *name;
	char run;
};

int display_string(void *arg, char *string)
{
	display_ctx_t *data = (display_ctx_t *)arg;
	int length = strlen(string);
	int indent = (data->linelength - length) / 2;
	if (indent < 0)
	{
		char *cut = strstr(string," ");
		if (cut != NULL)
		{
			*cut = 0;
			cut++;
			display_string(data, string);
		}
		else
		{
			string[data->linelength] = 0;
			printf("%s\n", string);
		}
	}
	else
	{
		printf("%*s%s\n",indent,"",string);
	}
	return 0;
}

int display_default(void *eventdata, json_t *json_params)
{
	display_ctx_t *data = (display_ctx_t *)eventdata;
	char *state;
	int id;
	json_t *info = json_object();

	json_unpack(json_params, "{ss,si,so}", "state", &state,"id", &id, "info", &info);
	data->print(data->print_data,  state);

	const char *title = NULL;
	const char *album = NULL;
	const char *artist = NULL;
	json_unpack(info, "{ss,ss,ss}", "Album", &album,"Title", &title, "Artist", &artist);
	if (artist != NULL)
	{
		char *string = strdup(artist);
		data->print(data->print_data, string);
		free(string);
	}
	if (title != NULL)
	{
		char *string = strdup(title);
		data->print(data->print_data, string);
		free(string);
	}
	if (album != NULL)
	{
		char *string = strdup(album);
		data->print(data->print_data, string);
		free(string);
	}
	return 0;
}

int run_client(char * socketpath, display_ctx_t *display_data)
{
	client_event_prototype_t display = display_default;

	client_data_t data = {0};
	client_unix(socketpath, &data);
	client_eventlistener(&data, "onchange", display, display_data);
	int ret = client_loop(&data);
	unlink(socketpath);
	return ret;
}

#ifdef USE_INOTIFY
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

static void *_check_socket(void *arg)
{
	display_ctx_t *ctx = (display_ctx_t *)arg;
	while (ctx->run)
	{
		char buffer[BUF_LEN];
		int i = 0;
		int length = read(ctx->inotifyfd, buffer, BUF_LEN);

		if (length < 0)
		{
			err("read");
		}

		while (i < length)
		{
			struct inotify_event *event =
				(struct inotify_event *) &buffer[i];
			if (event->len)
			{
				if (event->mask & IN_CREATE)
				{
					char *socketpath = malloc(strlen(ctx->root) + 1 + strlen(ctx->name) + 1);
					sprintf(socketpath, "%s/%s", ctx->root, ctx->name);

					if (!access(socketpath, R_OK | W_OK))
					{
						run_client(socketpath, ctx);
					}
					free(socketpath);
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
}
#endif

#define DAEMONIZE 0x01
int main(int argc, char **argv)
{
	int mode = 0;
	display_ctx_t display_data = {
		.root = "/tmp",
		.name = basename(argv[0]),
		.linelength = 40,
		.print = display_string,
		.print_data = &display_data,
	};
	
	int opt;
	do
	{
		opt = getopt(argc, argv, "R:n:hD");
		switch (opt)
		{
			case 'R':
				display_data.root = optarg;
			break;
			case 'n':
				display_data.name = optarg;
			break;
			case 'h':
				return -1;
			break;
			case 'D':
				mode |= DAEMONIZE;
			break;
		}
	} while(opt != -1);

	if ((mode & DAEMONIZE) && fork() != 0)
	{
		return 0;
	}

#ifdef USE_INOTIFY
	display_data.inotifyfd = inotify_init();
	int dirfd = inotify_add_watch(display_data.inotifyfd, display_data.root,
					IN_MODIFY | IN_CREATE | IN_DELETE);
	display_data.run = 1;
	_check_socket((void *)&display_data);
#else
	char *socketpath = malloc(strlen(display_data.root) + 1 + strlen(display_data.name) + 1);
	sprintf(socketpath, "%s/%s", display_data.root, display_data.name);

	run_client(socketpath, &display_data);
	free(socketpath);
#endif
	return 0;
}

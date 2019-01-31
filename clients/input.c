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
#include <fcntl.h>

#include <pthread.h>
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
#include "../version.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif


typedef struct input_ctx_s input_ctx_t;
struct input_ctx_s
{
	int inotifyfd;
	int dirfd;
	const char *root;
	const char *name;
	const char *input_path;
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

#ifdef USE_LIBINPUT
static int open_restricted(const char *path, int flags, void *user_data)
{
        int fd = open(path, flags);
        return fd < 0 ? -errno : fd;
}
static void close_restricted(int fd, void *user_data)
{
        close(fd);
}
const static struct libinput_interface interface = {
        .open_restricted = open_restricted,
        .close_restricted = close_restricted,
};
#endif

int input_checkstate(void *data, json_t *params)
{
	input_ctx_t *ctx = (input_ctx_t *)data;
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

int input_parseevent(input_ctx_t *ctx, const struct input_event *event)
{
	if (event->type != EV_KEY)
		return -1;
	if (event->value != 0) // check only keyrelease event
		return 0;
	switch (event->code)
	{
	case KEY_PLAYPAUSE:
		if (ctx->state == STATE_PLAY)
			client_pause(ctx->client, input_checkstate, ctx);
		else
			client_play(ctx->client, input_checkstate, ctx);
	break;
	case KEY_PLAYCD:
	case KEY_PLAY:
		client_play(ctx->client, input_checkstate, ctx);
	break;
	case KEY_PAUSECD:
	case KEY_PAUSE:
		client_pause(ctx->client, input_checkstate, ctx);
	break;
	case KEY_STOPCD:
	case KEY_STOP:
		client_stop(ctx->client, input_checkstate, ctx);
	break;
	case KEY_NEXTSONG:
	case KEY_NEXT:
		client_next(ctx->client, input_checkstate, ctx);
	break;
	case KEY_R:
	break;
	}
	return 0;
}

int run_client(void *arg)
{
	input_ctx_t *ctx = (input_ctx_t *)arg;

	client_data_t data = {0};
	client_unix(ctx->socketpath, &data);
	ctx->client = &data;

	pthread_t thread;
	pthread_create(&thread, NULL, client_loop, (void *)&data);

#ifdef USE_LIBINPUT
	struct libinput *li;
	struct libinput_event *ievent;
	struct udev *udev = udev_new();

	li = libinput_udev_create_context(&interface, NULL, udev);
	libinput_udev_assign_seat(li, "seat0");
	libinput_dispatch(li);
	while ((ievent = libinput_get_event(li)) != NULL) {
		// handle the event here
		if (libinput_event_get_type(ievent) == LIBINPUT_EVENT_KEYBOARD_KEY)
		{
			struct libinput_event_keyboard *event_kb = libinput_event_get_keyboard_event(ievent);
			if (event_kb)
			{
				dbg("event %p", event_kb);
				struct input_event event;
				event.type = EV_KEY;
				event.code = libinput_event_keyboard_get_key(event_kb);
				event.value = libinput_event_keyboard_get_key_state(event_kb);

				input_parseevent(ctx, &event);
			}
		}
		libinput_event_destroy(ievent);
		libinput_dispatch(li);
	}
	libinput_unref(li);
#else
	ctx->inputfd = open(ctx->input_path, O_RDONLY);
	while ((ctx->inputfd > 0 && ctx->run))
	{
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(ctx->inputfd, &rfds);
		int maxfd = ctx->inputfd;
		int ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if (ret > 0 && FD_ISSET(ctx->inputfd, &rfds))
		{
			struct input_event event;
			ret = read(ctx->inputfd, &event, sizeof(event));
			input_parseevent(ctx, &event);
		}
	}
#endif
	pthread_join(thread, NULL);
	unlink(ctx->socketpath);
	return 0;
}

#ifdef USE_INOTIFY
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

static void *_check_socket(void *arg)
{
	input_ctx_t *ctx = (input_ctx_t *)arg;
	ctx->socketpath = malloc(strlen(ctx->root) + 1 + strlen(ctx->name) + 1);
	sprintf(ctx->socketpath, "%s/%s", ctx->root, ctx->name);
	if (!access(ctx->socketpath, R_OK | W_OK))
	{
		run_client((void *)ctx);
	}
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
					if (!access(ctx->socketpath, R_OK | W_OK))
					{
						run_client((void *)ctx);
					}
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
	free(ctx->socketpath);
}
#endif

#define DAEMONIZE 0x01
int main(int argc, char **argv)
{
	int mode = 0;
	input_ctx_t input_data = {
		.root = "/tmp",
		.name = basename(argv[0]),
		.input_path = "/dev/input/event0",
	};
	
	int opt;
	do
	{
		opt = getopt(argc, argv, "R:n:i:hD");
		switch (opt)
		{
			case 'R':
				input_data.root = optarg;
			break;
			case 'n':
				input_data.name = optarg;
			break;
			case 'i':
				input_data.input_path = optarg;
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
	input_data.inotifyfd = inotify_init();
	int dirfd = inotify_add_watch(input_data.inotifyfd, input_data.root,
					IN_MODIFY | IN_CREATE | IN_DELETE);
	input_data.run = 1;
	_check_socket((void *)&input_data);
#else
	input_data.socketpath = malloc(strlen(input_data.root) + 1 + strlen(input_data.name) + 1);
	sprintf(input_data.socketpath, "%s/%s", input_data.root, input_data.name);

	run_client((void *)&input_data);
	free(input_data.socketpath);
#endif
	return 0;
}

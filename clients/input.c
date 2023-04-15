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

#include "daemonize.h"
#include "client_json.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

typedef void *(*__start_routine_t) (void *);

#define MAX_INPUTS 3

typedef struct input_ctx_s input_ctx_t;
struct input_ctx_s
{
	int inotifyfd;
	int dirfd;
	const char *root;
	const char *name;
	const char *input_path;
	json_t *media;
	int media_id;
	int inputfd[MAX_INPUTS];
	char *socketpath;
	client_data_t *client;
	char run;
	enum
	{
		STATE_UNKNOWN,
		STATE_PLAY,
		STATE_PAUSE,
		STATE_STOP,
		STATE_RANDOM = 0x10,
		STATE_LOOP = 0x20,
	} state;
};
#define STATE_OPTIONSMASK (STATE_RANDOM | STATE_LOOP)

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

static int input_checkstate(void *data, json_t *params)
{
	input_ctx_t *ctx = (input_ctx_t *)data;
	json_t *jid = json_object_get(params, "id");
	int id = -1;
	if (json_is_number(jid))
		id = json_integer_value(jid);

	json_t *jstate = json_object_get(params, "state");
	if (json_is_string(jstate))
	{
		const char *state = json_string_value(jstate);
		if (!strcmp(state, "play"))
			ctx->state = STATE_PLAY;
		else if (!strcmp(state, "pause"))
			ctx->state = STATE_PAUSE;
		else if (!strcmp(state, "stop"))
			ctx->state = STATE_STOP;
	}

	json_t *value;
	size_t index;
	json_t *joptions = json_object_get(params, "options");
	json_array_foreach(joptions, index, value)
	{
		const char *string = json_string_value(value);
		if (string && !strcmp(string, "random"))
			ctx->state |= STATE_RANDOM;
		if (string && !strcmp(string, "loop"))
			ctx->state |= STATE_LOOP;
	}

	return 0;
}

static int input_parseevent_rel(input_ctx_t *ctx, const struct input_event *event)
{
	if (event->type != EV_REL)
		return -4;

	if (event->code != REL_HWHEEL)
		return -4;
	dbg("rel: %d", event->value);

	int ret = -1;
	ret = client_volume(ctx->client, NULL, ctx, event->value * 5);

	return ret;
}

static int input_parseevent_key(input_ctx_t *ctx, const struct input_event *event)
{
	if (event->type != EV_KEY)
	{
		return -4;
	}
	if (event->value != 0) // check only keyrelease event
	{
		return 0;
	}

	int ret = 0;
	switch (event->code)
	{
	case KEY_PLAYPAUSE:
		dbg("key KEY_PLAYPAUSE %X", ctx->state);
		if ((ctx->state & ~STATE_OPTIONSMASK) == STATE_PLAY)
			ret = client_pause(ctx->client, input_checkstate, ctx);
		else
			ret = client_play(ctx->client, input_checkstate, ctx);
	break;
	case KEY_PLAYCD:
	case KEY_PLAY:
		dbg("key KEY_PLAY");
		ret = client_play(ctx->client, input_checkstate, ctx);
	break;
	case KEY_PAUSECD:
	case KEY_PAUSE:
		dbg("key KEY_PAUSE");
		ret = client_pause(ctx->client, input_checkstate, ctx);
	break;
	case KEY_STOPCD:
	case KEY_STOP:
		dbg("key KEY_STOP");
		ret = client_stop(ctx->client, input_checkstate, ctx);
	break;
	case KEY_NEXTSONG:
	case KEY_NEXT:
		dbg("key KEY_NEXT");
		ret = client_next(ctx->client, input_checkstate, ctx);
	break;
	case KEY_VOLUMEDOWN:
		dbg("key KEY_VOLUMEDOWN");
		ret = client_volume(ctx->client, NULL, ctx, -5);
	break;
	case KEY_VOLUMEUP:
		dbg("key KEY_VOLUMEUP");
		ret = client_volume(ctx->client, NULL, ctx, +5);
	break;
	case KEY_SHUFFLE:
		dbg("key KEY_SHUFFLE");
		ret = media_options(ctx->client, NULL, ctx, (ctx->state & STATE_RANDOM)?0:1, (ctx->state & STATE_LOOP)?0:1);
	break;
	case KEY_PROG1:
		dbg("key KEY_PROG1");
		if (ctx->media)
		{
			int i = ctx->media_id;
			if (i >= json_array_size(ctx->media))
				i = 0;
			json_t *media = json_array_get(ctx->media, i);
			if (json_is_object(media))
			{
				ret = media_change(ctx->client, NULL, ctx, json_incref(media));
			}
			ctx->media_id = i + 1;
		}
	break;
	case KEY_PROG2:
		dbg("key KEY_PROG2");
		if (ctx->media)
		{
			int i = ctx->media_id;
			if (i == -1)
				i = json_array_size(ctx->media) - 1;
			json_t *media = json_array_get(ctx->media, i);
			if (json_is_object(media))
			{
				ret = media_change(ctx->client, NULL, ctx, json_incref(media));
			}
			ctx->media_id = i - 1;
		}
	break;
	default:
		dbg("key %d", event->code);
	}
	dbg("%s ret %d", __FUNCTION__, ret);
	return ret;
}

static int input_closing(void *data, json_t *params)
{
	input_ctx_t *ctx = (input_ctx_t *)data;
	ctx->run = 0;
	return 0;
}

#ifdef USE_LIBINPUT
static int run_libinput(input_ctx_t *ctx)
{
	struct libinput *li;
	struct libinput_event *ievent;
	struct udev *udev = udev_new();
	li = libinput_udev_create_context(&interface, NULL, udev);
	libinput_udev_assign_seat(li, "seat0");
	libinput_dispatch(li);
	while ((ievent = libinput_get_event(li)) != NULL) {
		// handle the event here
		switch (libinput_event_get_type(ievent))
		{
			case LIBINPUT_EVENT_KEYBOARD_KEY:
			{
				struct libinput_event_keyboard *event_kb = libinput_event_get_keyboard_event(ievent);
				if (event_kb)
				{
					struct input_event event;
					event.type = EV_KEY;
					event.code = libinput_event_keyboard_get_key(event_kb);
					event.value = libinput_event_keyboard_get_key_state(event_kb);

					input_parseevent(ctx, &event);
				}
			}
			break;
			case LIBINPUT_EVENT_POINTER_AXIS:
			{
				struct libinput_event_pointer *event_pt = libinput_event_get_pointer_event(ievent);
				if (libinput_event_pointer_has_axis(event_pt, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
				{
					int value = libinput_event_pointer_get_axis_value(event_pt, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
				}
			}
			break;
		}
		libinput_event_destroy(ievent);
		libinput_dispatch(li);
	}
	libinput_unref(li);
	return0
}
#endif

static int run(input_ctx_t *ctx)
{
	while (ctx->run)
	{
		struct input_event event;
		int maxfd = 0;
		fd_set rfds;
		struct timeval timeout = {2,0};

		FD_ZERO(&rfds);
		for (int i = 0; i < MAX_INPUTS && ctx->inputfd[i]; i++)
		{
			FD_SET(ctx->inputfd[i], &rfds);
			maxfd = (maxfd < ctx->inputfd[i])?ctx->inputfd[i]:maxfd;
		}
		int ret = select(maxfd + 1, &rfds, NULL, NULL, &timeout);
		for (int i = 0; i < MAX_INPUTS && ctx->inputfd[i] > 0; i++)
		{
			if (FD_ISSET(ctx->inputfd[i], &rfds))
			{
				ret = read(ctx->inputfd[i], &event, sizeof(event));
			}
		}
		if (ret > 0 && ctx->client != NULL)
		{
			ret = input_parseevent_key(ctx, &event);
			if (ret == -4)
				ret = input_parseevent_rel(ctx, &event);
			if (ret == -4)
				ret = 0;
		}
		if (ret < 0 && ret != CLIENT_WAITING)
		{
			err("input: client error %s", strerror(errno));
			break;
		}
	}
	for (int i = 0; i < MAX_INPUTS && ctx->inputfd[i]; i++)
	{
		close(ctx->inputfd[i]);
	}
	return 0;
}

static int run_client(void *arg)
{
	input_ctx_t *ctx = (input_ctx_t *)arg;

	client_data_t data = {0};
	if (client_unix(ctx->socketpath, &data) < 0)
	{
		warn("input: server not ready");
		return -1;
	}
	client_async(&data, 1);
	client_eventlistener(&data, "onchange", input_checkstate, ctx);
	ctx->client = &data;

	int ret = client_loop(&data);
	ctx->client = NULL;

	client_disconnect(&data);
	return ret;
}

#ifdef USE_INOTIFY
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

static void *_check_socket(void *arg)
{
	input_ctx_t *ctx = (input_ctx_t *)arg;

	int inotifyfd = inotify_init();
	int dirfd = inotify_add_watch(inotifyfd, ctx->root,
					IN_MODIFY | IN_CREATE | IN_DELETE);

	while (1)
	{
		if (!access(ctx->socketpath, R_OK | W_OK))
		{
			if (run_client((void *)ctx) < 0)
			{
				// wait and retry
				sleep(1);
				continue;
			}
		}

		char buffer[BUF_LEN];
		int length;
		length = read(inotifyfd, buffer, BUF_LEN);

		if (length < 0)
		{
			break;
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
					dbg("The file %s was unlinked.", event->name);
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

#define DAEMONIZE 0x01
int main(int argc, char **argv)
{
	const char *pidfile = NULL;
	int mode = 0;
	input_ctx_t data = {
		.root = "/tmp",
		.name = "putv",
		.inputfd = {0},
	};
	const char *media_path;
	int nbinputs = 0;

	int opt;
	int fd;
	do
	{
		opt = getopt(argc, argv, "R:n:i:m:hDp:L:");
		switch (opt)
		{
			case 'R':
				data.root = optarg;
			break;
			case 'n':
				data.name = optarg;
			break;
			case 'i':
				fd = open(optarg, O_RDONLY);
				if (fd > 0)
					data.inputfd[nbinputs++] = fd;
			break;
			case 'm':
				media_path = optarg;
			break;
			case 'h':
				fprintf(stderr, "input -R <dir> -n <socketname> -D -m <jsonfile>\n");
				fprintf(stderr, "send events from input to putv applications\n");
				fprintf(stderr, " -D         daemonize\n");
				fprintf(stderr, " -R <DIR>   change the socket directory directory\n");
				fprintf(stderr, " -n <NAME>  change the socket name\n");
				fprintf(stderr, " -m <FILE>  load Json file to manage media\n");
				fprintf(stderr, " -i <PATH>  input device\n");
				return -1;
			break;
			case 'D':
				mode |= DAEMONIZE;
			break;
			case 'p':
				pidfile = optarg;
			break;
			case 'L':
			{
				int logfd = open(optarg, O_WRONLY | O_CREAT | O_TRUNC, 00644);
				if (logfd > 0)
				{
					dup2(logfd, 1);
					dup2(logfd, 2);
					close(logfd);
				}
			}
			break;
		}
	} while(opt != -1);

	if ((mode & DAEMONIZE) && daemonize(pidfile) == -1)
	{
		return 0;
	}

	if (nbinputs == 0)
		data.inputfd[nbinputs++] = open("/dev/input/event0", O_RDONLY);
	json_error_t error;
	if (media_path)
	{
		json_t *media = NULL;
		media = json_load_file(media_path, 0, &error);
		if (media && json_is_object(media))
		{
			data.media = json_object_get(media, "media");
		}
		else
			err("media error: %d,%d %s", error.line, error.column, error.text);
	}
	if (data.media && ! json_is_array(data.media))
	{
		json_decref(data.media);
		data.media = NULL;
	}
	data.run = 1;
	data.socketpath = malloc(strlen(data.root) + 1 + strlen(data.name) + 1);
	sprintf(data.socketpath, "%s/%s", data.root, data.name);

	pthread_t thread;
#ifdef USE_INOTIFY
	pthread_create(&thread, NULL, (__start_routine_t)_check_socket, (void *)&data);
#else
	pthread_create(&thread, NULL, (__start_routine_t)run_client, (void *)&data);
#endif
	run(&data);

	dbg("waiting end of thread");
	pthread_join(thread, NULL);

	json_decref(data.media);
	free(data.socketpath);
	return 0;
}

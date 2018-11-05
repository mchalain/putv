/*****************************************************************************
 * cmds_input.c
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
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>

#include <pthread.h>

#include "player.h"
typedef struct cmds_ctx_s cmds_ctx_t;
struct cmds_ctx_s
{
	player_ctx_t *player;
	media_t *media;
	pthread_t thread;
	char *inputpath;
	int run;
	int fd;
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

typedef int (*method_t)(cmds_ctx_t *ctx, char *arg);

static int method_play(cmds_ctx_t *ctx, char *arg)
{
	return (player_state(ctx->player, STATE_PLAY) == STATE_PLAY);
}

static int method_pause(cmds_ctx_t *ctx, char *arg)
{
	return (player_state(ctx->player, STATE_PAUSE) == STATE_PAUSE);
}

static int method_stop(cmds_ctx_t *ctx, char *arg)
{
	return (player_state(ctx->player, STATE_STOP) == STATE_STOP);
}

static int method_quit(cmds_ctx_t *ctx, char *arg)
{
	return (player_state(ctx->player, STATE_ERROR) == STATE_ERROR);
}

static int method_next(cmds_ctx_t *ctx, char *arg)
{
	return (player_state(ctx->player, STATE_CHANGE) == STATE_CHANGE);
}

static int method_random(cmds_ctx_t *ctx, char *arg)
{
	int enable = 1;
	if (arg != NULL)
		atoi(arg);
	enable = ctx->media->ops->options(ctx->media->ctx, MEDIA_RANDOM, enable);
	return enable;
}

static int cmds_input_cmd(cmds_ctx_t *ctx)
{
	ctx->run = 1;

	while (ctx->run)
	{
		int ret;
		fd_set rfds;
		struct timeval timeout = {1, 0};

		FD_ZERO(&rfds);
		FD_SET(ctx->fd, &rfds);
		int maxfd = ctx->fd;
		ret = select(maxfd + 1, &rfds, NULL, NULL, &timeout);
		if (ret > 0 && FD_ISSET(ctx->fd, &rfds))
		{
			struct input_event event;
			ret = read(ctx->fd, &event, sizeof(event));
			method_t method = NULL;
			if (event.type != EV_KEY)
				continue;
			if (event.value != 0) // check only keyrelease event
				continue;
			switch (event.code)
			{
			case KEY_PLAYPAUSE:
				if (player_state(ctx->player, STATE_UNKNOWN) == STATE_PAUSE)
					method = method_play;
				else
					method = method_pause;
			break;
			case KEY_PLAYCD:
			case KEY_PLAY:
				method = method_play;
			break;
			case KEY_PAUSECD:
			case KEY_PAUSE:
				method = method_pause;
			break;
			case KEY_STOPCD:
			case KEY_STOP:
				method = method_stop;
			break;
			case KEY_NEXTSONG:
			case KEY_NEXT:
				method = method_next;
			break;
			case KEY_R:
				method = method_random;
			break;
			case KEY_EXIT:
				method = method_quit;
				ctx->run = 0;
			break;
			}
			if (method)
			{
				method(ctx, NULL);
			}
		}
	}
	return 0;
}

static void *_cmds_input_pthread(void *arg)
{
	int ret;
	cmds_ctx_t *ctx = (cmds_ctx_t *)arg;
	ret = cmds_input_cmd(ctx);
	return (void*)ret;
}

static int cmds_input_run(cmds_ctx_t *ctx)
{
	dbg("input open %s", ctx->inputpath);
	ctx->fd = open(ctx->inputpath, O_RDONLY);
	pthread_create(&ctx->thread, NULL, _cmds_input_pthread, (void *)ctx);

	return 0;
}

static cmds_ctx_t *cmds_input_init(player_ctx_t *player, media_t *media, void *arg)
{
	cmds_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->player = player;
	ctx->media = media;
	ctx->inputpath = arg;
	return ctx;
}

static void cmds_input_destroy(cmds_ctx_t *ctx)
{
	ctx->run = 0;
	pthread_join(ctx->thread, NULL);
	close(ctx->fd);
	free(ctx);
}

cmds_ops_t *cmds_input = &(cmds_ops_t)
{
	.init = cmds_input_init,
	.run = cmds_input_run,
	.destroy = cmds_input_destroy,
};

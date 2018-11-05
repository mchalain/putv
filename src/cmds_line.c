/*****************************************************************************
 * cmds_line.c
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
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <pthread.h>

#include "player.h"
typedef struct cmds_ctx_s cmds_ctx_t;
struct cmds_ctx_s
{
	player_ctx_t *player;
	media_t *media;
	pthread_t thread;
	int run;
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

static int method_append(cmds_ctx_t *ctx, char *arg)
{
	return ctx->media->ops->insert(ctx->media->ctx, arg, NULL, NULL);
}

static int method_remove(cmds_ctx_t *ctx, char *arg)
{
	if (arg != NULL)
	{
		int id = atoi(arg);
		if (id > 0)
			return ctx->media->ops->remove(ctx->media->ctx, id, NULL);
		else
			return ctx->media->ops->remove(ctx->media->ctx, 0, arg);
	}
	else
		return -1;
}

static int _print_entry(void *arg, const char *url,
		const char *info, const char *mime)
{
	int *index = (int*)arg;
	printf("playlist[%d]: %s\n", *index, url);
	if (info != NULL)
		printf("\t%s\n", info);
	(*index)++;
	return 0;
}

static int method_list(cmds_ctx_t *ctx, char *arg)
{
	int value = 0;
	return ctx->media->ops->list(ctx->media->ctx, _print_entry, (void *)&value);
}

static int _import_entry(void *arg, const char *url,
		const char *info, const char *mime)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)arg;
	ctx->media->ops->insert(ctx->media->ctx, url, info, mime);
	return 0;
}

static int method_import(cmds_ctx_t *ctx, char *arg)
{
	struct stat _stat;

	stat(arg, &_stat);
#ifdef MEDIA_IMPORT
	if (S_ISDIR(_stat.st_mode))
	{
		media_ctx_t *media_ctx = media_dir->init(arg);
		media_dir->list(media_ctx, _import_entry, (void *)ctx);
		media_dir->destroy(media_ctx);
	}
#endif
	return 0;
}

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

static int method_loop(cmds_ctx_t *ctx, char *arg)
{
	int enable = 1;
	if (arg != NULL)
		atoi(arg);
	enable = ctx->media->ops->options(ctx->media->ctx, MEDIA_LOOP, enable);
	return enable;
}

static int _display(void *arg, const char *url, const char *info, const char *mime)
{
	cmds_ctx_t *ctx = (cmds_ctx_t*)arg;
	printf("player: media %s\n", url);
}

void cmds_line_onchange(void *arg, player_ctx_t *player, state_t state)
{
	cmds_ctx_t *ctx = (cmds_ctx_t*)arg;

	switch (state)
	{
	case STATE_PLAY:
		printf("player: play\n");
	break;
	case STATE_PAUSE:
		printf("player: pause\n");
	break;
	case STATE_STOP:
		printf("player: stop\n");
	break;
	case STATE_ERROR:
		printf("player: error\n");
	break;
	case STATE_CHANGE:
		printf("player: change\n");
	break;
	default:
		dbg("cmd line onchange");
	}
	int id = player_mediaid(player);
	ctx->media->ops->find(ctx->media->ctx, id, _display, ctx);
}

static cmds_ctx_t *cmds_line_init(player_ctx_t *player, media_t *media, void *arg)
{
	cmds_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->player = player;
	ctx->media = media;
	return ctx;
}

static int cmds_line_cmd(cmds_ctx_t *ctx)
{
	int fd = 0;
	ctx->run = 1;

	player_onchange(ctx->player, cmds_line_onchange, ctx);
	while (ctx->run)
	{
		int ret;
		fd_set rfds;
		struct timeval timeout = {1, 0};

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		int maxfd = fd;
		ret = select(maxfd + 1, &rfds, NULL, NULL, &timeout);
		if (ret > 0 && FD_ISSET(fd, &rfds))
		{
			int length;
			ret = ioctl(fd, FIONREAD, &length);
			char *buffer = malloc(length + 1);
			ret = read(fd, buffer, length);
			int i;
			method_t method = NULL;
			char *arg = NULL;
			for (i = 0; i < length; i++)
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
				if (!strncmp(buffer + i, "append", 6))
				{
					method = method_append;
					i += 6;
				}
				if (!strncmp(buffer + i, "remove", 6))
				{
					method = method_remove;
					i += 6;
				}
				if (!strncmp(buffer + i, "list",4))
				{
					method = method_list;
					i += 4;
				}
				if (!strncmp(buffer + i, "import",6))
				{
					method = method_import;
					i += 6;
				}
				if (!strncmp(buffer + i, "play",4))
				{
					method = method_play;
					i += 4;
				}
				if (!strncmp(buffer + i, "pause",5))
				{
					method = method_pause;
					i += 5;
				}
				if (!strncmp(buffer + i, "stop",4))
				{
					method = method_stop;
					i += 4;
				}
				if (!strncmp(buffer + i, "next",4))
				{
					method = method_next;
					i += 4;
				}
				if (!strncmp(buffer + i, "loop",4))
				{
					method = method_loop;
					i += 4;
				}
				if (!strncmp(buffer + i, "quit",4))
				{
					method = method_quit;
					ctx->run = 0;
				}
			}
			if (method)
			{
				char *end = NULL;
				if (arg)
					end = strchr(arg, '\n');
				if (end != NULL)
					*end = '\0';
				method(ctx, arg);
			}
		}
	}
	return 0;
}

static void *_cmds_line_pthread(void *arg)
{
	int ret;
	cmds_ctx_t *ctx = (cmds_ctx_t *)arg;
	ret = cmds_line_cmd(ctx);
	return (void*)ret;
}

static int cmds_line_run(cmds_ctx_t *ctx)
{
	pthread_create(&ctx->thread, NULL, _cmds_line_pthread, (void *)ctx);

	return 0;
}

static void cmds_line_destroy(cmds_ctx_t *ctx)
{
	ctx->run = 0;
	pthread_join(ctx->thread, NULL);
	free(ctx);
}

cmds_ops_t *cmds_line = &(cmds_ops_t)
{
	.init = cmds_line_init,
	.run = cmds_line_run,
	.destroy = cmds_line_destroy,
};

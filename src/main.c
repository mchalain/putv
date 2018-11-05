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

#include <libgen.h>

#include <time.h>
#include <signal.h>

#include <pthread.h>

#include "player.h"
#include "encoder.h"
#include "sink.h"
#include "media.h"
#include "cmds.h"
#include "../version.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#ifdef USE_TIMER
timer_t timerid;

static void _autostart(union sigval arg)
{
	player_ctx_t *player = (player_ctx_t *)arg.sival_ptr;
	if (player_state(player, STATE_UNKNOWN) != STATE_PLAY)
		player_state(player, STATE_PLAY);
	if (player_mediaid(player) < 0)
	{
		struct itimerspec timer = {{5, 0}, {5, 0}};
		timer_settime(timerid, 0, &timer,NULL);
	}
	else
		timer_delete(timerid);
}
#endif

static int run_player(player_ctx_t *player, jitter_t *sink_jitter)
{
	int ret;
	const encoder_t *encoder;
	encoder_ctx_t *encoder_ctx;
	jitter_t *encoder_jitter = NULL;

	encoder = ENCODER;
	encoder_ctx = encoder->init(player);
	encoder->run(encoder_ctx, sink_jitter);
	encoder_jitter = encoder->jitter(encoder_ctx);

	if (encoder_jitter != NULL)
		ret = player_run(player, encoder_jitter);
	encoder->destroy(encoder_ctx);
	return ret;
}

#define DAEMONIZE 0x01
#define SRC_STDIN 0x02
#define AUTOSTART 0x04
#define LOOP 0x08
#define RANDOM 0x10
int main(int argc, char **argv)
{
	const char *mediapath = SYSCONFDIR"/putv.db";
	const char *outarg = "default";
	media_ctx_t *media_ctx;
	pthread_t thread;
	const char *root = "/tmp";
	int mode = 0;
	const char *name = basename(argv[0]);
	
	int opt;
	do
	{
		opt = getopt(argc, argv, "R:m:o:hDVxalr");
		switch (opt)
		{
			case 'R':
				root = optarg;
			break;
			case 'm':
				mediapath = optarg;
			break;
			case 'o':
				outarg = optarg;
			break;
			case 'h':
				return -1;
			break;
			case 'x':
				mode |= SRC_STDIN;
			break;
			case 'D':
				mode |= DAEMONIZE;
			break;
			case 'a':
				mode |= AUTOSTART;
			break;
			case 'l':
				mode |= LOOP;
			break;
			case 'r':
				mode |= RANDOM;
			break;
		}
	} while(opt != -1);

	if ((mode & DAEMONIZE) && fork() != 0)
	{
		return 0;
	}

	media_ctx = MEDIA->init(mediapath);
	if (media_ctx == NULL)
	{
		err("media not found %s", mediapath);
		return -1;
	}
	media_t *media = &(media_t)
	{
		.ops = MEDIA,
		.ctx = media_ctx,
	};

	if (mode & SRC_STDIN)
	{
		dbg("insert stdin");
		media->ops->insert(media_ctx, "-", NULL, "audio/mp3");
	}

	if (mode & LOOP)
	{
		media->ops->options(media_ctx, MEDIA_LOOP, 1);
	}

	if (mode & RANDOM)
	{
		media->ops->options(media_ctx, MEDIA_RANDOM, 1);
	}

	player_ctx_t *player = player_init(media);

	if (mode & AUTOSTART)
	{
		player_state(player, STATE_PLAY);
#ifdef USE_TIMER
		if (media->ops->current(media_ctx, NULL, NULL) != 0)
		{
			int ret;
			struct sigevent event;
			event.sigev_notify = SIGEV_THREAD;
			event.sigev_value.sival_ptr = player;
			event.sigev_notify_function = _autostart;
			event.sigev_notify_attributes = NULL;
			ret = timer_create(CLOCK_REALTIME, &event, &timerid);

			struct itimerspec timer = {{5, 0}, {5, 0}};
			ret = timer_settime(timerid, 0, &timer,NULL);

		}
#endif
	}

	cmds_t cmds[3];
	int nbcmds = 0;
#ifdef CMDLINE
	if (!(mode & DAEMONIZE))
	{
		cmds[nbcmds].ops = cmds_line;
		cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, media, NULL);
		nbcmds++;
	}
#endif
#ifdef CMDINPUT
	cmds[nbcmds].ops = cmds_input;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, media, CMDINPUT_PATH);
	nbcmds++;
#endif
#ifdef JSONRPC
	char socketpath[256];
	snprintf(socketpath, sizeof(socketpath) - 1, "%s/%s", root, name);
	cmds[nbcmds].ops = cmds_json;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, media, (void *)socketpath);
	nbcmds++;
#endif

	int i;
	for (i = 0; i < nbcmds; i++)
		cmds[i].ops->run(cmds[i].ctx);

	const sink_t *sink;
	sink_ctx_t *sink_ctx;
	jitter_t *sink_jitter = NULL;

	sink = SINK;
	sink_ctx = sink->init(player, outarg);
	sink->run(sink_ctx);
	sink_jitter = sink->jitter(sink_ctx);

	run_player(player, sink_jitter);

	sink->destroy(sink_ctx);
	player_destroy(player);

	for (i = 0; i < nbcmds; i++)
	{
		cmds[i].ops->destroy(cmds[i].ctx);
	}
	return 0;
}

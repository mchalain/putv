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
#include <stdlib.h>
#include <unistd.h>

#include <libgen.h>

#include <time.h>
#include <signal.h>

#include <pthread.h>
#include <string.h>
#include <errno.h>

/**
 * user database access
 */
# include <pwd.h>
# include <grp.h>
/**
 * directory access
 */
# include <sys/stat.h>
# include <sys/types.h>
# include <unistd.h>
/**
 * file access
 */
# include <fcntl.h>

/**
 * process priority
 */
#include <sys/resource.h>

#include "player.h"
#include "encoder.h"
#include "sink.h"
#include "media.h"
#include "cmds.h"
#include "daemonize.h"

#define STINGIFY(text) #text

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
	usleep(100 * 1000);
	if (player_mediaid(player) < 0)
	{
		struct itimerspec timer = {{0, 0}, {5, 0}};
		timer_settime(timerid, 0, &timer,NULL);
	}
	else
		timer_delete(timerid);
}
#endif

void help(const char *name)
{
	fprintf(stderr, "%s [-R <websocketdir>][-m <media>][-o <output>][-p <pidfile>]\n", name);
	fprintf(stderr, "\t...[-f <filtername>][-x][-D][-a][-r][-l][-L <logfile>]\n");
	fprintf(stderr, "\t...[-d <directory>][-R <directory>]\n");
	fprintf(stderr, "\t...[-P [0-99]]\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t -m <media>\tSet the media supporting audio files\n");
	fprintf(stderr, "\t -o <output>\tSet the sink URL (default: alsa:default)\n");
	fprintf(stderr, "\t -a\t\tAuto play enabled\n");
	fprintf(stderr, "\t -r\t\tShuffle enabled\n");
	fprintf(stderr, "\t -l\t\tLoop enabled\n");
	fprintf(stderr, "\t -R <directory>\tSet the directory for command socket file\n");
	fprintf(stderr, "\t -d <directory>\tSet the working directory\n");
	fprintf(stderr, "\t -P <priority>\tSet the process priority\n");
	fprintf(stderr, "\t -f <filter>\tSet a filter and its features (default: pcm\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t filters:\n");
	fprintf(stderr, "\t pcm\tstereo interleaved stream\n");
	fprintf(stderr, "\t pcm?mono=left\tmono stream with left channel\n");
	fprintf(stderr, "\t pcm?mono=right\tmono stream with right channel\n");
	fprintf(stderr, "\t pcm?mono=mixed\tmono stream with left+right channels\n");
	fprintf(stderr, "\t pcm?stats\tprint statistics about the stream\n");

}

static encoder_t *main_encoder(player_ctx_t *player, sink_t *sink)
{
	encoder_t *encoder = malloc(sizeof(*encoder));

	/**
	 * the sink must to run before to start the encoder
	 */
	sink->ops->run(sink->ctx);
	// encoder initialization
	encoder->ops = sink->ops->encoder(sink->ctx);
	encoder->ctx = encoder->ops->init(player);
	if (encoder->ctx == NULL)
	{
		err("encoder not found");
		sink->ops->destroy(sink->ctx);
		free(encoder);
		return NULL;
	}
	// retreive an index of jitter for this kind of encoder
	int index = sink->ops->attach(sink->ctx, encoder->ops->mime(encoder->ctx));
	jitter_t *sink_jitter;
	sink_jitter = sink->ops->jitter(sink->ctx, index);

	// start encoder
	encoder->ops->run(encoder->ctx, sink_jitter);

	return encoder;
}

#define DAEMONIZE 0x01
#define SRC_STDIN 0x02
#define AUTOSTART 0x04
#define LOOP 0x08
#define RANDOM 0x10
#define KILLDAEMON 0x20
int main(int argc, char **argv)
{
	int priority = 0;
	const char *mediapath = NULL;
	const char *outarg = "default";
	pthread_t thread;
	const char *root = "/tmp";
	int mode = 0;
	const char *name = basename(argv[0]);
	const char *user = NULL;
	const char *pidfile = NULL;
	const char *filtername = "pcm";
	const char *logfile = NULL;
	const char *cwd = NULL;

	int opt;
	do
	{
		opt = getopt(argc, argv, "R:n:m:o:u:p:f:hDKVxalrL:d:P:");
		switch (opt)
		{
			case 'R':
				root = optarg;
			break;
			case 'n':
				name = optarg;
			break;
			case 'm':
				mediapath = optarg;
			break;
			case 'o':
				outarg = optarg;
			break;
			case 'u':
				user = optarg;
			break;
			case 'p':
				pidfile = optarg;
			break;
			case 'f':
				filtername = optarg;
			break;
			case 'h':
				help(basename(argv[0]));
				return -1;
			break;
			case 'x':
				mode |= SRC_STDIN;
			break;
			case 'D':
				mode |= DAEMONIZE;
			break;
			case 'K':
				mode |= KILLDAEMON;
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
			case 'L':
				logfile = optarg;
			break;
			case 'd':
				cwd = optarg;
			break;
			case 'P':
				priority = strtol(optarg, NULL, 10);
			break;
		}
	} while(opt != -1);

	if (mode & KILLDAEMON)
	{
		killdaemon(pidfile);
		return 0;
	}
	if ((mode & DAEMONIZE) && daemonize(pidfile) == -1)
	{
		return 0;
	}
	utils_srandom();

	if (logfile != NULL && logfile[0] != '\0')
	{
		int logfd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 00644);
		if (logfd > 0)
		{
			dup2(logfd, 1);
			dup2(logfd, 2);
			close(logfd);
		}
		else
			err("log file error %s", strerror(errno));
	}

	if (cwd != NULL && chdir(cwd) != 0)
		err("main: working directory %s", strerror(errno));

	if (priority > 0)
	{
		int gpriority = getpriority(PRIO_PGRP, 0);
		dbg("main: priority %d", gpriority);
		gpriority = priority * 40 / 100;
		gpriority -= 19;
		dbg("main: priority %d", gpriority);
		setpriority(PRIO_PGRP, 0, gpriority);
#ifdef USE_REALTIME
		int priority_limit;
		priority_limit = sched_get_priority_max(REALTIME_SCHED);
		if (priority > priority_limit)
			priority = priority_limit;
		priority_limit = sched_get_priority_min(REALTIME_SCHED);
		if (priority < priority_limit)
			priority = priority_limit;
		struct sched_param params;
		params.sched_priority = priority;
		dbg("main: priority %d", params.sched_priority);
		if (sched_setscheduler(0, REALTIME_SCHED, &params))
			err("schedluder modification error %s", strerror(errno));
#endif
	}

	player_ctx_t *player = player_init(filtername);
	if (player == NULL)
		return -1;

	uid_t pw_uid = getuid();
	gid_t pw_gid = getgid();
	if (user != NULL)
	{
		struct passwd *result;
		result = getpwnam(user);
		if (result == NULL)
		{
			err("Error: user %s not found\n", user);
			return -1;
		}
		pw_uid = result->pw_uid;
		pw_gid = result->pw_gid;
	}

	struct stat rootstat;
	int ret = stat(root,&rootstat);
	if (ret != 0)
	{
		mkdir(root, 0770);
		ret = stat(root,&rootstat);
		if (ret != 0)
		{
			err("the directory %s is not available", root);
		}
	}

	/**
	 * cmds_json must be initialize as soon as possible.
	 * Other applications mays needs it "immediatly"
	 */
	cmds_t cmds[3];
	int nbcmds = 0;
#ifdef JSONRPC
	char socketpath[256];
	snprintf(socketpath, sizeof(socketpath) - 1, "%s/%s", root, name);
	cmds[nbcmds].ops = cmds_json;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, (void *)socketpath);
	nbcmds++;
#endif

	if (!(mode & DAEMONIZE))
	{
#ifdef CMDLINE
		cmds[nbcmds].ops = cmds_line;
		cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, NULL);
		nbcmds++;
#endif
	}
	else
	{
#ifndef DEBUG
		int nullfd = open("/dev/null", O_WRONLY);
		if (nullfd > 0)
		{
			dup2(nullfd, 0);
			if (logfile == NULL)
			{
				dup2(nullfd, 1);
				dup2(nullfd, 2);
			}
			close(nullfd);
		}
#endif
	}
#ifdef CMDINPUT
	cmds[nbcmds].ops = cmds_input;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, CMDINPUT_PATH);
	nbcmds++;
#endif
#ifdef TINYSVCMDNS
#define MDNS_PATH "path="INDEX_HTML
	const char *txt[] =
	{
		MDNS_PATH,
#ifdef NETIF
		"if="NETIF,
#endif
		NULL,
	};
	cmds[nbcmds].ops = cmds_tinysvcmdns;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, txt);
	nbcmds++;
#ifdef NETIF2
	const char *txt2[] =
	{
		"path="STRINGIFY(INDEX_HTML),
		"if="NETIF2,
		NULL,
	};
	cmds[nbcmds].ops = cmds_tinysvcmdns;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, txt2);
	nbcmds++;
#endif
#endif

	sink_t *sink = NULL;

	sink = sink_build(player, outarg);

	if (sink == NULL)
	{
		err("output not set");
		goto end;
	}
	encoder_t *encoder = main_encoder(player, sink);

	int i;
	for (i = 0; i < nbcmds; i++)
	{
		if(cmds[i].ctx != NULL)
		{
			cmds[i].ops->run(cmds[i].ctx, sink);
		}
	}

	if (setegid(pw_gid))
		err("main: change group %s", strerror(errno));
	if (seteuid(pw_uid))
		err("main: start server as root");

	player_subscribe(player, encoder);

	if (mode & AUTOSTART)
	{
		dbg("autostart");
#ifdef USE_TIMER
		if (player_mediaid(player) < 0)
		{
			int ret;
			struct sigevent event;
			event.sigev_notify = SIGEV_THREAD;
			event.sigev_value.sival_ptr = player;
			event.sigev_notify_function = _autostart;
			event.sigev_notify_attributes = NULL;
			ret = timer_create(CLOCK_REALTIME, &event, &timerid);

			struct itimerspec timer = {{0, 0}, {0, 100 * 1000}};
			ret = timer_settime(timerid, 0, &timer,NULL);
		}
#endif
	}

	player_change(player, mediapath, ((mode & RANDOM) == RANDOM), ((mode & LOOP) == LOOP), 0);

	ret = player_run(player);

	encoder->ops->destroy(encoder->ctx);
	sink->ops->destroy(sink->ctx);
	player_destroy(player);

end:
	for (i = 0; i < nbcmds; i++)
	{
		if (cmds[i].ctx != NULL)
			cmds[i].ops->destroy(cmds[i].ctx);
	}
	killdaemon(pidfile);
	return 0;
}

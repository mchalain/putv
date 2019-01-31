/*****************************************************************************
 * src_alsa.c
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
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <pwd.h>

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define src_dbg(...)

typedef struct unixsocket_s
{
	int handle;
	int input;
	int output;
	enum
	{
		STATE_RUN,
		STATE_STOP,
		STATE_ERROR,
	} state;
} unixsocket_t;

static unixsocket_t *_init(const char *url)
{
	int count = 2;
	unixsocket_t *ctx = calloc(1, sizeof(*ctx));
	const char *path = NULL;

	if (strstr(url, "://") != NULL)
	{
		path = strstr(url, "unix://") + 7;
		if (path == NULL)
			return NULL;
	}
	else
	{
		path = url;
	}
	if (path[0] == '~')
	{
		struct passwd *pw = NULL;
		pw = getpwuid(geteuid());
		chdir(pw->pw_dir);
		path++;
		if (path[0] == '/')
			path++;
	}

	ctx->handle = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ctx->handle == 0)
	{
		free(ctx);
		return NULL;
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	int ret = connect(ctx->handle, (struct sockaddr *) &addr, sizeof(addr));
	if (ret < 0)
	{
		close(ctx->handle);
		free(ctx);
		return NULL;
	}

	ctx->input = 0;
	ctx->output = 1;
	return ctx;
}

static int _run(unixsocket_t *ctx)
{
	int ret;
	while (ctx->state == STATE_RUN)
	{
		fd_set rfds;
		int maxfd = ctx->input;

		FD_ZERO(&rfds);
		FD_SET(ctx->input, &rfds);
		if (ctx->handle)
		{
			maxfd = (maxfd < ctx->handle)?ctx->handle:maxfd;
			FD_SET(ctx->handle, &rfds);
		}
		ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if (ret > 0 && FD_ISSET(ctx->input, &rfds))
		{
			int ret;
			char buff[256];
			do
			{
				ret = read(ctx->input, buff, 255);
				if (ret > 0)
					ret = send(ctx->handle, buff, ret, MSG_NOSIGNAL);
			} while (ret > 0);
			if (ret < 0)
				ctx->state == STATE_ERROR;
		}
		if (ret > 0 && FD_ISSET(ctx->handle, &rfds))
		{
			char buff[256];
			int ret;

			do
			{
				ret = recv(ctx->handle, buff, 255, MSG_NOSIGNAL);
				if (ret > 0)
				{
					buff[ret] = '\0';
					dprintf(ctx->output, "%s", buff);
				}
				if (ret < 0)
					ctx->state == STATE_ERROR;
			} while (ret > 0);
		}
	}
	return ret;
}

int main(int argc, char *argv[])
{
	char *url = argv[1];
	unixsocket_t *ctx;

	if (argc > 1)
	{
		ctx = _init(url);
		_run(ctx);
	}
	return 0;
}
